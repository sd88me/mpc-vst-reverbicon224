// src/i8080.hpp against a cycle-stepped 8080 (floooh's chips i8080 as bundled with reflecticon-224), instruction by instruction, on the
// Lexicon 224's own firmware: the same memory map (ROM1-5 at 0x0000-0x27FF, RAM writable above) and the same deterministic port values.
// After every instruction the registers, the flags and the clock states must match.      i8080_lockstep ROM_DIR [INSTRUCTIONS]
#include "../src/i8080.hpp"
#define I8080_API static
#define CHIPS_IMPL
#include "i8080.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static uint8_t port_value(uint8_t port, uint64_t n) { return uint8_t(port * 37 + (n >> 10) * 11); }   // changes slowly, both sides alike

struct Mem : r224::Bus {
    uint8_t m[65536] = {};
    uint64_t ins = 0, outs = 0;
    uint8_t read(uint16_t a) override { return m[a]; }
    void write(uint16_t a, uint8_t v) override { if (a >= 0x2800) m[a] = v; }
    uint8_t in(uint8_t p) override { return port_value(p, ins++); }
    void out(uint8_t, uint8_t) override { outs++; }
};

int main(int argc, char **argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s ROM_DIR [N]\n", argv[0]); return 2; }
    const long long N = argc > 2 ? std::atoll(argv[2]) : 5000000;
    Mem ours, theirs;
    for (int chip = 1; chip <= 5; chip++) {
        std::string p = std::string(argv[1]) + "/ROM" + std::to_string(chip) + " 2716.BIN";
        FILE *f = std::fopen(p.c_str(), "rb");
        if (!f) { std::fprintf(stderr, "no %s\n", p.c_str()); return 2; }
        std::fread(ours.m + (chip - 1) * 0x800, 1, 0x800, f);
        std::fclose(f);
    }
    std::memcpy(theirs.m, ours.m, sizeof ours.m);
    r224::I8080 cpu; cpu.bus = &ours;
    i8080_t ref; uint64_t pins = i8080_init(&ref);
    long long bad = 0, states_ref = 0;
    auto tick = [&] {
        pins = i8080_tick(&ref, pins | I8080_READY);
        states_ref++;
        const uint8_t status = (pins & I8080_SYNC) ? I8080_GET_DATA(pins) : 0;
        static uint8_t cycle_status = 0;
        if (pins & I8080_SYNC) cycle_status = status;
        if (pins & I8080_DBIN) {
            const uint16_t a = I8080_GET_ADDR(pins);
            uint8_t v = (cycle_status & I8080_STATUS_INP) ? theirs.in(uint8_t(a)) : theirs.read(a);
            I8080_SET_DATA(pins, v);
        }
        if ((pins & I8080_WR) && !(pins & I8080_WAIT)) {
            static uint64_t last_wr = ~0ULL;
            const uint64_t key = states_ref;
            if (last_wr != key - 1) {                         // one write per WR pulse (it can span TW and T3)
                if (cycle_status & I8080_STATUS_OUT) theirs.out(uint8_t(I8080_GET_ADDR(pins)), I8080_GET_DATA(pins));
                else theirs.write(I8080_GET_ADDR(pins), I8080_GET_DATA(pins));
            }
            last_wr = key;
        }
    };
    // to the first instruction boundary
    while (!i8080_opdone(&ref)) tick();
    for (long long i = 0; i < N; i++) {
        const uint16_t pc0 = cpu.pc;
        const long long s0 = states_ref;
        const int st = cpu.step();
        tick();                                              // leave the boundary
        while (!i8080_opdone(&ref)) tick();
        const long long took = states_ref - s0;
        // the pending ALU result lands in the next fetch's T2: run T1 and T2 on a copy to read A and F
        i8080_t peek = ref; uint64_t pp = pins;
        for (int k = 0; k < 2; k++) { pp = i8080_tick(&peek, pp | I8080_READY); if (pp & I8080_DBIN) { I8080_SET_DATA(pp, theirs.m[I8080_GET_ADDR(pp)]); } }
        const bool ok = ref.pc == cpu.pc && ref.sp == cpu.sp && ref.bc == cpu.bc() && ref.de == cpu.de() && ref.hl == cpu.hl() &&
                        peek.a == cpu.a && (peek.f & 0xd5) == (cpu.flags() & 0xd5) && took == st;
        if (!ok) {
            if (bad < 8)
                std::printf("after %lld at %04x (op %02x): theirs pc %04x sp %04x bc %04x de %04x hl %04x a %02x f %02x states %lld | ours pc %04x sp %04x bc %04x de %04x hl %04x a %02x f %02x states %d\n",
                            i, pc0, ours.m[pc0], ref.pc, ref.sp, ref.bc, ref.de, ref.hl, peek.a, peek.f, took, cpu.pc, cpu.sp, cpu.bc(), cpu.de(), cpu.hl(), cpu.a, cpu.flags(), st);
            bad++;
            // resynchronise ours to theirs so one difference does not hide the next
            cpu.pc = ref.pc; cpu.sp = ref.sp; cpu.set_bc(ref.bc); cpu.set_de(ref.de); cpu.set_hl(ref.hl); cpu.a = peek.a; cpu.set_flags(peek.f);
        }
    }
    const bool mem = !std::memcmp(ours.m, theirs.m, sizeof ours.m);
    std::printf("%lld instructions, %lld states, %lld differ, memory %s, port reads %llu/%llu, writes %llu/%llu\n", N, states_ref, bad,
                mem ? "identical" : "DIFFERS", (unsigned long long)ours.ins, (unsigned long long)theirs.ins, (unsigned long long)ours.outs,
                (unsigned long long)theirs.outs);
    return bad || !mem;
}
