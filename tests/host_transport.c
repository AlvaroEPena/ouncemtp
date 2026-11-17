/*
 * Host-side transport for tests: emulates a USB bulk IN/OUT pair over a
 * Unix stream socket. Each USB *transfer* is one frame: [u32 len][bytes].
 *
 * Emulated semantics (the ones the MTP code depends on):
 *  - OUT: a posted read completes with min(posted, bytes left in the host's
 *    current transfer). A zero-length frame is a ZLP. Posted sizes must be a
 *    multiple of wMaxPacketSize (real controllers babble otherwise) - asserted.
 *  - IN: each device write is one frame; the Python host joins frames until
 *    it sees one that is not a multiple of wMaxPacketSize, exactly like a USB
 *    host waiting for a short packet. A missing ZLP therefore shows up as a
 *    merged response and fails the test.
 *  - Buffers must be 4 KiB aligned and only one read + one write may be in
 *    flight - asserted, since usb:ds enforces both.
 * SPDX-License-Identifier: ISC
 */
#include "../source/transport.h"

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int g_tp_fd = -1;
static uint32_t g_mps = 512;
static int g_eof;

static void    *g_rd_buf; static uint32_t g_rd_size; static int g_rd_posted;
static const void *g_wr_buf; static uint32_t g_wr_size; static int g_wr_posted;

static uint8_t *g_pend;     /* current host transfer being consumed */
static uint32_t g_pend_len, g_pend_off;
static int      g_have_pend;

static int read_full(void *p, size_t n, uint32_t timeout_ms)
{
    uint8_t *b = p;
    while (n) {
        struct pollfd pf = { g_tp_fd, POLLIN, 0 };
        int pr = poll(&pf, 1, timeout_ms ? (int)timeout_ms : -1);
        if (pr == 0) return TP_TIMEOUT;
        if (pr < 0) return TP_ERROR;
        ssize_t r = read(g_tp_fd, b, n);
        if (r <= 0) { g_eof = 1; return TP_DISCONNECTED; }
        b += r; n -= (size_t)r;
    }
    return TP_OK;
}

static int write_full(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        ssize_t r = write(g_tp_fd, b, n);
        if (r <= 0) { g_eof = 1; return TP_DISCONNECTED; }
        b += r; n -= (size_t)r;
    }
    return TP_OK;
}

int tp_init(void)
{
    const char *m = getenv("MTP_MPS");
    if (m) g_mps = (uint32_t)atoi(m);
    return TP_OK;
}
void tp_exit(void) { free(g_pend); }

int tp_wait_configured(uint32_t timeout_ms) { (void)timeout_ms; return g_eof ? TP_STOPPED : TP_OK; }
int tp_is_configured(void) { return !g_eof; }
uint32_t tp_max_packet(void) { return g_mps; }

int tp_read_begin(void *buf, uint32_t size)
{
    assert(!g_rd_posted && "two reads in flight");
    assert(((uintptr_t)buf & 0xFFF) == 0 && "unaligned read buffer");
    if (size % g_mps != 0) {
        fprintf(stderr, "FATAL: OUT post of %u bytes is not a multiple of %u\n", size, g_mps);
        abort();
    }
    g_rd_buf = buf; g_rd_size = size; g_rd_posted = 1;
    return TP_OK;
}

int tp_read_end(uint32_t *transferred, uint32_t timeout_ms)
{
    assert(g_rd_posted);
    g_rd_posted = 0;
    if (g_eof) return TP_DISCONNECTED;
    uint32_t n = 0;
    for (;;) {
        if (!g_have_pend) {
            uint8_t h[4];
            int r = read_full(h, 4, timeout_ms);
            if (r) return r;
            g_pend_len = (uint32_t)h[0] | h[1] << 8 | h[2] << 16 | (uint32_t)h[3] << 24;
            free(g_pend);
            g_pend = malloc(g_pend_len ? g_pend_len : 1);
            if (g_pend_len && (r = read_full(g_pend, g_pend_len, timeout_ms))) return r;
            g_pend_off = 0;
            g_have_pend = 1;
        }
        uint32_t left = g_pend_len - g_pend_off;
        uint32_t room = g_rd_size - n;
        uint32_t k = left < room ? left : room;
        memcpy((uint8_t *)g_rd_buf + n, g_pend + g_pend_off, k);
        g_pend_off += k;
        n += k;
        if (g_pend_off == g_pend_len) {
            g_have_pend = 0;
            /* The host's transfer ended in a short packet (or ZLP): the read
             * completes. If it ended on a packet boundary with no ZLP, a real
             * controller sees no end and keeps filling from the next transfer -
             * reproduce that, so an over-sized post shows up as a merge. */
            if (g_pend_len == 0 || g_pend_len % g_mps != 0) break;
        }
        if (n == g_rd_size) break;   /* posted buffer full */
    }
    *transferred = n;
    return TP_OK;
}

int tp_write_begin(const void *buf, uint32_t size)
{
    assert(!g_wr_posted && "two writes in flight");
    assert(((uintptr_t)buf & 0xFFF) == 0 && "unaligned write buffer");
    g_wr_buf = buf; g_wr_size = size; g_wr_posted = 1;
    return TP_OK;
}

int tp_write_end(uint32_t *transferred, uint32_t timeout_ms)
{
    (void)timeout_ms;
    assert(g_wr_posted);
    g_wr_posted = 0;
    uint8_t h[4] = { (uint8_t)g_wr_size, (uint8_t)(g_wr_size >> 8), (uint8_t)(g_wr_size >> 16), (uint8_t)(g_wr_size >> 24) };
    int r = write_full(h, 4);
    if (!r && g_wr_size) r = write_full(g_wr_buf, g_wr_size);
    *transferred = r ? 0 : g_wr_size;
    return r;
}

void tp_abort(void) { g_rd_posted = 0; g_wr_posted = 0; }
