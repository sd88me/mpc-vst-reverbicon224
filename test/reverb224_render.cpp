// Render through src/reverb224.hpp: boot the v4.4 firmware, then an impulse (and a short noise burst) into the left input, and write a
// 32-bit float stereo WAV; print the level every 250 ms and the speed.     reverb224_render ROM_DIR OUT.wav [RATE] [SECONDS]
#include "../src/reverb224.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const double rate = argc > 3 ? std::atof(argv[3]) : 48000.0, secs = argc > 4 ? std::atof(argv[4]) : 4.0;
    auto r = std::make_unique<r224::Reverb224>(rate);
    for (int chip = 1; chip <= 5; chip++) {
        uint8_t b[2048];
        FILE *f = std::fopen((std::string(argv[1]) + "/ROM" + std::to_string(chip) + " 2716.BIN").c_str(), "rb");
        if (!f || std::fread(b, 1, 2048, f) != 2048) return 2;
        std::fclose(f);
        r->load_rom(chip, b);
    }
    auto t0 = std::chrono::steady_clock::now();
    r->boot();
    const double tboot = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const int n = int(secs * rate), block = 128;
    std::vector<float> L(n), R(n), inl(n, 0.f);
    inl[int(0.1 * rate)] = 0.9f;
    uint32_t rng = 1;
    for (int i = int(1.5 * rate); i < int(1.55 * rate); i++) { rng = rng * 1664525u + 1013904223u; inl[i] = 0.3f * (float(rng >> 8) / 8388608.f - 1.f); }
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i += block) r->process(&inl[i], nullptr, &L[i], &R[i], std::min(block, n - i));
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (int w = 0; w < int(secs * 4); w++) {
        double s = 0; int a = int(w * 0.25 * rate), b = std::min(n, int((w + 1) * 0.25 * rate));
        for (int i = a; i < b; i++) s += double(L[i]) * L[i] + double(R[i]) * R[i];
        std::printf("%5.2f s  rms %.5f\n", w * 0.25, std::sqrt(s / (2.0 * (b - a))));
    }
    std::printf("boot %.2f s; %.1f s of audio in %.3f s (%.1fx realtime)\n", tboot, secs, t, secs / t);
    FILE *o = std::fopen(argv[2], "wb");
    const uint32_t data = uint32_t(n) * 8, riff = 36 + data, fmt = 16, sr = uint32_t(rate), bps = sr * 8;
    const uint16_t pcm = 3, ch = 2, align = 8, bits = 32;
    std::fwrite("RIFF", 1, 4, o); std::fwrite(&riff, 4, 1, o); std::fwrite("WAVEfmt ", 1, 8, o); std::fwrite(&fmt, 4, 1, o); std::fwrite(&pcm, 2, 1, o);
    std::fwrite(&ch, 2, 1, o); std::fwrite(&sr, 4, 1, o); std::fwrite(&bps, 4, 1, o); std::fwrite(&align, 2, 1, o); std::fwrite(&bits, 2, 1, o);
    std::fwrite("data", 1, 4, o); std::fwrite(&data, 4, 1, o);
    for (int i = 0; i < n; i++) { std::fwrite(&L[i], 4, 1, o); std::fwrite(&R[i], 4, 1, o); }
    std::fclose(o);
}
