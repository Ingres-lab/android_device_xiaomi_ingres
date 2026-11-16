/*
 * SPDX-FileCopyrightText: Neo
 * SPDX-License-Identifier: Apache-2.0
 *
 * Prototype player for the AW86927 haptic driver's real-time "custom wave" mode.
 * Usage: neo_haptic_play <file.s8> [repeat] [gap_ms]
 * The file is raw signed 8-bit samples (24 kHz). Needs root (sysfs write + input device).
 */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CUSTOM_DATA_LEN 3

static int find_input(char *eventPath, size_t n, char *sysPath, size_t m) {
    for (int i = 0; i < 32; i++) {
        char p[64], name[64] = {0};
        snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int fd = open(p, O_RDWR);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0 && !strcmp(name, "aw8697_haptic")) {
            snprintf(eventPath, n, "%s", p);
            snprintf(sysPath, m, "/sys/class/input/input%d/device/custom_wave", i - 0);
            /* the event number is not the input number: resolve through the event node */
            char link[128], real[PATH_MAX];
            snprintf(link, sizeof(link), "/sys/class/input/event%d/device/device/custom_wave", i);
            if (realpath(link, real)) snprintf(sysPath, m, "%s", real);
            else snprintf(sysPath, m, "%s", link);
            close(fd);
            return 0;
        }
        close(fd);
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.s8 [repeat] [gap_ms]\n", argv[0]); return 2; }
    int repeat = argc > 2 ? atoi(argv[2]) : 1;
    int gap_ms = argc > 3 ? atoi(argv[3]) : 400;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open wave"); return 1; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    signed char *buf = malloc(len);
    if (fread(buf, 1, len, f) != (size_t)len) { perror("read"); return 1; }
    fclose(f);

    char ev[64], sys[PATH_MAX];
    if (find_input(ev, sizeof(ev), sys, sizeof(sys))) { fprintf(stderr, "aw8697_haptic not found\n"); return 1; }
    int fd = open(ev, O_RDWR);
    if (fd < 0) { perror("open input"); return 1; }

    /* custom wave id = effect_max (197 on ingres); read it back from sysfs */
    int id = 197;
    char info[256] = {0};
    int sfd = open(sys, O_RDONLY);
    if (sfd >= 0) { read(sfd, info, sizeof(info) - 1); close(sfd); char *p = strstr(info, "custom_wave_id="); if (p) id = atoi(p + 15); }

    for (int r = 0; r < repeat; r++) {
        short data[CUSTOM_DATA_LEN] = { (short)id, 0, 0 };
        struct ff_effect e;
        memset(&e, 0, sizeof(e));
        e.type = FF_PERIODIC;
        e.id = -1;
        e.u.periodic.waveform = FF_CUSTOM;
        e.u.periodic.magnitude = 0x7fff;
        e.u.periodic.custom_len = CUSTOM_DATA_LEN;
        e.u.periodic.custom_data = data;
        e.replay.length = 1000;
        if (ioctl(fd, EVIOCSFF, &e) < 0) { perror("EVIOCSFF"); return 1; }

        int w = open(sys, O_WRONLY);
        if (w < 0) { perror("open custom_wave"); return 1; }
        long off = 0;
        while (off < len) {              /* 512-byte periods; the last short write ends the stream */
            long n = len - off > 512 ? 512 : len - off;
            if (write(w, buf + off, n) < 0) { perror("write wave"); break; }
            off += n;
        }
        if (len % 512 == 0) write(w, buf, 0);   /* exact multiple: terminate with an empty write */
        close(w);

        struct input_event pe = { .type = EV_FF, .code = (unsigned short)e.id, .value = 1 };
        write(fd, &pe, sizeof(pe));
        usleep((len * 1000000L / 24000) + 60000);
        pe.value = 0; write(fd, &pe, sizeof(pe));
        ioctl(fd, EVIOCRMFF, e.id);
        usleep(gap_ms * 1000);
    }
    close(fd);
    free(buf);
    return 0;
}
