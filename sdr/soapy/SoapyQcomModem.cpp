// SPDX-FileCopyrightText: 2026 Piotr Krysik
// SPDX-License-Identifier: GPL-3.0-only
// SoapyQcomModem -- SoapySDR receive driver for the MSM8916 modem sample stream.
//
// The driver talks to the `sdrd` service on the phone over plain TCP: a control connection (default port 5701) and a
// sample connection (default port 5700). No other process is needed on the host.
//
// Device arguments
//   host=<phone address>   default 172.16.42.1, environment QCOM_PHONE_HOST
//   port=<control port>    default 5701
//   cfo_hz=<Hz>            frequency error of the receiver oscillator to remove digitally (default 0)
//
// Controls
//   frequency   LTE downlink bands 1, 2, 3, 4, 5, 7, 8, 17, 20, 26, 27, 28 and free tuning outside them (the list comes
//               from the phone). The hardware tunes in 100 kHz steps inside a band and in 1 MHz steps outside; the
//               driver shifts the remainder digitally, so any frequency in a range can be set. A step to another
//               hardware channel retunes the receiver (about 0.3 - 0.5 s with a short gap in the data); a smaller
//               change is instant.
//   gain        "LNA", 0 .. 18.2 dB in six steps (nearest step is used); changes immediately.
//   bandwidth   analog filter 1.4 3 5 10 15 20 40 MHz (nearest). From 5 MHz up the digital channel filter, which cuts
//               at about 1.75 MHz, is bypassed. Changing between the two groups restarts the stream.
//   sample rate 7.2 .. 11.25 and 14.4 .. 14.8 MS/s, any value (the hardware reaches it within a few ppm; getSampleRate
//               returns the real value). Changing it restarts the stream (about 1 s). Default 7.68 MS/s.
//   antenna     "PRX" (the secondary receiver has no antenna).
//   time        every readStream() reports the time of its first sample (ns, on the receiver's own 19.2 MHz clock) with
//               SOAPY_SDR_HAS_TIME.
//
// Sample records are 3232 bytes: a 32-byte header {seq, t_a, t_b, ring block, ticks, flags, sample counter, clock
// ticks} and 1280 packed samples. Each sample is 20 bits: bits 0-7 Q mantissa, 8-15 I mantissa (signed), 16-19 shared
// exponent e; value = mantissa * 2^(8-e), delivered as mantissa << (16-e) so that the whole mantissa is kept (digital
// gain 2^8; exact for e >= 8, CS16 saturates below that).
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const size_t REC_BYTES = 3232, HDR_BYTES = 32, SPR = 1280; // record size, header size, samples per record
const size_t RING_RECS = 2048;                             // ~0.35 s of buffering
const double FS_DEF = 7.68e6, FS_MIN = 7.2e6, FS_MAX = 14.8e6, FS_GAP_LO = 11.25e6, FS_GAP_HI = 14.4e6,
             CLOCK_HZ    = 19.2e6;
const char* DEFAULT_HOST = "172.16.42.1";
const int   DEFAULT_PORT = 5701;
const int   NGAIN = 6, NBW = 7;
// gain state -> dB above the lowest state (measured with a tone)
const double GAIN_DB[NGAIN] = { 18.2, 9.6, 5.7, 1.3, 0.9, 0.0 };
const double BW_HZ[NBW]     = { 1.4e6, 3e6, 5e6, 10e6, 15e6, 20e6, 40e6 };
// bandwidth index from which the digital channel filter is bypassed
const int WIDE_FROM         = 2;

double now_s()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
int fcntl_nonblock(int fd, int on)
{
    int f = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
}

// Connects with a timeout; returns the socket or -1.
int tcp_connect(const char* host, int port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    fcntl_nonblock(fd, 1);
    int r = connect(fd, (struct sockaddr*)&a, sizeof a);
    if (r != 0 && errno == EINPROGRESS) {
        struct pollfd p = { fd, POLLOUT, 0 };
        if (poll(&p, 1, timeout_ms) == 1) {
            int       e = 0;
            socklen_t l = sizeof e;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
            r = e ? -1 : 0;
        } else
            r = -1;
    }
    if (r != 0) {
        close(fd);
        return -1;
    }
    fcntl_nonblock(fd, 0);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

// Sends one command line and reads one reply line. Returns false on error or timeout.
bool ctl_cmd(int fd, const char* cmd, char* reply, size_t rsz, int timeout_ms)
{
    char line[128];
    int  n = snprintf(line, sizeof line, "%s\n", cmd);
    if (send(fd, line, n, MSG_NOSIGNAL) != n)
        return false;
    size_t got = 0;
    double end = now_s() + timeout_ms / 1000.0;
    while (got + 1 < rsz) {
        double left = end - now_s();
        if (left <= 0)
            return false;
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, (int)(left * 1000) + 1) != 1)
            continue;
        char    c;
        ssize_t k = recv(fd, &c, 1, 0);
        if (k <= 0)
            return false;
        if (c == '\n') {
            reply[got] = 0;
            return true;
        }
        reply[got++] = c;
    }
    return false;
}
double reply_value(const char* reply, const char* key, double def)
{
    const char* p = strstr(reply, key);
    return p ? atof(p + strlen(key)) : def;
}

// Decodes sample k of a record payload.
inline void pcfl_get(const uint8_t* p, size_t k, int32_t& i, int32_t& q)
{
    size_t   bit = k * 20;
    uint64_t w;
    memcpy(&w, p + (bit >> 3), 8);
    uint32_t v  = (uint32_t)((w >> (bit & 7)) & 0xFFFFF);
    int32_t  qq = (int8_t)(v & 0xFF), ii = (int8_t)((v >> 8) & 0xFF);
    int      e = (int)((v >> 16) & 0xF);
    i          = ii * (1 << (16 - e));
    q          = qq * (1 << (16 - e));
}

} // namespace

class SoapyQcomModem : public SoapySDR::Device
{
public:
    explicit SoapyQcomModem(const SoapySDR::Kwargs& args)
    {
        const char* env = getenv("QCOM_PHONE_HOST");
        host_           = args.count("host") ? args.at("host") : (env && *env ? env : DEFAULT_HOST);
        port_           = args.count("port") ? atoi(args.at("port").c_str()) : DEFAULT_PORT;
        cfo_            = args.count("cfo_hz") ? atof(args.at("cfo_hz").c_str()) : 0.0;
        ring_           = (uint8_t*)malloc(RING_RECS * REC_BYTES);
        gap_            = (uint8_t*)calloc(RING_RECS, 1);
        anchor_         = (long long*)calloc(RING_RECS, sizeof(long long));
        pthread_mutex_init(&m_, 0);
        pthread_cond_init(&cv_, 0);
        ctl_ = tcp_connect(host_.c_str(), port_, 3000);
        char r[512];
        if (ctl_ < 0 || !ctl_cmd(ctl_, "HELLO", r, sizeof r, 3000) || strncmp(r, "OK", 2)) {
            if (ctl_ >= 0)
                close(ctl_);
            free(ring_);
            free(gap_);
            free(anchor_);
            throw std::runtime_error("qcom: no sdrd service at " + host_ + ":" + std::to_string(port_) +
                                     " (is the modem service running on the phone?)");
        }
        lo_ = fc_ = reply_value(r, "fc=", 1879.0e6);
        want_fs_ = fs_ = reply_value(r, "fs=", FS_DEF);
        data_port_     = (int)reply_value(r, "port=", 5700);
        if (ctl_cmd(ctl_, "BANDS", r, sizeof r, 3000) && !strncmp(r, "OK", 2)) {
            for (const char* p = r + 2; *p;) {
                int    b;
                double lo, hi;
                int    n = 0;
                if (sscanf(p, " %d:%lf-%lf%n", &b, &lo, &hi, &n) >= 3 && n > 0) {
                    ranges_.push_back(SoapySDR::Range(lo * 1e6, hi * 1e6 + 0.1e6, 0.0));
                    p += n;
                } else
                    break;
            }
        }
        if (ranges_.empty())
            ranges_.push_back(SoapySDR::Range(1805.0e6, 1880.0e6, 0.0));
        update_nco();
        SoapySDR::logf(SOAPY_SDR_INFO,
                       "qcom: connected to %s:%d, %d band ranges, fc %.1f MHz",
                       host_.c_str(),
                       port_,
                       (int)ranges_.size(),
                       fc_ / 1e6);
    }
    ~SoapyQcomModem() override
    {
        stop_stream();
        if (ctl_ >= 0)
            close(ctl_);
        free(ring_);
        free(gap_);
        free(anchor_);
    }

    std::string getDriverKey(void) const override
    {
        return "qcom";
    }
    std::string getHardwareKey(void) const override
    {
        return "MSM8916";
    }
    size_t getNumChannels(const int dir) const override
    {
        return dir == SOAPY_SDR_RX ? 1 : 0;
    }

    // Antenna: only the primary receiver is connected to an antenna.
    std::vector<std::string> listAntennas(const int, const size_t) const override
    {
        return { "PRX" };
    }
    void setAntenna(const int, const size_t, const std::string& name) override
    {
        if (!name.empty() && name != "PRX")
            SoapySDR::logf(SOAPY_SDR_WARNING, "qcom: antenna %s is not available, using PRX", name.c_str());
    }
    std::string getAntenna(const int, const size_t) const override
    {
        return "PRX";
    }

    // Gain: six steps of the low-noise amplifier; the nearest step to the requested value is used.
    bool hasGainMode(const int, const size_t) const override
    {
        return false;
    }
    std::vector<std::string> listGains(const int, const size_t) const override
    {
        return { "LNA" };
    }
    void setGain(const int, const size_t, const double v) override
    {
        set_gain(v);
    }
    void setGain(const int, const size_t, const std::string&, const double v) override
    {
        set_gain(v);
    }
    double getGain(const int, const size_t) const override
    {
        return GAIN_DB[gain_];
    }
    double getGain(const int, const size_t, const std::string&) const override
    {
        return GAIN_DB[gain_];
    }
    SoapySDR::Range getGainRange(const int, const size_t) const override
    {
        return SoapySDR::Range(0.0, GAIN_DB[0], 0.0);
    }
    SoapySDR::Range getGainRange(const int, const size_t, const std::string&) const override
    {
        return SoapySDR::Range(0.0, GAIN_DB[0], 0.0);
    }

    // Frequency: any frequency in a supported band (100 kHz hardware raster plus digital shift).
    std::vector<std::string> listFrequencies(const int, const size_t) const override
    {
        return { "RF" };
    }
    void setFrequency(const int, const size_t, const double f, const SoapySDR::Kwargs&) override
    {
        set_freq(f);
    }
    void setFrequency(const int, const size_t, const std::string&, const double f, const SoapySDR::Kwargs&) override
    {
        set_freq(f);
    }
    double getFrequency(const int, const size_t) const override
    {
        return fc_;
    }
    double getFrequency(const int, const size_t, const std::string&) const override
    {
        return fc_;
    }
    SoapySDR::RangeList getFrequencyRange(const int, const size_t) const override
    {
        return ranges_;
    }
    SoapySDR::RangeList getFrequencyRange(const int, const size_t, const std::string&) const override
    {
        return ranges_;
    }

    // Fixed sample rate; analog filter bandwidth selectable.
    void setSampleRate(const int, const size_t, const double r) override
    {
        set_rate(r);
    }
    double getSampleRate(const int, const size_t) const override
    {
        return fs_;
    }
    // suggestions: any value in the range works
    std::vector<double> listSampleRates(const int, const size_t) const override
    {
        return { 7.2e6, 7.68e6, 8.0e6, 9.0e6, 10.0e6, 11.0e6, 11.25e6, 14.4e6, 14.8e6 };
    }
    SoapySDR::RangeList getSampleRateRange(const int, const size_t) const override
    {
        return { SoapySDR::Range(FS_MIN, FS_GAP_LO, 0.0), SoapySDR::Range(FS_GAP_HI, FS_MAX, 0.0) };
    }
    void setBandwidth(const int, const size_t, const double bw) override
    {
        set_bandwidth(bw);
    }
    double getBandwidth(const int, const size_t) const override
    {
        return BW_HZ[bw_];
    }
    std::vector<double> listBandwidths(const int, const size_t) const override
    {
        return std::vector<double>(BW_HZ, BW_HZ + NBW);
    }
    // one range per filter: applications (SDR++) list them as choices
    SoapySDR::RangeList getBandwidthRange(const int, const size_t) const override
    {
        SoapySDR::RangeList l;
        for (int i = 0; i < NBW; i++)
            l.push_back(SoapySDR::Range(BW_HZ[i], BW_HZ[i], 0.0));
        return l;
    }

    std::vector<std::string> getStreamFormats(const int, const size_t) const override
    {
        return { SOAPY_SDR_CF32, SOAPY_SDR_CS16 };
    }
    std::string getNativeStreamFormat(const int, const size_t, double& fullScale) const override
    {
        fullScale = 32768.0;
        return SOAPY_SDR_CS16;
    }
    SoapySDR::ArgInfoList getStreamArgsInfo(const int, const size_t) const override
    {
        return {};
    }

    SoapySDR::Stream* setupStream(const int                  dir,
                                  const std::string&         format,
                                  const std::vector<size_t>& channels = std::vector<size_t>(),
                                  const SoapySDR::Kwargs&             = SoapySDR::Kwargs()) override
    {
        if (dir != SOAPY_SDR_RX)
            throw std::runtime_error("qcom: receive only");
        if (!channels.empty() && (channels.size() != 1 || channels[0] != 0))
            throw std::runtime_error("qcom: only channel 0");
        if (format == SOAPY_SDR_CF32)
            fmt_ = 0;
        else if (format == SOAPY_SDR_CS16)
            fmt_ = 1;
        else
            throw std::runtime_error("qcom: unsupported format " + format + " (CF32, CS16)");
        return (SoapySDR::Stream*)this;
    }
    void closeStream(SoapySDR::Stream*) override
    {
        stop_stream();
    }
    size_t getStreamMTU(SoapySDR::Stream*) const override
    {
        return SPR * 8;
    }

    int activateStream(SoapySDR::Stream*, const int = 0, const long long = 0, const size_t = 0) override
    {
        return start_stream() ? 0 : SOAPY_SDR_STREAM_ERROR;
    }
    int deactivateStream(SoapySDR::Stream*, const int = 0, const long long = 0) override
    {
        stop_stream();
        return 0;
    }

    int readStream(SoapySDR::Stream*,
                   void* const* buffs,
                   const size_t numElems,
                   int&         flags,
                   long long&   timeNs,
                   const long   timeoutUs = 100000) override
    {
        flags   = 0;
        timeNs  = 0;
        int got = fmt_ == 0 ? read_samples<true>(buffs[0], numElems, timeoutUs)
                            : read_samples<false>(buffs[0], numElems, timeoutUs);
        if (got > 0 && have_time_) {
            flags |= SOAPY_SDR_HAS_TIME;
            timeNs = first_ts_;
        }
        return got;
    }

private:
    bool start_stream()
    {
        if (running_)
            return true;
        char r[256];
        SoapySDR::logf(SOAPY_SDR_INFO,
                       "qcom: starting the stream on the phone (a change of channel takes about 1.5 s)");
        if (!ctl_cmd(ctl_, "START", r, sizeof r, 60000) || strncmp(r, "OK", 2)) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "qcom: start failed: %s", r);
            return false;
        }
        data_port_ = (int)reply_value(r, "port=", data_port_);
        double lo  = reply_value(r, "fc=", lo_);
        fs_        = reply_value(r, "rate=", fs_);
        if (std::fabs(lo - lo_) > 1.0) {
            lo_ = lo;
        }
        update_nco();
        for (int i = 0; i < 100 && data_ < 0; i++) {
            data_ = tcp_connect(host_.c_str(), data_port_, 1000);
            if (data_ < 0)
                usleep(50000);
        }
        if (data_ < 0) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "qcom: cannot connect to the sample port %d", data_port_);
            return false;
        }
        int rcv = 4 << 20;
        setsockopt(data_, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
        pthread_mutex_lock(&m_);
        head_ = tail_ = 0;
        cur_off_      = 0;
        have_last_    = false;
        next_bad_     = true;
        anchor_init_ = anchor_init_lat_ = false;
        have_time_                      = false;
        have_prev_ts_                   = false;
        pthread_mutex_unlock(&m_);
        running_ = true;
        if (pthread_create(&thr_, 0, &SoapyQcomModem::rx_thread, this)) {
            running_ = false;
            close(data_);
            data_ = -1;
            return false;
        }
        return true;
    }
    void stop_stream()
    {
        if (running_) {
            running_ = false;
            if (data_ >= 0)
                shutdown(data_, SHUT_RDWR);
            pthread_cond_broadcast(&cv_);
            pthread_join(thr_, 0);
        }
        if (data_ >= 0) {
            close(data_);
            data_ = -1;
        }
        if (ctl_ >= 0 && was_started_) {
            char r[128];
            ctl_cmd(ctl_, "STOP", r, sizeof r, 10000);
        }
        was_started_ = false;
    }
    // runs f() with the stream stopped when `need` and it is running
    template <class F>
    void with_restart(bool need, F f)
    {
        bool was = running_ && need;
        if (was)
            stop_stream();
        f();
        if (was)
            start_stream();
    }
    void set_freq(double f)
    {
        char cmd[64], r[256];
        snprintf(cmd, sizeof cmd, "TUNE %.0f", f);
        if (!ctl_cmd(ctl_, cmd, r, sizeof r, 3000) || strncmp(r, "OK", 2)) {
            if (now_s() - last_warn_ > 5.0) {
                last_warn_ = now_s();
                SoapySDR::logf(SOAPY_SDR_WARNING, "qcom: cannot tune to %.3f MHz: %s", f / 1e6, r);
            }
            return;
        }
        double lo = reply_value(r, "lo=", lo_);
        with_restart(std::fabs(lo - lo_) > 1.0, [&] { lo_ = lo; });
        fc_ = f;
        update_nco();
    }
    void set_rate(double r)
    {
        if (std::fabs(r - fs_) < 1.0 && std::fabs(r - want_fs_) < 1.0)
            return;
        char cmd[48], reply[256];
        snprintf(cmd, sizeof cmd, "RATE %.0f", r);
        if (!ctl_cmd(ctl_, cmd, reply, sizeof reply, 5000) || strncmp(reply, "OK", 2)) {
            SoapySDR::logf(SOAPY_SDR_WARNING, "qcom: sample rate %.3f MS/s refused: %s", r / 1e6, reply);
            return;
        }
        double act = reply_value(reply, "rate=", r);
        if (std::fabs(act - r) > 1.0)
            SoapySDR::logf(
                SOAPY_SDR_INFO, "qcom: sample rate %.6f MS/s requested, using %.6f MS/s", r / 1e6, act / 1e6);
        with_restart(true, [&] {
            want_fs_ = act;
            fs_      = act;
        });
        update_nco();
    }
    void set_gain(double v)
    {
        int s = 0;
        for (int i = 1; i < NGAIN; i++)
            if (std::fabs(GAIN_DB[i] - v) < std::fabs(GAIN_DB[s] - v))
                s = i;
        char cmd[32], r[256];
        snprintf(cmd, sizeof cmd, "GAIN %d", s);
        if (ctl_cmd(ctl_, cmd, r, sizeof r, 5000) && !strncmp(r, "OK", 2))
            gain_ = s;
        else
            SoapySDR::logf(SOAPY_SDR_WARNING, "qcom: gain control is not available on this channel (%s)", r);
    }
    void set_bandwidth(double bw)
    {
        int s = 0;
        for (int i = 1; i < NBW; i++)
            if (std::fabs(BW_HZ[i] - bw) < std::fabs(BW_HZ[s] - bw))
                s = i;
        char cmd[32], r[256];
        snprintf(cmd, sizeof cmd, "BW %g", BW_HZ[s] / 1e6);
        bool changes_group = (s >= WIDE_FROM) != (bw_ >= WIDE_FROM);
        bool ok            = false;
        with_restart(changes_group, [&] { ok = ctl_cmd(ctl_, cmd, r, sizeof r, 5000) && !strncmp(r, "OK", 2); });
        if (ok)
            bw_ = s;
        else
            SoapySDR::logf(SOAPY_SDR_WARNING, "qcom: bandwidth control is not available on this channel (%s)", r);
    }
    // Digital shift of the remainder between the requested frequency and the hardware channel (phasor recurrence).
    void update_nco()
    {
        double hz = fc_ - lo_ - cfo_, a = -2.0 * M_PI * hz / fs_;
        pthread_mutex_lock(&m_);
        new_step_re_ = (float)std::cos(a);
        new_step_im_ = (float)std::sin(a);
        nco_on_      = std::fabs(hz) > 0.5;
        nco_dirty_   = true;
        pthread_mutex_unlock(&m_);
    }

    static void* rx_thread(void* arg)
    {
        static_cast<SoapyQcomModem*>(arg)->rx_loop();
        return 0;
    }
    bool recv_all(uint8_t* dst, size_t n)
    {
        size_t got = 0;
        while (got < n && running_) {
            ssize_t k = recv(data_, dst + got, n - got, 0);
            if (k <= 0)
                return false;
            got += (size_t)k;
        }
        return got == n;
    }
    // Time of the first sample of a record on the receiver clock (ns): the record header holds the sample counter and
    // clock tick of the snapshot that released the block, and the ring position of the block.
    long long record_time(const uint8_t* rec)
    {
        uint32_t samp, tlat, rblk;
        memcpy(&samp, rec + 24, 4);
        memcpy(&tlat, rec + 28, 4);
        memcpy(&rblk, rec + 12, 4);
        if (anchor_init_)
            samp64_ += (int32_t)(samp - last_samp_u32_);
        else {
            samp64_      = samp;
            anchor_init_ = true;
        }
        last_samp_u32_ = samp;
        if (anchor_init_lat_)
            tlat64_ += (int32_t)(tlat - last_tlat_u32_); // the 32-bit clock wraps every ~223 s
        else {
            tlat64_          = tlat;
            anchor_init_lat_ = true;
        }
        last_tlat_u32_ = tlat;
        long long rem = samp64_ % (long long)SPR, head_blk = (samp64_ / (long long)SPR) % 20,
                  k = (head_blk - (long long)rblk + 20) % 20;
        // samples from the block start to the snapshot
        double dist = (double)(k * (long long)SPR + rem);
        return (long long)std::llround((double)tlat64_ * 1e9 / CLOCK_HZ - dist * 1e9 / fs_);
    }
    void rx_loop()
    {
        was_started_ = true;
        uint8_t rec[REC_BYTES];
        while (running_) {
            if (!recv_all(rec, REC_BYTES))
                break;
            uint32_t seq, late, flg;
            memcpy(&seq, rec, 4);
            memcpy(&late, rec + 16, 4);
            memcpy(&flg, rec + 20, 4);
            bool gap               = have_last_ && seq != last_seq_ + 1;
            have_last_             = true;
            last_seq_              = seq;
            bool bad               = gap || late > (uint32_t)(18.0 * SPR / fs_ * CLOCK_HZ) || (flg & 2u) || next_bad_;
            next_bad_              = false;
            long long ts           = record_time(rec);
            // About every 128 ms the copy program delivers a run of records that repeat earlier data; their times step
            // back. They are dropped; the records after them continue the timeline without a gap. A forward step in
            // time means that data is really missing.
            const long long rec_ns = (long long)(1e9 * SPR / fs_);
            if (have_prev_ts_ && ts < prev_ts_ + rec_ns / 2) {
                // far longer than a repeated run: the time reference itself jumped, so resynchronise
                if (++stale_run_ < 400) {
                    stale_++;
                    continue;
                }
                SoapySDR::logf(
                    SOAPY_SDR_WARNING,
                    "qcom: record times stuck behind the stream (ts %lld, last %lld, step %lld ns); resynchronising",
                    ts,
                    prev_ts_,
                    rec_ns);
                have_prev_ts_ = false;
                bad           = true;
            }
            stale_run_ = 0;
            if (have_prev_ts_ && ts > prev_ts_ + rec_ns * 3 / 2)
                bad = true;
            prev_ts_      = ts;
            have_prev_ts_ = true;
            pthread_mutex_lock(&m_);
            // consumer too slow: drop the oldest record
            if (head_ - tail_ >= RING_RECS - 1) {
                tail_++;
                cur_off_ = 0;
                bad      = true;
            }
            size_t slot = head_ % RING_RECS;
            memcpy(&ring_[slot * REC_BYTES], rec, REC_BYTES);
            gap_[slot]    = bad ? 1 : 0;
            anchor_[slot] = ts;
            head_++;
            pthread_cond_signal(&cv_);
            pthread_mutex_unlock(&m_);
        }
        running_ = false;
        pthread_cond_broadcast(&cv_);
    }

    // Copies/decodes up to n samples. Returns the count, SOAPY_SDR_TIMEOUT, SOAPY_SDR_OVERFLOW (a gap precedes the next
    // data) or SOAPY_SDR_STREAM_ERROR.
    template <bool CF32>
    int read_samples(void* out, size_t n, long timeout_us)
    {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        long long ns = dl.tv_nsec + (long long)timeout_us * 1000;
        dl.tv_sec += ns / 1000000000LL;
        dl.tv_nsec  = ns % 1000000000LL;
        size_t done = 0;
        pthread_mutex_lock(&m_);
        if (nco_dirty_) {
            step_re_   = new_step_re_;
            step_im_   = new_step_im_;
            rot_re_    = 1.0f;
            rot_im_    = 0.0f;
            rot_n_     = 0;
            nco_dirty_ = false;
        }
        const bool nco = nco_on_;
        while (done < n) {
            if (head_ == tail_) {
                if (!running_) {
                    pthread_mutex_unlock(&m_);
                    return done ? (int)done : SOAPY_SDR_STREAM_ERROR;
                }
                if (pthread_cond_timedwait(&cv_, &m_, &dl) == ETIMEDOUT && head_ == tail_) {
                    pthread_mutex_unlock(&m_);
                    return done ? (int)done : SOAPY_SDR_TIMEOUT;
                }
                continue;
            }
            size_t slot = tail_ % RING_RECS;
            if (cur_off_ == 0 && gap_[slot]) {
                gap_[slot] = 0;
                if (done) {
                    gap_[slot] = 1;
                    break;
                }
                pthread_mutex_unlock(&m_);
                return SOAPY_SDR_OVERFLOW;
            }
            if (done == 0) {
                first_ts_  = anchor_[slot] + (long long)std::llround((double)cur_off_ * 1e9 / fs_);
                have_time_ = true;
            }
            const uint8_t* pay   = &ring_[slot * REC_BYTES + HDR_BYTES];
            size_t         take  = std::min(n - done, SPR - cur_off_);
            uint64_t       saved = tail_;
            pthread_mutex_unlock(&m_);
            for (size_t k = 0; k < take; k++) {
                int32_t i, q;
                pcfl_get(pay, cur_off_ + k, i, q);
                float fi = (float)i, fq = (float)q;
                if (nco) {
                    float ri = fi * rot_re_ - fq * rot_im_, rq = fi * rot_im_ + fq * rot_re_;
                    fi       = ri;
                    fq       = rq;
                    float nr = rot_re_ * step_re_ - rot_im_ * step_im_, ni = rot_re_ * step_im_ + rot_im_ * step_re_;
                    rot_re_ = nr;
                    rot_im_ = ni;
                    if ((++rot_n_ & 1023u) == 0) {
                        float m = 1.0f / std::sqrt(rot_re_ * rot_re_ + rot_im_ * rot_im_);
                        rot_re_ *= m;
                        rot_im_ *= m;
                    }
                }
                if (CF32) {
                    float* f              = (float*)out;
                    f[2 * (done + k)]     = fi * (1.0f / 32768.0f);
                    f[2 * (done + k) + 1] = fq * (1.0f / 32768.0f);
                } else {
                    int16_t* s            = (int16_t*)out;
                    s[2 * (done + k)]     = (int16_t)std::max(-32768.f, std::min(32767.f, std::nearbyint(fi)));
                    s[2 * (done + k) + 1] = (int16_t)std::max(-32768.f, std::min(32767.f, std::nearbyint(fq)));
                }
            }
            pthread_mutex_lock(&m_);
            done += take;
            if (tail_ != saved)
                continue; // the producer dropped this record meanwhile
            cur_off_ += take;
            if (cur_off_ >= SPR) {
                cur_off_ = 0;
                tail_++;
            }
        }
        pthread_mutex_unlock(&m_);
        return (int)done;
    }

    std::string host_;
    int         port_ = DEFAULT_PORT, data_port_ = 5700, ctl_ = -1, data_ = -1, fmt_ = 0, gain_ = 0, bw_ = NBW - 1;
    // sample rate reached (from the phone) / last requested
    double fs_ = FS_DEF, want_fs_ = FS_DEF;
    // requested center, hardware channel, oscillator error
    double                       fc_ = 1879.0e6, lo_ = 1879.0e6, cfo_ = 0.0;
    std::vector<SoapySDR::Range> ranges_;
    double                       last_warn_ = -100.0;
    volatile bool                running_ = false, was_started_ = false;
    pthread_t                    thr_;
    pthread_mutex_t              m_;
    pthread_cond_t               cv_;
    uint8_t *                    ring_ = 0, *gap_ = 0;
    long long*                   anchor_ = 0;
    uint64_t                     head_ = 0, tail_ = 0;
    size_t                       cur_off_   = 0;
    bool                         have_last_ = false, next_bad_ = false;
    uint32_t                     last_seq_ = 0;
    // record time tracking
    long long samp64_ = 0, tlat64_ = 0, first_ts_ = 0, prev_ts_ = 0;
    uint64_t  stale_ = 0, stale_run_ = 0;
    bool      have_prev_ts_  = false;
    uint32_t  last_samp_u32_ = 0, last_tlat_u32_ = 0;
    bool      anchor_init_ = false, anchor_init_lat_ = false, have_time_ = false;
    // digital frequency shift
    float    rot_re_ = 1.0f, rot_im_ = 0.0f, step_re_ = 1.0f, step_im_ = 0.0f, new_step_re_ = 1.0f, new_step_im_ = 0.0f;
    uint32_t rot_n_  = 0;
    bool     nco_on_ = false, nco_dirty_ = false;
};

static SoapySDR::KwargsList findQcom(const SoapySDR::Kwargs& args)
{
    SoapySDR::KwargsList out;
    SoapySDR::Kwargs     k;
    k["driver"]      = "qcom";
    const char* env  = getenv("QCOM_PHONE_HOST");
    std::string host = args.count("host") ? args.at("host") : (env && *env ? env : DEFAULT_HOST);
    int         port = args.count("port") ? atoi(args.at("port").c_str()) : DEFAULT_PORT;
    int         fd   = tcp_connect(host.c_str(), port, 300);
    if (fd < 0)
        return out;
    char r[256];
    bool ok = ctl_cmd(fd, "HELLO", r, sizeof r, 500) && !strncmp(r, "OK", 2);
    close(fd);
    if (!ok)
        return out;
    k["host"]  = host;
    k["port"]  = std::to_string(port);
    k["label"] = "Qualcomm MSM8916 modem @" + host;
    out.push_back(k);
    return out;
}
static SoapySDR::Device* makeQcom(const SoapySDR::Kwargs& args)
{
    return new SoapyQcomModem(args);
}
static SoapySDR::Registry registerQcom("qcom", &findQcom, &makeQcom, SOAPY_SDR_ABI_VERSION);
