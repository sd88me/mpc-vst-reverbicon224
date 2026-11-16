// The DSP-port traffic of the v4.4 firmware on reflecticon-224's board-level host and on src/machine224.hpp, side by side: the first
// accesses that differ (pc, direction, port, value).     machine224_portdiff ROM_DIR [SECONDS]
#include "host.hpp"
#define R224_SBC_INCLUDED
#include "../src/machine224.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct Ev { uint16_t pc; bool write; unsigned port; uint8_t value; uint64_t cycle; };
struct Ours : r224::Machine224 {
    std::vector<Ev> log; uint16_t ipc = 0;
    uint8_t in(uint8_t p) override { uint8_t v = Machine224::in(p); if (!lexicon224x::sbc::is_sbc_port_224(p)) log.push_back({ipc, false, p, v, states}); return v; }
    void out(uint8_t p, uint8_t v) override { if (!lexicon224x::sbc::is_sbc_port_224(p)) log.push_back({ipc, true, p, v, states}); Machine224::out(p, v); }
    void go(uint64_t rows) { const uint64_t end = states + rows; while (states < end) { ipc = cpu.pc; wait_states = 0; const int st = cpu.step() + int(wait_states); for (int i = 0; i < st; i++) { dsp.row(); if (single && dsp.prev.reset) dsp.run = false; states++; } } }
};

int main(int argc, char **argv) {
    const double secs = argc > 2 ? std::atof(argv[2]) : 0.5;
    auto ours = std::make_unique<Ours>();
    auto ref = std::make_unique<lexicon224x::cpu::Host>(lexicon224x::Model::Lexicon224);
    for (int chip = 1; chip <= 5; chip++) {
        uint8_t b[2048];
        FILE *f = std::fopen((std::string(argv[1]) + "/ROM" + std::to_string(chip) + " 2716.BIN").c_str(), "rb");
        std::fread(b, 1, 2048, f); std::fclose(f);
        ours->load_rom(chip, b);
        std::copy(b, b + 2048, ref->memory.begin() + (chip - 1) * 0x800);
    }
    std::vector<Ev> rlog;
    ref->port_trace = [&](uint64_t cyc, uint16_t pc, bool w, unsigned port, uint8_t v) { rlog.push_back({pc, w, port, v, cyc}); };
    const uint64_t rows = uint64_t(secs * 2048000);
    for (double t : {0.5, 2.0, 4.0, 6.0, 8.0, 10.0, 12.0}) {
        const uint64_t r = uint64_t(t * 2048000);
        if (r > rows) break;
        ref->run_until(r);
        ours->go(r - ours->states);
        int dw = 0; for (int k = 0; k < 128; k++) dw += ref->dsp->wcs[k] != ours->dsp.wcs[k];
        int dm = 0; for (int a = 0x3c00; a < 0x4000; a++) dm += ref->peek(uint16_t(a)) != ours->mem[a];
        auto s = ref->snapshot();
        std::printf("t %.2f s: WCS rows differing %d, RAM bytes differing %d, pc %04x / %04x, digits %02x%02x%02x%02x / %02x%02x%02x%02x\n", t, dw, dm,
                    s.pc, ours->cpu.pc, ref->panel.digits[0], ref->panel.digits[1], ref->panel.digits[2], ref->panel.digits[3],
                    ours->panel.digits[0], ours->panel.digits[1], ours->panel.digits[2], ours->panel.digits[3]);
    }
    std::printf("reflecticon %zu port accesses, ours %zu\n", rlog.size(), ours->log.size());
    int shown = 0;
    for (size_t i = 0; i < std::min(rlog.size(), ours->log.size()) && shown < 12; i++) {
        const Ev &a = rlog[i], &b = ours->log[i];
        if (a.pc != b.pc || a.write != b.write || a.port != b.port || a.value != b.value) {
            std::printf("#%zu theirs pc %04x %s port %u = %02x (cycle %llu) | ours pc %04x %s port %u = %02x (cycle %llu)\n", i, a.pc, a.write ? "OUT" : "IN ",
                        a.port, a.value, (unsigned long long)a.cycle, b.pc, b.write ? "OUT" : "IN ", b.port, b.value, (unsigned long long)b.cycle);
            shown++;
        }
    }
}
