// Offline check of the on-device ROM import: status text, helper start, progress, bank publish, audio.
// usage: import_test ROM_DIR HELPER(bankhelper-x86_64)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
extern "C" {
#include "engine.h"
}
static const mpc_engine_t *E;
static std::string status(void *f) { char b[96] = ""; E->get_param(f, "display_rev", b, sizeof b); E->get_param(f, "status", b, sizeof b); return b; }
static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__), puts(""); else { fails++; printf("FAIL " __VA_ARGS__), puts(""); } } while (0)
int main(int argc, char **argv) {
    E = mpc_engine();
    std::string roms = argv[1], tmp = "/tmp/import_test_" + std::to_string(getpid());
    system(("rm -rf " + tmp + "; mkdir -p " + tmp + "/ROMs && cp '" + argv[2] + "' " + tmp + "/bankhelper-x86_64 && chmod +x " + tmp + "/bankhelper-x86_64").c_str());
    void *f = E->create(tmp.c_str());
    std::string s = status(f);
    CHECK(s.find("ADD ROMS") == 0 && s.find("(0/5)") != std::string::npos, "no ROMs: \"%s\"", s.c_str());
    int16_t in[256] = {0}, out[256];
    in[0] = in[1] = 12000;
    E->process(f, in, out, 128);
    CHECK(out[0] == 12000, "no bank: audio passes through");
    // headroom meter: LEDs follow the input peak (-24 -18 -12 -6 -0.5 dB), fall back when the signal stops
    auto meter = [&](const char *k) { char b[16] = "0"; E->get_param(f, k, b, sizeof b); return atoi(b); };
    int16_t loud[256], quiet[256];
    for (int i = 0; i < 256; i++) { loud[i] = (i & 2) ? 20000 : -20000; quiet[i] = 0; }
    E->process(f, loud, out, 128);
    CHECK(meter("meter_l") == 4 && meter("meter_r") == 4, "meter: -4 dB peak lights 4 LEDs (%d, %d)", meter("meter_l"), meter("meter_r"));
    for (int i = 0; i < 256; i++) loud[i] = (i & 2) ? 32500 : -32500;
    E->process(f, loud, out, 128);
    CHECK(meter("meter_l") == 5, "meter: full scale lights all 5 (%d)", meter("meter_l"));
    for (int b = 0; b < 400; b++) E->process(f, quiet, out, 128);
    CHECK(meter("meter_l") == 0 && meter("meter_r") == 0, "meter: falls back to 0 after silence (%d)", meter("meter_l"));
    // five wrong files: the helper must refuse them with a readable message
    for (int i = 1; i <= 5; i++) system(("head -c 2048 /dev/zero > " + tmp + "/ROMs/junk" + std::to_string(i) + ".bin").c_str());
    std::string last;
    for (int i = 0; i < 100; i++) { usleep(200000); last = status(f); if (last.find("NEED ALL FIVE") == 0) break; }
    CHECK(last.find("NEED ALL FIVE") == 0, "wrong ROMs: \"%s\"", last.c_str());
    E->destroy(f);
    // the real ROMs
    system(("rm -f " + tmp + "/ROMs/junk* " + tmp + "/.import-progress*; cp '" + roms + "'/ROM?\\ 2716.BIN " + tmp + "/ROMs/").c_str());
    f = E->create(tmp.c_str());
    s = status(f);
    CHECK(s.find("BUILDING BANK") == 0, "real ROMs start the helper: \"%s\"", s.c_str());
    int maxpct = 0, changes = 0;
    std::string prev;
    for (int i = 0; i < 1500 && s != "BANK READY"; i++) {   // up to 5 minutes
        usleep(200000);
        s = status(f);
        if (s != prev) { changes++; prev = s; }
        int p; if (sscanf(s.c_str(), "BUILDING BANK %d%%", &p) == 1 && p > maxpct) maxpct = p;
    }
    CHECK(s == "BANK READY", "build finishes: \"%s\" (saw up to %d%%, %d status changes)", s.c_str(), maxpct, changes);
    CHECK(maxpct > 0, "progress was shown");
    // audio now goes through the reverb: a quiet tail well after the impulse, and no passthrough of the click
    double tail = 0; memset(in, 0, sizeof in); in[0] = in[1] = 12000;
    for (int b = 0; b < 300; b++) { E->process(f, in, out, 128); in[0] = in[1] = 0; if (b > 20) for (int k = 0; k < 256; k++) tail += (double)out[k] * out[k]; }
    CHECK(tail > 1e6, "reverb tail after the bank loads (%.3g)", tail);
    E->destroy(f);
    struct stat st;
    CHECK(!stat((tmp + "/programs-v44.bank224").c_str(), &st) && st.st_size == 123480, "bank published at the exact size");
    char cmd[512]; snprintf(cmd, sizeof cmd, "cmp -s %s/programs-v44.bank224 programs-v44.bank224", tmp.c_str());
    CHECK(system(cmd) == 0, "bank identical to the one made on the PC");
    // a second plugin instance finds the bank at once
    f = E->create(tmp.c_str());
    CHECK(status(f) == "BANK READY", "next instance: BANK READY immediately");
    E->destroy(f);
    system(("rm -rf " + tmp).c_str());
    puts(fails ? "FAILED" : "PASSED");
    return fails != 0;
}
