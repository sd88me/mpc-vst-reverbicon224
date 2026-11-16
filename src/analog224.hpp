// Reverbicon 224's analog boundary: the Lexicon 224's input board (filter chain, sample-and-hold, gain-ranging ADC) and output board
// (DAC hold, filter chain), at any host rate, without allocating on the audio path, plus an optional "vintage" layer.
//
// The filter chains are reflecticon-224's (src/third_party/reflecticon/analog: the boards' SPICE netlists reduced to poles and residues,
// AGPL-3.0); this is a faster implementation of the same exact modal integration, checked against theirs (test/analog224_compare.cpp):
//   - conjugate pole pairs are one mode with weight 2 (the input is real), so a 10-pole chain is 5 modes;
//   - the step coefficients (e^{pT} and its integrals for a held or a straight-line input) sit in a small fixed table keyed by the step
//     length; the lengths come from a fixed rate ratio, so the table fills once;
//   - the input upsampler (4x, Kaiser-windowed sinc) is designed for the host rate (reflecticon's is fixed at 48 kHz), the queues are rings.
// The vintage layer (Vintage, below) is our own approximation, off at amount 0: converter hiss, the gain-ranging stages' small gain
// mismatches, a soft op-amp limit before the ADC, and a little hold droop. It is not measured from a real 224.
#pragma once
#include "third_party/reflecticon/analog/filters.hpp"
#include "third_party/reflecticon/analog/filters_224.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>

namespace r224::analog {

using Tick = uint64_t;                                   // 1/576 ns, the machine's time base (reflecticon's)
constexpr double kSecondsPerTick = 1e-9 / 576.0;
constexpr Tick kTicksPerSecond = 576000000000ULL;

class Chain {
public:
    static constexpr int kMaxModes = 8;
    explicit Chain(const lexicon224x::analog::Modal &m) {
        scale_ = m.scale; direct_ = m.direct;
        for (size_t k = 0; k < m.poles.size(); k++) {
            const auto p = m.poles[k];
            if (p.imag() < -1e-9) continue;                    // its conjugate stands for both
            const bool pair = p.imag() > 1e-9;
            pole_[n_] = p;
            const std::complex<double> r = m.residues[k] * (pair ? 2.0 : 1.0);
            rr_[n_] = r.real(); ri_[n_] = r.imag(); n_++;
        }
    }
    void reset() { for (int k = 0; k < n_; k++) zr_[k] = zi_[k] = 0; }
    // complex arithmetic written out on real and imaginary parts: std::complex multiplication calls __muldc3 (NaN / infinity
    // handling) unless the whole build uses -ffast-math, which costs ~10x here
    void advance(Tick ticks, double u0, double slope) {
        if (ticks < kGrid / 2) return;
        const Coef &c = coef(ticks);
        for (int k = 0; k < n_; k++) {
            const double zr = zr_[k], zi = zi_[k];
            zr_[k] = c.dr[k] * zr - c.di[k] * zi + u0 * c.sr[k] + slope * c.rr[k];
            zi_[k] = c.dr[k] * zi + c.di[k] * zr + u0 * c.si[k] + slope * c.ri[k];
        }
    }
    double output(double u) const {
        double y = 0;
        for (int k = 0; k < n_; k++) y += rr_[k] * zr_[k] - ri_[k] * zi_[k];
        return (y + direct_ * u) * scale_;
    }
private:
    struct Coef { Tick ticks = 0; double dr[kMaxModes], di[kMaxModes], sr[kMaxModes], si[kMaxModes], rr[kMaxModes], ri[kMaxModes]; };
    // The coefficients are for the step length rounded to kGrid ticks (5.4 ns): at 44.1 kHz the host frames and the 224's rows share no
    // short period, so exact lengths would almost never repeat. The clock itself stays exact (nothing drifts); a step of microseconds
    // integrated over a length up to 2.7 ns off is a negligible error. At 48 kHz every length is already on the grid.
    static constexpr Tick kGrid = 3125;
    const Coef &coef(Tick exact) {
        const Tick ticks = (exact + kGrid / 2) / kGrid * kGrid;          // 0 for a step under 2.7 ns: the identity (see advance)
        Coef &c = cache_[(ticks * 0x9E3779B97F4A7C15ULL) >> (64 - 12)];   // Fibonacci hashing into the 4096 entries
        if (c.ticks == ticks) return c;
        misses++;
        const double T = double(ticks) * kSecondsPerTick;
        for (int k = 0; k < n_; k++) {
            const std::complex<double> p = pole_[k], x = p * T, e = std::exp(x);
            std::complex<double> em1, em1x;
            if (std::abs(x) < 1e-3) {
                em1 = x * (1.0 + x / 2.0 + x * x / 6.0 + x * x * x / 24.0);
                em1x = x * x * (0.5 + x / 6.0 + x * x / 24.0 + x * x * x / 120.0);
            } else {
                em1 = e - 1.0; em1x = e - 1.0 - x;
            }
            const std::complex<double> st = em1 / p, rp = em1x / (p * p);
            c.dr[k] = e.real(); c.di[k] = e.imag(); c.sr[k] = st.real(); c.si[k] = st.imag(); c.rr[k] = rp.real(); c.ri[k] = rp.imag();
        }
        c.ticks = ticks;
        return c;
    }
    static constexpr int kCache = 4096;
    int n_ = 0;
    std::complex<double> pole_[kMaxModes];
    double rr_[kMaxModes] = {}, ri_[kMaxModes] = {}, zr_[kMaxModes] = {}, zi_[kMaxModes] = {};   // residues (weighted) and states
    double scale_ = 1, direct_ = 0;
    std::unique_ptr<Coef[]> cache_ = std::make_unique<Coef[]>(kCache);
  public:
    uint64_t misses = 0;
};

// DAC hold -> output chain. hold() queues a capture (time, value), sample() reads the output at a time.
class Output {
public:
    explicit Output(const lexicon224x::analog::Modal &m) : chain_(m) {}
    void hold(Tick t, double v) { if (count_ < kQ) { q_[(head_ + count_) % kQ] = {t, v}; count_++; } }
    double sample(Tick t) {
        while (count_ && q_[head_].t <= t) { advance_to(q_[head_].t); held_ = q_[head_].v * droop_gain_; head_ = (head_ + 1) % kQ; count_--; }
        advance_to(t);
        return chain_.output(held_);
    }
    void set_droop(double g) { droop_gain_ = g; }
private:
    void advance_to(Tick t) { if (t > now_) { chain_.advance(t - now_, held_, 0.0); now_ = t; } }
    struct Ev { Tick t; double v; };
    static constexpr int kQ = 512;
    Ev q_[kQ];
    int head_ = 0, count_ = 0;
    Chain chain_;
    Tick now_ = 0;
    double held_ = 0, droop_gain_ = 1;
};

// host-rate audio -> 4x band-limited upsampling -> input chain, read at any later time (the ADC's hold instants).
class Input {
public:
    static constexpr int kFactor = 4, kTaps = 32;          // per phase: 128 taps at 4x
    Input(const lexicon224x::analog::Modal &m, double host_rate) : chain_(m) {
        step_ = Tick(double(kTicksPerSecond) / host_rate / kFactor + 0.5);
        const int n = kFactor * kTaps;
        const double fc = std::min(22000.0, host_rate * 22000.0 / 48000.0) / (host_rate * kFactor), beta = 8.0;   // 22 kHz at 48 kHz, scaled below it
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        for (int i = 0; i < n; i++) {
            const double t = i - (n - 1) / 2.0, sinc = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
            const double r = 2.0 * i / (n - 1) - 1.0;
            h_[i] = sinc * i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta) * kFactor;
        }
    }
    void push(double x) {
        hist_[hpos_] = x; hpos_ = (hpos_ + 1) % kTaps;
        for (int ph = 0; ph < kFactor; ph++) {
            double y = 0;
            for (int k = 0; k < kTaps; k++) y += hist_[(hpos_ + kTaps - 1 - k) % kTaps] * h_[k * kFactor + ph];
            if (pcount_ < kP) { pts_[(phead_ + pcount_) % kP] = y; pcount_++; }
        }
    }
    double sample(Tick t) {
        t = std::max(t, now_);
        while (pcount_ >= 2) {
            const double p0 = pts_[phead_], p1 = pts_[(phead_ + 1) % kP];
            const Tick end = start_ + step_;
            const double slope = (p1 - p0) / (double(step_) * kSecondsPerTick);
            const double u_now = p0 + slope * double(now_ - start_) * kSecondsPerTick;
            if (t < end) {
                chain_.advance(t - now_, u_now, slope);
                now_ = t;
                return chain_.output(p0 + slope * double(t - start_) * kSecondsPerTick);
            }
            chain_.advance(end - now_, u_now, slope);
            now_ = start_ = end;
            phead_ = (phead_ + 1) % kP; pcount_--;
        }
        return chain_.output(pcount_ ? pts_[phead_] : 0.0);
    }
    Tick latency() const { return step_ * kFactor * kTaps / 2; }
private:
    static constexpr int kP = 1024;
    Chain chain_;
    Tick step_ = 0, start_ = 0, now_ = 0;
    double h_[kFactor * kTaps] = {}, hist_[kTaps] = {}, pts_[kP] = {};
    int hpos_ = 0, phead_ = 0, pcount_ = 0;
};

// The gain-ranging ADC (reflecticon's convert(): the comparators at 0.56 / 1.12 / 2.24 V pick IGA, the code is 12-bit two's complement).
struct Converted { unsigned code, iga; };
inline Converted convert(double volts, bool gain_ranging = true) {
    const double a = std::abs(volts);
    unsigned iga = 0;
    if (gain_ranging) iga = a < 0.56 ? 3 : a < 1.12 ? 2 : a < 2.24 ? 1 : 0;
    const double x = volts * double(1u << iga) / 5.0 * 2047.0;
    const int code = int(std::lround(std::clamp(x, -2048.0, 2047.0)));
    return {unsigned(code) & 0xfff, iga};
}

// ---- the vintage layer: our approximation of what the linear model leaves out (amount 0 = off) ------------------------------
struct Vintage {
    double amount = 0;                  // 0 .. 1
    uint32_t rng = 0x1234567u;
    // the four gain ranges' gain errors (a fraction), as a real unit's resistor tolerances would give: fixed per instance
    double range_error[4] = {0.0, 0.004, -0.006, 0.009};
    double noise() {                    // a cheap gaussian-ish sample (sum of four uniforms), unit variance
        double s = 0;
        for (int i = 0; i < 4; i++) { rng = rng * 1664525u + 1013904223u; s += double(rng >> 8) / 16777216.0 - 0.5; }
        return s * 1.7320508;
    }
    // before the ADC: the input stage's soft limit (rails about 2.4x the converter's full scale) and a little hiss
    double input(double volts) {
        if (amount <= 0) return volts;
        const double rail = 12.0;
        const double soft = rail * std::tanh(volts / rail);
        return volts + amount * (soft - volts) + amount * 0.0008 * noise();
    }
    // a converted code's gain-range mismatch: the DSP sees the input scaled by the range's error
    double range_gain(unsigned iga) const { return 1.0 + amount * range_error[iga & 3]; }
    // after the DAC: hiss at about -86 dBFS at full amount and a slight hold droop
    double output(double v) { return amount <= 0 ? v : v + amount * 0.00005 * noise(); }
    double droop_gain() const { return 1.0 - amount * 0.0005; }
};

}  // namespace r224::analog
