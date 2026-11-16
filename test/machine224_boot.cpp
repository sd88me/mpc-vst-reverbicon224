// Boot the v4.4 firmware on src/machine224.hpp and print the WCS (as reflecticon-224's probe prints it: "wcs" + 128 words), the control
// head's digits, how many DAC captures a second gives, and the speed.     machine224_boot ROM_DIR [SECONDS]
#include "../src/machine224.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const double secs = argc > 2 ? std::atof(argv[2]) : 12.0;
    auto m = std::make_unique<r224::Machine224>();
    for (int chip = 1; chip <= 5; chip++) {
        uint8_t b[2048];
        FILE *f = std::fopen((std::string(argv[1]) + "/ROM" + std::to_string(chip) + " 2716.BIN").c_str(), "rb");
        if (!f || std::fread(b, 1, 2048, f) != 2048) { std::fprintf(stderr, "ROM%d?\n", chip); return 2; }
        std::fclose(f);
        m->load_rom(chip, b);
    }
    const uint64_t rows = uint64_t(secs * 2048000.0);
    long long caps = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t done = 0; done < rows; done += 20480) { m->run(20480); caps += m->ncaptures; }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("wcs");
    for (int r = 0; r < 128; r++) std::printf(" %x", m->dsp.wcs[r]);
    std::printf("\ndigits");
    for (auto dg : m->panel.digits) std::printf(" %02x", dg);
    std::printf("\n%.1f s of machine time in %.3f s (%.1fx realtime), %lld DAC captures (%.0f a second), pc %04x\n", secs, wall, secs / wall, caps,
                caps / secs, m->cpu.pc);
}
