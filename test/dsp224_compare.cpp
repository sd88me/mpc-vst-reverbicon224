// src/dsp224.hpp against reflecticon-224's row machine (isa-level-cpp/lexicon224x.hpp), row for row: same WCS image, same ADC stimulus
// (silence, an impulse, then noise with changing gain ranges), and every row's DAC output, RR and ACC must match; then the delay memory.
// With a third argument it also times both.     dsp224_compare WCS.bin MODEL(224|224x) [ROWS]
#include "../src/dsp224.hpp"
#include "lexicon224x.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

int main(int argc, char **argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s WCS.bin 224|224x [ROWS]\n", argv[0]); return 2; }
    uint8_t image[512];
    FILE *f = std::fopen(argv[1], "rb");
    if (!f || std::fread(image, 1, 512, f) != 512) { std::fprintf(stderr, "need a 512-byte image\n"); return 2; }
    std::fclose(f);
    const bool orig = !std::strcmp(argv[2], "224");
    const long long rows = argc > 3 ? std::atoll(argv[3]) : 3413333;   // one second
    auto ref = std::make_unique<lexicon224x::Machine>();
    ref->model = orig ? lexicon224x::Model::Lexicon224 : lexicon224x::Model::Lexicon224X;
    lexicon224x::load_wcs(*ref, image);
    auto dsp = std::make_unique<r224::Dsp>();
    dsp->model = orig ? r224::Model::L224 : r224::Model::L224X;
    dsp->load_image(image);
    uint32_t rng = 12345;
    long long bad = 0, outputs = 0;
    for (long long r = 0; r < rows; r++) {
        unsigned l = 0, rt = 0, gl = 0, gr = 0;
        if (r >= 4096 && r < 6144) rt = 512;
        else if (r >= 200000) {
            if (r % 50 == 0) { rng = rng * 1664525u + 1013904223u; }
            l = (rng >> 8) & 0xfff; rt = (rng >> 20) & 0xfff; gl = (rng >> 4) & 3; gr = (rng >> 6) & 3;
        }
        ref->adc_left = l; ref->adc_right = rt; ref->gain_left = gl; ref->gain_right = gr;
        dsp->adc_left = l; dsp->adc_right = rt; dsp->gain_left = gl; dsp->gain_right = gr;
        lexicon224x::step_row(*ref);
        dsp->row();
        outputs += ref->dac_channels != 0;
        if (ref->dac_channels != dsp->dac_channels || (ref->dac_channels && (ref->dac_code != dsp->dac_code || ref->dac_gain != dsp->dac_gain)) ||
            ref->RR != dsp->rr || ref->ACC != dsp->acc) {
            if (bad < 5) std::printf("row %lld differs: dac %u/%u code %u/%u gain %u/%u RR %d/%d ACC %d/%d\n", r, ref->dac_channels, dsp->dac_channels,
                                     ref->dac_code, dsp->dac_code, ref->dac_gain, dsp->dac_gain, ref->RR, dsp->rr, ref->ACC, dsp->acc);
            bad++;
        }
    }
    const bool mem = !std::memcmp(ref->memory, dsp->memory, sizeof dsp->memory);
    std::printf("%lld rows, %lld DAC outputs, %lld rows differ, delay memory %s\n", rows, outputs, bad, mem ? "identical" : "DIFFERS");
    if (argc > 3) {
        auto t0 = std::chrono::steady_clock::now();
        for (long long r = 0; r < rows; r++) lexicon224x::step_row(*ref);
        auto t1 = std::chrono::steady_clock::now();
        for (long long r = 0; r < rows; r++) dsp->row();
        auto t2 = std::chrono::steady_clock::now();
        const double a = std::chrono::duration<double>(t1 - t0).count() * 1e9 / rows, b = std::chrono::duration<double>(t2 - t1).count() * 1e9 / rows;
        std::printf("reflecticon %.1f ns/row (%.1fx realtime), ours %.1f ns/row (%.1fx realtime)\n", a, 292.97 / a, b, 292.97 / b);
    }
    return bad || !mem;
}
