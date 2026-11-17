// The 224's own display text for every stored step of every slider, per program: the slider's select button is held (bank 2;
// with SHIFT for DIFFUSION) so the digits show it, as reflecticon-224's operator reads values (panel224_operator.hpp showing()),
// and decoded as its panel_display.hpp does. Prints "P<prog> S<slot> <stored> <text>" lines (slot 6 = DIFFUSION).
#include "../src/reverb224.hpp"
#include "../src/slider_tables224.hpp"
#include <cstdio>
#include <cstring>
#include <string>
using namespace r224;
static char seg(uint8_t p) {
    static const uint8_t pat[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f, 0x00, 0x40};
    static const char ch[] = "0123456789 -";
    for (int i = 0; i < 12; i++) if (pat[i] == p) return ch[i];
    return '?';
}
static std::string decode(const uint8_t *raw) {
    uint8_t d[9]; for (int i = 0; i < 9; i++) d[i] = uint8_t(~raw[i]);
    std::string t;
    for (int i : {2, 1, 0}) { t += seg(d[i] & 0x7f); if (d[i] & 0x80) t += '.'; }
    while (!t.empty() && t[0] == ' ') t.erase(0, 1);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    static const char *u[4] = {"SEC", "MS", "HZ", "KHZ"};
    for (int b = 0; b < 4; b++) if (d[5] >> b & 1) return t + " " + u[b];
    return t;
}
int main(int argc, char **argv) {
    auto r = std::make_unique<Reverb224>(44100.0);
    for (int c = 1; c <= 5; c++) { uint8_t b[2048]; FILE *f = fopen((std::string(argv[1]) + "/ROM" + std::to_string(c) + " 2716.BIN").c_str(), "rb"); if (!f || fread(b, 1, 2048, f) != 2048) return 2; fclose(f); r->load_rom(c, b); }
    r->boot();
    auto &p = r->panel(); auto &m = r->machine();
    auto run = [&](double s) { m.run(uint64_t(s * 2048000)); };
    for (int prog = 1; prog <= 6; prog++) {
        p.load_program(prog); r->settle_panel();
        for (int slot = 0; slot < 7; slot++) {
            const uint8_t *tab = slot == 6 ? kStoredDiffusion : slot == 5 ? kStoredPredelay[prog - 1] : kStored[slot];
            const int lo = tab[0], hi = tab[253];
            for (int v = lo; v <= hi; v++) {
                const unsigned raw = raw_for(tab, v);
                if (tab[raw] != v) continue;                       // a value no pot position stores
                if (slot == 6) p.set_diffusion(raw); else p.set_slider(slot, raw);
                r->settle_panel(); run(0.05);
                // show it: hold its select button (SHIFT too for DIFFUSION) until SHOWN picks it, let the digits settle
                const uint8_t want = slot == 6 ? 0x40 : uint8_t(1u << slot);
                if (slot == 6) { m.panel.switches[1] = uint8_t(~0x08); run(0.1); }
                m.panel.switches[2] = uint8_t(~(1u << (slot == 6 ? 4 : slot)));
                for (int k = 0; k < 50 && m.mem[0x3f44] != want; k++) run(0.01);
                run(0.15);
                const std::string text = m.mem[0x3f44] == want ? decode(m.panel.digits.data()) : std::string("?");
                m.panel.switches[2] = 0xff; m.panel.switches[1] = 0xff; run(0.1);
                if (m.mem[0x3f3f] & 0x08) { m.panel.switches[1] = uint8_t(~0x08); run(0.1); m.panel.switches[1] = 0xff; run(0.1); }   // SHIFT latch off
                std::printf("P%d S%d %d %s\n", prog, slot, v, text.c_str());
            }
            std::fflush(stdout);
        }
    }
}
