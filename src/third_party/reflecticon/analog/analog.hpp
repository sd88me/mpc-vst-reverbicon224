// The 224X analog boundary: AIN (input filter chain, sample-and-hold, gain
// ranger, ADC) and AOUT (DAC sample-and-hold, output filter chain), for
// driving the bit-exact digital machine from 48 kHz audio and back.
//
// Each analog chain is a linear circuit, reduced to modal form (poles p_k,
// residues r_k, direct term d) by tools/mna.py from its ngspice netlist.
// A mode obeys z' = p z + u; the chain's output is y = Re(sum r_k z_k) + d u.
// Between two instants the input is either constant (the DAC hold) or a
// straight line (the upsampled audio input), and for both the modes advance
// exactly:
//   z(t+T) = e^{pT} z + u0 (e^{pT} - 1)/p + slope (e^{pT} - 1 - pT)/p^2
// Step lengths are integer ticks (1/576 ns, the machine's time base). Only
// a few distinct lengths occur (48 kHz and the machine's pass rate are in a
// 45:32 ratio), so their coefficients are cached.
//
// This models the analog boards; it makes no claim about the digital
// machine, which is exact without it.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lexicon224x::analog {

using Tick = uint64_t;
constexpr double seconds_per_tick = 1e-9 / 576.0;

struct Modal {
    std::vector<std::complex<double>> poles;     // rad/s
    std::vector<std::complex<double>> residues;
    double direct = 0;
    double scale = 1;                            // level normalization applied to the output
};

// One chain's state, advanced through piecewise-linear input.
class Chain {
public:
    explicit Chain(const Modal &m) : m_(m), z_(m.poles.size()) {}

    // Advance by `ticks` with input u0 at the start and slope (per second).
    void advance(Tick ticks, double u0, double slope) {
        if (ticks == 0) {
            return;
        }
        const Coefficients &c = coefficients(ticks);
        for (size_t k = 0; k < z_.size(); k++) {
            z_[k] = c.decay[k] * z_[k] + u0 * c.step[k] + slope * c.ramp[k];
        }
    }

    double output(double u) const {
        std::complex<double> y = 0;
        for (size_t k = 0; k < z_.size(); k++) {
            y += m_.residues[k] * z_[k];
        }
        return (y.real() + m_.direct * u) * m_.scale;
    }

private:
    struct Coefficients {
        std::vector<std::complex<double>> decay, step, ramp;
    };

    const Coefficients &coefficients(Tick ticks) {
        auto found = cache_.find(ticks);
        if (found != cache_.end()) {
            return found->second;
        }
        const double T = double(ticks) * seconds_per_tick;
        Coefficients c;
        for (auto p : m_.poles) {
            std::complex<double> x = p * T;
            std::complex<double> e = std::exp(x);
            std::complex<double> em1, em1x;
            if (std::abs(x) < 1e-3) {   // series: avoid cancellation for short steps
                em1 = x * (1.0 + x / 2.0 + x * x / 6.0 + x * x * x / 24.0);
                em1x = x * x * (0.5 + x / 6.0 + x * x / 24.0 + x * x * x / 120.0);
            } else {
                em1 = e - 1.0;
                em1x = e - 1.0 - x;
            }
            c.decay.push_back(e);
            c.step.push_back(em1 / p);
            c.ramp.push_back(em1x / (p * p));
        }
        if (cache_.size() > 4096) {
            cache_.clear();
        }
        return cache_.emplace(ticks, std::move(c)).first->second;
    }

    const Modal &m_;
    std::vector<std::complex<double>> z_;
    std::unordered_map<Tick, Coefficients> cache_;
};

// ---- AOUT: DAC sample-and-hold -> output chain ---------------------------
// The hold is piecewise constant: it changes at each DAC capture. Captures
// may be reported ahead of the output being read (the machine can run past
// a frame boundary), so they are queued and applied in time order.
class Output {
public:
    explicit Output(const Modal &m) : chain_(m) {}

    // A DAC capture at time t (ticks) with value v (full scale = 1.0).
    void hold(Tick t, double v) {
        pending_.push_back({t, v});
    }

    // The analog output at time t (ticks). Captures at or before t take effect.
    double sample(Tick t) {
        size_t used = 0;
        for (; used < pending_.size() && pending_[used].first <= t; used++) {
            advance_to(pending_[used].first);
            held_ = pending_[used].second;
        }
        pending_.erase(pending_.begin(), pending_.begin() + long(used));
        advance_to(t);
        return chain_.output(held_);
    }

private:
    void advance_to(Tick t) {
        if (t > now_) {
            chain_.advance(t - now_, held_, 0.0);
            now_ = t;
        }
    }

    Chain chain_;
    Tick now_ = 0;
    double held_ = 0;
    std::vector<std::pair<Tick, double>> pending_;
};

// ---- AIN: 48 kHz audio -> x4 band-limited upsampling -> input chain -------
// The upsampled sequence is played as straight lines between its points
// (a first-order hold at 192 kHz: -0.18 dB at 15 kHz, images above 177 kHz).
// The AIN chain then filters it like the real board.
class Input {
public:
    static constexpr int factor = 4;
    static constexpr int taps_per_phase = 32;   // windowed sinc, 128 taps at 192 kHz

    Input(const Modal &m, Tick frame_ticks) : chain_(m), step_(frame_ticks / factor) {
        // Kaiser-windowed sinc, cutoff 22 kHz at 192 kHz; gain `factor` per phase sum.
        const int n = factor * taps_per_phase;
        const double fc = 22000.0 / (48000.0 * factor);
        const double beta = 8.0;
        auto bessel0 = [](double x) {
            double sum = 1, term = 1;
            for (int k = 1; k < 30; k++) {
                term *= (x / (2 * k)) * (x / (2 * k));
                sum += term;
            }
            return sum;
        };
        for (int i = 0; i < n; i++) {
            double t = i - (n - 1) / 2.0;
            double sinc = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
            double r = 2.0 * i / (n - 1) - 1.0;
            double w = bessel0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel0(beta);
            h_.push_back(sinc * w * factor);
        }
        history_.assign(taps_per_phase, 0.0);
    }

    // Push one 48 kHz input sample; its upsampled points follow the previous
    // ones at 192 kHz. Latency: the filter's delay (taps_per_phase/2 frames).
    void push(double x) {
        history_.erase(history_.begin());
        history_.push_back(x);
        for (int phase = 0; phase < factor; phase++) {
            double y = 0;
            for (int k = 0; k < taps_per_phase; k++) {
                y += history_[taps_per_phase - 1 - k] * h_[k * factor + phase];
            }
            points_.push_back(y);
        }
    }

    // The chain's output at time t (ticks, on the input's own clock:
    // point j of the upsampled stream is at j * step). Consumes points.
    double sample(Tick t) {
        // Never step backward: a late correction of the sampling instant
        // reads the chain where it stands.
        t = std::max(t, now_);
        while (true) {
            if (points_.size() < 2) {
                break;                          // no more input: hold the last line
            }
            Tick segment_end = start_ + step_;
            double slope = (points_[1] - points_[0]) / (double(step_) * seconds_per_tick);
            if (t < segment_end) {
                double u = points_[0] + slope * double(t - now_) * seconds_per_tick;
                chain_.advance(t - now_, points_[0] + slope * double(now_ - start_) * seconds_per_tick, slope);
                now_ = t;
                return chain_.output(u);
            }
            chain_.advance(segment_end - now_, points_[0] + slope * double(now_ - start_) * seconds_per_tick,
                           slope);
            now_ = start_ = segment_end;
            points_.erase(points_.begin());
        }
        return chain_.output(points_.empty() ? 0.0 : points_.front());
    }

    Tick latency() const {
        return step_ * factor * taps_per_phase / 2;
    }

private:
    Chain chain_;
    Tick step_;
    std::vector<double> h_, history_, points_;
    Tick start_ = 0, now_ = 0;
};

// ---- The gain ranger and ADC (service manual Table 3.3) -------------------
// v is the voltage at the converter input, with 5 V = the onset of ADC
// clipping. The ranger picks the largest gain that keeps the sample below
// clipping (6 dB steps), and the FPC shifts the code by 4 - IGA to undo it.
struct Converted {
    unsigned code;   // 12-bit two's complement
    unsigned iga;    // IGA1:IGA0
};

inline Converted convert(double volts, bool gain_ranging = true) {
    double a = std::abs(volts);
    unsigned iga = 0;
    if (gain_ranging) {
        if (a < 0.56) {
            iga = 3;
        } else if (a < 1.12) {
            iga = 2;
        } else if (a < 2.24) {
            iga = 1;
        }
    }
    double x = volts * double(1u << iga) / 5.0 * 2047.0;
    int code = int(std::lround(std::clamp(x, -2048.0, 2047.0)));
    return {unsigned(code) & 0xfff, iga};
}

// The same comparators as seen by the headroom display: five thresholds at
// 0.28, 0.56, 1.12, 2.24 and 5.0 V (service manual 3.9; the 5 V and 0.28 V
// ones are for the display only). Bit k set = comparator k exceeded.
inline unsigned level_detectors(double volts) {
    static constexpr double thresholds[5] = {0.28, 0.56, 1.12, 2.24, 5.0};
    double a = std::abs(volts);
    unsigned asserted = 0;
    for (unsigned k = 0; k < 5; k++) {
        if (a >= thresholds[k]) {
            asserted |= 1u << k;
        }
    }
    return asserted;
}

}  // namespace lexicon224x::analog
