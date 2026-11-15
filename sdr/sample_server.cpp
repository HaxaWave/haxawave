// SPDX-FileCopyrightText: 2026 Piotr Krysik
// SPDX-License-Identifier: GPL-3.0-only
// sample_server.cpp -- phone side: sends the sample records that the DSP copies into DDR to one TCP client.
//
//   sample_server (--port <port> | --listen-fd <fd>) --log <addr> --ring <addr> [--rec <bytes>] [-q <MB>]
//
// With --listen-fd the already listening socket is inherited from the parent (sdrd), so the port is open before this
// program starts. <addr> are physical addresses of the DSP control block (log) and of the record ring, as used by sdrd.
// The server waits for one client, then waits until the DSP program is running, then streams the records unchanged
// (record size 3232 bytes by default). Two threads: the reader copies new records from the (uncached) ring into a
// queue, the sender writes the queue to the socket. If the queue is full the reader drops records instead of blocking,
// because the DSP would overwrite the ring anyway. It exits when the client disconnects, when it is terminated, or when
// the DSP program ends.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// ---------------- the DSP control block (u32 words, see sdr/dsp/mcdma.c)
enum {
    CB_MAGIC          = 0,  // MAGIC_CHS2
    CB_OUT_RECORDS    = 2,  // records in the ring
    CB_STATUS         = 10, // STATUS_RUNNING, STATUS_DONE or an error
    CB_RECORDS_COPIED = 11, // records the DSP has copied so far; record k sits at slot k % ring size
};
static const uint32_t MAGIC_CHS2     = 0x32534843u; // control block signature written by sdrd
static const uint32_t STATUS_RUNNING = 1, STATUS_DONE = 2;

// ---------------- limits and timing
static const uint32_t MIN_RING_RECORDS = 8, MAX_RING_RECORDS = 4096;
static const uint32_t READ_CHUNK       = 300; // records copied into the queue at a time
static const uint32_t LAG_LIMIT_MARGIN = 30;  // behind by more than ring size - this: skip ahead to half a ring behind
static const uint32_t OVERWRITE_MARGIN = 5;   // the copied records may have been overwritten if the DSP got this close
static const double   DSP_WAIT_S       = 60;  // how long to wait for the DSP program to start
static const uint64_t SEND_MAX         = 1u << 20; // bytes per send()
static const int      SOCKET_SNDBUF    = 1 << 20;
static const long     SENDER_IDLE_US = 50, READER_IDLE_US = 100, DSP_POLL_US = 1000;

// ---------------- settings (command line)
static uint64_t g_log_pa = 0, g_ring_pa = 0; // physical addresses of the control block and of the ring
static uint32_t g_rec       = 3232;          // record size in bytes
static int      g_port      = 0;
static int      g_listen_fd = -1;
static int      g_queue_mb  = 32;

// ---------------- state
static int                g_mem_fd = -1; // /dev/mem
static long               g_page   = 4096;
static volatile uint32_t* g_cb;   // control block words
static const uint8_t*     g_ring; // the record ring (uncached mapping)
static uint32_t           g_ring_records;
static int                g_sock = -1;
static uint8_t*           g_qbuf;             // byte queue between the reader and the sender
static uint64_t           g_qcap;             // a multiple of the record size
static volatile uint64_t  g_q_head, g_q_tail; // head: written by the reader, tail: by the sender (byte counters)
static volatile int       g_stop, g_done;
static uint64_t           st_read, st_lost, st_bad, st_drop, st_sent;

typedef uint8_t v16 __attribute__((vector_size(16), aligned(16)));

// Copy from the uncached mapping, 64 bytes per iteration (four 16-byte loads); plain memcpy for the remainder.
static void copy_nc(uint8_t* dst, const uint8_t* src, size_t n)
{
    if ((((uintptr_t)dst | (uintptr_t)src) & 15) == 0) {
        while (n >= 64) {
            const v16* s = (const v16*)src;
            v16*       d = (v16*)dst;
            v16        a = s[0], b = s[1], c = s[2], e = s[3];
            d[0] = a;
            d[1] = b;
            d[2] = c;
            d[3] = e;
            src += 64;
            dst += 64;
            n -= 64;
        }
    }
    if (n)
        memcpy(dst, src, n);
}

static inline uint32_t cb_word(int i)
{
    return __atomic_load_n(&g_cb[i], __ATOMIC_ACQUIRE);
}

static double now()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void nap_us(long us)
{
    timespec t;
    t.tv_sec  = 0;
    t.tv_nsec = us * 1000;
    nanosleep(&t, 0);
}

static void on_sig(int)
{
    g_stop = 1;
}

static bool parse_args(int argc, char** argv)
{
    if (argc < 2)
        return false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--listen-fd") && i + 1 < argc)
            g_listen_fd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            g_log_pa = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--ring") && i + 1 < argc)
            g_ring_pa = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--rec") && i + 1 < argc)
            g_rec = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q") && i + 1 < argc)
            g_queue_mb = atoi(argv[++i]);
        else
            return false;
    }
    return (g_port || g_listen_fd >= 0) && g_log_pa && g_ring_pa && g_rec >= 64 && g_rec <= 65536 && g_queue_mb >= 1 &&
           g_queue_mb <= 256;
}

// Maps `len` bytes of physical memory at `pa` (read only); returns the address of `pa` or 0.
static const uint8_t* map_phys(uint64_t pa, size_t len, const char* what)
{
    uint64_t base = pa & ~(uint64_t)(g_page - 1);
    size_t   size = ((size_t)(pa - base) + len + (size_t)g_page - 1) & ~(size_t)(g_page - 1);
    void*    m    = mmap(0, size, PROT_READ, MAP_SHARED, g_mem_fd, (off_t)base);
    if (m == MAP_FAILED) {
        perror(what);
        return 0;
    }
    return (const uint8_t*)m + (pa - base);
}

// Waits for the client (on the inherited socket or on our own one). Returns the connected socket, -1 when accept()
// was interrupted (no client: not an error), or -2 when the port cannot be opened.
static int accept_client()
{
    int ls = g_listen_fd, one = 1;
    if (ls < 0) {
        ls = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port        = htons((uint16_t)g_port);
        if (bind(ls, (sockaddr*)&a, sizeof a) || listen(ls, 1)) {
            perror("bind/listen");
            return -2;
        }
    }
    fprintf(stderr, "sample_server: waiting for a client\n");
    int s = accept(ls, 0, 0);
    if (g_listen_fd < 0)
        close(ls);
    if (s < 0)
        return -1;
    int sb = SOCKET_SNDBUF;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
    return s;
}

// Waits until the DSP copy program runs; then the control block holds the ring size.
static bool wait_for_dsp()
{
    double t0 = now();
    while (!(cb_word(CB_MAGIC) == MAGIC_CHS2 && cb_word(CB_STATUS) == STATUS_RUNNING)) {
        if (g_stop || now() - t0 > DSP_WAIT_S) {
            fprintf(stderr, "sample_server: the DSP program is not running\n");
            return false;
        }
        nap_us(DSP_POLL_US);
    }
    g_ring_records = cb_word(CB_OUT_RECORDS);
    if (g_ring_records < MIN_RING_RECORDS || g_ring_records > MAX_RING_RECORDS) {
        fprintf(stderr, "sample_server: bad ring size %u\n", g_ring_records);
        return false;
    }
    return true;
}

// Sender thread: writes the queue to the socket.
static void* sender(void*)
{
    for (;;) {
        uint64_t t = g_q_tail, h = __atomic_load_n(&g_q_head, __ATOMIC_ACQUIRE);
        if (t == h) {
            if (g_done)
                break;
            nap_us(SENDER_IDLE_US);
            continue;
        }
        uint64_t pos = t % g_qcap, len = h - t;
        if (len > g_qcap - pos)
            len = g_qcap - pos; // up to the end of the buffer, then wrap
        if (len > SEND_MAX)
            len = SEND_MAX;
        ssize_t k = send(g_sock, g_qbuf + pos, (size_t)len, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            g_stop = 1; // client gone
            return 0;
        }
        st_sent += (uint64_t)k;
        __atomic_store_n(&g_q_tail, t + (uint64_t)k, __ATOMIC_RELEASE);
    }
    return 0;
}

// Reader loop: copies the records the DSP has finished from the ring into the queue, in ring order.
static void read_records()
{
    const uint32_t M = g_ring_records, S = g_rec;
    uint32_t       k = cb_word(CB_RECORDS_COPIED); // next record to read
    while (!g_stop) {
        uint32_t done = cb_word(CB_RECORDS_COPIED);
        int32_t  lag  = (int32_t)(done - k);
        if (lag <= 0) {
            if (cb_word(CB_STATUS) == STATUS_DONE && cb_word(CB_RECORDS_COPIED) == k)
                break; // the DSP program has finished
            nap_us(READER_IDLE_US);
            continue;
        }
        if ((uint32_t)lag > M - LAG_LIMIT_MARGIN) { // too far behind: the ring has been overwritten
            uint32_t skip = (uint32_t)lag - M / 2;
            st_lost += skip;
            k += skip;
            continue;
        }
        uint32_t n = (uint32_t)lag < READ_CHUNK ? (uint32_t)lag : READ_CHUNK, i = k % M;
        if (n > M - i)
            n = M - i; // do not run over the end of the ring
        uint64_t h = g_q_head, t = __atomic_load_n(&g_q_tail, __ATOMIC_ACQUIRE);
        if (h - t + (uint64_t)n * S > g_qcap) { // queue full: drop, do not block
            st_drop += n;
            k += n;
            continue;
        }
        uint64_t pos = h % g_qcap;
        if (g_qcap - pos < (uint64_t)n * S)
            n = (uint32_t)((g_qcap - pos) / S); // do not split a record at the wrap
        uint8_t* dst = g_qbuf + pos;
        copy_nc(dst, g_ring + (size_t)i * S, (size_t)n * S);
        if ((uint32_t)(cb_word(CB_RECORDS_COPIED) - k) >= M - OVERWRITE_MARGIN) { // overwritten while copying
            st_lost += n;
            k += n;
            continue;
        }
        uint32_t s0, s1; // sequence numbers of the first and the last record copied
        memcpy(&s0, dst, 4);
        memcpy(&s1, dst + (size_t)(n - 1) * S, 4);
        if (s0 != k || s1 != k + n - 1)
            st_bad++;
        st_read += (uint64_t)n * S;
        __atomic_store_n(&g_q_head, h + (uint64_t)n * S, __ATOMIC_RELEASE);
        k += n;
    }
}

int main(int argc, char** argv)
{
    if (!parse_args(argc, argv)) {
        fprintf(stderr,
                "usage: sample_server (--port <port> | --listen-fd <fd>) --log <addr> --ring <addr> "
                "[--rec <bytes>] [-q <MB>]\n");
        return 2;
    }

    struct sigaction sa; // no SA_RESTART: accept() returns on a signal
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, 0);
    sigaction(SIGTERM, &sa, 0);
    signal(SIGPIPE, SIG_IGN);

    g_mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (g_mem_fd < 0) {
        perror("/dev/mem");
        return 1;
    }
    g_page = sysconf(_SC_PAGESIZE);
    g_cb   = (volatile uint32_t*)map_phys(g_log_pa, (size_t)g_page - (g_log_pa & (g_page - 1)), "mmap log");
    if (!g_cb)
        return 1;

    // wait for the client first, so that no records are lost while it connects
    g_sock = accept_client();
    if (g_sock < 0)
        return g_sock == -2 ? 1 : 0;
    if (!wait_for_dsp())
        return 2;
    g_ring = map_phys(g_ring_pa, (size_t)g_ring_records * g_rec, "mmap ring");
    if (!g_ring)
        return 1;

    g_qcap = ((uint64_t)g_queue_mb << 20) / g_rec * g_rec;
    g_qbuf = (uint8_t*)aligned_alloc(64, (size_t)g_qcap);
    if (!g_qbuf) {
        fprintf(stderr, "sample_server: out of memory\n");
        return 1;
    }
    memset(g_qbuf, 0, (size_t)g_qcap);
    pthread_t th;
    pthread_create(&th, 0, sender, 0);

    double tstart = now();
    read_records();
    g_done = 1;
    pthread_join(th, 0);
    double dt = now() - tstart;
    fprintf(stderr,
            "sample_server: %.1f s, read %.1f MB (%.1f MB/s), sent %.1f MB, lost %llu, dropped %llu, bad %llu\n",
            dt,
            st_read / 1e6,
            st_read / 1e6 / dt,
            st_sent / 1e6,
            (unsigned long long)st_lost,
            (unsigned long long)st_drop,
            (unsigned long long)st_bad);
    return 0;
}
