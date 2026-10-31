// make_bank ROM_DIR OUT.bank224 [PROGRESS_FILE]: v4.4 ROM1-5 -> the prepared bank the plugin loads (same steps as
// cineolX's importer). With PROGRESS_FILE it is the plugin's on-device helper: it holds a lock on PROGRESS_FILE.lock for
// its lifetime (a second copy exits 3), writes "<state>\t<percent>\t<text>\n" there (state RUN, DONE or ERR) and
// publishes OUT atomically (temporary file, then rename), so the plugin never sees a half-written bank.
#include "import/bank_import.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace native_hall;
static std::string progress_path;
static void report(const char *state, int pct, const std::string &text) {
    if (progress_path.empty()) return;
    std::string tmp = progress_path + ".tmp";
    if (FILE *f = fopen(tmp.c_str(), "w")) {
        fprintf(f, "%s\t%d\t%s\n", state, pct, text.c_str());
        fclose(f);
        rename(tmp.c_str(), progress_path.c_str());
    }
}
int main(int argc, char **argv) try {
    if (argc < 3 || argc > 4) { std::cerr << "usage: make_bank ROM_DIR OUT.bank224 [PROGRESS_FILE]\n"; return 2; }
    if (argc == 4) {
        progress_path = argv[3];
        nice(10);   /* a background job on the MPC: its audio threads always come first */
        int fd = open((progress_path + ".lock").c_str(), O_CREAT | O_RDWR, 0644);
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) return 3;   /* another helper is building: keep its progress file */
    }
    report("RUN", 0, "starting");
    import::RomSet roms{};
    unsigned mask = 0;
    for (auto &e : std::filesystem::directory_iterator(argv[1])) {
        if (!e.is_regular_file() || e.file_size() != 2048) continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::array<uint8_t, 2048> d{};
        in.read((char *)d.data(), d.size());
        int c = import::rom_chip(d.data(), d.size());
        if (c < 0) continue;
        roms[c] = d;
        mask |= 1u << c;
    }
    if (mask != 31) {
        report("ERR", 0, "NEED ALL FIVE V4.4 ROMS (ROM1-5)");
        std::cerr << "need all five v4.4 chips, found mask " << mask << "\n";
        return 1;
    }
    int last = -1;
    import::Callbacks cb;
    cb.progress = [&](double p, const char *s) {
        int pct = int(p * 100);
        if (pct != last) { last = pct; report("RUN", pct, s ? s : ""); }
        std::cerr << "\r" << pct << "% " << (s ? s : "") << "   ";
        return true;
    };
    auto bank = import::prepare_bank(roms, cb);
    BankHeader h;
    h.checksum = profile_checksum(bank.get(), sizeof(*bank));
    std::string tmp = std::string(argv[2]) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out.write((const char *)&h, sizeof h);
        out.write((const char *)bank.get(), sizeof(*bank));
        if (!out) throw std::runtime_error("could not write the bank");
    }
    if (rename(tmp.c_str(), argv[2])) throw std::runtime_error("could not publish the bank");
    report("DONE", 100, "ok");
    std::cerr << "\nwrote " << argv[2] << "\n";
    return 0;
} catch (std::exception &e) {
    report("ERR", 0, std::string("IMPORT FAILED: ") + e.what());
    std::cerr << "error: " << e.what() << "\n";
    return 1;
}
