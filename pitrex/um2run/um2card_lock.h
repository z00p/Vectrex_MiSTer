/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UM2CARD_LOCK_H
#define UM2CARD_LOCK_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* Existing um2run statuses: 0 success, 1 input/setup error, 2 usage/runtime setup,
 * 3 unsupported image/backend error, 4 bus timeout, 5 frame timeout. */
#define UM2_CARD_IN_USE 6

static int um2card_lock(const char *card) {
    size_t n = strlen(card);
    char *path = malloc(n + sizeof ".lock");
    if (!path) { perror("um2run: card lock"); return 1; }
    memcpy(path, card, n);
    memcpy(path + n, ".lock", sizeof ".lock");
    int fd = open(path, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror(path); free(path); return 1; }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        int err = errno;
        if (err == EWOULDBLOCK || err == EAGAIN)
            fprintf(stderr, "um2run: card %s is in use (being rebuilt?)\n", card);
        else { errno = err; perror(path); }
        close(fd);
        free(path);
        return err == EWOULDBLOCK || err == EAGAIN ? UM2_CARD_IN_USE : 1;
    }
    free(path);
    return 0;                       /* fd stays open until the process exits */
}
#endif
