// Reverbicon 224's engine: host audio -> the 224's input board (src/analog224.hpp) -> its gain-ranging ADC -> the machine (the v4.4
// firmware on our 8080 driving our DSP, src/machine224.hpp) -> the DAC captures -> the output board -> host audio, at any host rate.
//
// Time is the machine's: one row is one 8080 state (2.048 MHz on the original 224, 281250 ticks of 1/576 ns). Per host block the machine
// runs to the block's end in chunks of 25 rows; before the chunk in which a channel's ADC load falls (the converter takes the right input
// at scan address 39 and the left at 89) that channel's pins get the input board's output, so each input is sampled once a pass at about
// its real instant (the input board cuts off below 10 kHz, under the 20.48 kHz pass rate's Nyquist). DAC captures go to the four output boards at their row times, and the
// host samples the chosen pair at its frame times. Latency: the input upsampler's 16 host frames.
//
// Controls are the 224's own front panel (Panel224, below): the six program keys and PROG 7 / 8 on switch bank 0, SHIFT on bank 1, and
// the six pots, which the firmware scans and takes over with soft pickup (it ignores a pot until its reading crosses the stored value):
// a slider move first arms the pot's pickup byte in RAM (bit 7), as a hand would by sweeping through the value. RAM addresses: the v4
// firmware's (224 v4.3 / v4.4), from reflecticon-224's panel224_layouts.hpp.
#pragma once
#include "analog224.hpp"
#include "machine224.hpp"
#include "panel224.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace r224 {

// Denormals: the analog chains' states decay into them in silence, and an x86 core then runs ~15x slower. Flush them for the duration
// of a block and put the caller's mode back (as the plugin's former engine did).
struct ScopedFTZ {
#if defined(__x86_64__) || defined(__i386__)
    unsigned old;
    ScopedFTZ() : old(_mm_getcsr()) { _mm_setcsr(old | 0x8040); }
    ~ScopedFTZ() { _mm_setcsr(old); }
#elif defined(__aarch64__)
    uint64_t old;
    ScopedFTZ() { asm volatile("mrs %0, fpcr" : "=r"(old)); uint64_t n = old | (1ull << 24); asm volatile("msr fpcr, %0" ::"r"(n)); }
    ~ScopedFTZ() { asm volatile("msr fpcr, %0" ::"r"(old)); }
#elif defined(__arm__)
    unsigned old;
    ScopedFTZ() { asm volatile("vmrs %0, fpscr" : "=r"(old)); unsigned n = old | (1u << 24); asm volatile("vmsr fpscr, %0" ::"r"(n)); }
    ~ScopedFTZ() { asm volatile("vmsr fpscr, %0" ::"r"(old)); }
#else
    ScopedFTZ() {}
#endif
};

class Reverb224 {
public:
    static constexpr analog::Tick kRow = 281250;          // one row of the original 224 (488.28 ns)
    static constexpr uint64_t kRowsPerSecond = 2048000;

    explicit Reverb224(double host_rate)
        : rate_(host_rate), frame_(analog::Tick(double(analog::kTicksPerSecond) / host_rate + 0.5)),
          in_{analog::Input(lexicon224x::analog::ain_224_modal(), host_rate), analog::Input(lexicon224x::analog::ain_224_modal(), host_rate)},
          out_{analog::Output(lexicon224x::analog::aout_224_modal()), analog::Output(lexicon224x::analog::aout_224_modal()),
               analog::Output(lexicon224x::analog::aout_224_modal()), analog::Output(lexicon224x::analog::aout_224_modal())} {}

    Machine224 &machine() { return *m_; }
    Panel224 &panel() { return panel_; }
    void load_rom(int chip, const uint8_t *data) { m_->load_rom(chip, data); }

    // run the firmware through its power-up (boot and self-test) without audio: about 8 s of machine time
    void boot(double seconds = 8.0) {
        m_->run(uint64_t(seconds * kRowsPerSecond));
        settle_panel();
        t_ = m_->states * kRow;
        t0_ = t_;
    }

    // run the machine (no audio) until the operator's queue is done, at most `seconds`: for setting up controls before audio starts
    void settle_panel(double seconds = 3.0) {
        const uint64_t end = m_->states + uint64_t(seconds * kRowsPerSecond);
        while (!panel_.idle() && m_->states < end) { panel_.tick(); m_->run(256); }
        t_ = m_->states * kRow;
        t0_ = t_;
    }

    // the outputs the host hears: two of the four DAC outputs A-D (0-3); the 224's stereo programs use A and D... B and C differ in phase
    void set_outputs(int left, int right) { out_l_ = left & 3; out_r_ = right & 3; }
    analog::Vintage &vintage() { return vintage_; }

    void process(const float *inl, const float *inr, float *outl, float *outr, int frames) {
        ScopedFTZ ftz;
        for (int f = 0; f < frames; f++) {
            in_[0].push(inl ? inl[f] : 0.0f);
            in_[1].push(inr ? inr[f] : 0.0f);
            const analog::Tick end = t_ + frame_;
            while (m_->states * kRow < end) {
                panel_.tick();
                aim_pins();
                m_->run(kChunk);
                for (int k = 0; k < m_->ncaptures; k++) {
                    const auto &c = m_->captures[k];
                    int mant = int(c.code ^ 0x800);
                    if (mant & 0x800) mant -= 0x1000;
                    const double v = double(mant * (1 << (4 - (c.gain & 3)))) / 32768.0;
                    const analog::Tick at = c.row * kRow - t0_ + kDacObserved;
                    for (int ch : {out_l_, out_r_})                          // only the two outputs the host hears
                        if (c.channels >> ch & 1) out_[ch].hold(at, v);
                }
            }
            t_ = end;
            const analog::Tick now = t_ - t0_;
            outl[f] = float(vintage_.output(out_[out_l_].sample(now)));
            outr[f] = float(vintage_.output(out_[out_r_].sample(now)));
        }
    }

private:
    static constexpr analog::Tick kDacObserved = 309920 * 5 / 3;   // a capture reaches the outputs ~517 ns after its row starts (224 timing)

    // Set a channel's ADC pins only when its load comes up in the next chunk (scan address 89 for the left input, 39 for the right):
    // the converter reads them only then, so each channel's input board is read once a pass.
    void aim_pins() {
        const analog::Tick now = m_->states * kRow - t0_;
        Dsp &d = m_->dsp;
        for (int c = 0; c < 2; c++) {
            const unsigned load = c == 0 ? 89 : 39;
            const analog::Tick lat = in_[c].latency();
            const double held = in_[c].sample(now > lat ? now - lat : 0);   // always: the input board runs while the DSP is halted
            if (((load + 256 - d.count) & 255) >= unsigned(kChunk) + 2) continue;
            const double volts = vintage_.input(5.0 * held);
            const analog::Converted k = analog::convert(volts);
            const double g = vintage_.range_gain(k.iga);
            const unsigned code = g == 1.0 ? k.code : analog::convert(volts * g).code;
            if (c == 0) { d.adc_left = code; d.gain_left = k.iga; } else { d.adc_right = code; d.gain_right = k.iga; }
            unsigned asserted = 0;                                   // the level detectors for the headroom registers
            const double a = std::abs(volts);
            static constexpr double th[5] = {0.28, 0.56, 1.12, 2.24, 5.0};
            for (unsigned b = 0; b < 5; b++) if (a >= th[b]) asserted |= 1u << b;
            m_->level_detected(c, asserted);
        }
    }
    static constexpr int kChunk = 25;

    double rate_;
    analog::Tick frame_;
    std::unique_ptr<Machine224> m_ = std::make_unique<Machine224>();
    Panel224 panel_{*m_};
    analog::Input in_[2];
    analog::Output out_[4];
    analog::Vintage vintage_;
    analog::Tick t_ = 0, t0_ = 0;
    int out_l_ = 0, out_r_ = 2;   // A and C, as the plugin's defaults
};

}  // namespace r224
