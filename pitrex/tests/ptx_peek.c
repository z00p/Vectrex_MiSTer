/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/* ptx_peek.c - dump the PiTrex shared DDR block (header, status, input).
 * usage: ptx_peek [repeat] [interval_ms]       (needs root for /dev/mem) */
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int repeat = argc > 1 ? atoi(argv[1]) : 1, ms = argc > 2 ? atoi(argv[2]) : 500;
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	volatile uint64_t *w = mmap(0, 0x1000, PROT_READ, MAP_SHARED, fd, 0x34000000);
	if (w == MAP_FAILED) { perror("mmap"); return 1; }
	for (int r = 0; r < repeat; r++) {
		printf("--- sample %d\n", r);
		if ((uint32_t)w[11] == 0x43585450u)
			printf("core build: B%u\n", (unsigned)((w[11] >> 32) & 0xFF));
		else
			printf("core build: not reported (pre-B4 core or PiTrex Mode off)\n");
		for (int i = 0; i < 20; i++)
			printf("word %2d (0x%03x): %016" PRIx64 "\n", i, i * 8, w[i]);
		fflush(stdout);
		if (r + 1 < repeat) usleep(ms * 1000);
	}
	return 0;
}
