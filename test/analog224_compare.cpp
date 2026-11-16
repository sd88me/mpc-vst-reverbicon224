// src/analog224.hpp against reflecticon-224's analog boards (src/third_party/reflecticon/analog/analog.hpp): the 224's output chain fed
// DAC holds at row times and read at 48 kHz frames, the input chain fed 48 kHz noise and read at the ADC's hold instants. Both must agree
// to 1e-9; then the speed of each.        analog224_compare
#include "../src/analog224.hpp"
#include <chrono>
#include <cstdio>

int main() {
    namespace ref = lexicon224x::analog;
    const ref::Modal ain = ref::ain_224_modal(), aout = ref::aout_224_modal();
    const r224::analog::Tick row = 281250, frame = 576000000000ULL / 48000;     // the 224's row (488.28 ns), a 48 kHz frame
    double worst_out = 0, worst_in = 0;
    {
        ref::Output a(aout);
        r224::analog::Output b(aout);
        uint32_t rng = 1;
        r224::analog::Tick t = 0, next_frame = frame;
        for (int i = 0; i < 2000000; i++) {
            t += row;
            if (i % 100 == 37) {                                             // one DAC capture per pass
                rng = rng * 1664525u + 1013904223u;
                const double v = double(int(rng >> 20) - 2048) / 2048.0;
                a.hold(t, v); b.hold(t, v);
            }
            while (next_frame <= t) {
                const double x = a.sample(next_frame), y = b.sample(next_frame);
                worst_out = std::max(worst_out, std::abs(x - y));
                next_frame += frame;
            }
        }
    }
    {
        ref::Input a(ain, frame);
        r224::analog::Input b(ain, 48000.0);
        uint32_t rng = 7;
        r224::analog::Tick t_hold = 0;
        for (int i = 0; i < 400000; i++) {
            rng = rng * 1664525u + 1013904223u;
            const double x = double(int(rng >> 16) - 32768) / 40000.0;
            a.push(x); b.push(x);
            for (int k = 0; k < 2; k++) {                                    // about two ADC holds per frame (the 224's pass ~ 20.5 kHz)
                t_hold += 100 * row;
                if (t_hold < a.latency()) continue;
                const double p = a.sample(t_hold - a.latency()), q = b.sample(t_hold - b.latency());
                worst_in = std::max(worst_in, std::abs(p - q));
            }
        }
    }
    std::printf("output chain: largest difference %.3g; input chain: %.3g\n", worst_out, worst_in);
    // speed: one second of output (48000 frames, 20480 holds) and of input
    auto bench = [&](auto &&fn) { auto t0 = std::chrono::steady_clock::now(); fn(); return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    const double ta = bench([&] { ref::Output a(aout); r224::analog::Tick t = 0; double s = 0; for (int f = 0; f < 480000; f++) { t += frame; if (f % 2) a.hold(t - 7, 0.1 * (f % 5)); s += a.sample(t); } std::printf("", s); });
    const double tb = bench([&] { r224::analog::Output a(aout); r224::analog::Tick t = 0; double s = 0; for (int f = 0; f < 480000; f++) { t += frame; if (f % 2) a.hold(t - 7, 0.1 * (f % 5)); s += a.sample(t); } std::printf("", s); });
    std::printf("output chain, 10 s at 48 kHz: reflecticon %.1f ms, ours %.1f ms\n", ta * 1e3, tb * 1e3);
    return worst_out < 1e-9 && worst_in < 1e-9 ? 0 : 1;
}
