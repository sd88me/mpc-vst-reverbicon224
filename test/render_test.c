#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
extern "C" {
#include "engine.h"
}
int main(int argc, char **argv) {
    const mpc_engine_t *e = mpc_engine();
    void *f = e->create(argv[1]);
    char b[32];
    e->get_param(f, "rom_status", b, sizeof b);
    printf("bank loaded: %s\n", b);
    for (int prog = 0; prog < 6; prog++) {
        snprintf(b, sizeof b, "%d", prog);
        e->set_param(f, "algorithm", b);
        e->set_param(f, "mix", "100");
        int16_t in[256], out[256];
        double e0 = 0, e1 = 0, e2 = 0, peak = 0; int bad = 0;
        double t0 = 0;
        struct timespec a, c;
        clock_gettime(CLOCK_MONOTONIC, &a);
        int blocks = 44100 * 6 / 128;
        for (int blk = 0; blk < blocks; blk++) {
            memset(in, 0, sizeof in);
            if (blk == 20) in[0] = in[1] = 20000;
            e->process(f, in, out, 128);
            for (int i = 0; i < 256; i++) {
                double v = out[i];
                if (blk < 20) e0 += v * v;
                else if (blk < 120) e1 += v * v;
                else if (blk > 300) e2 += v * v;
                if (fabs(v) > peak) peak = fabs(v);
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &c);
        t0 = (c.tv_sec - a.tv_sec) + (c.tv_nsec - a.tv_nsec) * 1e-9;
        printf("prog %d: pre-impulse %.0f  early tail %.3g  late tail %.3g  peak %.0f  cpu %.2f%%\n", prog, e0, e1, e2, peak, 100 * t0 / 6.0);
    }
    e->destroy(f);
    return 0;
}
