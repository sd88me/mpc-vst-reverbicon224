// The front-panel operator (src/panel224.hpp) on the booted v4.4 firmware: every program key loads its program, the six sliders and
// DIFFUSION reach the firmware's stored bytes, and Mode Enhancement / Decay Optimization toggle. Prints OK or the failures.
#include "../src/reverb224.hpp"
#include <cstdio>
#include <string>

int main(int argc, char **argv) {
    auto r = std::make_unique<r224::Reverb224>(48000.0);
    for (int chip = 1; chip <= 5; chip++) {
        uint8_t b[2048];
        FILE *f = std::fopen((std::string(argv[1]) + "/ROM" + std::to_string(chip) + " 2716.BIN").c_str(), "rb");
        if (!f || std::fread(b, 1, 2048, f) != 2048) return 2;
        std::fclose(f);
        r->load_rom(chip, b);
    }
    r->boot();
    auto &p = r->panel();
    int fails = 0;
    std::printf("after boot: PROGRAM %02x, sliders", p.program_byte());
    for (int s = 0; s < 6; s++) std::printf(" %u", p.stored(s));
    std::printf(", diffusion %u\n", p.stored_diffusion());
    for (int prog = 1; prog <= 6; prog++) {
        p.load_program(prog);
        r->settle_panel();
        const bool ok = (p.program_byte() & 0x3f) == (1 << (prog - 1));
        fails += !ok;
        std::printf("%s program %d: PROGRAM %02x, sliders", ok ? "ok  " : "FAIL", prog, p.program_byte());
        for (int s = 0; s < 6; s++) std::printf(" %u", p.stored(s));
        std::printf("\n");
    }
    auto wait = [&](double secs) { float z[480] = {}, o1[480], o2[480]; for (int i = 0; i < int(secs * 100); i++) r->process(z, z, o1, o2, 480); };
    static const char *names[6] = {"BASS", "MID", "CROSSOVER", "TREBLE DECAY", "DEPTH", "PRE-DELAY"};
    for (int s = 0; s < 6; s++) {                         // the firmware stores its own quantized value: check range and return
        int got[3];
        const unsigned seq[3] = {0, 253, 0};
        for (int k = 0; k < 3; k++) { p.set_slider(s, seq[k]); r->settle_panel(); wait(0.1); got[k] = p.stored(s); }
        const bool ok = got[1] != got[0] && got[2] == got[0];
        fails += !ok;
        std::printf("%s %-12s 0 -> %d, 253 -> %d, 0 -> %d\n", ok ? "ok  " : "FAIL", names[s], got[0], got[1], got[2]);
    }
    std::printf("sliders: done\n");
    p.set_diffusion(30); r->settle_panel();
    const int d1 = p.stored_diffusion();
    p.set_diffusion(220); r->settle_panel();
    const int d2 = p.stored_diffusion();
    fails += !(d2 > d1);
    std::printf("%s diffusion 30 -> %d, 220 -> %d\n", d2 > d1 ? "ok  " : "FAIL", d1, d2);
    for (int bit : {6, 7}) {
        for (bool on : {false, true, false}) {
            p.set_toggle(bit, on); r->settle_panel();
            const bool got = (p.program_byte() >> bit) & 1;
            fails += got != on;
            std::printf("%s PROGRAM bit %d -> %d (%02x)\n", got == on ? "ok  " : "FAIL", bit, on, p.program_byte());
        }
    }
    std::printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails != 0;
}
