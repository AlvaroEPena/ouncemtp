/*
 * OunceMTP - USB file transfer for Switch 2 homebrew (kaiHEN / libounce).
 * SPDX-License-Identifier: ISC
 *
 * Threads:
 *   main   - applet loop, input, console UI redraw at ~10 Hz
 *   worker - MTP responder (blocking USB + SD I/O), see mtp.c
 * They share only the status block (status.c) and the stop flag.
 */
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <switch.h>

#include "mtp.h"
#include "status.h"
#include "transport.h"

#define APP_VERSION "1.0.0"
#define W 78   /* usable console columns (80 minus margins) */
#define ROWS 45   /* console rows (1-based; the last one must not end in \n) */
#define CREDIT "made by @venustrikes"

static volatile int g_stop;
static Thread       g_worker;

/* ---- ANSI helpers ----------------------------------------------------------
 * Bright colors are written as bold + a normal color ("1;3Xm"), not with the
 * 90-97 codes. libounce's (and libnx's) console renderer ignores the bright
 * flag that 90-97 set and only maps *bold* to the bright palette, so "\x1b[90m"
 * (gray) was drawn black on black: every dim line, the error text included,
 * was invisible. Each code starts with 0 so no attribute leaks between them. */
#define C_RST  "\x1b[0m"
#define C_DIM  "\x1b[0;1;30m"   /* gray                                      */
#define C_HI   "\x1b[0;1;37m"
#define C_CYAN "\x1b[0;1;36m"
#define C_GRN  "\x1b[0;1;32m"
#define C_YEL  "\x1b[0;1;33m"
#define C_RED  "\x1b[0;1;31m"
#define C_MAG  "\x1b[0;1;35m"
#define EOL    "\x1b[K\n"   /* clear rest of line: we redraw in place, no flicker */

/* The console font is 8-bit ASCII; collapse each UTF-8 sequence to '?' and
 * truncate with an ellipsis so long names never wrap the layout. */
static void ascii_name(const char *in, char *out, size_t max_cols)
{
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)in; *s && o < max_cols; ) {
        if (*s < 0x80) { out[o++] = (char)(*s >= 0x20 ? *s : '?'); s++; }
        else { out[o++] = '?'; s++; while ((*s & 0xC0) == 0x80) s++; }
    }
    if (in[0] && o == max_cols && strlen(in) > max_cols) { out[o - 3] = out[o - 2] = out[o - 1] = '.'; }
    out[o] = 0;
}

static void human(uint64_t n, char *out, size_t sz)
{
    if (n >= (1ull << 30))      snprintf(out, sz, "%.2f GiB", (double)n / (1ull << 30));
    else if (n >= (1ull << 20)) snprintf(out, sz, "%.1f MiB", (double)n / (1ull << 20));
    else if (n >= 1024)         snprintf(out, sz, "%.1f KiB", (double)n / 1024);
    else                        snprintf(out, sz, "%u B", (unsigned)n);
}

/* Length of the first line when `s` is wrapped at `cols`: at the last space
 * that keeps the line at least half full, else a hard cut. */
static int wrap_at(const char *s, int cols)
{
    int n = (int)strlen(s);
    if (n <= cols) return n;
    for (int i = cols; i > cols / 2; i--)
        if (s[i] == ' ') return i;
    return cols;
}

static void bar(double frac, int width, char *out)
{
    int fill = (int)(frac * width + 0.5);
    if (fill > width) fill = width;
    for (int i = 0; i < width; i++) out[i] = i < fill ? '#' : '.';
    out[width] = 0;
}

/* ---- rendering ---------------------------------------------------------------- */

static uint64_t g_sd_free, g_sd_total, g_sd_checked_ms;

static void refresh_sd_space(void)
{
    uint64_t now = status_now_ms();
    if (g_sd_checked_ms && now - g_sd_checked_ms < 3000) return;
    g_sd_checked_ms = now;
    struct statvfs v;
    if (statvfs("sdmc:/", &v) == 0) {
        g_sd_total = (uint64_t)v.f_blocks * v.f_frsize;
        g_sd_free  = (uint64_t)v.f_bavail * v.f_frsize;
    }
}

static void draw(const app_status_t *st)
{
    char a[64], b[64], c[64], nm[W + 1], pb[64];

    printf("\x1b[1;1H");
    printf(" " C_CYAN "OunceMTP" C_RST " " C_DIM "v" APP_VERSION C_RST
           "   USB file transfer for Switch 2" "                " C_DIM "[+] Exit" C_RST EOL);
    printf(" " C_DIM "------------------------------------------------------------------------------" C_RST EOL);

    /* status line + hint */
    const char *dot, *label, *hint;
    switch (st->state) {
        case ST_STARTING:      dot = C_DIM;  label = "Starting...";            hint = ""; break;
        case ST_WAITING_CABLE: dot = C_YEL;  label = "Waiting for a PC";       hint = "Connect a USB-C *data* cable from the console to your computer."; break;
        case ST_CONNECTED:     dot = C_GRN;  label = "Connected";              hint = "Open \"Nintendo Switch 2\" in your file manager to browse the SD card."; break;
        case ST_SESSION:       dot = C_GRN;  label = "Ready";                  hint = "Drag files in and out. Keep this app open while transferring."; break;
        case ST_TRANSFER:      dot = C_MAG;  label = "Transferring";           hint = "Keep this app open while transferring."; break;
        default:               dot = C_RED;  label = "Error";                  hint = st->error; break;
    }
    printf(EOL);
    printf("  Status    %s*" C_RST " " C_HI "%s" C_RST EOL, dot, label);
    /* Two lines for the hint so a whole error message fits; errors in yellow. */
    const char *hint_col = st->state == ST_ERROR ? C_YEL : C_DIM;
    const int cols = W - 12;
    int cut = wrap_at(hint, cols);
    const char *rest = hint + cut;
    while (*rest == ' ') rest++;
    printf("            %s%.*s" C_RST EOL, hint_col, cut, hint);
    printf("            %s%.*s" C_RST EOL, hint_col, cols, rest);

    if (st->usb_speed_mps)
        printf("  Link      %s" EOL, st->usb_speed_mps >= 1024 ? "USB 3 SuperSpeed" : st->usb_speed_mps >= 512 ? "USB 2 High-Speed" : "USB 1 Full-Speed (slow cable/port?)");
    else
        printf("  Link      " C_DIM "-" C_RST EOL);

    human(g_sd_free, a, sizeof(a));
    human(g_sd_total, b, sizeof(b));
    printf("  SD card   %s free of %s" EOL, a, b);
    printf(EOL);

    /* active transfer */
    if (st->state == ST_TRANSFER) {
        const char *dir = st->dir == DIR_TO_PC ? C_CYAN "Switch -> PC" : st->dir == DIR_TO_SWITCH ? C_GRN "PC -> Switch" : C_YEL "Copy on SD ";
        ascii_name(st->name, nm, W - 26);
        printf("  Transfer  %s" C_RST "  " C_HI "%s" C_RST EOL, dir, nm);

        uint64_t el = status_now_ms() - st->start_ms;
        double rate = el > 0 ? (double)st->done * 1000.0 / (double)el : 0;
        human(st->done, a, sizeof(a));
        human((uint64_t)rate, c, sizeof(c));
        if (st->total != UINT64_MAX && st->total > 0) {
            double frac = (double)st->done / (double)st->total;
            human(st->total, b, sizeof(b));
            bar(frac, 30, pb);
            uint64_t eta = rate > 1 ? (uint64_t)((double)(st->total - (st->done < st->total ? st->done : st->total)) / rate) : 0;
            printf("            [%s] %3d%%" EOL, pb, (int)(frac * 100.0));
            printf("            %s / %s   %s/s   ETA %llu:%02llu" EOL, a, b, c,
                   (unsigned long long)(eta / 60), (unsigned long long)(eta % 60));
        } else {
            printf("            [%s]" EOL, "..............................");
            printf("            %s   %s/s" EOL, a, c);
        }
    } else {
        printf("  Transfer  " C_DIM "idle" C_RST EOL EOL EOL);
    }
    printf(EOL);

    human(st->bytes_to_pc, a, sizeof(a));
    human(st->bytes_to_switch, b, sizeof(b));
    /* One line per direction: a single combined line overflows 80 columns
     * (and wraps the whole layout) once the counts get large. */
    printf("  To PC     " C_CYAN "%u" C_RST " file(s), %s" EOL, st->files_to_pc, a);
    printf("  To Switch " C_GRN "%u" C_RST " file(s), %s" EOL, st->files_to_switch, b);
    printf(EOL);

    printf("  " C_DIM "Recent activity" C_RST EOL);
    uint32_t first = st->log_head > STATUS_LOG_LINES ? st->log_head - STATUS_LOG_LINES : 0;
    int lines = 0;
    for (uint32_t i = st->log_head; i > first; i--, lines++) {   /* newest first */
        ascii_name(st->log[(i - 1) % STATUS_LOG_LINES], nm, W - 4);
        printf("    %s%s" C_RST EOL, nm[0] == '!' ? C_RED : (lines == 0 ? C_HI : C_DIM), nm);
    }
    for (; lines < STATUS_LOG_LINES; lines++) printf(EOL);

    printf(EOL);
    printf("  " C_DIM "Windows: This PC > Nintendo Switch 2   macOS: OpenMTP   Linux: file manager" C_RST EOL);
    printf("\x1b[J");   /* clear anything below */

    /* Credit, centered on the last row. No newline after it: a newline on the
     * last row scrolls the whole console up. */
    printf("\x1b[%d;%dH" C_DIM "made by " C_CYAN "@venustrikes" C_RST,
           ROWS, (80 - (int)(sizeof(CREDIT) - 1)) / 2 + 1);
}

/* ---- worker ------------------------------------------------------------------- */

static void worker_main(void *arg)
{
    (void)arg;
    mtp_run();
}

static void read_serial(char *out, size_t sz)
{
    snprintf(out, sz, "OUNCEMTP0001");
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysSerialNumber sn;
        if (R_SUCCEEDED(setsysGetSerialNumber(&sn)) && sn.number[0])
            snprintf(out, sz, "%s", sn.number);
        setsysExit();
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    status_init();
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    /* No dimming / auto-sleep mid-transfer: sleep would drop the USB link. */
    appletSetMediaPlaybackState(true);

    char serial[32];
    read_serial(serial, sizeof(serial));
    tp_set_serial(serial);
    tp_set_stop_flag(&g_stop);

    static const char version[] = "OunceMTP " APP_VERSION;
    mtp_config_t cfg = {
        .root_path     = "sdmc:/",
        .storage_desc  = "SD Card",
        .manufacturer  = "Nintendo",
        .model         = "Switch 2",
        .friendly_name = "Nintendo Switch 2",
        .version       = version,
        .serial        = serial,
        .stop          = &g_stop,
    };

    /* Every failure names its step and the Result (as the 2XXX-YYYY code the
     * system shows, plus raw hex), so a report from the screen pins it down. */
    bool worker_running = false;
    Result trc = 0;
    if (!mtp_init(&cfg)) {
        status_set_error("Out of memory allocating the 2 MiB of transfer buffers.");
    } else if (tp_init() != TP_OK) {
        u32 rc = tp_last_result();
        status_set_error("USB setup failed at \"%s\": error %04u-%04u (0x%X). Details under Recent activity.",
                         tp_last_step(), 2000 + R_MODULE(rc), R_DESCRIPTION(rc), rc);
        status_log("!! USB %s: %04u-%04u (0x%X)", tp_last_step(), 2000 + R_MODULE(rc), R_DESCRIPTION(rc), rc);
    } else if (R_FAILED(trc = threadCreate(&g_worker, worker_main, NULL, NULL, 0x20000, 0x2B, -2)) ||
               R_FAILED(trc = threadStart(&g_worker))) {
        status_set_error("Could not start the transfer thread: error %04u-%04u (0x%X).",
                         2000 + R_MODULE(trc), R_DESCRIPTION(trc), trc);
        tp_exit();
    } else {
        worker_running = true;
    }

    printf("\x1b[2J");
    uint64_t last_draw = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;

        uint64_t now = status_now_ms();
        if (now - last_draw >= 100) {          /* 10 Hz is plenty for a status screen */
            last_draw = now;
            refresh_sd_space();
            app_status_t st;
            status_snapshot(&st);
            draw(&st);
            consoleUpdate(NULL);
        }
        svcSleepThread(16000000ull);
    }

    /* Shutdown: the worker polls g_stop every 100 ms inside every USB wait. */
    g_stop = 1;
    if (worker_running) {
        printf("\x1b[2J\x1b[1;1H  Closing USB connection...\n");
        consoleUpdate(NULL);
        threadWaitForExit(&g_worker);
        threadClose(&g_worker);
        tp_exit();
    }
    mtp_exit();
    appletSetMediaPlaybackState(false);
    consoleExit(NULL);
    return 0;
}
