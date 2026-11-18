// SPDX-License-Identifier: GPL-3.0-only
// The U0U audio path built natively (cc), for counting instructions per sample the way Felucca's tests/cpu_baseline.txt does:
//   cc -O2 ... web/emu/bench_native.c -o bench && /usr/bin/time -l ./bench LEVEL SECONDS      (LEVEL 0 idle, 1 drums, 2 + effects, 3 + LFOs and master)
#include <stdio.h>
#include <stdlib.h>
#include "drumbox_web.c"
int main(int argc, char **argv)
{
    int level = argc > 1 ? atoi(argv[1]) : 3, sec = argc > 2 ? atoi(argv[2]) : 10, n;
    web_nor_erase();
    web_boot();
    web_render(1024);
    for (n = 0; n < 1000; n++) web_ms();                 /* (the boot screen's 900 ms) */
    if (level > 0) {
        web_test_heavy((uint32_t)level);
        dbx_seq_play(1);
    }
    for (n = 0; n < sec * 44100 / 1024; n++)
        web_render(1024);
    printf("%d s level %d: %u steps\n", sec, level, (unsigned)dbx_seq.steps_done);
    return 0;
}
