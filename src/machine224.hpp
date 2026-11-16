// Reverbicon 224's machine: the Lexicon 224's own firmware (ROM1-5, v4.4) on our 8080 (src/i8080.hpp), driving our DSP (src/dsp224.hpp)
// through the WCS and the DSP ports, with the control head's 8255 (faders, program switches, display digits) as reflecticon-224 models it.
//
// Time: on the original 224 one DSP row is one 8080 clock state (2.048 MHz; 100 rows a pass = 20.48 kHz). The CPU runs an instruction,
// then the DSP runs as many rows as the instruction took states. A WCS write or a port access therefore lands at the end of its
// instruction (a few rows later than on the hardware, where it lands mid-instruction and displaces one microinstruction): the firmware
// cannot tell, and the audio difference is a few rows of a slow modulation.
//
// What the board-level machine models and this does not: the T&C's WCS-access timing (displaced rows, held multiplicand clocks), the
// diagnostic monitors' sampling within a row (ports 3-5 show the last row), and the halt latches' gate delays.
#pragma once
#include "dsp224.hpp"
#include "i8080.hpp"
#ifndef R224_SBC_INCLUDED   // a test that includes reflecticon-224's own host already has these
#include "third_party/reflecticon/sbc/memory_map.hpp"
#include "third_party/reflecticon/sbc/peripherals.hpp"
#endif
#include <cstdint>
#include <cstring>
#include <memory>

namespace r224 {

struct Machine224 : Bus {
    Dsp dsp;
    I8080 cpu;
    lexicon224x::sbc::Panel panel;
    uint8_t mem[65536] = {};
    uint64_t states = 0;                 // 8080 clock states = DSP rows since power-up
    uint8_t bus_test = 0;
    bool single = false;
    unsigned wait_states = 0;            // TW states the instruction now running has picked up (WCS accesses wait for a slot)                 // DMEM's single-cycle latch: a run strobe then runs one pass, halting at its RESET row
    uint8_t headroom[2] = {0x1f, 0x1f};  // the FPC's peak registers (bit k clear = level detector k fired since the last read)
    // DAC captures of the rows just run: what an audio host reads after run()
    struct Capture { uint64_t row; uint8_t channels; uint16_t code; uint8_t gain; };
    static constexpr int kCaptures = 4096;
    Capture captures[kCaptures];
    int ncaptures = 0;

    Machine224() { dsp.model = Model::L224; cpu.bus = this; }

    // ROM1-5 (2048 bytes each) at 0x0000-0x27FF
    void load_rom(int chip, const uint8_t *data) { std::memcpy(mem + (chip - 1) * 0x800, data, 0x800); }

    // run the firmware and the DSP for at least `rows` rows (stops at an instruction boundary)
    void run(uint64_t rows) {
        ncaptures = 0;
        const uint64_t end = states + rows;
        while (states < end) {
            wait_states = 0;
            const int st = cpu.step() + int(wait_states);
            for (int i = 0; i < st; i++) {
                dsp.row();
                if (single && dsp.prev.reset) dsp.run = false;
                if (dsp.dac_channels && ncaptures < kCaptures)
                    captures[ncaptures++] = {states, uint8_t(dsp.dac_channels), uint16_t(dsp.dac_code), uint8_t(dsp.dac_gain)};
                states++;
            }
        }
    }

    // the FPC's level detectors clock a headroom register (the analog input's comparators: bit k = level k exceeded)
    void level_detected(unsigned channel, unsigned asserted) { headroom[channel & 1] &= uint8_t(~asserted & 0x1f); }

    // A WCS access waits (TW states) until the T&C grants it at a row whose microinstruction lacks the protect bit (any row while
    // the DSP is halted), plus the acknowledge: reflecticon's WcsAccess, without its pair flip-flop and gate delays.
    void wcs_wait() {
        unsigned k = 0;
        if (dsp.run) {
            unsigned pc = dsp.pc;
            while (k < 128 && dsp.ops[pc].protect) { pc = (pc + 1) & 127; k++; }
        }
        wait_states += k + 1;
    }

    // ---- the bus ----
    uint8_t read(uint16_t a) override {
        if (lexicon224x::sbc::is_wcs(a)) wcs_wait();
        if (lexicon224x::sbc::is_wcs(a)) return uint8_t(~(dsp.wcs[lexicon224x::sbc::wcs_row(a)] >> (8 * lexicon224x::sbc::wcs_lane(a))));
        return mem[a];
    }
    void write(uint16_t a, uint8_t v) override {
        if (lexicon224x::sbc::is_wcs(a)) {
            wcs_wait();
            const unsigned row = lexicon224x::sbc::wcs_row(a), lane = lexicon224x::sbc::wcs_lane(a);
            const uint32_t w = (dsp.wcs[row] & ~(0xffu << (8 * lane))) | uint32_t(uint8_t(~v)) << (8 * lane);
            dsp.write_wcs(row, w);
            return;
        }
        if (a >= lexicon224x::sbc::ram_start_224 && a < lexicon224x::sbc::ram_end) mem[a] = v;   // RAM; ROM ignores writes
    }
    uint8_t in(uint8_t port) override {
        if (lexicon224x::sbc::is_sbc_port_224(port)) {
            if (port == lexicon224x::sbc::SerialControl224) return 5;                 // the diagnostic serial port: idle, ready to send
            if (port == lexicon224x::sbc::SerialData224) return 0;
            return panel.read(port, states);
        }
        wait_states += 1;                                                             // DMEM's XACK/ comes after the 8080's first READY sample
        uint8_t v = 0;                                                                // DSP ports: the SBC reads the complement
        switch (port) {
        case 0: v = uint8_t(dsp.microinstruction); break;
        case 1: v = uint8_t(dsp.microinstruction >> 8); break;
        case 2: v = bus_test; break;
        case 6: v = uint8_t(dsp.xreg_to_cpu); break;
        case 7: v = uint8_t(dsp.xreg_to_cpu >> 8); break;
        case 8: case 9: {
            const uint8_t p = headroom[port - 8];
            v = uint8_t(0xe0 | (p & 1) << 4 | (p & 2) << 2 | (p & 4) | (p & 8) >> 2 | (p & 16) >> 4);
            headroom[port - 8] = 0x1f;                                                // reading restarts the peak
            break;
        }
        case 3: v = dsp.arithmetic_monitor(2048000.0); break;                         // the T&C's monitors (the self-test reads them)
        case 4: v = dsp.microinstruction_monitor(); break;
        case 5: v = dsp.timing_monitor(); break;
        default: v = 0; break;
        }
        return uint8_t(~v);
    }
    void out(uint8_t port, uint8_t value) override {
        if (lexicon224x::sbc::is_sbc_port_224(port)) {
            if (port != lexicon224x::sbc::SerialData224 && port != lexicon224x::sbc::SerialControl224) panel.write(port, value, states);
            return;
        }
        wait_states += 1;
        const uint8_t v = uint8_t(~value);                                            // the bus carries the complement
        switch (port) {
        case 2: dsp.run = false; break;                                               // halt
        case 3: dsp.run = true; break;                                                // run
        case 5: dsp.cpc = 0; break;                                                   // clear the delay counter
        case 6: dsp.xreg_from_cpu = uint16_t((dsp.xreg_from_cpu & 0xff00) | v); break;
        case 7: dsp.xreg_from_cpu = uint16_t((dsp.xreg_from_cpu & 0x00ff) | v << 8); bus_test = v; break;
        case 0: single = true; break;                                                 // select single cycle
        case 1: single = false; break;                                                // select continuous
        default: break;
        }
    }
};

}  // namespace r224
