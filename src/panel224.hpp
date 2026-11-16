// Reverbicon 224's front-panel operator: works the original 224's control head the way a hand would, so the firmware itself loads
// programs and takes slider moves. A queue of small steps, advanced from the audio thread with the machine's time (tick()):
//   - a button: hold exactly a mask on a switch bank until the firmware's scan has seen it, then release it the same way;
//   - a slider: arm the parameter's pickup byte (bit 7, the state the firmware reaches by itself when a pot crosses the stored value),
//     then move the pot, minding the scan's hysteresis (it ignores a rise of 1 or 2 and records a bigger rise as the reading minus 2);
//   - a program: SHIFT off, CALL mode, then the program's key, until the firmware's PROGRAM byte shows it;
//   - Mode Enhancement / Decay Optimization: PROG 7 / PROG 8 toggle PROGRAM bits 6 / 7: pressed when the bit differs.
// The recipe and every RAM address are reflecticon-224's 224 operator (juce-plugin/source/operator/panel224_operator.hpp and
// panel224_layouts.hpp, LAYOUT_V4: 224 v4.3 / v4.4), restated as a step queue.
#pragma once
#include "machine224.hpp"
#include <cstdint>

namespace r224 {

struct Layout224 {
    static constexpr uint16_t POTS = 0x3f20, SCAN = 0x3f26, LEDS = 0x3f3f, SHOWN = 0x3f44, MODE = 0x3f45, PROGRAM = 0x3f65;
    static constexpr uint16_t cell[6] = {0x3f67, 0x3f66, 0x3f68, 0x3f69, 0x3f6a, 0x3f6d};      // BASS MID CROSSOVER TREBLE DEPTH PRE-DELAY
    static constexpr uint16_t pickup[6] = {0x3f2b, 0x3f2a, 0x3f2c, 0x3f2d, 0x3f2e, 0x3f2f};
    static constexpr uint16_t diffusion_cell = 0x3f6e, diffusion_pickup = 0x3f30;              // page 2 (SHIFT held): DIFFUSION on DEPTH
    static constexpr uint8_t CALL = 0x04, SHIFT = 0x08, PROG7 = 0x40, PROG8 = 0x80;
};

class Panel224 {
public:
    explicit Panel224(Machine224 &m) : m_(m) {}

    // requests (any thread that owns the engine; they queue)
    void load_program(int program /* 1..6 */) {
        if (program < 1 || program > 6) return;
        push({Op::ShiftOff});
        push({Op::CallMode});
        push({Op::Press, 0, uint8_t(1u << (program - 1))});
        push({Op::WaitProgram, 0, uint8_t(1u << (program - 1))});
    }
    void set_slider(int slot /* 0..5 */, unsigned raw /* 0..253 */) { push({Op::Slider, uint8_t(slot), 0, uint8_t(raw > 253 ? 253 : raw)}); }
    void set_diffusion(unsigned raw) { push({Op::Diffusion, 4, 0, uint8_t(raw > 253 ? 253 : raw)}); }
    void set_toggle(int bit /* 6: Mode Enhancement, 7: Decay Optimization */, bool on) { push({Op::Toggle, uint8_t(bit), 0, uint8_t(on)}); }
    bool idle() const { return n_ == 0; }

    // the firmware's own view, for the plugin's display and write-back
    uint8_t program_byte() const { return m_.mem[Layout224::PROGRAM]; }
    uint8_t stored(int slot) const { return m_.mem[Layout224::cell[slot]]; }
    uint8_t stored_diffusion() const { return m_.mem[Layout224::diffusion_cell]; }

    // advance: called with the machine's row count (2.048 MHz), between machine runs
    void tick() {
        const uint64_t now = m_.states;
        for (int guard = 0; guard < 8 && n_; guard++) {
            Step &s = q_[head_];
            if (!s.started) { s.started = true; s.since = now; s.phase = 0; }
            const bool timed_out = now - s.since > kTimeout;
            if (run(s, now) || timed_out) { head_ = (head_ + 1) % kQ; n_--; continue; }
            break;
        }
    }

private:
    enum class Op : uint8_t { ShiftOff, CallMode, Press, WaitProgram, Slider, Diffusion, Toggle };
    struct Step { Op op; uint8_t a = 0, b = 0, c = 0; bool started = false; uint8_t phase = 0; uint64_t since = 0; };
    static constexpr uint64_t kTimeout = 2048000 / 2;            // 0.5 s: a step the firmware never answers is dropped
    static constexpr int kQ = 64;

    void push(Step s) { if (n_ < kQ) { q_[(head_ + n_) % kQ] = s; n_++; } }
    uint8_t peek(uint16_t a) const { return m_.mem[a]; }
    void hold(unsigned bank, uint8_t mask) { m_.panel.switches[bank] = uint8_t(~mask); }
    bool seen(unsigned bank, uint8_t mask) const { return peek(uint16_t(Layout224::SCAN + bank)) == uint8_t(~mask); }

    // one step's progress; true when it is done
    bool run(Step &s, uint64_t now) {
        switch (s.op) {
        case Op::Press:                                           // hold until seen, then release until seen
            if (s.phase == 0) { hold(s.a, s.b); s.phase = 1; return false; }
            if (s.phase == 1) { if (!seen(s.a, s.b)) return false; hold(s.a, 0); s.phase = 2; return false; }
            return seen(s.a, 0);
        case Op::ShiftOff:                                        // a lit SHIFT LED makes the program keys do nothing
            if (s.phase == 0) {
                if (!(peek(Layout224::LEDS) & Layout224::SHIFT)) return true;
                s.op = Op::Press; s.a = 1; s.b = Layout224::SHIFT; s.phase = 0; return false;
            }
            return true;
        case Op::CallMode:
            if (peek(Layout224::MODE) == 2) return true;
            s.op = Op::Press; s.a = 1; s.b = Layout224::CALL; s.phase = 0; return false;
        case Op::WaitProgram:
            return (peek(Layout224::PROGRAM) & 0x3f) == s.b;
        case Op::Slider: case Op::Diffusion: {
            const bool diff = s.op == Op::Diffusion;
            const uint16_t pick = diff ? Layout224::diffusion_pickup : Layout224::pickup[s.a];
            const uint16_t pots = uint16_t(Layout224::POTS + s.a);
            if (s.phase == 0) {
                if (diff) hold(1, Layout224::SHIFT);              // page 2: SHIFT held puts DIFFUSION on the DEPTH pot
                s.phase = 1; return false;
            }
            if (s.phase == 1) {
                if (diff && !seen(1, Layout224::SHIFT)) return false;
                m_.mem[pick] = uint8_t(m_.mem[pick] | 0x80);      // take the parameter over
                const unsigned recorded = peek(pots), v = s.c;
                if (recorded != v) m_.panel.pots[s.a] = uint8_t(v < recorded ? v : v + 2);
                s.phase = 2; s.since = now; return false;
            }
            if (s.phase == 2) {
                if (peek(pots) != s.c && now - s.since < kTimeout) return false;
                if (!diff) return true;
                hold(1, 0); s.phase = 3; return false;
            }
            return seen(1, 0);
        }
        case Op::Toggle: {
            const bool on = (peek(Layout224::PROGRAM) >> s.a) & 1;
            if (s.phase == 0) {
                if (on == bool(s.c)) return true;
                s.phase = 1;
            }
            // press PROG 7 / 8 (with SHIFT off and CALL mode not needed: they act on the loaded program)
            Step p{Op::Press, 0, uint8_t(s.a == 6 ? Layout224::PROG7 : Layout224::PROG8)};
            s = p; s.started = true; s.since = now;
            return false;
        }
        }
        return true;
    }

    Machine224 &m_;
    Step q_[kQ];
    int head_ = 0, n_ = 0;
};

}  // namespace r224
