// SPDX-License-Identifier: GPL-2.0-or-later
/* Esc on the keyboard: back to the Vectrex menu.
 * Main holds the keyboard, so Linux programs cannot see it. The core counts the presses and writes the
 * count to the status block; this reads it five times a second, read-only, and leaves a file for
 * pitrexd, which starts the menu. pitrexd starts this program and it runs for as long as pitrexd does.
 * Env PTX_KEYWATCH_FILE, test hook: read the count from the first byte of that file instead. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PTX_DDR_BASE 0x34000000u
#define STATUS_MAGIC 0x43585450u          /* "PTXC", word at 0x58 */

static int ours(void)
{
	char b[32] = "";
	int f = open("/tmp/CORENAME", O_RDONLY);
	if (f < 0) return 0;
	int n = (int)read(f, b, sizeof b - 1);
	close(f);
	return n > 0 && !strncmp(b, "Vectrex-PiTrex", 14);
}

int main(void)
{
	const char *t = getenv("PITREXD_TMP"), *test = getenv("PTX_KEYWATCH_FILE");
	char flag[300];
	snprintf(flag, sizeof flag, "%s/pitrex_menu_key", t ? t : "/tmp");
	volatile uint8_t *b = NULL;
	if (!test) {
		int m = open("/dev/mem", O_RDONLY | O_SYNC);
		if (m < 0) { perror("ptx_keywatch: /dev/mem"); return 1; }
		void *p = mmap(0, 4096, PROT_READ, MAP_SHARED, m, PTX_DDR_BASE);
		if (p == MAP_FAILED) { perror("ptx_keywatch: mmap"); return 1; }
		b = p;
	}
	int last = -1;
	for (;;) {
		struct timespec ts = { 0, 200000000 };
		nanosleep(&ts, NULL);
		int now;
		if (test) {
			unsigned char c = 0;
			int f = open(test, O_RDONLY);
			if (f < 0) continue;
			if (read(f, &c, 1) != 1) c = 0;
			close(f);
			now = c;
		} else {
			if (!ours() || *(volatile uint32_t *)(b + 0x58) != STATUS_MAGIC) { last = -1; continue; }
			now = b[0x87];
		}
		if (last >= 0 && now != last) {
			int f = open(flag, O_WRONLY | O_CREAT, 0644);
			if (f >= 0) close(f);
		}
		last = now;
	}
}
