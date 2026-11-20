// SPDX-FileCopyrightText: 2026 Piotr Krysik
// SPDX-License-Identifier: GPL-3.0-only
// sdrd.cpp -- phone-side daemon of the SDR sample stream. Runs as root on the phone (systemd), controlled over TCP.
//
// It talks to the sandbox task of the custom modem firmware through /dev/mem, brings the modem into the LTE factory
// test mode through the DIAG channel, configures and starts the DMA copy program on the DSP, and runs `sample_server`
// (which serves the sample records over a second TCP connection). Samples never pass through this process.
//
// Control protocol (TCP, default port 5701): one text command per line, one reply line starting with "OK" or "ERR".
//   HELLO        -> OK sdrd 4 fs=<Hz> fc=<Hz> port=<sample port> rec=3232
//   BANDS        -> OK <band>:<low MHz>-<high MHz> ...
//                   ranges that can be tuned (band 0 = free tuning outside the LTE bands)
//   TUNE <Hz>    -> OK lo=<Hz> earfcn=<n> band=<b>
//                   selects the centre frequency; applied by START. Inside an LTE band: the nearest 100 kHz channel;
//                   elsewhere (band 0): a 1 MHz step reached with the receiver's own tuning script
//   GAIN <0-5>   -> OK gain=<state>
//                   receiver gain state, 0 = highest gain, 5 = lowest (about 18 dB range)
//   BW <MHz>     -> OK bw=<name>
//                   analog filter bandwidth: 1.4 3 5 10 15 20 40
//   RATE <Hz>    -> OK rate=<Hz> mode=<A|B> clamped=<0|1>
//                   sample rate 7.2 .. 11.25 and 14.4 .. 14.8 MS/s (clamped to the nearest reachable value; applied
//                   by START)
//   START        -> brings up the test mode on the tuned channel, starts the copy program and the sample server;
//                   the client then connects to <sample port>
//   STOP         -> stops the copy program and the sample server
//   STATUS       -> OK state=<idle|running> rate=<Hz> band=<b> earfcn=<n> fc=<Hz> gain=<s> bw=<name> port=<rf port>
//                   records=<n> late=<n> slips=<n> clients=<n>
//
// C-like C++: no STL, exceptions or RTTI.  Build: see build.sh.
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t u16;
typedef uint8_t  u8;
static double    std_round(double v)
{
    return v < 0 ? -(double)(long long)(-v + 0.5) : (double)(long long)(v + 0.5);
}

// ---------------- configuration
static const char*  g_blobs_path  = "/usr/lib/sdr/dsp_blobs.bin";
static const char*  g_server_path = "/usr/lib/sdr/sample_server";
static int          g_ctl_port = 5701, g_data_port = 5700;
static const u32    DEFAULT_EARFCN = 1940; // band 3, 1879.0 MHz
static const char*  g_luts_path    = "/usr/lib/sdr/wtr_luts.bin";
static const double FS_DEFAULT     = 7.68e6;
static const u32    REC_BYTES = 3232, RING_RECS = 900, LOG_BYTES = 4096, BLOCK_BYTES = 3145728;

static void logf_(const char* f, ...)
{
    va_list ap;
    va_start(ap, f);
    fputs("sdrd: ", stderr);
    vfprintf(stderr, f, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}
static void msleep(int ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, 0);
}
static double now_s()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

// ---------------- sandbox access (/dev/mem)
static int       g_mem = -1;
static u64       g_base_pa, g_size;
static u32       g_ctrl_va, g_scratch_va, g_counter_va;
static u64       g_ctrl_pa;
static const u32 BASE_VA = 0xC0000000u;

// IMEM table published by the rf_sandbox task: {'SBFR', version, &control block, &scratch area, &task counter, ...}
static const u64 SB_IMEM_TABLE_PA = 0x08600F00u;
static const u32 SB_IMEM_MAGIC = 0x52464253u, SB_IMEM_VERSION = 1; // "SBFR"
static const u32 SB_CTRL_MAGIC = 0x58424452u;                      // "RDBX"
// words of the rf_sandbox control block (rf_sandbox_task.h)
enum { SB_MAGIC = 0, SB_SEQ = 1, SB_ACK = 2, SB_CMD = 3, SB_STATUS = 6, SB_RESULT = 7, SB_PERIOD_MS = 9, SB_DATA = 10 };
static const u32 SB_CMD_CALL_FN    = 29;
static const u32 REG_WRITE_REFUSED = 0xDEAD0001u; // reply of the reg_write program for a register it does not allow

static bool mem_read(u64 pa, void* d, size_t n)
{
    return pread(g_mem, d, n, (off_t)pa) == (ssize_t)n;
}
static bool mem_write(u64 pa, const void* d, size_t n)
{
    return pwrite(g_mem, d, n, (off_t)pa) == (ssize_t)n;
}
static u32 rd32pa(u64 pa)
{
    u32 v = 0;
    mem_read(pa, &v, 4);
    return v;
}
static void wr32pa(u64 pa, u32 v)
{
    mem_write(pa, &v, 4);
}
static u64 phys(u32 a)
{
    return (a >= BASE_VA && a < BASE_VA + g_size) ? (u64)a - BASE_VA + g_base_pa : (u64)a;
}

static bool find_modem_region()
{
    glob_t g;
    if (glob("/sys/firmware/devicetree/base/reserved-memory/mpss@*", 0, 0, &g))
        return false;
    char path[512];
    snprintf(path, sizeof path, "%s/reg", g.gl_pathv[0]);
    globfree(&g);
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    unsigned char b[16];
    size_t        n = fread(b, 1, 16, f);
    fclose(f);
    if (n != 16)
        return false;
    u32 w[4];
    for (int i = 0; i < 4; i++)
        w[i] = (u32)b[4 * i] << 24 | b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3];
    g_base_pa = ((u64)w[0] << 32) | w[1];
    g_size    = ((u64)w[2] << 32) | w[3];
    return true;
}
static bool sandbox_init()
{
    if (g_mem < 0)
        g_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (g_mem < 0) {
        logf_("cannot open /dev/mem: %s", strerror(errno));
        return false;
    }
    if (!g_base_pa && !find_modem_region()) {
        logf_("no modem memory region in the device tree");
        return false;
    }
    u32 t[6];
    if (!mem_read(SB_IMEM_TABLE_PA, t, sizeof t) || t[0] != SB_IMEM_MAGIC || t[1] != SB_IMEM_VERSION)
        return false;
    g_ctrl_va    = t[2];
    g_scratch_va = t[3];
    g_counter_va = t[4];
    g_ctrl_pa    = phys(g_ctrl_va);
    return rd32pa(g_ctrl_pa + 4 * SB_MAGIC) == SB_CTRL_MAGIC;
}
struct Ctrl {
    u32 w[26];
};
static bool ctrl_get(Ctrl* c)
{
    return mem_read(g_ctrl_pa, c, sizeof *c);
}
static bool wait_done(double sec)
{
    double end = now_s() + sec;
    for (;;) {
        Ctrl c;
        if (!ctrl_get(&c))
            return false;
        if (c.w[SB_SEQ] == c.w[SB_ACK])
            return true;
        if (now_s() > end)
            return false;
        msleep(1);
    }
}
static bool sandbox_cmd(u32 cmd, u32 a0, u32 a1, u32 d0, u32 d1, bool async_)
{
    Ctrl c;
    if (!ctrl_get(&c) || c.w[SB_SEQ] != c.w[SB_ACK]) {
        logf_("sandbox busy");
        return false;
    }
    u32 h[3] = { cmd, a0, a1 }; // cmd, arg0, arg1
    mem_write(g_ctrl_pa + 4 * SB_CMD, h, sizeof h);
    wr32pa(g_ctrl_pa + 4 * SB_STATUS, 0);
    u32 d[2] = { d0, d1 };
    mem_write(g_ctrl_pa + 4 * SB_DATA, d, sizeof d);
    wr32pa(g_ctrl_pa + 4 * SB_SEQ, c.w[SB_SEQ] + 1); // a new seq starts the command
    return async_ ? true : wait_done(5.0);
}

// ---------------- DSP programs
struct Blob {
    char           name[17];
    u32            entry;
    u32            size;
    unsigned char* code;
};
static Blob g_blobs[8];
static int  g_nblobs;
static bool load_blobs()
{
    FILE* f = fopen(g_blobs_path, "rb");
    if (!f) {
        logf_("cannot open %s", g_blobs_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* b = (unsigned char*)malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) {
        fclose(f);
        return false;
    }
    fclose(f);
    u32 cnt;
    memcpy(&cnt, b + 4, 4);
    if (memcmp(b, "SDRB", 4) || cnt > 8) {
        logf_("bad blob file");
        return false;
    }
    for (u32 i = 0; i < cnt; i++) {
        const unsigned char* e = b + 8 + 28 * i;
        Blob&                k = g_blobs[i];
        memcpy(k.name, e, 16);
        k.name[16] = 0;
        u32 off;
        memcpy(&k.entry, e + 16, 4);
        memcpy(&k.size, e + 20, 4);
        memcpy(&off, e + 24, 4);
        if (off + k.size > (u32)n || k.size > 0x800)
            return false;
        k.code = (unsigned char*)malloc(k.size);
        memcpy(k.code, b + off, k.size);
    }
    g_nblobs = (int)cnt;
    free(b);
    return true;
}
// Runs a DSP program; returns false on a protocol error. *result receives the program's return value.
static bool dsp_call(const char* name, u32 a, u32 b, u32 c, u32* result, bool async_ = false)
{
    const Blob* k = 0;
    for (int i = 0; i < g_nblobs; i++)
        if (!strcmp(g_blobs[i].name, name))
            k = &g_blobs[i];
    if (!k) {
        logf_("no DSP program %s", name);
        return false;
    }
    Ctrl s;
    if (!ctrl_get(&s) || s.w[SB_SEQ] != s.w[SB_ACK]) {
        logf_("sandbox busy: refusing to overwrite active code");
        return false;
    }
    if (!mem_write(phys(g_scratch_va), k->code, k->size))
        return false;
    if (!sandbox_cmd(SB_CMD_CALL_FN, k->entry, a, b, c, async_)) {
        logf_("%s: no acknowledgement", name);
        return false;
    }
    if (!async_) {
        Ctrl r;
        ctrl_get(&r);
        if (result)
            *result = r.w[SB_RESULT];
    }
    return true;
}
static u32 reg_read(u32 a)
{
    u32 r = 0;
    dsp_call("reg_read", a, 0, 0, &r);
    return r;
}
static bool reg_write(u32 a, u32 v)
{
    u32 r = 0;
    if (!dsp_call("reg_write", a, v, 0, &r))
        return false;
    if (r == REG_WRITE_REFUSED) {
        logf_("write to %08x refused", a);
        return false;
    }
    return true;
}

// ---------------- DIAG channel (modem test mode)
static u32 crc16(const unsigned char* p, size_t n)
{
    u32 c = 0xffff;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
    }
    return c ^ 0xffff;
}
static size_t hdlc_frame(const unsigned char* p, size_t n, unsigned char* out)
{
    unsigned char tmp[600];
    memcpy(tmp, p, n);
    u32 c      = crc16(p, n);
    tmp[n]     = c & 0xff;
    tmp[n + 1] = c >> 8;
    n += 2;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (tmp[i] == 0x7d || tmp[i] == 0x7e) {
            out[o++] = 0x7d;
            out[o++] = tmp[i] ^ 0x20;
        } else
            out[o++] = tmp[i];
    }
    out[o++] = 0x7e;
    return o;
}
static int hdlc_unframe(const unsigned char* p, size_t n, unsigned char* out)
{ // returns payload length or -1
    unsigned char t[1200];
    size_t        o   = 0;
    bool          esc = false;
    for (size_t i = 0; i < n && o < sizeof t; i++) {
        if (esc) {
            t[o++] = p[i] ^ 0x20;
            esc    = false;
        } else if (p[i] == 0x7d)
            esc = true;
        else
            t[o++] = p[i];
    }
    if (o < 3)
        return -1;
    u32 c = crc16(t, o - 2);
    if ((c & 0xff) != t[o - 2] || (c >> 8) != t[o - 1])
        return -1;
    memcpy(out, t, o - 2);
    return (int)(o - 2);
}
static int g_diag = -1;
// Sends one request and returns the last response frame whose prefix matches (length), or -1.
static int diag_request(const unsigned char* req, size_t n, double secs, unsigned char* resp, int settle_ms = 150)
{
    if (g_diag < 0)
        g_diag = open("/dev/wwan0qcdm0", O_RDWR | O_NONBLOCK);
    if (g_diag < 0) {
        logf_("cannot open /dev/wwan0qcdm0: %s", strerror(errno));
        return -1;
    }
    unsigned char junk[4096];
    while (read(g_diag, junk, sizeof junk) > 0) {
    }
    unsigned char fr[700];
    size_t        fn = hdlc_frame(req, n, fr);
    // the port is recreated when the modem restarts: reopen it next time
    if (write(g_diag, fr, fn) != (ssize_t)fn) {
        close(g_diag);
        g_diag = -1;
        return -1;
    }
    size_t        plen = req[0] == 0x4b ? 6 : 3;
    unsigned char buf[8192];
    size_t        bl   = 0;
    int           best = -1;
    double        end  = now_s() + secs;
    while (now_s() < end) {
        struct pollfd pf = { g_diag, POLLIN, 0 };
        if (poll(&pf, 1, 200) <= 0)
            continue;
        ssize_t r = read(g_diag, buf + bl, sizeof buf - bl);
        if (r <= 0)
            continue;
        bl += r;
        for (;;) {
            unsigned char* e = (unsigned char*)memchr(buf, 0x7e, bl);
            if (!e)
                break;
            size_t        part = e - buf;
            unsigned char out[1200];
            int           l = part ? hdlc_unframe(buf, part, out) : -1;
            if (l >= (int)plen && !memcmp(out, req, plen)) {
                memcpy(resp, out, l);
                best = l;
            }
            memmove(buf, e + 1, bl - part - 1);
            bl -= part + 1;
        }
        if (best >= 0) {
            if (settle_ms)
                msleep(settle_ms);
            return best;
        }
    }
    return best;
}
// DIAG request header of the factory test mode: subsystem dispatch (0x4b), FTM subsystem (0x0b), FTM mode (u16)
static const unsigned char DIAG_SUBSYS_CMD = 0x4b, DIAG_SUBSYS_FTM = 0x0b;
static const unsigned char FTM_MODE_LTE_NS = 0x1e; // FTM_LTE_NS_C (30): LTE non-signalling test mode
static const unsigned char FTM_MODE_COMMON = 0x14; // FTM_COMMON_C (20)
static const u32           FTM_DO_RFFE_CMD = 623;  // RFFE register access of the RF chips
// LTE non-signalling commands FTM_LTE_NS_START, FTM_LTE_NS_ACQ (receive on a channel) and FTM_LTE_NS_STOP
static const u32 NS_START = 500, NS_ACQUIRE = 501, NS_STOP = 504;
// status codes of the LTE non-signalling commands
static const int NS_OK = 0, NS_NO_CELL = 1, NS_NOT_ALLOWED = 2, NS_ALREADY_DONE = 13;
static bool
ftm_ns(u32 cmd, const unsigned char* body, size_t bn, double secs, int ok1, int ok2, unsigned char* resp, int* rl)
{
    unsigned char req[64] = {
        DIAG_SUBSYS_CMD, DIAG_SUBSYS_FTM, FTM_MODE_LTE_NS, 0x00, (unsigned char)(cmd & 0xff), (unsigned char)(cmd >> 8)
    };
    memcpy(req + 6, body, bn);
    int l = diag_request(req, 6 + bn, secs, resp);
    if (l < 10) {
        logf_("ns%u: no response", cmd);
        return false;
    }
    u32 st;
    memcpy(&st, resp + 6, 4);
    logf_("ns%u status=%u", cmd, st);
    if (rl)
        *rl = l;
    return (int)st == ok1 || (int)st == ok2;
}
// ---------------- bands and tuning
// downlink start, first and last EARFCN (3GPP channel numbers, 100 kHz raster)
struct Band {
    int    num;
    double dl_low_mhz;
    int    e_lo, e_hi;
};
static const Band  BANDS[] = { { 1, 2110.0, 0, 599 },     { 2, 1930.0, 600, 1199 },  { 3, 1805.0, 1200, 1949 },
                               { 4, 2110.0, 1950, 2399 }, { 5, 869.0, 2400, 2649 },  { 7, 2620.0, 2750, 3449 },
                               { 8, 925.0, 3450, 3799 },  { 17, 734.0, 5730, 5849 }, { 20, 791.0, 6150, 6449 },
                               { 26, 859.0, 8690, 9039 }, { 27, 852.0, 9040, 9209 }, { 28, 758.0, 9210, 9659 } };
static const int   NBANDS  = (int)(sizeof BANDS / sizeof BANDS[0]);
static const Band* band_of_earfcn(u32 e)
{
    for (int i = 0; i < NBANDS; i++)
        if ((int)e >= BANDS[i].e_lo && (int)e <= BANDS[i].e_hi)
            return &BANDS[i];
    return 0;
}
static double earfcn_to_hz(const Band* b, u32 e)
{
    return b->dl_low_mhz * 1e6 + ((int)e - b->e_lo) * 1e5;
}
// An EARFCN with a byte of 0x7d or 0x7e is damaged by the framing of the DIAG channel (the test mode answers "status
// 2"): use a neighbour.
static bool hdlc_safe(u32 e)
{
    for (int i = 0; i < 4; i++) {
        unsigned b = (e >> (8 * i)) & 0xff;
        if (b == 0x7d || b == 0x7e)
            return false;
    }
    return true;
}
// Nearest channel to a frequency; false when the frequency is more than REACH from every supported band. A centre a
// little outside a band is accepted: the receiver is tuned to the band edge and the driver shifts the rest digitally
// (half of the 7.68 MHz span is still in band).
static const double REACH = 3.0e6;
static bool         pick_channel(double hz, u32* earfcn)
{
    const Band* best = 0;
    double      bd   = 1e30;
    for (int i = 0; i < NBANDS; i++) {
        const Band* b  = &BANDS[i];
        double      lo = b->dl_low_mhz * 1e6, hi = earfcn_to_hz(b, (u32)b->e_hi);
        double      d = hz < lo ? lo - hz : (hz > hi ? hz - hi : 0.0);
        if (d < bd) {
            bd   = d;
            best = b;
        }
    }
    if (!best || bd > REACH)
        return false;
    double lo = best->dl_low_mhz * 1e6;
    int    e  = best->e_lo + (int)((hz - lo) / 1e5 + 0.5);
    if (e < best->e_lo)
        e = best->e_lo;
    if (e > best->e_hi)
        e = best->e_hi;
    if (!hdlc_safe((u32)e))
        e = (e + 1 <= best->e_hi) ? e + 1 : e - 1;
    *earfcn = (u32)e;
    return true;
}

// ---------------- free tuning outside the LTE bands
// The test mode tunes only to LTE channels. The receiver chip's tuning is a short script of register writes (event
// sequence) that the firmware sends to it; replaying that script with a different PLL value tunes the chip anywhere its
// oscillator can lock. Two scripts are used: the band 3 one (oscillator = receive frequency, 1380 - 2230 MHz) and the
// band 20 one (oscillator = 3 x receive frequency, 760 - 990 MHz). A range may also replace some register values of the
// script (a "block", taken from the firmware's table of another band) before it is sent: the first 3 values select the
// oscillator's operating range and the input stage, so a script of one band can reach frequencies of another.
struct Patch {
    u16 reg;
    u8  n;
    u8  data[5];
};
static const Patch BLOCK_B1[] = { // band 1 group (ports pmb2/pmb3/phb1/phb2), from the firmware's per-band tables
    { 0x10d, 2, { 0x8a, 0x5e } },
    { 0x112, 2, { 0x08, 0x21 } },
    { 0x13d, 1, { 0x81 } },
    { 0x146, 1, { 0x07 } },
    { 0x448, 1, { 0x62 } },
    { 0x451, 2, { 0x02, 0x01 } },
    { 0x456, 5, { 0x42, 0x40, 0x40, 0x40, 0x13 } },
    { 0x583, 1, { 0x02 } },
    { 0x5a9, 2, { 0x48, 0x42 } },
    { 0x5ad, 4, { 0x43, 0x46, 0x4c, 0x2b } },
    { 0x5b3, 2, { 0x40, 0x40 } }
};
struct FreeRange {
    const char*  fam;
    double       lo_mhz, hi_mhz;
    int          vco_mult;
    int          band;
    const char*  port;
    const Patch* block;
    int          nblock;
};
static const FreeRange FREE[] = {
    { "b20", 735.0, 995.0, 3, 20, "plb1", 0, 0 },
    { "b3", 1195.0, 2230.0, 1, 3, "pmb3", 0, 0 },
    { "b3", 2230.0, 2700.0, 1, 1, "pmb2", BLOCK_B1, (int)(sizeof BLOCK_B1 / sizeof BLOCK_B1[0]) }
};
static const int NFREE        = 3;
// the receiver is tuned in 1 MHz steps; the driver shifts the remainder digitally
static const double FREE_STEP = 1.0e6;
static const u32    EVT_OFF = 0x2F0000, EVT_BYTES = 4096; // where the event image is built, inside the stream block
static const char*  g_tune_path = "/usr/lib/sdr/ccs_tune.bin";
struct TEvent {
    u32                  count, hlen, plen, gap;
    const unsigned char *hdr, *pay;
};
struct TFam {
    char   name[9];
    int    nev;
    TEvent ev[8];
};
static TFam g_fam[4];
static int  g_nfam = 0;
static bool load_tune()
{
    FILE* f = fopen(g_tune_path, "rb");
    if (!f) {
        logf_("cannot open %s (free tuning disabled)", g_tune_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* b = (unsigned char*)malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return false;
    }
    fclose(f);
    u32 nf;
    if (n < 8 || memcmp(b, "CTUN", 4)) {
        free(b);
        return false;
    }
    memcpy(&nf, b + 4, 4);
    if (nf > 4) {
        free(b);
        return false;
    }
    // per family {name[8], nev} then nev * {count, hlen, plen, gap}; the header and payload bytes of all events follow
    // in the same order
    long meta = 8, dataoff;
    for (u32 i = 0; i < nf; i++) {
        u32 nev;
        memcpy(&nev, b + meta + 8, 4);
        meta += 12 + 16 * (long)nev;
    }
    dataoff = meta;
    meta    = 8;
    for (u32 i = 0; i < nf; i++) {
        TFam* F = &g_fam[i];
        memcpy(F->name, b + meta, 8);
        F->name[8] = 0;
        u32 nev;
        memcpy(&nev, b + meta + 8, 4);
        meta += 12;
        if (nev > 8) {
            free(b);
            return false;
        }
        F->nev = (int)nev;
        for (u32 k = 0; k < nev; k++, meta += 16) {
            TEvent* e = &F->ev[k];
            memcpy(&e->count, b + meta, 4);
            memcpy(&e->hlen, b + meta + 4, 4);
            memcpy(&e->plen, b + meta + 8, 4);
            memcpy(&e->gap, b + meta + 12, 4);
            if (e->hlen > 64 || dataoff + e->hlen + e->plen > n) {
                free(b);
                return false;
            }
            e->hdr = b + dataoff;
            e->pay = b + dataoff + e->hlen;
            dataoff += e->hlen + e->plen;
        }
    }
    // b stays allocated: the events point into it
    g_nfam = (int)nf;
    logf_("%d tuning scripts loaded", g_nfam);
    return true;
}
static const FreeRange* pick_free(double hz, double* lo)
{
    for (int i = 0; i < NFREE; i++) {
        if (hz < FREE[i].lo_mhz * 1e6 - 0.5e6 || hz > FREE[i].hi_mhz * 1e6 + 0.5e6)
            continue;
        double l = std_round(hz / FREE_STEP) * FREE_STEP;
        if (l < FREE[i].lo_mhz * 1e6)
            l = FREE[i].lo_mhz * 1e6;
        if (l > FREE[i].hi_mhz * 1e6)
            l = FREE[i].hi_mhz * 1e6;
        *lo = l;
        return &FREE[i];
    }
    return 0;
}

// Explicit choice of the tuning method: "B<n>" = LTE band n (receiver channel), "free-b20" / "free-b3" / "free-b3b1" =
// tuning script (the range limits are ignored).
static bool force_method(const char* m, double hz, u32* earfcn, double* lo, const FreeRange** R)
{
    if ((m[0] == 'B' || m[0] == 'b') && m[1] >= '0' && m[1] <= '9') {
        int         n = atoi(m + 1);
        const Band* b = 0;
        for (int i = 0; i < NBANDS; i++)
            if (BANDS[i].num == n)
                b = &BANDS[i];
        if (!b)
            return false;
        double blo = b->dl_low_mhz * 1e6, bhi = earfcn_to_hz(b, (u32)b->e_hi);
        if (hz < blo - REACH || hz > bhi + REACH)
            return false;
        int e = b->e_lo + (int)((hz - blo) / 1e5 + 0.5);
        if (e < b->e_lo)
            e = b->e_lo;
        if (e > b->e_hi)
            e = b->e_hi;
        if (!hdlc_safe((u32)e))
            e = (e + 1 <= b->e_hi) ? e + 1 : e - 1;
        *earfcn = (u32)e;
        *R      = 0;
        return true;
    }
    int idx = !strcmp(m, "free-b20") ? 0 : !strcmp(m, "free-b3") ? 1 : !strcmp(m, "free-b3b1") ? 2 : -1;
    if (idx < 0 || hz < 700e6 || hz > 3000e6)
        return false;
    *R      = &FREE[idx];
    *lo     = std_round(hz / FREE_STEP) * FREE_STEP;
    *earfcn = 0;
    return true;
}
// Which method tunes a frequency: ranges in MHz, tried in order; method = LTE band number, or 0 with a tuning script
// (index into FREE). Without a matching range the generic rule applies: the LTE band containing the frequency, else the
// tuning script whose range contains it.
struct Route {
    double lo_mhz, hi_mhz;
    int    band;
    int    free_idx;
};
// Measured with a test tone on the 10 MHz grid 740 - 2700 MHz: every method that can serve a point, at the same tone
// level. The smoothed best wins (mean over +-20 MHz); the current method is kept unless another is more than 4 dB
// better, which avoids switching back and forth between near equals.
static const Route ROUTES[] = { { 735.0, 755.0, 0, 0 },   { 755.0, 785.0, 28, 0 },  { 788.0, 824.0, 20, 0 },
                                { 825.0, 845.0, 0, 0 },   { 849.0, 872.0, 27, 0 },  { 875.0, 895.0, 5, 0 },
                                { 895.0, 925.0, 0, 0 },   { 925.0, 960.0, 8, 0 },   { 960.0, 995.0, 0, 0 },
                                { 1195.0, 2105.0, 0, 1 }, { 2107.0, 2155.0, 4, 0 }, { 2155.0, 2173.0, 1, 0 },
                                { 2175.0, 2185.0, 0, 2 }, { 2195.0, 2615.0, 0, 2 }, { 2617.0, 2693.0, 7, 0 } };
static const int   NROUTES  = (int)(sizeof ROUTES / sizeof ROUTES[0]);
static bool        choose_route(double hz, u32* earfcn, double* lo, const FreeRange** R)
{
    double mhz = hz / 1e6;
    for (int i = 0; i < NROUTES; i++) {
        if (mhz < ROUTES[i].lo_mhz || mhz >= ROUTES[i].hi_mhz)
            continue;
        if (ROUTES[i].band) {
            char m[8];
            snprintf(m, sizeof m, "B%d", ROUTES[i].band);
            if (force_method(m, hz, earfcn, lo, R))
                return true;
        } else if (g_nfam) {
            const char* nm[3] = { "free-b20", "free-b3", "free-b3b1" };
            if (force_method(nm[ROUTES[i].free_idx], hz, earfcn, lo, R))
                return true;
        }
    }
    if (pick_channel(hz, earfcn)) {
        *R = 0;
        return true;
    }
    if (g_nfam && (*R = pick_free(hz, lo))) {
        *earfcn = 0;
        return true;
    }
    return false;
}

// The oscillator words of the receiver chip for a receive frequency: the five bytes that the tuning script carries for the
// oscillator (the integer and the fractional part of its PLL).
//
// The oscillator runs at vco_mult times the receive frequency (1, or 3 for the band 20 script) and is set to
//     oscillator = (N + frac / 2^24) * 38.4 MHz,
// where 38.4 MHz is the reference of the chip. The words are
//     w[0..2]  frac, 24 bits, most significant byte first
//     w[3..4]  the integer field (N & 3) | ((N >> 2) << 3), 11 bits, most significant byte first
// This is a formula that was worked out from the words in the stock tuning scripts; it does not come from the firmware.
static void pll_words_formula(double lo_hz, int vco_mult, unsigned char w[5])
{
    double x = lo_hz * vco_mult / 38.4e6;
    u32    N = (u32)x, frac = (u32)(std_round((x - N) * 16777216.0));
    if (frac >= (1u << 24)) {
        N++;
        frac -= 1u << 24;
    }
    u32 intf = (N & 3) | ((N >> 2) << 3);
    w[0]     = (unsigned char)(frac >> 16);
    w[1]     = (unsigned char)(frac >> 8);
    w[2]     = (unsigned char)frac;
    w[3]     = (unsigned char)((intf >> 8) & 7);
    w[4]     = (unsigned char)intf;
}

// Builds the event image (event table, headers, payloads) for a family and a receive frequency. Returns the number of
// events, 0 on error.
static int g_patched = 0;
static int build_replay(const FreeRange* R, double lo_hz, unsigned char* img, u32 va)
{
    const TFam* F = 0;
    for (int i = 0; i < g_nfam; i++)
        if (!strcmp(g_fam[i].name, R->fam))
            F = &g_fam[i];
    if (!F)
        return 0;
    memset(img, 0, EVT_BYTES);
    int  n      = F->nev;
    u32  off    = (16 * n + 31) & ~31u;
    long p_frac = -1, p_int = -1;
    // chip memory (slave 2) address 0x400.. -> byte offset in the image
    static short s2map[0x400];
    for (int i = 0; i < 0x400; i++)
        s2map[i] = -1;
    for (int i = 0; i < n; i++) {
        const TEvent* e = &F->ev[i];
        unsigned char hdr[64];
        memcpy(hdr, e->hdr, e->hlen);
        u32 eva = va + off, pva = (eva + e->hlen + 15) & ~15u, po = pva - va, two = 2, zero = 0;
        if (po + e->plen > EVT_BYTES || e->hlen < 36)
            return 0;
        // event type, payload pointer, status
        memcpy(hdr + 8, &two, 4);
        memcpy(hdr + 28, &pva, 4);
        memcpy(hdr + 32, &zero, 4);
        memcpy(img + off, hdr, e->hlen);
        memcpy(img + po, e->pay, e->plen);
        // the script carries the PLL words: 01 32 03 <frac24> 01 30 02 <int>
        if (p_frac < 0)
            for (u32 k = 0; k + 11 <= e->plen; k++) {
                const unsigned char* q = e->pay + k;
                if (q[0] == 1 && q[1] == 0x32 && q[2] == 3 && q[6] == 1 && q[7] == 0x30 && q[8] == 2) {
                    p_frac = po + k + 3;
                    p_int  = po + k + 9;
                    break;
                }
            }
        // descriptors {address | control << 16}, then the data bytes
        u32 doff = 4 * e->count;
        for (u32 j = 0; j < e->count; j++) {
            u32 d;
            memcpy(&d, e->pay + 4 * j, 4);
            u32 ctrl = d >> 16, nd = ((ctrl >> 2) & 3) + 1, a = d & 0xffff;
            if (((ctrl >> 4) & 15) == 2 && (ctrl & 3) == 2)
                for (u32 q = 0; q < nd; q++)
                    if (a + q >= 0x400 && a + q < 0x800)
                        s2map[a + q - 0x400] = (short)(po + doff + q);
            doff += nd;
        }
        u32 ent[4] = { eva, 0x7f8, e->gap, 0 };
        memcpy(img + 16 * i, ent, 16);
        off = (po + e->plen + 15) & ~15u;
    }
    g_patched = 0;
    // block: find the register's write packet [reg_hi reg_lo n data...] in the script
    for (int k = 0; k < R->nblock; k++) {
        const Patch* pt  = &R->block[k];
        int          hit = -1;
        for (int a = 0x400; a < 0x5f0 && hit < 0; a++) {
            bool ok = true;
            for (int q = 0; q < 3 + pt->n && ok; q++)
                ok = a + q < 0x800 && s2map[a + q - 0x400] >= 0;
            if (ok && img[s2map[a - 0x400]] == (pt->reg >> 8) && img[s2map[a + 1 - 0x400]] == (pt->reg & 255) &&
                img[s2map[a + 2 - 0x400]] == pt->n)
                hit = a;
        }
        if (hit >= 0)
            for (int q = 0; q < pt->n; q++)
                img[s2map[hit + 3 + q - 0x400]] = pt->data[q];
        if (hit >= 0)
            g_patched++;
    }
    if (p_frac < 0)
        return 0;
    unsigned char w[5];
    pll_words_formula(lo_hz, R->vco_mult, w);
    img[p_frac]     = w[0];
    img[p_frac + 1] = w[1];
    img[p_frac + 2] = w[2];
    img[p_int]      = w[3];
    img[p_int + 1]  = w[4];
    return n;
}

// ---------------- RF chip registers (through the test mode)
// The receiver chip is written over the RFFE bus with the test-mode command 623: 20-byte packet {4b 0b 14 00 6f 02 0a
// 00 00 00 00 00, ext, rw, channel, slave, addr, data, 0}.
static size_t rffe_packet(unsigned char* p, int rw, u32 addr, u32 data)
{
    // DIAG subsystem dispatch, FTM, mode FTM_COMMON_C (20), command FTM_DO_RFFE_CMD (623), then 0x0a and zeros
    static const unsigned char head[12] = { DIAG_SUBSYS_CMD,
                                            DIAG_SUBSYS_FTM,
                                            FTM_MODE_COMMON,
                                            0x00,
                                            FTM_DO_RFFE_CMD & 0xff,
                                            FTM_DO_RFFE_CMD >> 8,
                                            0x0a,
                                            0x00,
                                            0x00,
                                            0x00,
                                            0x00,
                                            0x00 };
    // primary receiver: channel 4, slave 1
    memcpy(p, head, 12);
    p[12] = 0;
    p[13] = (unsigned char)rw;
    p[14] = 4;
    p[15] = 1;
    p[16] = (unsigned char)addr;
    p[17] = (unsigned char)(addr >> 8);
    p[18] = (unsigned char)data;
    p[19] = 0;
    return 20;
}
static int rffe_read(u32 addr)
{
    unsigned char q[20], r[1200];
    rffe_packet(q, 1, addr, 0);
    int l = diag_request(q, 20, 1.0, r, 0);
    return l >= 19 ? r[18] : -1;
}
static int RFFE_GAP_MS = getenv("SDR_GAP") ? atoi(getenv("SDR_GAP")) : 2;
// Writes registers addr0 .. addr0+n-1 back to back, then collects the answers.
static bool rffe_write_block(u32 addr0, const unsigned char* v, int n, u32 latch)
{
    if (g_diag < 0)
        return false;
    unsigned char junk[4096];
    while (read(g_diag, junk, sizeof junk) > 0) {
    }
    int total = n + 1;
    for (int i = 0; i < total; i++) {
        unsigned char q[20], fr[64];
        rffe_packet(q, 0, i < n ? addr0 + i : latch, i < n ? v[i] : 1);
        size_t fn  = hdlc_frame(q, 20, fr);
        size_t off = 0;
        double end = now_s() + 2.0;
        while (off < fn && now_s() < end) {
            ssize_t k = write(g_diag, fr + off, fn - off);
            if (k > 0)
                off += k;
            else {
                struct pollfd pf = { g_diag, POLLOUT, 0 };
                poll(&pf, 1, 20);
            }
        }
        if (off < fn)
            return false;
        msleep(RFFE_GAP_MS);
    }
    unsigned char buf[8192];
    size_t        bl  = 0;
    int           got = 0;
    double        end = now_s() + 2.0;
    while (got < total && now_s() < end) {
        struct pollfd pf = { g_diag, POLLIN, 0 };
        if (poll(&pf, 1, 100) <= 0)
            continue;
        ssize_t r = read(g_diag, buf + bl, sizeof buf - bl);
        if (r <= 0)
            continue;
        bl += r;
        for (;;) {
            unsigned char* e = (unsigned char*)memchr(buf, 0x7e, bl);
            if (!e)
                break;
            size_t        part = e - buf;
            unsigned char out[1200];
            int           l = part ? hdlc_unframe(buf, part, out) : -1;
            if (l >= 6 && out[0] == 0x4b && out[1] == 0x0b && out[4] == 0x6f && out[5] == 0x02)
                got++;
            memmove(buf, e + 1, bl - part - 1);
            bl -= part + 1;
        }
    }
    if (got < total)
        logf_("RFFE block write: %d of %d answers", got, total);
    return got >= total;
}

// ---------------- receiver gain and analog filter (tables from the firmware image)
// One table per band, filter bandwidth and RF port: six rows (gain states) of 27 register values for registers
// 0x045E..0x0478.
struct Lut {
    int                  band;
    char                 bw[8];
    char                 port[8];
    const unsigned char* rows;
};
static Lut*      g_luts   = 0;
static int       g_nluts  = 0;
static const int LUT_REGS = 27, LUT_FIRST = 0x045E, LUT_LATCH = 0x0479;
static bool      load_luts()
{
    FILE* f = fopen(g_luts_path, "rb");
    if (!f) {
        logf_("cannot open %s (gain and bandwidth control disabled)", g_luts_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* b = (unsigned char*)malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return false;
    }
    fclose(f);
    u32 cnt;
    memcpy(&cnt, b + 4, 4);
    const long E = 40 + 6 * LUT_REGS;
    if (memcmp(b, "WLUT", 4) || 8 + (long)cnt * E > n) {
        free(b);
        return false;
    }
    g_luts = (Lut*)calloc(cnt, sizeof(Lut));
    for (u32 i = 0; i < cnt; i++) {
        const unsigned char* e = b + 8 + (size_t)i * E;
        Lut*                 l = &g_luts[g_nluts];
        if (sscanf((const char*)e, "b%d_bw%7[^_]_%7s", &l->band, l->bw, l->port) == 3) {
            l->rows = e + 40;
            g_nluts++;
        }
    }
    logf_("%d gain tables loaded", g_nluts);
    return g_nluts > 0;
}
static const double      BW_MHZ[]  = { 1.4, 3, 5, 10, 15, 20, 40 };
static const char* const BW_NAME[] = { "1p4m", "3m", "5m", "10m", "15m", "20m", "40m" };
static int               bw_index(const char* name)
{
    for (int i = 0; i < 7; i++)
        if (!strcmp(BW_NAME[i], name))
            return i;
    return -1;
}
// requested gain state and filter bandwidth index; applied after every channel change
static int        g_want_gain = 0, g_want_bw = 6;
static int        g_cur_gain = -1, g_cur_bw = -1; // what the chip holds now (-1 = unknown)
static char       g_cur_port[8] = "";
static const Lut* find_lut(int band, const char* bw, const char* port)
{
    for (int i = 0; i < g_nluts; i++)
        if (g_luts[i].band == band && !strcmp(g_luts[i].bw, bw) && !strcmp(g_luts[i].port, port))
            return &g_luts[i];
    return 0;
}
// After a channel change the chip holds the default row of the band: read it back and find which table, bandwidth and
// gain state it is. This identifies the RF port that the band uses without a per-band table.
static bool detect_frontend(int band)
{
    g_cur_gain = g_cur_bw = -1;
    g_cur_port[0]         = 0;
    if (!g_nluts)
        return false;
    unsigned char live[LUT_REGS];
    for (int i = 0; i < LUT_REGS; i++) {
        int v = rffe_read(LUT_FIRST + i);
        if (v < 0)
            return false;
        live[i] = (unsigned char)v;
    }
    int matches = 0;
    for (int i = 0; i < g_nluts; i++) {
        if (g_luts[i].band != band)
            continue;
        for (int s = 0; s < 6; s++)
            if (!memcmp(g_luts[i].rows + s * LUT_REGS, live, LUT_REGS)) {
                // several ports can hold the same default row: the last one (highest port number) is the one the band-3
                // receive path uses
                g_cur_gain = s;
                g_cur_bw   = bw_index(g_luts[i].bw);
                snprintf(g_cur_port, sizeof g_cur_port, "%s", g_luts[i].port);
                matches++;
            }
    }
    if (!matches) {
        logf_("band %d: the chip's gain registers match no table; gain and bandwidth control disabled for this channel",
              band);
        return false;
    }
    if (matches > 1)
        logf_("band %d: %d tables match the chip state, using port %s", band, matches, g_cur_port);
    return true;
}
static bool apply_frontend(int band)
{
    if (!g_cur_port[0])
        return false;
    if (g_cur_gain == g_want_gain && g_cur_bw == g_want_bw)
        return true;
    const Lut* l = find_lut(band, BW_NAME[g_want_bw], g_cur_port);
    if (!l) {
        logf_("no table for band %d, %s MHz filter, port %s", band, BW_NAME[g_want_bw], g_cur_port);
        return false;
    }
    if (!rffe_write_block(LUT_FIRST, l->rows + g_want_gain * LUT_REGS, LUT_REGS, LUT_LATCH)) {
        logf_("writing the gain row failed");
        return false;
    }
    g_cur_gain = g_want_gain;
    g_cur_bw   = g_want_bw;
    return true;
}

// ---------------- test mode bring-up
static u32 g_ftm_earfcn = 0, g_want_earfcn = DEFAULT_EARFCN; // channel of the last ns501 / wanted channel
// free tuning: frequency set by script replay / wanted (0 = not used)
static double           g_cur_free_hz = 0, g_want_free_hz = 0;
static const FreeRange* g_cur_range  = 0;
static const FreeRange* g_want_range = 0;
// band whose gain tables apply to the chip's current state
static int  g_cur_band               = 0;
static bool g_ns_up                  = false; // test mode entered
// DSP virtual address of the 3 MiB block (log + ring + stack); equals the physical address
static u32  g_block                  = 0;
static bool g_bands_enabled          = false;
// the last ns501 found a real cell (status 0): the test mode then refuses a new ns501 until it is stopped (ns504) and
// started (ns500) again
static bool g_ns_acquired            = false;
static bool ns501(u32 earfcn)
{
    const Band* b = band_of_earfcn(earfcn);
    if (!b)
        return false;
    unsigned char r[1200], body[5] = { (unsigned char)(b->num - 1),
                                       (unsigned char)earfcn,
                                       (unsigned char)(earfcn >> 8),
                                       (unsigned char)(earfcn >> 16),
                                       (unsigned char)(earfcn >> 24) };
    unsigned char z[10] = {};
    for (int attempt = 0; attempt < 2; attempt++) {
        if (g_ns_acquired || attempt) { // leave the "cell found" state: stop the test, then start it again
            ftm_ns(NS_STOP, z, 0, 8, NS_OK, NS_ALREADY_DONE, r, 0);
            ftm_ns(NS_START, z, sizeof z, 8, NS_OK, NS_ALREADY_DONE, r, 0);
            g_ns_acquired = false;
        }
        int           l = 0, st = -1;
        unsigned char req[16] = { DIAG_SUBSYS_CMD,   DIAG_SUBSYS_FTM, FTM_MODE_LTE_NS, 0x00,
                                  NS_ACQUIRE & 0xff, NS_ACQUIRE >> 8 };
        memcpy(req + 6, body, 5);
        l = diag_request(req, 11, 14, r);
        if (l < 10) {
            logf_("ns501: no response");
            return false;
        }
        memcpy(&st, r + 6, 4);
        logf_("ns501 status=%d", st);
        if (st == NS_NO_CELL)
            return true; // no cell found: fine for sampling
        if (st == NS_OK) {
            g_ns_acquired = true;
            return true;
        } // a real cell was found (E_SUCCESS)
        // NS_ALREADY_DONE or NS_NOT_ALLOWED: the test mode is in a state that needs NS_START first; retry once
    }
    return false;
}
// The firmware builds the per-band tables only for the bands in its factory mask (3, 7 and 20). Calling its table
// builder again with a wider mask, once the test mode runs, makes the other bands usable. The call rebuilds (and
// clears) the tables of the bands in the mask.
static bool enable_bands()
{
    u64 mask = 0;
    for (int i = 0; i < NBANDS; i++)
        mask |= 1ull << (BANDS[i].num - 1);
    u64 st[2] = { mask, 0 };
    u32 at    = g_block + 2048;
    if (!mem_write(phys(at), st, sizeof st))
        return false;
    u32 r = 0;
    if (!dsp_call("nv_bands", at, 0, 0, &r))
        return false;
    logf_("bands enabled (mask %#llx, result %u)", (unsigned long long)mask, r);
    g_bands_enabled = true;
    return true;
}
// ---------------- receive path switch lines (GRFC pins of the modem chip)
// The low-band receive path of this board needs three pins of the modem chip (GPIO 79, 84, 85; the card calls them
// RF_PATH_SEL_03, _14 and _07) at a high level for band 8 (925 - 960 MHz): the manufacturer's firmware sets them when
// it enters the band, the card data of this firmware does not. Measured: with them high the 935.6 MHz GSM carrier shows
// 15 dB above the noise, with them low it is invisible; in every other band they must stay low (11 - 25 dB less
// sensitivity). They are driven as plain outputs by this program after a tuning step to band 8 and handed back to the
// firmware (function GRFC, level low) at every other tuning step: a channel change does not reprogram them (bands 3, 5,
// 20 ... do not use these lines), so they would stay high.
static const int PATH_PINS[3]  = { 79, 84, 85 };
static const u64 TLMM_BASE     = 0x01000000ull;
static bool      g_pins_driven = false;
static u32       tlmm_rd(u64 a)
{
    u32 v = 0;
    mem_read(a, &v, 4);
    return v;
}
static void tlmm_wr(u64 a, u32 v)
{
    mem_write(a, &v, 4);
}
static void set_path_pins(bool high)
{
    if (high) {
        for (int i = 0; i < 3; i++) {
            u64 a   = TLMM_BASE + 0x1000ull * PATH_PINS[i];
            u32 cfg = tlmm_rd(a);
            // function 0 (GPIO), output enabled, level high
            tlmm_wr(a + 4, 2);
            tlmm_wr(a, (cfg & ~(7u << 2)) | 0x200u);
        }
        if (!g_pins_driven)
            logf_("receive path pins GPIO %d/%d/%d driven high", PATH_PINS[0], PATH_PINS[1], PATH_PINS[2]);
        g_pins_driven = true;
    } else if (g_pins_driven) {
        // back to GRFC (function 1), level low
        for (int i = 0; i < 3; i++) {
            u64 a = TLMM_BASE + 0x1000ull * PATH_PINS[i];
            tlmm_wr(a + 4, 0);
            tlmm_wr(a, (tlmm_rd(a) & ~(7u << 2)) | (1u << 2));
        }
        g_pins_driven = false;
        logf_("receive path pins released");
    }
}
static void save_block();
static bool ns_up()
{ // enters the test mode once, then builds the tables of the other bands once
    unsigned char r[1200];
    if (!g_ns_up) {
        // Masked, not just stopped: NetworkManager starts ModemManager again within a minute, and its start-up commands
        // crash the modem in test mode. The mask lives in /run (until the next boot); undo it with "systemctl unmask
        // --runtime ModemManager".
        if (system("systemctl mask --runtime --now ModemManager"))
            logf_("could not stop ModemManager");
        const unsigned char enter[3] = { 0x29, 0x03, 0x00 };
        if (diag_request(enter, 3, 4, r) < 0) {
            logf_("enter test mode failed");
            return false;
        }
        unsigned char nvw[133] = { 0x27, 0xc5, 0x01 }, nvr[133] = { 0x26, 0xc5, 0x01 };
        diag_request(nvw, sizeof nvw, 4, r);
        int l = diag_request(nvr, sizeof nvr, 4, r);
        if (l < 133 || r[3] != 0 || r[131] || r[132]) {
            logf_("NV 453 is not zero");
            return false;
        }
        unsigned char z[10] = {};
        if (!ftm_ns(NS_START, z, sizeof z, 8, NS_OK, NS_ALREADY_DONE, r, 0))
            return false;
        // first channel on band 3 (always available): initialises the receiver chip, which the tuning scripts need
        if (!ns501(DEFAULT_EARFCN))
            return false;
        g_ns_up       = true;
        g_ftm_earfcn  = DEFAULT_EARFCN;
        g_cur_free_hz = 0;
        g_cur_band    = 3;
        g_cur_range   = 0;
        detect_frontend(3);
        apply_frontend(3);
    }
    if (!g_bands_enabled && g_block) {
        if (enable_bands())
            save_block();
    } // then widen the band mask
    return true;
}
static bool ftm_up(u32 earfcn)
{
    const Band* b = band_of_earfcn(earfcn);
    if (!b) {
        logf_("EARFCN %u is in no supported band", earfcn);
        return false;
    }
    if (!ns_up())
        return false;
    if (g_ftm_earfcn == earfcn && g_cur_free_hz == 0)
        return true;
    if (!ns501(earfcn))
        return false;
    g_ftm_earfcn  = earfcn;
    g_cur_free_hz = 0;
    g_cur_range   = 0;
    g_cur_band    = b->num;
    detect_frontend(b->num);
    apply_frontend(b->num);
    set_path_pins(b->num == 8);
    return true;
}
// Free tuning: replay the receiver's tuning script with the PLL words of the wanted frequency.
static bool ftm_up_free(const FreeRange* R, double lo_hz)
{
    if (!ns_up())
        return false;
    if (g_cur_free_hz == lo_hz && g_cur_range == R)
        return true;
    if (!g_block)
        return false;
    static unsigned char img[EVT_BYTES];
    u32                  va = g_block + EVT_OFF;
    int                  n  = build_replay(R, lo_hz, img, va);
    if (!n) {
        logf_("tuning script %s is not available", R->fam);
        return false;
    }
    if (!mem_write(phys(va), img, EVT_BYTES))
        return false;
    double t0   = now_s();
    u32    mask = 0;
    if (!dsp_call("ccs_replay", va, (u32)n, va, &mask))
        return false;
    double t1 = now_s();
    if (mask != (1u << n) - 1) {
        logf_("tuning script: %d of %d events completed (mask %#x)", __builtin_popcount(mask), n, mask);
        return false;
    }
    g_cur_free_hz = lo_hz;
    g_cur_range   = R;
    g_cur_band    = R->band;
    // the script leaves the chip in its default state of that band: 1.4 MHz filter, gain state 0
    g_cur_gain    = 0;
    g_cur_bw      = 0;
    snprintf(g_cur_port, sizeof g_cur_port, "%s", R->port);
    apply_frontend(R->band);
    set_path_pins(false);
    logf_("tuned to %.3f MHz with the %s script, %d/%d block registers (script %.0f ms, gain/filter %.0f ms)",
          lo_hz / 1e6,
          R->fam,
          g_patched,
          R->nblock,
          (t1 - t0) * 1e3,
          (now_s() - t1) * 1e3);
    return true;
}

// ---------------- stream
static u32 g_last_seq           = 0; // used to notice a modem restart
// rate reached / wanted / the wanted value that was last applied
static double          g_cur_fs = FS_DEFAULT, g_want_fs = FS_DEFAULT, g_applied_fs = 0;
static u32             g_fs_orig[6];
static bool            g_fs_have = false; // factory values of the six sample-rate registers
static bool            g_running = false;
static pid_t           g_server  = 0;
static int             g_data_ls = -1; // listening sample socket, owned by this process and inherited by sample_server
static pthread_mutex_t g_lock    = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_quit = 0;
static void                  on_term(int)
{
    g_quit = 1;
}

// words of the copy program's control block at the start of the stream block (sdr/dsp/mcdma.c)
enum {
    CB_MAGIC           = 0,
    CB_OUT_RING        = 1,
    CB_OUT_RECORDS     = 2,
    CB_RECORDS_TO_COPY = 3,
    CB_STOP            = 5,
    CB_HEAD_REG        = 6,
    CB_RING_SAMPLES    = 7,
    CB_BLOCK_BYTES     = 8,
    CB_STATUS          = 10,
    CB_RECORDS_COPIED  = 11,
    CB_LATE            = 14,
    CB_SLIPS           = 16,
    CB_RATE_MUL        = 21,
    CB_WORDS           = 128
};
static const u32 CB_MAGIC_CHS2 = 0x32534843u; // "CHS2"
static const u32 CB_RUNNING    = 1;
static const u32 NB0_HEAD_REG = 0xEC3680D4u, NB1_HEAD_REG = 0xEC3690D4u; // write positions of narrowband writers 0, 1
static const u32 WRITER_RING_SAMPLES = 25600, WRITER_BLOCK_BYTES = 3200;
static u32       log_word(int i)
{
    return rd32pa(phys(g_block) + 4 * i);
}
static void reap_server()
{
    if (g_server > 0 && waitpid(g_server, 0, WNOHANG) == g_server)
        g_server = 0;
}
static void stop_server()
{
    if (g_server <= 0)
        return;
    kill(g_server, SIGTERM);
    for (int i = 0; i < 50; i++) {
        if (waitpid(g_server, 0, WNOHANG) == g_server) {
            g_server = 0;
            return;
        }
        msleep(20);
    }
    // it may sit in accept() where SIGTERM does not interrupt it
    kill(g_server, SIGKILL);
    waitpid(g_server, 0, 0);
    g_server = 0;
}
// sandbox period: short while in use, long after 10 s without a stream
static bool   g_fast       = false;
static double g_idle_since = 0;
// forget everything learnt about a modem that has been restarted
static void reset_modem_state()
{
    if (g_diag >= 0) {
        close(g_diag);
        g_diag = -1;
    }
    g_block         = 0;
    g_running       = false;
    g_ftm_earfcn    = 0;
    g_ns_up         = false;
    g_ns_acquired   = false;
    g_cur_free_hz   = 0;
    g_cur_range     = 0;
    g_cur_band      = 0;
    g_bands_enabled = false;
    g_cur_gain = g_cur_bw = -1;
    g_fs_have             = false;
    g_applied_fs          = 0;
    g_cur_fs              = FS_DEFAULT;
    g_pins_driven         = false;
    g_fast                = false;
    stop_server();
}
static bool detect_restart()
{
    Ctrl c;
    if (!ctrl_get(&c))
        return true;
    bool restarted = c.w[SB_SEQ] < g_last_seq;
    g_last_seq     = c.w[SB_SEQ];
    if (restarted) {
        logf_("modem restart detected");
        reset_modem_state();
    }
    return restarted;
}
// The sandbox task wakes up once per period (1000 ms by default); a short period makes every DSP call take milliseconds
// instead of a second. period_ms of the control block: the task takes it at its next wake-up (at most one old period
// later)
static void set_period(u32 ms)
{
    wr32pa(g_ctrl_pa + 4 * SB_PERIOD_MS, ms);
    g_fast = ms < 1000;
}
// The modem heap is small and never freed, so the block is remembered across restarts of this service in /run (cleared
// at reboot, like the modem). It is trusted only while the sandbox call counter has not gone back below the value saved
// with it (it restarts from 0 with the modem).
static const char* BLOCK_FILE = "/run/sdr/block";
static void        save_block()
{
    Ctrl c;
    if (!ctrl_get(&c))
        return;
    mkdir("/run/sdr", 0755);
    FILE* f = fopen(BLOCK_FILE, "w");
    if (!f)
        return;
    fprintf(f, "%u %u %d\n", g_block, c.w[SB_SEQ], g_bands_enabled ? 1 : 0);
    fclose(f);
}
static void load_block()
{
    FILE* f = fopen(BLOCK_FILE, "r");
    if (!f)
        return;
    unsigned b = 0, seq = 0;
    int      bands = 0, n = fscanf(f, "%u %u %d", &b, &seq, &bands);
    fclose(f);
    Ctrl c;
    if (n < 2 || !ctrl_get(&c))
        return;
    if (c.w[SB_SEQ] >= seq) {
        g_block         = b;
        g_bands_enabled = bands == 1;
        logf_("reusing block %08x", g_block);
    } else
        logf_("saved block is stale (modem restarted)");
}
static bool moving(u32 reg, const char* what)
{
    u32 first = reg_read(reg);
    for (int i = 0; i < 6; i++) {
        msleep(20);
        if (reg_read(reg) != first)
            return true;
    }
    logf_("%s writer head is not moving", what);
    return false;
}
// ---------------- sample rate
// The sample rate is set by the ADC clock (115.2 or 72 MHz), the decimation after it and a fractional resampler
// (T2BYT1M1 = m * 2^32, rate = f_in / (dec * (1 + m)), m in [0.6, 1)). Mode A (ADC 115.2 MHz, decimation 8) covers 7.2
// .. 9.0 MS/s, mode B (72 MHz, decimation 4) 9.0 .. 11.25 MS/s, mode C (115.2 MHz, decimation 4) 14.4 .. 18 MS/s,
// limited here to 14.8. Between 11.25 and 14.4 MS/s no mode reaches. f_in carries the firmware's own correction of the
// oscillator error: the factory T2BYT1M1 (m0) gives exactly 7.68 MS/s, so f_in = 7.68e6 * 8 * (1 + m0) * f_adc /
// 115.2e6.
struct RateMode {
    const char* name;
    double      f_adc;
    int         dec;
    u32         mux, misc, logic, wb_dec;
};
static const int      NRATE_MODES   = 3;
static const RateMode RATE_MODES[3] = { { "A", 115.2e6, 8, 0, 1, 0, 0x19 },
                                        { "B", 72.0e6, 4, 2, 3, 1, 0x18 },
                                        { "C", 115.2e6, 4, 0, 1, 0, 0x18 } };
// the 20-bit samples (2.5 bytes each) nearly fill the USB link (about 37.5 MB/s); above this records are lost after a
// while
static const double RATE_MAX        = 14.8e6;
static const double M_MIN = 0.6, M_MAX = 0.9999999;
// clock divider, mux, ADC logic, decimator, resampler config, T2BYT1M1
static const u32   FS_REGS[6] = { 0xEC181138u, 0xEC180068u, 0xEC360004u, 0xEC3640B4u, 0xEC364088u, 0xEC36408Cu };
static const char* FS_FILE    = "/run/sdr/fs_orig";
struct RatePlan {
    int    mode;
    u32    m_reg;
    double actual;
    bool   clamped;
    u32    t1;
};
static RatePlan plan_rate(double fs, double m0)
{
    bool capped = fs > RATE_MAX;
    if (capped)
        fs = RATE_MAX;
    const double k     = 7.68e6 * 8 * (1 + m0) / 115.2e6;
    RatePlan     best  = { -1, 0, 0, false, 0 };
    double       bestd = 1e30, fs_t = fs;
    int          mode = -1;
    for (int i = 0; i < NRATE_MODES && mode < 0; i++) {
        double fin = RATE_MODES[i].f_adc * k;
        double lo = fin / (RATE_MODES[i].dec * (1 + M_MAX)), hi = fin / (RATE_MODES[i].dec * (1 + M_MIN));
        if (fs >= lo && fs <= hi)
            mode = i;
    }
    bool clamped = capped;
    if (mode < 0) { // out of range: nearest edge of any mode
        for (int i = 0; i < NRATE_MODES; i++) {
            double fin  = RATE_MODES[i].f_adc * k;
            double e[2] = { fin / (RATE_MODES[i].dec * (1 + M_MAX)), fin / (RATE_MODES[i].dec * (1 + M_MIN)) };
            for (int j = 0; j < 2; j++) {
                double d = e[j] > fs ? e[j] - fs : fs - e[j];
                if (d < bestd) {
                    bestd = d;
                    mode  = i;
                    fs_t  = e[j];
                }
            }
        }
        clamped = true;
    }
    const RateMode& M   = RATE_MODES[mode];
    double          fin = M.f_adc * k, m = fin / (M.dec * fs_t) - 1;
    double          mr = std_round(m * 4294967296.0);
    if (mr < 1)
        mr = 1;
    if (mr > 4294967295.0)
        mr = 4294967295.0;
    u32    m_reg  = (u32)mr;
    double actual = fin / (M.dec * (1 + m_reg / 4294967296.0));
    double t1     = std_round(128.0 * M.dec * actual / M.f_adc);
    if (t1 < 1)
        t1 = 1;
    if (t1 > 127)
        t1 = 127;
    best.mode    = mode;
    best.m_reg   = m_reg;
    best.actual  = actual;
    best.clamped = clamped;
    best.t1      = (u32)t1;
    return best;
}
static void save_fs_orig()
{
    Ctrl c;
    if (!ctrl_get(&c))
        return;
    mkdir("/run/sdr", 0755);
    FILE* f = fopen(FS_FILE, "w");
    if (!f)
        return;
    fprintf(f, "%u", c.w[SB_SEQ]);
    for (int i = 0; i < 6; i++)
        fprintf(f, " %u", g_fs_orig[i]);
    fprintf(f, "\n");
    fclose(f);
}
// the factory values survive a restart of this service (same validity rule as the memory block)
static void load_fs_orig()
{
    if (g_fs_have)
        return;
    FILE* f = fopen(FS_FILE, "r");
    if (!f)
        return;
    unsigned seq = 0, v[6];
    int      n   = fscanf(f, "%u %u %u %u %u %u %u", &seq, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    fclose(f);
    Ctrl c;
    if (n != 7 || !ctrl_get(&c) || c.w[SB_SEQ] < seq)
        return;
    for (int i = 0; i < 6; i++)
        g_fs_orig[i] = v[i];
    g_fs_have = true;
}
// Programs the clock, decimator and resampler for g_want_fs; sets g_cur_fs to the rate actually reached. Needs an idle
// sandbox.
static bool apply_rate()
{
    load_fs_orig();
    if (!g_fs_have) {
        for (int i = 0; i < 6; i++)
            g_fs_orig[i] = reg_read(FS_REGS[i]);
        g_fs_have = true;
        save_fs_orig();
    }
    double          m0      = g_fs_orig[5] / 4294967296.0;
    RatePlan        P       = plan_rate(g_want_fs, m0);
    const RateMode& M       = RATE_MODES[P.mode];
    u32             want[6] = { M.misc, M.mux, M.logic, M.wb_dec, P.mode == 0 ? g_fs_orig[4] : (P.t1 << 14), P.m_reg };
    // mode A = the factory state
    if (P.mode == 0) {
        want[0] = g_fs_orig[0];
        want[1] = g_fs_orig[1];
        want[2] = g_fs_orig[2];
        want[3] = g_fs_orig[3];
    }
    for (int i = 0; i < 6; i++) {
        if (i < 5 && reg_read(FS_REGS[i]) == want[i])
            continue;
        if (!reg_write(FS_REGS[i], want[i]))
            return false;
        if (i < 3)
            msleep(300); // a clock switch needs time to settle
    }
    if (!reg_write(0xEC364090u, 0x3u))
        return false; // resampler command: load the new factor
    g_cur_fs     = P.actual;
    g_applied_fs = g_want_fs;
    logf_("sample rate %.6f MS/s (mode %s%s)", P.actual / 1e6, M.name, P.clamped ? ", clamped" : "");
    return true;
}

static bool configure_stream()
{
    if (!moving(NB1_HEAD_REG, "NB1"))
        return false;
    static const u32 w[5][2] = { { 0xEC36D000u, 0x1a38c800u },
                                 { 0xEC36D004u, 0x800u },
                                 { 0xEC3680B4u, 0x42u },
                                 { 0xEC36D028u, 0x13u },
                                 { 0xEC338008u, 0x80000800u } };
    for (int i = 0; i < 5; i++)
        if (!reg_write(w[i][0], w[i][1]))
            return false;
    // clock, decimator and resampler for the wanted rate (the factory plan = 7.68 MS/s)
    if (!apply_rate())
        return false;
    return moving(NB0_HEAD_REG, "NB0");
}
// The digital channel filter after the ADC (11 coefficients) cuts at about 1.75 MHz. For analog filter bandwidths of 5
// MHz and more it is replaced by a pass-through so that the analog response determines the band; the factory
// coefficients are read back first and restored.
static const u32 ICI_BASE = 0xEC36803Cu;
static const int ICI_N    = 11;
static u32       g_ici_fact[ICI_N];
static bool      g_ici_have = false;
static bool      ici_is_delta(const u32* v)
{
    for (int i = 0; i < ICI_N; i++)
        if (v[i] != (i == 5 ? 0x1ffu : 0u))
            return false;
    return true;
}
static bool ici_apply(bool wide)
{
    u32 live[ICI_N];
    for (int i = 0; i < ICI_N; i++)
        live[i] = reg_read(ICI_BASE + 4 * i);
    if (!ici_is_delta(live)) {
        memcpy(g_ici_fact, live, sizeof live);
        g_ici_have = true;
    }
    if (wide == ici_is_delta(live))
        return true;
    if (!wide && !g_ici_have)
        return true;
    for (int i = 0; i < ICI_N; i++) {
        u32 v = wide ? (i == 5 ? 0x1ffu : 0u) : g_ici_fact[i];
        if (!reg_write(ICI_BASE + 4 * i, v))
            return false;
    }
    return true;
}
// ---------------- firmware management
// The custom modem firmware is loaded on demand: a stream start finds out whether it runs and, if not, switches the
// modem to it (and masks ModemManager, which the custom firmware does not tolerate). After a restart of the modem every
// piece of state learnt before is dropped. The device node of the modem's diagnostic port is recreated at every restart
// of the modem: its change time tells the restarts apart.
static long long modem_generation()
{
    struct stat st;
    if (stat("/dev/wwan0qcdm0", &st))
        return 0;
    return (long long)st.st_ctim.tv_sec * 1000000000LL + st.st_ctim.tv_nsec;
}
static long long g_gen_ok = 0; // generation at the last successful start (0: unknown)
// sender queue in MB: a client that falls behind loses records instead of getting everything late (4 MB = 0.1 s at 14.8
// MS/s)
static int g_queue_mb     = 4;
// seconds without a stream after which the stock firmware and ModemManager come back (0: never)
static int g_idle_stock   = 0;
// read only: the task counter of the sandbox moves (one tick per period, 1 s by default)
static bool sandbox_live()
{
    if (!g_counter_va)
        return false;
    u32 first = rd32pa(phys(g_counter_va));
    for (int i = 0; i < 70; i++) {
        msleep(50);
        if (rd32pa(phys(g_counter_va)) != first)
            return true;
    }
    return false;
}
static bool ensure_firmware()
{
    long long gen = modem_generation();
    // nothing happened to the modem since the last start
    if (gen && gen == g_gen_ok && sandbox_init())
        return true;
    bool live = sandbox_init() && sandbox_live();
    if (!live) {
        logf_("the custom firmware is not running: switching the modem to it");
        if (system("systemctl mask --runtime --now ModemManager"))
            logf_("could not stop ModemManager");
        if (system("qcom-sdr-switch-fw custom")) {
            logf_("the modem does not start (is it offline? reboot the phone)");
            return false;
        }
        double end = now_s() + 45;
        while (now_s() < end && !live) {
            msleep(500);
            live = sandbox_init() && sandbox_live();
        }
        if (!live) {
            logf_("the sandbox task does not answer after the switch");
            return false;
        }
        gen = modem_generation();
    }
    if (gen != g_gen_ok) {
        if (g_gen_ok)
            logf_("the modem was restarted: dropping the old state");
        reset_modem_state();
    }
    g_gen_ok = gen;
    return true;
}
static void return_to_stock()
{
    logf_("no stream for %d s: back to the stock firmware and ModemManager", g_idle_stock);
    if (system("qcom-sdr-switch-fw stock"))
        logf_("switch to the stock firmware failed");
    system("systemctl unmask --runtime ModemManager");
    system("systemctl start ModemManager");
    reset_modem_state();
    g_gen_ok     = 0;
    g_idle_since = 0;
}
// Starts the sample sender: it waits for one client on the shared listening socket, so a client that connected earlier
// is served at once.
static bool spawn_server()
{
    char a1[32], a2[32], a3[32], a4[32], a5[32];
    snprintf(a1, sizeof a1, "%d", g_data_ls);
    snprintf(a2, sizeof a2, "0x%x", g_block);
    snprintf(a3, sizeof a3, "0x%x", g_block + LOG_BYTES);
    snprintf(a4, sizeof a4, "%u", REC_BYTES);
    snprintf(a5, sizeof a5, "%d", g_queue_mb);
    pid_t p = fork();
    if (p == 0) {
        execl(g_server_path,
              g_server_path,
              "--listen-fd",
              a1,
              "--log",
              a2,
              "--ring",
              a3,
              "--rec",
              a4,
              "-q",
              a5,
              (char*)0);
        _exit(127);
    }
    if (p < 0)
        return false;
    g_server = p;
    return true;
}
static bool start_stream()
{
    double           t0      = now_s();
    u32              earfcn  = g_want_earfcn;
    double           free_hz = g_want_free_hz;
    const FreeRange* range   = g_want_range;
    detect_restart();
    if (!g_block)
        load_block();
    if (g_block && !g_running && log_word(CB_MAGIC) == CB_MAGIC_CHS2 && log_word(CB_STATUS) == CB_RUNNING) {
        g_running = true;
        logf_("copy program from a previous run found");
    }
    if (g_running && log_word(CB_STATUS) == CB_RUNNING && g_want_fs == g_applied_fs &&
        (free_hz > 0 ? (g_cur_free_hz == free_hz && g_cur_range == range)
                     : (g_cur_free_hz == 0 && g_ftm_earfcn == earfcn))) {
        reap_server();
        goto serve;
    }
    if (g_running) {
        u32 one = 1;
        mem_write(phys(g_block) + 4 * CB_STOP, &one, 4);
        wait_done(5.0);
        g_running = false;
    }
    if (!g_fast)
        set_period(5);
    if (!g_block) {
        u32 r = 0;
        if (!dsp_call("mem_alloc", BLOCK_BYTES, 0, 0, &r) || !r) {
            logf_("modem heap allocation failed (restart the modem)");
            return false;
        }
        g_block = (r + 31) & ~31u;
        logf_("block %08x", g_block);
    }
    g_block = (g_block + 31) & ~31u; // the copy program needs a 32-byte aligned ring
    if (!(free_hz > 0 ? ftm_up_free(range, free_hz) : ftm_up(earfcn)))
        return false;
    logf_("test mode ready after %.2f s", now_s() - t0);
    if (!configure_stream())
        return false;
    ici_apply(g_want_bw >= 2);
    logf_("stream configured after %.2f s", now_s() - t0);
    {
        u32 h[CB_WORDS]       = {};
        h[CB_MAGIC]           = CB_MAGIC_CHS2;
        h[CB_OUT_RING]        = g_block + LOG_BYTES;
        h[CB_OUT_RECORDS]     = RING_RECS;
        h[CB_RECORDS_TO_COPY] = 0x7fffffffu; // run until stopped
        h[CB_HEAD_REG]        = NB0_HEAD_REG;
        h[CB_RING_SAMPLES]    = WRITER_RING_SAMPLES;
        h[CB_BLOCK_BYTES]     = WRITER_BLOCK_BYTES;
        h[CB_RATE_MUL]        = (u32)(g_cur_fs / 19.2e6 * (1 << 30) + 0.5); // Fs / 19.2 MHz in Q30
        if (!mem_write(phys(g_block), h, sizeof h))
            return false;
        u32 zero = 0;
        mem_write(phys(g_block) + 4 * CB_STOP, &zero, 4);
        if (!dsp_call("mcdma", g_block, 0, 0, 0, true))
            return false;
        for (int i = 0; i < 100 && log_word(CB_STATUS) != CB_RUNNING; i++)
            msleep(20);
        if (log_word(CB_STATUS) != CB_RUNNING) {
            logf_("copy program did not start (status %08x)", log_word(CB_STATUS));
            return false;
        }
        g_running = true;
        save_block();
    }
serve:
    logf_("copy program running after %.2f s", now_s() - t0);
    stop_server();
    if (!spawn_server())
        return false;
    return true;
}
static void stop_stream()
{
    stop_server();
    if (g_running) {
        u32 one = 1;
        mem_write(phys(g_block) + 4 * CB_STOP, &one, 4);
        wait_done(5.0);
        g_running = false;
    }
    // the sandbox keeps its short period for a while: the next start is then immediate
    g_idle_since = now_s();
}

// ---------------- control server
static void reply(int fd, const char* f, ...)
{
    char    b[512];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(b, sizeof b - 1, f, ap);
    va_end(ap);
    b[n++] = '\n';
    if (write(fd, b, n) < 0) {
    }
}
static void handle(int fd, char* line)
{
    char* sp = strchr(line, ' ');
    if (sp)
        *sp++ = 0;
    pthread_mutex_lock(&g_lock);
    if (!strcmp(line, "HELLO")) {
        const Band* b = band_of_earfcn(g_want_earfcn);
        reply(fd,
              "OK sdrd 4 fs=%.0f fc=%.0f port=%d rec=%u",
              g_cur_fs,
              g_want_free_hz > 0 ? g_want_free_hz : (b ? earfcn_to_hz(b, g_want_earfcn) : 0.0),
              g_data_port,
              REC_BYTES);
    } else if (!strcmp(line, "RATE")) {
        double fs = sp ? atof(sp) : 0.0;
        if (fs <= 0)
            reply(fd, "ERR usage: RATE <Hz>");
        else {
            RatePlan P = plan_rate(fs, g_fs_have ? g_fs_orig[5] / 4294967296.0 : 0xDFFF4720u / 4294967296.0);
            g_want_fs  = P.actual;
            reply(fd, "OK rate=%.0f mode=%s clamped=%d", P.actual, RATE_MODES[P.mode].name, P.clamped ? 1 : 0);
        }
    } else if (!strcmp(line, "BANDS")) {
        char b[512];
        int  n = snprintf(b, sizeof b, "OK");
        for (int i = 0; i < NBANDS; i++)
            n += snprintf(b + n,
                          sizeof b - n,
                          " %d:%.1f-%.1f",
                          BANDS[i].num,
                          BANDS[i].dl_low_mhz,
                          earfcn_to_hz(&BANDS[i], (u32)BANDS[i].e_hi) / 1e6);
        for (int i = 0; g_nfam && i < NFREE; i++)
            n += snprintf(b + n, sizeof b - n, " 0:%.1f-%.1f", FREE[i].lo_mhz, FREE[i].hi_mhz);
        reply(fd, "%s", b);
    } else if (!strcmp(line, "TUNE")) {
        u32              e  = 0;
        double           hz = sp ? atof(sp) : 0.0;
        double           lo = 0;
        const FreeRange* R  = 0;
        bool             ok = false;
        ok                  = choose_route(hz, &e, &lo, &R);
        if (!ok)
            reply(fd, "ERR %.3f MHz cannot be tuned (see BANDS)", hz / 1e6);
        else if (!R) {
            g_want_earfcn  = e;
            g_want_free_hz = 0;
            g_want_range   = 0;
            const Band* b  = band_of_earfcn(e);
            reply(fd, "OK lo=%.0f earfcn=%u band=%d", earfcn_to_hz(b, e), e, b->num);
        } else {
            g_want_free_hz = lo;
            g_want_range   = R;
            reply(fd, "OK lo=%.0f earfcn=0 band=0", lo);
        }
    } else if (!strcmp(line, "GAIN") || !strcmp(line, "BW")) {
        bool gain = line[0] == 'G';
        int  v    = -1;
        if (sp) {
            if (gain) {
                v = atoi(sp);
                if (v < 0 || v > 5)
                    v = -1;
            } else {
                double m = atof(sp);
                for (int i = 0; i < 7; i++)
                    if (m > BW_MHZ[i] - 0.01 && m < BW_MHZ[i] + 0.01)
                        v = i;
            }
        }
        if (v < 0)
            reply(fd, gain ? "ERR gain state is 0..5" : "ERR bandwidth is one of 1.4 3 5 10 15 20 40 (MHz)");
        else {
            if (gain)
                g_want_gain = v;
            else
                g_want_bw = v;
            bool ok = true;
            if (g_cur_band)
                ok = apply_frontend(g_cur_band); // otherwise it is applied by START
            if (!ok)
                reply(fd, "ERR the chip's gain table for this channel is not available");
            else if (gain)
                reply(fd, "OK gain=%d", g_want_gain);
            else
                reply(fd, "OK bw=%s", BW_NAME[g_want_bw]);
        }
    } else if (!strcmp(line, "START")) {
        if (!ensure_firmware())
            reply(fd, "ERR custom firmware is not running and could not be loaded (see the service log)");
        else if (start_stream()) {
            const Band* b = band_of_earfcn(g_ftm_earfcn);
            reply(fd,
                  "OK started fc=%.0f port=%d band=%d rate=%.0f",
                  g_cur_free_hz > 0 ? g_cur_free_hz : earfcn_to_hz(b, g_ftm_earfcn),
                  g_data_port,
                  g_cur_band,
                  g_cur_fs);
        } else
            reply(fd, "ERR start failed (see the service log)");
    } else if (!strcmp(line, "STOP")) {
        stop_stream();
        reply(fd, "OK stopped");
    } else if (!strcmp(line, "STATUS")) {
        reap_server();
        bool        ok = g_block && g_running;
        const Band* b  = g_cur_free_hz > 0 ? 0 : band_of_earfcn(g_ftm_earfcn);
        reply(fd,
              "OK state=%s rate=%.0f band=%d earfcn=%u fc=%.0f gain=%d bw=%s port=%s records=%u late=%u slips=%u "
              "clients=%d",
              ok ? "running" : "idle",
              g_cur_fs,
              g_cur_band,
              g_cur_free_hz > 0 ? 0u : g_ftm_earfcn,
              g_cur_free_hz > 0 ? g_cur_free_hz : (b ? earfcn_to_hz(b, g_ftm_earfcn) : 0.0),
              g_cur_gain,
              g_cur_bw >= 0 ? BW_NAME[g_cur_bw] : "?",
              g_cur_port[0] ? g_cur_port : "?",
              ok ? log_word(CB_RECORDS_COPIED) : 0,
              ok ? log_word(CB_LATE) : 0,
              ok ? log_word(CB_SLIPS) : 0,
              g_server > 0 ? 1 : 0);
    } else
        reply(fd, "ERR unknown command");
    pthread_mutex_unlock(&g_lock);
}
static void* client_thread(void* arg)
{
    int    fd = (int)(intptr_t)arg;
    char   buf[256];
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, buf + n, sizeof buf - 1 - n);
        if (r <= 0)
            break;
        n += r;
        char* nl;
        while ((nl = (char*)memchr(buf, '\n', n))) {
            *nl      = 0;
            size_t l = nl - buf;
            if (l && buf[l - 1] == '\r')
                buf[l - 1] = 0;
            handle(fd, buf);
            memmove(buf, nl + 1, n - (l + 1));
            n -= l + 1;
        }
        if (n >= sizeof buf - 1)
            break;
    }
    close(fd);
    return 0;
}
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--blobs") && i + 1 < argc)
            g_blobs_path = argv[++i];
        else if (!strcmp(argv[i], "--luts") && i + 1 < argc)
            g_luts_path = argv[++i];
        else if (!strcmp(argv[i], "--tune") && i + 1 < argc)
            g_tune_path = argv[++i];
        else if (!strcmp(argv[i], "--sample-server") && i + 1 < argc)
            g_server_path = argv[++i];
        else if (!strcmp(argv[i], "--control-port") && i + 1 < argc)
            g_ctl_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--queue-mb") && i + 1 < argc)
            g_queue_mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle-stock") && i + 1 < argc)
            g_idle_stock = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sample-port") && i + 1 < argc)
            g_data_port = atoi(argv[++i]);
        else {
            fprintf(stderr,
                    "usage: sdrd [--blobs FILE] [--luts FILE] [--sample-server FILE] [--control-port N] [--sample-port "
                    "N] [--idle-stock SECONDS] [--queue-mb N]\n");
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    // no SA_RESTART: accept() returns
    {
        struct sigaction sa = {};
        sa.sa_handler       = on_term;
        sigaction(SIGTERM, &sa, 0);
        sigaction(SIGINT, &sa, 0);
    }
    if (!load_blobs())
        return 1;
    load_luts();
    load_tune();
    int ls  = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a     = {};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port        = htons((uint16_t)g_ctl_port);
    if (bind(ls, (sockaddr*)&a, sizeof a) || listen(ls, 4)) {
        perror("bind");
        return 1;
    }
    g_data_ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(g_data_ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in d     = {};
    d.sin_family      = AF_INET;
    d.sin_addr.s_addr = htonl(INADDR_ANY);
    d.sin_port        = htons((uint16_t)g_data_port);
    if (bind(g_data_ls, (sockaddr*)&d, sizeof d) || listen(g_data_ls, 1)) {
        perror("bind sample port");
        return 1;
    }
    logf_("control port %d, sample port %d", g_ctl_port, g_data_port);
    while (!g_quit) {
        struct pollfd pf = { ls, POLLIN, 0 };
        if (poll(&pf, 1, 1000) <= 0) {
            pthread_mutex_lock(&g_lock);
            if (g_idle_stock > 0 && !g_running && g_gen_ok && g_idle_since > 0 && now_s() - g_idle_since > g_idle_stock)
                return_to_stock();
            reap_server();
            // The sender ends when its client leaves. While the copy program runs, a new one takes over, so a client
            // that reconnects (or sat in the listen queue while no sender existed) is served instead of waiting
            // forever.
            if (g_running && g_server == 0 && g_block && log_word(CB_STATUS) == CB_RUNNING)
                spawn_server();
            if (g_fast && !g_running && g_idle_since > 0 && now_s() - g_idle_since > 10.0 && sandbox_init())
                set_period(1000);
            pthread_mutex_unlock(&g_lock);
            continue;
        }
        int c = accept(ls, 0, 0);
        if (c < 0)
            continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        pthread_t      t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, client_thread, (void*)(intptr_t)c))
            close(c);
    }
    // leave the DSP idle when the service is stopped
    pthread_mutex_lock(&g_lock);
    stop_stream();
    if (g_fast && sandbox_init())
        set_period(1000);
    pthread_mutex_unlock(&g_lock);
    return 0;
}
