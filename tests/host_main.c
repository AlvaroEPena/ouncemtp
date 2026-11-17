/*
 * Host test driver: serves MTP for `root` over a connected Unix socket.
 *   host_mtp <root-dir> <socket-path>
 * SPDX-License-Identifier: ISC
 */
#include "../source/mtp.h"
#include "../source/status.h"
#include "../source/transport.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern int g_tp_fd;

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s root socket\n", argv[0]); return 2; }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", argv[2]);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { perror("connect"); return 1; }
    g_tp_fd = fd;

    static volatile int stop = 0;
    mtp_config_t cfg = {
        .root_path = argv[1], .storage_desc = "SD Card", .manufacturer = "Nintendo",
        .model = "Switch 2", .friendly_name = "Nintendo Switch 2", .version = "test",
        .serial = "TEST0001", .stop = &stop,
    };
    status_init();
    tp_init();
    if (!mtp_init(&cfg)) { fprintf(stderr, "mtp_init failed\n"); return 1; }
    mtp_run();

    /* Dump the UI log so the test can check status reporting too. */
    app_status_t st;
    status_snapshot(&st);
    uint32_t first = st.log_head > STATUS_LOG_LINES ? st.log_head - STATUS_LOG_LINES : 0;
    for (uint32_t i = first; i < st.log_head; i++) fprintf(stderr, "LOG: %s\n", st.log[i % STATUS_LOG_LINES]);
    fprintf(stderr, "TOTALS: to_pc=%llu/%u to_switch=%llu/%u\n",
            (unsigned long long)st.bytes_to_pc, st.files_to_pc,
            (unsigned long long)st.bytes_to_switch, st.files_to_switch);
    mtp_exit();
    tp_exit();
    return 0;
}
