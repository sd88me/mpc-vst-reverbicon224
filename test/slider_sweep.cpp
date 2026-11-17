// Sweeps every slider (and DIFFUSION) over raw 0..253 on the booted v4.4 firmware, per program, and prints stored(raw).
#include "../src/reverb224.hpp"
#include <cstdio>
#include <string>
int main(int argc, char **argv) {
    auto r = std::make_unique<r224::Reverb224>(48000.0);
    for (int chip = 1; chip <= 5; chip++) {
        uint8_t b[2048];
        FILE *f = std::fopen((std::string(argv[1]) + "/ROM" + std::to_string(chip) + " 2716.BIN").c_str(), "rb");
        if (!f || std::fread(b, 1, 2048, f) != 2048) return 2;
        std::fclose(f); r->load_rom(chip, b);
    }
    r->boot();
    auto &p = r->panel();
    const int progs = argc > 2 ? atoi(argv[2]) : 6;
    for (int prog = 1; prog <= progs; prog++) {
        p.load_program(prog); r->settle_panel();
        for (int s = 0; s < 7; s++) {
            std::printf("P%d S%d", prog, s);
            for (int v = 0; v <= 253; v++) {
                if (s == 6) p.set_diffusion(v); else p.set_slider(s, v);
                r->settle_panel(); r->machine().run(2048000 / 50);
                std::printf(" %d", s == 6 ? p.stored_diffusion() : p.stored(s));
            }
            std::printf("\n"); std::fflush(stdout);
        }
    }
}
