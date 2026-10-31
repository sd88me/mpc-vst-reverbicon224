/* Cineol-X 224 for MPC: the mpc_engine() glue (wrapper/engine.h) around the original-224 (v4.4) native hall core.
 * The core runs at 48 kHz (Engine48), the host at 44.1 kHz: a 160/147 rational filter pair brackets it. The prepared
 * bank (programs-v44.bank224, built off-device from the user's own ROMs) is read from the plugin's data folder; without
 * it the effect passes the dry signal. */
extern "C" {
#include "engine.h"
}
#include "core/engine48.hpp"
#include "core/control_scales.hpp"
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern char **environ;

namespace {
using namespace native_hall;

enum { P_ALGO, P_BASS, P_MID, P_XOVER, P_TREBLE, P_DEPTH, P_PREDELAY, P_DIFF, P_INPUT, P_MIX, P_DIRT,
       P_MODENH, P_DECAYOPT, P_OUTL, P_OUTR, NP };
const char *const KEYS[NP] = {"algorithm", "bass", "mid", "xover", "treble", "depth", "predelay", "diffusion", "input",
                              "mix", "dirt", "mode_enh", "decay_opt", "out_l", "out_r"};
const float DEFAULTS[NP] = {2, 17, 14, 5, 23, 21, 0, 1, 0, 100, 0, 1, 1, 0, 2};

/* Denormals in the filter tails cost ~5x the CPU (14% vs 3% on x86), so flush them for the duration of process() and
 * put the caller's mode back (MPC's own audio code may rely on it). */
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

/* The prepared bank comes from the user's own ROMs (Content/ROMs/), built once by a helper process (bankhelper-<arch>
 * next to the plugin's other files) that this engine starts in the background and follows through a progress file. */
#if defined(__aarch64__)
#define HELPER_ARCH "aarch64"
#elif defined(__arm__)
#define HELPER_ARCH "armv7"
#else
#define HELPER_ARCH "x86_64"
#endif

struct Fx {
    float p[NP];
    ProgramBank bank;
    bool have_bank = false;
    std::string dir;                 /* the plugin's Content folder */
    pid_t child = 0;
    int spawns = 0;
    float peak[2] = {0, 0};          /* headroom meter: decaying input peaks */
    int meter[2] = {0, 0};           /* LEDs lit per channel, 0..5 (24, 18, 12, 6, 0 dB) */
    bool failed = false;             /* the helper reported an error: don't restart it until the plugin is reloaded */
    char status[64] = "";
    unsigned rev = 1;
    std::chrono::steady_clock::time_point next_poll{};
    bool dirty = true;
    int program = -1;
    Engine48 eng;
    RationalFilter<160, 147, 64, 2> up;    /* 44.1 -> 48 */
    RationalFilter<147, 160, 64, 2> down;  /* 48 -> 44.1 */
    std::vector<std::array<float, 2>> fifo;
    unsigned rd = 0, wr = 0;
};

bool load_bank(Fx *f, const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/programs-v44.bank224", dir ? dir : ".");
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    std::vector<uint8_t> buf(sizeof(BankHeader) + sizeof(ProgramBank) + 1);
    size_t n = fread(buf.data(), 1, buf.size(), fp);
    fclose(fp);
    return read_bank(buf.data(), n, f->bank);
}

void init_engine(Fx *f);

void set_status(Fx *f, const char *text) {
    if (strcmp(f->status, text)) {
        snprintf(f->status, sizeof f->status, "%s", text);
        f->rev++;
    }
}

int count_roms(const Fx *f) {   /* files of the size of a 2716 EPROM in Content/ROMs: the helper decides which chips they are */
    std::string d = f->dir + "/ROMs";
    DIR *dp = opendir(d.c_str());
    if (!dp) return 0;
    int n = 0;
    while (dirent *e = readdir(dp)) {
        struct stat st;
        if (e->d_name[0] != '.' && !stat((d + "/" + e->d_name).c_str(), &st) && S_ISREG(st.st_mode) && st.st_size == 2048) n++;
    }
    closedir(dp);
    return n;
}

bool spawn_helper(Fx *f) {
    std::string helper = f->dir + "/bankhelper-" HELPER_ARCH, roms = f->dir + "/ROMs", out = f->dir + "/programs-v44.bank224",
                prog = f->dir + "/.import-progress";
    if (access(helper.c_str(), X_OK)) return false;
    std::vector<char *> env;   /* no LD_PRELOAD: the helper must not inherit MPC's addins */
    for (char **e = environ; e && *e; e++)
        if (strncmp(*e, "LD_PRELOAD=", 11)) env.push_back(*e);
    env.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    for (int fd = 0; fd < 3; fd++) posix_spawn_file_actions_addopen(&fa, fd, "/dev/null", O_RDWR, 0);
    char *argv[] = {(char *)helper.c_str(), (char *)roms.c_str(), (char *)out.c_str(), (char *)prog.c_str(), nullptr};
    int rc = posix_spawn(&f->child, helper.c_str(), &fa, nullptr, argv, env.data());
    posix_spawn_file_actions_destroy(&fa);
    if (rc) f->child = 0;
    return rc == 0;
}

/* Called about every 100 ms from the wrapper's display poll (never from the audio path): follow the import and load
 * the bank the moment it is published. */
void poll(Fx *f) {
    auto now = std::chrono::steady_clock::now();
    if (now < f->next_poll) return;
    f->next_poll = now + std::chrono::milliseconds(400);
    if (f->child) {
        int st;
        if (waitpid(f->child, &st, WNOHANG) == f->child) f->child = 0;
    }
    if (f->have_bank) {
        set_status(f, "BANK READY");
        return;
    }
    if (load_bank(f, f->dir.c_str())) {
        f->have_bank = true;
        init_engine(f);
        set_status(f, "BANK READY");
        return;
    }
    char line[160] = "", state[8] = "";
    int pct = 0;
    bool fresh = false;
    std::string pp = f->dir + "/.import-progress";
    struct stat st;
    if (FILE *fp = fopen(pp.c_str(), "r")) {
        if (fgets(line, sizeof line, fp)) {
            char *tab = strchr(line, '\t');
            if (tab) {
                snprintf(state, sizeof state, "%.*s", (int)std::min<ptrdiff_t>(tab - line, 7), line);
                pct = atoi(tab + 1);
                char *t2 = strchr(tab + 1, '\t');
                if (t2) {
                    char *nl = strchr(t2, '\n');
                    if (nl) *nl = 0;
                    memmove(line, t2 + 1, strlen(t2 + 1) + 1);
                } else line[0] = 0;
            }
        }
        fclose(fp);
        fresh = !stat(pp.c_str(), &st) && time(nullptr) - st.st_mtime < 120;
    }
    if (!strcmp(state, "ERR") && fresh) {
        f->failed = true;
        char t[64];
        snprintf(t, sizeof t, "%s", line[0] ? line : "IMPORT FAILED");
        set_status(f, t);
        return;
    }
    if (!strcmp(state, "RUN") && (f->child || fresh)) {   /* ours, or another instance's helper */
        char t[64];
        snprintf(t, sizeof t, "BUILDING BANK %d%%", pct);
        set_status(f, t);
        return;
    }
    if (f->failed) return;
    int n = count_roms(f);
    if (n < 5) {
        char t[64];
        snprintf(t, sizeof t, "ADD ROMS TO CONTENT/ROMS (%d/5)", n);
        set_status(f, t);
        return;
    }
    if (++f->spawns <= 3 && spawn_helper(f)) set_status(f, "BUILDING BANK 0%");
    else {
        f->failed = true;
        set_status(f, "BANK HELPER MISSING");
    }
}

void *create(const char *dir) {
    Fx *f = new Fx();
    memcpy(f->p, DEFAULTS, sizeof f->p);
    f->dir = dir ? dir : ".";
    f->have_bank = load_bank(f, dir);
    if (f->have_bank) init_engine(f);
    poll(f);
    f->up.prepare();
    f->down.prepare();
    f->fifo.assign(64, {});
    f->wr = 2;   /* two samples of latency keep the 48 -> 44.1 side from running dry */
    return f;
}

void destroy(void *inst) { delete (Fx *)inst; }
void midi(void *, const uint8_t *, int) {}

/* The one-time setup (filter tables, delay memory) costs several blocks of audio time on the Force, so it runs when the bank
 * loads (create, or the display poll), never inside the first process() call. */
void init_engine(Fx *f) {
    int prog = (int)f->p[P_ALGO];
    if (prog < 0) prog = 0;
    if (prog >= (int)program_count) prog = program_count - 1;
    f->eng.prepare(f->bank, (unsigned)prog);
    f->eng.activate(f->bank, (unsigned)prog);
    f->program = prog;
    f->dirty = true;
}

void apply(Fx *f) {
    if (!f->have_bank) return;
    int prog = (int)f->p[P_ALGO];
    if (prog < 0) prog = 0;
    if (prog >= (int)program_count) prog = program_count - 1;
    if (f->program < 0) init_engine(f);   /* normally done already, off the audio thread, when the bank loaded */
    Parameters q;
    q.program = (unsigned)prog;
    q.hall.bass = (int)f->p[P_BASS];
    q.hall.mid = (int)f->p[P_MID];
    q.hall.crossover = (int)f->p[P_XOVER];
    q.hall.treble = (int)f->p[P_TREBLE];
    q.hall.depth = (int)f->p[P_DEPTH];
    q.hall.predelay_ms = (int)f->p[P_PREDELAY] + predelay_minima[prog];
    q.hall.diffusion = (int)f->p[P_DIFF];
    q.hall.mode_enhancement = f->p[P_MODENH] >= 0.5f;
    q.hall.decay_optimization = f->p[P_DECAYOPT] >= 0.5f;
    q.input_db = f->p[P_INPUT];
    q.mix = f->p[P_MIX] / 100.0f;
    q.analog = true;
    float d = f->p[P_DIRT] / 100.0f;
    q.dirt = d * d;
    q.output_left = (int)f->p[P_OUTL];
    q.output_right = (int)f->p[P_OUTR];
    f->eng.set_parameters(q);
    f->program = prog;
    f->dirty = false;
}

void set_param(void *inst, const char *key, const char *val) {
    Fx *f = (Fx *)inst;
    for (int i = 0; i < NP; i++)
        if (!strcmp(key, KEYS[i])) {
            f->p[i] = (float)atof(val);
            f->dirty = true;
            return;
        }
}

int get_param(void *inst, const char *key, char *buf, int n) {
    Fx *f = (Fx *)inst;
    for (int i = 0; i < NP; i++)
        if (!strcmp(key, KEYS[i])) return snprintf(buf, n, "%.3f", (double)f->p[i]);
    if (!strcmp(key, "bass_display")) return snprintf(buf, n, "%.1f s", (double)scales::times[(int)f->p[P_BASS]]);
    if (!strcmp(key, "mid_display")) return snprintf(buf, n, "%.1f s", (double)scales::times[(int)f->p[P_MID]]);
    if (!strcmp(key, "xover_display")) return snprintf(buf, n, "%d Hz", scales::frequencies[(int)f->p[P_XOVER]]);
    if (!strcmp(key, "treble_display")) return snprintf(buf, n, "%d Hz", scales::frequencies[(int)f->p[P_TREBLE]]);
    if (!strcmp(key, "rom_status")) return snprintf(buf, n, "%d", f->have_bank ? 1 : 0);
    if (!strcmp(key, "display_rev")) {
        poll(f);
        return snprintf(buf, n, "%u", f->rev);
    }
    if (!strcmp(key, "status")) return snprintf(buf, n, "%s", f->status);
    if (!strcmp(key, "meter_l")) return snprintf(buf, n, "%d", f->meter[0]);
    if (!strcmp(key, "meter_r")) return snprintf(buf, n, "%d", f->meter[1]);
    return 0;
}

/* The headroom meter of the 224: five LEDs per channel for the input level after the Input fader. Peak with a fall of about
 * 12 dB per 250 ms, so a LED stays lit long enough to see; display_rev moves only when the number of lit LEDs changes. */
void update_meters(Fx *f, const int16_t *in, int frames) {
    static const float TH[5] = {0.0631f, 0.1259f, 0.2512f, 0.5012f, 0.944f};   /* -24 -18 -12 -6 -0.5 dB */
    const float gain = powf(10.0f, f->p[P_INPUT] / 20.0f) / 32768.0f, fall = powf(0.984f, frames / 128.0f);
    bool changed = false;
    for (int c = 0; c < 2; c++) {
        int m = 0;
        for (int i = 0; i < frames; i++) m = std::max(m, std::abs((int)in[2 * i + c]));
        f->peak[c] = std::max(f->peak[c] * fall, m * gain);
        int n = 0;
        while (n < 5 && f->peak[c] >= TH[n]) n++;
        if (n != f->meter[c]) {
            f->meter[c] = n;
            changed = true;
        }
    }
    if (changed) f->rev++;
}

void process(void *inst, const int16_t *in, int16_t *out, int frames) {
    Fx *f = (Fx *)inst;
    update_meters(f, in, frames);
    if (!f->have_bank) {
        memcpy(out, in, (size_t)frames * 4);
        return;
    }
    ScopedFTZ ftz;
    if (f->dirty) apply(f);
    const unsigned mask = (unsigned)f->fifo.size() - 1;
    for (int i = 0; i < frames; i++) {
        float x[2] = {in[2 * i] / 32768.0f, in[2 * i + 1] / 32768.0f};
        f->up.process(x, [&](const float *h) {
            float l, r;
            f->eng.process(h[0], h[1], l, r);
            float w[2] = {l, r};
            f->down.process(w, [&](const float *o) { f->fifo[f->wr++ & mask] = {o[0], o[1]}; });
        });
        std::array<float, 2> y = f->rd != f->wr ? f->fifo[f->rd++ & mask] : std::array<float, 2>{0, 0};
        for (int c = 0; c < 2; c++) {
            float s = y[c] * 32768.0f;
            if (!(s == s)) s = 0;
            out[2 * i + c] = (int16_t)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : lrintf(s));
        }
    }
}

void render(void *, int16_t *out, int frames) { memset(out, 0, (size_t)frames * 4); }

const mpc_engine_t ENGINE = {create, destroy, midi, set_param, get_param, render, process};
}  // namespace

extern "C" const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
