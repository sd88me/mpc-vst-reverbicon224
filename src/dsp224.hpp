// Reverbicon 224's DSP: the Lexicon 224's T&C + ARU + delay memory + floating-point converter, run fast.
//
// The machine is the one reflecticon-224 (joelanders/reflecticon-224, AGPL-3.0) describes in isa-level-cpp/lexicon224x.hpp, and this
// file is checked against it row for row (test/dsp224_compare.cpp). What is different is the speed: the 128 microinstructions are decoded
// once, when the WCS changes (decode()), into a table of the fields a row needs, and a row is the three ARU edges written out with no
// per-edge records, no SAT pins and no CPU-access holds. A host that writes the WCS (the 8080 running the firmware) does it between rows.
//
// Values are sample * 8 (three guard bits); ACC saturates to 19 bits at every partial-product add; RR takes ACC >> 3; a coefficient is
// c/32, applied two bits per ARU edge (x, then x/4, then x/16), so a row's multiply finishes in the next row.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace r224 {

enum class Model : uint8_t { L224X, L224 };   // the T&C board: the 224 (v4.4 firmware) or the 224X / XL

struct Op {                 // one decoded microinstruction
    uint8_t c54, c32, c10;  // the coefficient's bit pairs, edge by edge
    bool neg, zero, xfer;
    uint8_t ra, wa;
    uint8_t op;             // 0 NOP, 1 OPER, 2 MEMW, 3 MEMR
    uint16_t low;           // MEMR / MEMW: the complemented offset
    uint8_t source;         // OPER: 0 nothing, 1 RR, 2 XREG, 3 ADC
    uint8_t channels;       // OPER: DAC selects A..D (bit 0 = A)
    bool wr_da, wr_xreg, keep_shifting, reset;
};

inline unsigned bits(uint32_t w, unsigned hi, unsigned lo) { return w >> lo & ((1u << (hi - lo + 1)) - 1); }
inline bool bit(uint32_t w, unsigned n) { return w >> n & 1; }

inline Op decode(uint32_t w, Model model) {
    Op o{};
    const unsigned c = bits(w, 31, 26);
    o.c54 = uint8_t(c >> 4 & 3); o.c32 = uint8_t(c >> 2 & 3); o.c10 = uint8_t(c & 3);
    o.zero = bit(w, 25); o.xfer = bit(w, 24); o.neg = bit(w, 23);
    o.op = uint8_t(bits(w, 17, 16));
    const bool oper = o.op == 1;
    o.source = uint8_t(bits(w, 13, 12));
    o.channels = uint8_t(bit(w, 11) | bit(w, 10) << 1 | bit(w, 9) << 2 | bit(w, 8) << 3);
    o.wr_da = oper && bit(w, 7); o.wr_xreg = oper && bit(w, 6); o.keep_shifting = oper && bit(w, 4);
    if (model == Model::L224) {          // the original 224's T&C packs RA, WA, the offset and RESET differently
        o.ra = uint8_t(bits(w, 15, 14)); o.wa = uint8_t(bits(w, 20, 19));
        o.low = uint16_t(bits(w, 13, 0)); o.reset = bit(w, 22);
    } else {
        o.ra = uint8_t(bits(w, 21, 20)); o.wa = uint8_t(bits(w, 19, 18));
        o.low = uint16_t(bits(w, 15, 0)); o.reset = oper && bit(w, 3);
    }
    return o;
}

constexpr int32_t AccMax = 262143, AccMin = -262144;

struct Dsp {
    Model model = Model::L224;
    uint32_t wcs[128] = {};
    Op ops[128] = {};
    // sequencer
    unsigned pc = 0;
    Op none = decode(0, Model::L224);
    const Op *oldp = &none;   // the row finishing its multiply (points into ops[], or none)
    Op prev{};                 // a copy of it, so a WCS write during the row cannot change it
    bool counter_clear = false, restart = false, reset_pulse = false;
    // ARU
    int32_t operand = 0, partial = 0, acc = -1;   // partial is kept with its sign applied
    int16_t rr = 0;
    uint16_t R[4] = {};
    uint16_t cpc = 1;
    uint16_t memory[65536] = {};
    // converter (FPC)
    unsigned count = 1, input_gain = 0, output_gain = 12, age = 0;
    bool ch1 = false, busy = true, new_data = false;
    uint16_t input_sample = 0, waiting = 0, converting = 0;
    uint8_t waiting_channels = 0, channels = 0;
    // pins
    unsigned adc_left = 0, adc_right = 0, gain_left = 0, gain_right = 0;
    uint16_t xreg_from_cpu = 0, xreg_to_cpu = 0;
    // the last row's DAC output: the holds that took a sample, its code and gain
    unsigned dac_channels = 0, dac_code = 0, dac_gain = 0;

    void write_wcs(unsigned row, uint32_t word) { wcs[row & 127] = word; ops[row & 127] = decode(word, model); }
    void load(const uint32_t words[128]) { for (unsigned a = 0; a < 128; a++) write_wcs(a, words[a]); }
    // a 512-byte CPU-order image, as the 8080 sees the WCS: row a at (a ^ 127) * 4, bytes inverted
    void load_image(const uint8_t image[512]) {
        for (unsigned a = 0; a < 128; a++) {
            uint32_t w = 0;
            for (unsigned lane = 0; lane < 4; lane++) w |= uint32_t(image[(a ^ 127) * 4 + lane] ^ 0xff) << (8 * lane);
            write_wcs(a, w);
        }
    }

    static int32_t part(int32_t x, unsigned pair) { return ((pair & 2) ? x : 0) + ((pair & 1) ? (x >> 1) : 0); }
    int32_t settled() const { return std::clamp(acc + partial, AccMin, AccMax); }
    static int32_t spart(int32_t x, unsigned pair, bool neg) { const int32_t p = part(x, pair); return neg ? -p : p; }

    unsigned output_selects() const { return (age >= 9 && age < 23) ? channels : 0; }

    uint16_t source_value(const Op &o) const {
        if (o.op == 2) return uint16_t(rr);
        if (o.op != 1) return 0;
        switch (o.source) { case 1: return uint16_t(rr); case 2: return xreg_from_cpu; case 3: return input_sample; default: return 0; }
    }

    // the FPC timing ROM's load and CH1 outputs per scan position, as a table (bit 0 load, bit 1 CH1)
    struct Rom {
        uint8_t t[256];
        Rom() {
            for (unsigned a = 0; a < 256; a++) {
                const unsigned position = (a + 1) % 100;
                t[a] = a < 100 ? uint8_t((a % 50 >= 38 && a % 50 < 40) | (position >= 4 && position < 54) << 1) : 0;
            }
        }
    };
    static const Rom &rom() { static const Rom r; return r; }

    // one converter clock (reflecticon's clock_converter(), in place)
    void clock_converter(bool write_dac, uint8_t chans, uint16_t bus) {
        const unsigned t = rom().t[count];
        const bool load = t & 1, n_ch1 = t & 2;
        const unsigned adc = ch1 ? adc_right : adc_left, adc_gain = ch1 ? gain_right : gain_left;
        const bool shifting = input_gain & 4;
        count = restart ? 0 : std::min(count + 1, 255u);
        if (load && shifting) input_sample = uint16_t(int16_t(adc << 4) >> 4);
        else if (shifting) input_sample = uint16_t(input_sample << 1);
        else if (load) input_sample = uint16_t(0x8888 | (input_sample >> 1 & 0x7777));
        if (load) input_gain = 12 | adc_gain;
        else if (shifting) input_gain = (input_gain + 1) & 15;
        ch1 = n_ch1;
        if (!busy && new_data) {
            converting = waiting; channels = waiting_channels; output_gain = 12; age = 0; busy = true; new_data = false;
        } else if (busy) {
            const unsigned a0 = age;
            age = a0 + 1;
            if (output_gain < 15 && bit(converting, 15) == bit(converting, 14)) { converting = uint16_t(converting << 1 | 1); output_gain++; }
            if (age == 22) busy = false;
        } else if (age == 22) {
            age = 23;
        }
        if (write_dac) { waiting = bus; waiting_channels = chans; new_data = true; }
    }

    // one row: fetch, converter clock, the three ARU edges around the bus and register write
    void row() {
        const Op *c = &ops[pc];
        const Op *o = &prev;
        const bool clear_pc = model == Model::L224 ? c->reset : counter_clear;
        pc = clear_pc ? 0 : ((pc + 1) & 127);
        restart = counter_clear;
        counter_clear = c->reset;

        const unsigned before = output_selects();
        clock_converter(c->wr_da, c->channels, source_value(*c));
        dac_channels = output_selects() & ~before;
        dac_code = unsigned(!bit(converting, 15)) << 11 | bits(converting, 14, 4);
        dac_gain = output_gain;

        // edge 0: the old multiply's second shift and partial (c3 c2)
        int32_t sum = settled();
        partial = spart(operand, o->c32, o->neg); operand >>= 2; acc = sum;

        uint16_t bus = source_value(*c);
        if (reset_pulse) cpc++;
        reset_pulse = restart;
        uint16_t address = uint16_t(cpc + c->low + 1);
        if (model == Model::L224) address &= 0x3fff;
        if (c->op == 2) memory[address] = bus;
        if (c->op == 3) bus = memory[address];
        R[c->wa] = bus;
        if (c->wr_xreg) xreg_to_cpu = bus;

        // edge 1: this row's operand loads (or keeps shifting); the old one's last partial (c1 c0)
        sum = settled();
        partial = spart(operand, o->c10, o->neg);
        operand = c->keep_shifting ? (operand >> 2) : int32_t(int16_t(R[c->ra])) * 8;
        acc = sum;

        if (c->xfer) rr = int16_t(settled() >> 3);

        // edge 2: this row's first partial (c5 c4); ZERO starts a new sum
        sum = settled();
        partial = spart(operand, c->c54, c->neg); operand >>= 2;
        acc = c->zero ? 0 : sum;

        prev = *c;
    }
};

}  // namespace r224
