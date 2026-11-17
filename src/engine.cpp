/* Reverbicon 224 for MPC: the mpc_engine() glue (wrapper/engine.h) around Reverb224 (src/reverb224.hpp), the Lexicon 224's own v4.4
 * firmware on our 8080 and DSP between models of its input and output boards.
 *
 * The five ROMs (2716 EPROMs, 2048 bytes) are read from the plugin's Content/ROMs folder, recognised by checksum (or by the chip
 * number in the file name). The firmware's power-up runs on a worker thread; until it is done the effect passes the dry signal.
 *
 * Controls are worked on the 224's front panel through the firmware (Panel224): a program key, then the six sliders, DIFFUSION
 * and the two toggles, so the firmware takes each value as it would from a hand. The parameters keep the plugin's released
 * meaning, as the firmware's own stored steps (BASS/MID/CROSSOVER/TREBLE 1..31, DEPTH 0..71, DIFFUSION 1..63, PRE-DELAY in ms above
 * the program's minimum); src/slider_tables224.hpp maps a step to a pot position. Loading a program sets the firmware's sliders to
 * that program's values, so the plugin's values are sent again after every program change. */
extern "C" {
#include "engine.h"
}
#include "reverb224.hpp"
#include "slider_tables224.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

enum { P_ALGO, P_BASS, P_MID, P_XOVER, P_TREBLE, P_DEPTH, P_PREDELAY, P_DIFF, P_INPUT, P_MIX, P_DIRT,
       P_MODENH, P_DECAYOPT, P_OUTL, P_OUTR, NP };
const char *const KEYS[NP] = {"algorithm", "bass", "mid", "xover", "treble", "depth", "predelay", "diffusion", "input",
                              "mix", "dirt", "mode_enh", "decay_opt", "out_l", "out_r"};
const float DEFAULTS[NP] = {2, 17, 14, 5, 23, 21, 0, 1, 0, 100, 0, 1, 1, 0, 2};
const int PREDELAY_MIN[6] = {24, 0, 24, 25, 0, 24};   /* each program's lowest PRE-DELAY (ms), measured on the firmware */

/* v4.4's five chips by CRC-32 (ROM1..ROM5) */
const uint32_t ROM_CRC[5] = {0xee87bca0u, 0xb923c785u, 0xc5211473u, 0x6e91238eu, 0x961bf204u};

uint32_t crc32_of(const uint8_t *b, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

struct Fx {
    float p[NP];
    std::string dir;
    std::unique_ptr<r224::Reverb224> rev;
    std::thread boot;
    std::atomic<bool> ready{false};
    std::atomic<int> state{0};       /* 0 no ROMs, 1 starting, 2 ready, 3 not a working set */
    int roms_found = 0;
    char status[64] = "";
    unsigned rev_no = 1;
    float peak[2] = {0, 0};
    int meter[2] = {0, 0};
    bool dirty = true;
    int sent[NP];                    /* what the panel was last asked for (-1: never) */
    float mix_s = 1;
    float bl[128], br[128], ol[128], orr[128];
};

void set_status(Fx *f, const char *t) {
    if (strcmp(f->status, t)) { snprintf(f->status, sizeof f->status, "%s", t); f->rev_no++; }
}

/* read Content/ROMs: every 2048-byte file, placed by checksum, else by the first digit 1-5 in its name */
int read_roms(const std::string &dir, uint8_t (*roms)[2048]) {
    bool have[5] = {};
    std::string d = dir + "/ROMs";
    DIR *dp = opendir(d.c_str());
    if (!dp) return 0;
    struct Cand { uint8_t b[2048]; int digit; };
    std::unique_ptr<Cand[]> unknown(new Cand[16]);
    int nunk = 0;
    while (dirent *e = readdir(dp)) {
        if (e->d_name[0] == '.') continue;
        std::string path = d + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size != 2048) continue;
        uint8_t b[2048];
        FILE *fp = fopen(path.c_str(), "rb");
        if (!fp) continue;
        const bool ok = fread(b, 1, 2048, fp) == 2048;
        fclose(fp);
        if (!ok) continue;
        const uint32_t crc = crc32_of(b, 2048);
        int chip = -1;
        for (int i = 0; i < 5; i++) if (crc == ROM_CRC[i]) chip = i;
        if (chip >= 0) { memcpy(roms[chip], b, 2048); have[chip] = true; continue; }
        if (nunk < 16) {
            int digit = -1;
            for (const char *c = e->d_name; *c; c++) if (*c >= '1' && *c <= '5') { digit = *c - '1'; break; }
            memcpy(unknown[nunk].b, b, 2048); unknown[nunk].digit = digit; nunk++;
        }
    }
    closedir(dp);
    for (int i = 0; i < nunk; i++) {
        const int c = unknown[i].digit;
        if (c >= 0 && !have[c]) { memcpy(roms[c], unknown[i].b, 2048); have[c] = true; }
    }
    int n = 0;
    for (bool h : have) n += h;
    return n;
}

void boot_worker(Fx *f) {
    std::unique_ptr<uint8_t[][2048]> roms(new uint8_t[5][2048]);
    if (read_roms(f->dir, roms.get()) < 5) { f->state = 0; return; }
    auto r = std::make_unique<r224::Reverb224>(44100.0);
    for (int c = 0; c < 5; c++) r->load_rom(c + 1, roms[c]);
    r->boot();
    if ((r->panel().program_byte() & 0x3f) == 0) { f->state = 3; return; }   /* no program came up: not a working v4.4 set */
    f->rev = std::move(r);
    f->state = 2;
    f->ready.store(true, std::memory_order_release);
}

/* look for the ROMs and, with all five, start the power-up (display thread or create, never audio) */
void try_start(Fx *f) {
    std::unique_ptr<uint8_t[][2048]> roms(new uint8_t[5][2048]);
    f->roms_found = read_roms(f->dir, roms.get());
    if (f->roms_found < 5) return;
    if (f->boot.joinable()) f->boot.join();
    f->state = 1;
    f->boot = std::thread(boot_worker, f);
}

void poll(Fx *f) {
    switch (f->state.load()) {
    case 0: {
        try_start(f);
        if (f->state.load() == 1) { set_status(f, "STARTING"); break; }
        char t[64];
        snprintf(t, sizeof t, "ADD ROMS TO CONTENT/ROMS (%d/5)", f->roms_found);
        set_status(f, t);
        break;
    }
    case 1: set_status(f, "STARTING"); break;
    case 2: set_status(f, "READY"); break;
    default: set_status(f, "ROMS NOT RECOGNISED"); break;
    }
}

void *create(const char *dir) {
    Fx *f = new Fx();
    memcpy(f->p, DEFAULTS, sizeof f->p);
    for (int &s : f->sent) s = -1;
    f->dir = dir ? dir : ".";
    poll(f);
    return f;
}

void destroy(void *inst) {
    Fx *f = (Fx *)inst;
    if (f->boot.joinable()) f->boot.join();
    delete f;
}
void midi(void *, const uint8_t *, int) {}

int program_of(const Fx *f) { return std::clamp((int)f->p[P_ALGO], 0, 5); }

/* hand changed controls to the panel operator (audio thread, which owns the machine): the program first, then all it resets */
void apply(Fx *f) {
    auto &panel = f->rev->panel();
    const int prog = program_of(f);
    bool all = false;
    if (f->sent[P_ALGO] != prog) { panel.load_program(prog + 1); f->sent[P_ALGO] = prog; all = true; }
    static const int SLOT[5] = {P_BASS, P_MID, P_XOVER, P_TREBLE, P_DEPTH};
    for (int s = 0; s < 5; s++) {
        const int v = (int)lrintf(f->p[SLOT[s]]);
        if (all || f->sent[SLOT[s]] != v) { panel.set_slider(s, r224::raw_for(r224::kStored[s], v)); f->sent[SLOT[s]] = v; }
    }
    const int pd = (int)lrintf(f->p[P_PREDELAY]);
    if (all || f->sent[P_PREDELAY] != pd) {
        panel.set_slider(5, r224::raw_for(r224::kStoredPredelay[prog], PREDELAY_MIN[prog] + pd));
        f->sent[P_PREDELAY] = pd;
    }
    const int df = (int)lrintf(f->p[P_DIFF]);
    if (all || f->sent[P_DIFF] != df) { panel.set_diffusion(r224::raw_for(r224::kStoredDiffusion, df)); f->sent[P_DIFF] = df; }
    for (int k : {P_MODENH, P_DECAYOPT}) {
        const int on = f->p[k] >= 0.5f;
        if (all || f->sent[k] != on) { panel.set_toggle(k == P_MODENH ? 6 : 7, on); f->sent[k] = on; }
    }
    f->rev->set_outputs((int)f->p[P_OUTL], (int)f->p[P_OUTR]);
    const float d = f->p[P_DIRT] / 100.0f;
    f->rev->vintage().amount = d * d;
    f->dirty = false;
}

void set_param(void *inst, const char *key, const char *val) {
    Fx *f = (Fx *)inst;
    for (int i = 0; i < NP; i++)
        if (!strcmp(key, KEYS[i])) { f->p[i] = (float)atof(val); f->dirty = true; return; }
}

/* the firmware's stored step once running (what the 224's display shows), else the parameter's */
int stored_or(const Fx *f, int slot, int fallback) {
    if (!f->ready.load(std::memory_order_acquire)) return fallback;
    return slot == 6 ? f->rev->panel().stored_diffusion() : f->rev->panel().stored(slot);
}

int get_param(void *inst, const char *key, char *buf, int n) {
    Fx *f = (Fx *)inst;
    for (int i = 0; i < NP; i++)
        if (!strcmp(key, KEYS[i])) return snprintf(buf, n, "%.3f", (double)f->p[i]);
    using r224::kTimes; using r224::kFrequencies;
    auto idx = [](int v) { return std::clamp(v, 0, 31); };
    if (!strcmp(key, "bass_display")) return snprintf(buf, n, "%.1f s", (double)kTimes[idx(stored_or(f, 0, (int)f->p[P_BASS]))]);
    if (!strcmp(key, "mid_display")) return snprintf(buf, n, "%.1f s", (double)kTimes[idx(stored_or(f, 1, (int)f->p[P_MID]))]);
    if (!strcmp(key, "xover_display")) return snprintf(buf, n, "%d Hz", kFrequencies[idx(stored_or(f, 2, (int)f->p[P_XOVER]))]);
    if (!strcmp(key, "treble_display")) return snprintf(buf, n, "%d Hz", kFrequencies[idx(stored_or(f, 3, (int)f->p[P_TREBLE]))]);
    if (!strcmp(key, "rom_status")) return snprintf(buf, n, "%d", f->ready.load() ? 1 : 0);
    if (!strcmp(key, "display_rev")) { poll(f); return snprintf(buf, n, "%u", f->rev_no); }
    if (!strcmp(key, "debug") && f->ready.load()) {
        auto &m = f->rev->machine();
        return snprintf(buf, n, "PROGRAM %02x MODE %d LEDS %02x run %d idle %d stored %d %d %d %d %d %d diff %d", m.mem[0x3f65], m.mem[0x3f45],
                        m.mem[0x3f3f], (int)m.dsp.run, (int)f->rev->panel().idle(), m.mem[0x3f67], m.mem[0x3f66], m.mem[0x3f68], m.mem[0x3f69],
                        m.mem[0x3f6a], m.mem[0x3f6d], m.mem[0x3f6e]);
    }
    if (!strcmp(key, "status")) return snprintf(buf, n, "%s", f->status);
    if (!strcmp(key, "meter_l")) return snprintf(buf, n, "%d", f->meter[0]);
    if (!strcmp(key, "meter_r")) return snprintf(buf, n, "%d", f->meter[1]);
    return 0;
}

/* the 224's headroom meter: five LEDs per channel for the level after the Input fader (24, 18, 12, 6, 0 dB below full scale) */
void update_meters(Fx *f, const int16_t *in, int frames) {
    static const float TH[5] = {0.0631f, 0.1259f, 0.2512f, 0.5012f, 0.944f};
    const float gain = powf(10.0f, f->p[P_INPUT] / 20.0f) / 32768.0f, fall = powf(0.984f, frames / 128.0f);
    bool changed = false;
    for (int c = 0; c < 2; c++) {
        int m = 0;
        for (int i = 0; i < frames; i++) m = std::max(m, std::abs((int)in[2 * i + c]));
        f->peak[c] = std::max(f->peak[c] * fall, m * gain);
        int k = 0;
        while (k < 5 && f->peak[c] >= TH[k]) k++;
        if (k != f->meter[c]) { f->meter[c] = k; changed = true; }
    }
    if (changed) f->rev_no++;
}

void process(void *inst, const int16_t *in, int16_t *out, int frames) {
    Fx *f = (Fx *)inst;
    update_meters(f, in, frames);
    if (!f->ready.load(std::memory_order_acquire) || frames > 128) { memcpy(out, in, (size_t)frames * 4); return; }
    if (f->dirty) apply(f);
    const float gain = powf(10.0f, f->p[P_INPUT] / 20.0f), mix = std::clamp(f->p[P_MIX] / 100.0f, 0.0f, 1.0f);
    for (int i = 0; i < frames; i++) { f->bl[i] = in[2 * i] / 32768.0f * gain; f->br[i] = in[2 * i + 1] / 32768.0f * gain; }
    f->rev->process(f->bl, f->br, f->ol, f->orr, frames);
    for (int i = 0; i < frames; i++) {
        const float m = f->mix_s + (mix - f->mix_s) * ((i + 1) / (float)frames);   /* a ramp per block keeps mix moves click-free */
        const float dry[2] = {in[2 * i] / 32768.0f, in[2 * i + 1] / 32768.0f}, wet[2] = {f->ol[i], f->orr[i]};
        for (int c = 0; c < 2; c++) {
            float s = (dry[c] * (1 - m) + wet[c] * m) * 32768.0f;
            if (!(s == s)) s = 0;
            out[2 * i + c] = (int16_t)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : lrintf(s));
        }
    }
    f->mix_s = mix;
}

void render(void *, int16_t *out, int frames) { memset(out, 0, (size_t)frames * 4); }

const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, process};
}  // namespace

extern "C" const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
