/* miniwave — core rack engine
 *
 * Constants, types, globals, rack management, render mixer,
 * state persistence, limiter, bus write, MIDI dispatch.
 * All platform-independent.
 */

#ifndef MINIWAVE_RACK_H
#define MINIWAVE_RACK_H

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "osc.h"
#include "json-helpers.h"
#include "instruments.h"
#include "seq.h"
#include "keyseq.h"
#include "fm-synth.h"
#include "ym2413.h"
#include "sub-synth.h"
#include "fm-drums.h"
#include "additive.h"
#include "phase-dist.h"
#include "bird.h"
#include "platform.h"

/* ── Constants ──────────────────────────────────────────────────────── */

#define SAMPLE_RATE      48000
#define CHANNELS         2
#define DEFAULT_PERIOD   64
#define DEFAULT_OSC_PORT 9000
#define DEFAULT_HTTP_PORT 8080
#define DEFAULT_MCAST_PORT 9001
#define MCAST_GROUP "239.0.0.42"  /* link-local multicast group */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LIMITER_THRESHOLD 0.7f
#define LIMITER_CEILING   0.95f

/* ── HTTP / SSE Server Constants ───────────────────────────────────── */

#define MAX_HTTP_CLIENTS  16
#define HTTP_BUF_SIZE     8192
#define SSE_BUF_SIZE      16384

/* ── Shared Memory Bus ──────────────────────────────────────────────── */

#define WAVEOS_BUS_SLOTS     8
#define WAVEOS_BUS_RING_SIZE 4096
#define WAVEOS_BUS_SHM_NAME  "/waveos-bus"

typedef struct {
    _Atomic int64_t write_pos;
    float           ring[WAVEOS_BUS_RING_SIZE * 2];
    char            name[32];
    _Atomic int     active;
} WaveosBusSlot;

typedef struct {
    uint32_t        magic;
    int             sample_rate;
    WaveosBusSlot   slots[WAVEOS_BUS_SLOTS];
} WaveosBus;

/* ── Globals ────────────────────────────────────────────────────────── */

static volatile int g_quit = 0;

static void sighandler(int sig) {
    (void)sig;
    g_quit = 1;
}

/* ── Audio backend flag (set at startup) ────────────────────────────── */

static int g_use_jack = 0; /* 1 = JACK active, 0 = native fallback */
static int g_bus_active = 0;    /* 1 = shared memory bus connected */
static int g_bus_slot = -1;     /* which bus slot we claimed */
static int g_mcast_active = 0;  /* 1 = multicast broadcasting */

/* ── JSON string escaping (shared utility) ─────────────────────────── */

#ifndef JSON_ESCAPE_DEFINED
#define JSON_ESCAPE_DEFINED
static int json_escape(char *dst, int max, const char *src) {
    int j = 0;
    for (int i = 0; src[i] && j < max - 2; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { if (j+2>=max) break; dst[j++]='\\'; dst[j++]=c; }
        else if (c == '\n') { if (j+2>=max) break; dst[j++]='\\'; dst[j++]='n'; }
        else if (c == '\r') { if (j+2>=max) break; dst[j++]='\\'; dst[j++]='r'; }
        else if ((unsigned char)c < 0x20) continue;
        else dst[j++] = c;
    }
    dst[j] = '\0';
    return j;
}
#endif

/* ── Deferred free queue (RCU-lite) ─────────────────────────────────── */
/* Slot swaps queue old state here. The render thread drains it at the
 * start of each render_mix call — safe because render is the only
 * consumer that could have been holding a pointer to the old state. */

#define DEFERRED_FREE_MAX 32

typedef struct {
    void *ptr;
    void (*destroy)(void *state);  /* instrument destructor, or NULL for plain free */
} DeferredFree;

static DeferredFree g_deferred_free[DEFERRED_FREE_MAX];
static _Atomic int  g_deferred_free_count = 0;

static void deferred_free_push(void *ptr, void (*destroy)(void *)) {
    int idx = atomic_fetch_add(&g_deferred_free_count, 1);
    if (idx < DEFERRED_FREE_MAX) {
        g_deferred_free[idx].ptr = ptr;
        g_deferred_free[idx].destroy = destroy;
    } else {
        /* Queue full — fall back to immediate free (risky but bounded) */
        if (destroy) destroy(ptr);
        else free(ptr);
        atomic_fetch_sub(&g_deferred_free_count, 1);
    }
}

/* Reader count — HTTP/SSE handlers increment while reading slot state.
 * deferred_free_drain waits for readers to clear before freeing. */
static _Atomic int g_slot_readers = 0;

/* Debug counters — zero-cost when not logging */
static _Atomic uint64_t g_slot_read_enters = 0;
static _Atomic uint64_t g_slot_read_exits = 0;
static _Atomic uint64_t g_deferred_free_delayed = 0;
static _Atomic uint64_t g_deferred_free_drained = 0;

static inline void slot_read_begin(void) {
    atomic_fetch_add(&g_slot_readers, 1);
    atomic_fetch_add(&g_slot_read_enters, 1);
}
static inline void slot_read_end(void) {
    atomic_fetch_sub(&g_slot_readers, 1);
    atomic_fetch_add(&g_slot_read_exits, 1);
}

static void deferred_free_drain(void) {
    /* Don't free while any reader (HTTP/SSE/OSC/MIDI) is active */
    if (atomic_load(&g_slot_readers) > 0) {
        atomic_fetch_add(&g_deferred_free_delayed, 1);
        return;
    }

    int count = atomic_exchange(&g_deferred_free_count, 0);
    for (int i = 0; i < count && i < DEFERRED_FREE_MAX; i++) {
        DeferredFree *df = &g_deferred_free[i];
        if (df->ptr) {
            if (df->destroy) df->destroy(df->ptr);
            free(df->ptr);
            df->ptr = NULL;
            df->destroy = NULL;
            atomic_fetch_add(&g_deferred_free_drained, 1);
        }
    }
}

/* ── Master Limiter ─────────────────────────────────────────────────── */

static float g_master_limiter_env = 0.0f;

static inline float master_limiter(float sample) {
    float absval = fabsf(sample);

    if (absval > g_master_limiter_env)
        g_master_limiter_env += (absval - g_master_limiter_env) * 0.01f;
    else
        g_master_limiter_env += (absval - g_master_limiter_env) * 0.0001f;

    float gain = 1.0f;
    if (g_master_limiter_env > LIMITER_THRESHOLD) {
        gain = LIMITER_THRESHOLD / g_master_limiter_env;
    }

    sample *= gain;

    if (fabsf(sample) > LIMITER_THRESHOLD) {
        sample = tanhf(sample) * LIMITER_CEILING;
    }

    return sample;
}

/* ── Instrument Registry ────────────────────────────────────────────── */

#define MAX_INSTRUMENT_TYPES 32

static InstrumentType *g_type_registry[MAX_INSTRUMENT_TYPES];
static int             g_n_types = 0;

/* Forward declarations */
static void state_mark_dirty(void);

static void rack_register_type(InstrumentType *type) {
    if (g_n_types < MAX_INSTRUMENT_TYPES) {
        g_type_registry[g_n_types++] = type;
        fprintf(stderr, "[miniwave] registered type: %s (%s)\n",
                type->name, type->display_name);
    }
}

static int rack_find_type(const char *name) {
    for (int i = 0; i < g_n_types; i++) {
        if (strcmp(g_type_registry[i]->name, name) == 0) return i;
    }
    return -1;
}

/* ── Rack Management ────────────────────────────────────────────────── */

static Rack g_rack;
static char g_midi_device_name[128] = "";

static void rack_init(void) {
    memset(&g_rack, 0, sizeof(g_rack));
    g_rack.master_volume = 0.8f;
    for (int i = 0; i < MAX_SLOTS; i++) {
        g_rack.slots[i].active = 0;
        g_rack.slots[i].type_idx = -1;
        g_rack.slots[i].state = NULL;
        g_rack.slots[i].volume = 1.0f;
        g_rack.slots[i].mute = 0;
        g_rack.slots[i].solo = 0;
    }

    /* Register built-in types */
    rack_register_type(&fm_synth_type);
    rack_register_type(&ym2413_type);
    rack_register_type(&sub_synth_type);
    rack_register_type(&fm_drums_type);
    rack_register_type(&additive_type);
    rack_register_type(&phase_dist_type);
    rack_register_type(&bird_type);
}

static void state_save(void);  /* forward decl */

static int rack_set_slot(int channel, const char *type_name) {
    if (channel < 0 || channel >= MAX_SLOTS) return -1;

    /* Save current state before destroying old instrument */
    state_save();

    int tidx = rack_find_type(type_name);
    if (tidx < 0) {
        fprintf(stderr, "[miniwave] unknown instrument type: %s\n", type_name);
        return -1;
    }

    RackSlot *slot = &g_rack.slots[channel];

    /* Prepare new state BEFORE touching the slot (avoids race with render thread) */
    InstrumentType *itype = g_type_registry[tidx];
    void *new_state = calloc(1, itype->state_size);
    if (!new_state) return -1;
    itype->init(new_state);

    /* Capture old state */
    void *old_state = slot->state;
    int old_type_idx = slot->type_idx;
    int was_active = slot->active;

    /* Deactivate slot — atomic store ensures render/MIDI see it immediately */
    atomic_store(&slot->active, 0);
    atomic_fetch_add(&slot->gen, 1);

    /* Wait for in-flight readers to finish.
     * Readers (HTTP/SSE/OSC/MIDI) check active before dereferencing.
     * Once active=0, no new readers enter. Wait for existing ones. */
    int wait_us = 0;
    while (atomic_load(&g_slot_readers) > 0 && wait_us < 100000) {
        usleep(1000);
        wait_us += 1000;
    }
    /* Also wait one audio buffer cycle so render_mix finishes any
     * in-flight render that snapshotted state before active=0 */
    usleep(25000); /* ~1 buffer at 48kHz/1024 */

    /* Capture old slot-level seq/keyseq */
    MiniSeq *old_seq = slot->seq;
    KeySeq  *old_keyseq = slot->keyseq;

    /* Swap in new instrument */
    slot->state = new_state;
    slot->type_idx = tidx;
    slot->volume = 1.0f;
    slot->mute = 0;
    slot->solo = 0;
    slot->cents_mod = 0.0f;
    slot->pitch_bend = 0.0f;
    slot->pitch_bend_range = 2.0f;
    slot->mod_wheel = 0.0f;
    slot->vibrato_phase = 0.0f;

    /* Allocate slot-level seq and keyseq */
    slot->seq = calloc(1, sizeof(MiniSeq));
    slot->keyseq = calloc(1, sizeof(KeySeq));
    if (slot->seq) {
        seq_init(slot->seq);
        seq_bind(slot->seq, new_state, itype->midi, (uint8_t)(channel & 0x0F));
    }
    if (slot->keyseq) {
        keyseq_init(slot->keyseq);
        keyseq_bind(slot->keyseq, new_state, itype->midi,
                     itype->set_param ? itype->set_param : NULL,
                     (uint8_t)(channel & 0x0F));
    }

    atomic_fetch_add(&slot->gen, 1);
    atomic_store(&slot->active, 1);

    /* Deferred free — old state cleaned up by render thread next cycle */
    if (was_active && old_state && old_type_idx >= 0) {
        InstrumentType *old_type = g_type_registry[old_type_idx];
        deferred_free_push(old_state, old_type->destroy);
    }
    if (old_seq) deferred_free_push(old_seq, NULL);
    if (old_keyseq) deferred_free_push(old_keyseq, NULL);

    fprintf(stderr, "[miniwave] slot %d = %s\n", channel, itype->display_name);
    state_mark_dirty();
    return 0;
}

static void rack_clear_slot(int channel) {
    if (channel < 0 || channel >= MAX_SLOTS) return;

    RackSlot *slot = &g_rack.slots[channel];
    void *old_state = slot->state;
    int old_type_idx = slot->type_idx;
    int was_active = slot->active;

    atomic_store(&slot->active, 0);
    atomic_fetch_add(&slot->gen, 1);

    /* Wait for readers + one render cycle */
    { int wu = 0; while (atomic_load(&g_slot_readers) > 0 && wu < 100000) { usleep(1000); wu += 1000; } }
    usleep(25000);

    MiniSeq *old_seq = slot->seq;
    KeySeq  *old_keyseq = slot->keyseq;

    slot->state = NULL;
    slot->type_idx = -1;
    slot->volume = 1.0f;
    slot->mute = 0;
    slot->solo = 0;
    slot->seq = NULL;
    slot->keyseq = NULL;
    slot->cents_mod = 0.0f;

    /* Deferred free */
    if (was_active && old_state && old_type_idx >= 0) {
        InstrumentType *itype = g_type_registry[old_type_idx];
        deferred_free_push(old_state, itype->destroy);
    }
    if (old_seq) deferred_free_push(old_seq, NULL);
    if (old_keyseq) deferred_free_push(old_keyseq, NULL);

    fprintf(stderr, "[miniwave] slot %d cleared\n", channel);
    state_mark_dirty();
}

/* ══════════════════════════════════════════════════════════════════════
 *  State Persistence — auto-save/load rack to ~/.config/miniwave/rack.json
 * ══════════════════════════════════════════════════════════════════════ */

static char g_state_path[512] = "";
static char g_patches_path[512] = "";
static char g_keyseq_presets_path[512] = "";
static float g_bpm = 120.0f; /* global tempo */
static volatile int g_state_dirty = 0;

/* ── User patch storage ───────────────────────────────────────────────── */
/* Simple JSON file: array of {name, type, params:{...}} objects.
 * Max 256 patches. Loaded at startup, saved on mutation. */

#define MAX_USER_PATCHES 256
#define PATCH_NAME_MAX 64
#define PATCH_DATA_MAX 2048

typedef struct {
    char name[PATCH_NAME_MAX];
    char type[32];              /* instrument type name */
    char data[PATCH_DATA_MAX];  /* JSON params blob from json_save */
    char keyseq_dsl[512];       /* keyseq DSL string */
    char seq_dsl[256];          /* seq DSL string */
} UserPatch;

static UserPatch g_patches[MAX_USER_PATCHES];
static int g_num_patches = 0;

static void patches_save(void) {
    if (!g_patches_path[0]) return;
    FILE *f = fopen(g_patches_path, "w");
    if (!f) return;
    fprintf(f, "[\n");
    for (int i = 0; i < g_num_patches; i++) {
        char esc_name[128], esc_data[PATCH_DATA_MAX], esc_ks[1024], esc_seq[512];
        json_escape(esc_name, sizeof(esc_name), g_patches[i].name);
        json_escape(esc_data, sizeof(esc_data), g_patches[i].data);
        json_escape(esc_ks, sizeof(esc_ks), g_patches[i].keyseq_dsl);
        json_escape(esc_seq, sizeof(esc_seq), g_patches[i].seq_dsl);
        fprintf(f, "  {\"name\":\"%s\",\"type\":\"%s\",\"params\":\"%s\"",
                esc_name, g_patches[i].type, esc_data);
        if (g_patches[i].keyseq_dsl[0])
            fprintf(f, ",\"keyseq_dsl\":\"%s\"", esc_ks);
        if (g_patches[i].seq_dsl[0])
            fprintf(f, ",\"seq_dsl\":\"%s\"", esc_seq);
        fprintf(f, "}%s\n", (i < g_num_patches - 1) ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
}

static void patches_load(void) {
    if (!g_patches_path[0]) return;
    FILE *f = fopen(g_patches_path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 512 * 1024) { fclose(f); return; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    g_num_patches = 0;
    /* Simple array-of-objects parser */
    const char *p = buf;
    while (g_num_patches < MAX_USER_PATCHES) {
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *end = strchr(obj, '}');
        if (!end) break;

        int olen = (int)(end - obj + 1);
        char *ojson = calloc(1, (size_t)(olen + 1));
        if (!ojson) break;
        memcpy(ojson, obj, (size_t)olen);

        UserPatch *up = &g_patches[g_num_patches];
        memset(up, 0, sizeof(*up));
        json_get_string(ojson, "name", up->name, PATCH_NAME_MAX);
        json_get_string(ojson, "type", up->type, sizeof(up->type));
        json_get_string(ojson, "params", up->data, PATCH_DATA_MAX);
        json_get_string(ojson, "keyseq_dsl", up->keyseq_dsl, sizeof(up->keyseq_dsl));
        json_get_string(ojson, "seq_dsl", up->seq_dsl, sizeof(up->seq_dsl));

        if (up->name[0] && up->type[0]) g_num_patches++;
        free(ojson);
        p = end + 1;
    }
    free(buf);
    fprintf(stderr, "[miniwave] loaded %d user patches from %s\n", g_num_patches, g_patches_path);
}

static int patch_find(const char *name) {
    for (int i = 0; i < g_num_patches; i++)
        if (strcmp(g_patches[i].name, name) == 0) return i;
    return -1;
}

/* ── KeySeq preset storage ─────────────────────────────────────────── */

#define MAX_KEYSEQ_PRESETS 64

typedef struct {
    char name[PATCH_NAME_MAX];
    char dsl[512];
} KeySeqPreset;

static KeySeqPreset g_ks_presets[MAX_KEYSEQ_PRESETS];
static int g_num_ks_presets = 0;

static const char *FACTORY_KS_PRESETS[][2] = {
    {"My Sequence",    "t0.125; gated; algo; n:n+1; v:v-0.03; end:v<=0"},
    {"Shimmer",        "t0.125; g0.9; loop; 0,12,7,12; v1,0.7,0.5,0.3"},
    {"Climb",          "t0.125; gated; algo; n:n+1; v:v-0.03; end:v<=0"},
    {"Siren",          "t0.5; gated; algo; n:root+i%2*10-5; v:v; end:v<=0"},
    {"Rapid Fire",     "t0.2; g0.5; gated; algo; n:n; v:v-0.005; end:v<=0"},
    {"Circle of 5ths", "t0.125; gated; algo; n:root+i*7%12; v:v-0.01; end:v<=0"},
    {"Bounce",         "t0.125; gated; algo; n:root+abs(i%8-4)*3; v:v-0.02; end:v<=0"},
    {"Scatter",        "t0.1; g0.8; gated; algo; n:root+i*13%24-12; v:v-0.015; end:v<=0"},
    {NULL, NULL}
};

static void ks_presets_save(void) {
    if (!g_keyseq_presets_path[0]) return;
    FILE *f = fopen(g_keyseq_presets_path, "w");
    if (!f) return;
    fprintf(f, "[\n");
    for (int i = 0; i < g_num_ks_presets; i++) {
        char en[128], ed[1024];
        json_escape(en, sizeof(en), g_ks_presets[i].name);
        json_escape(ed, sizeof(ed), g_ks_presets[i].dsl);
        fprintf(f, "  {\"name\":\"%s\",\"dsl\":\"%s\"}%s\n",
                en, ed, (i < g_num_ks_presets - 1) ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
}

static void ks_presets_load(void) {
    if (!g_keyseq_presets_path[0]) return;
    FILE *f = fopen(g_keyseq_presets_path, "r");
    if (!f) {
        /* Seed factory presets */
        for (int i = 0; FACTORY_KS_PRESETS[i][0] && g_num_ks_presets < MAX_KEYSEQ_PRESETS; i++) {
            strncpy(g_ks_presets[g_num_ks_presets].name, FACTORY_KS_PRESETS[i][0], PATCH_NAME_MAX - 1);
            strncpy(g_ks_presets[g_num_ks_presets].dsl, FACTORY_KS_PRESETS[i][1], 511);
            g_num_ks_presets++;
        }
        ks_presets_save();
        fprintf(stderr, "[miniwave] seeded %d factory keyseq presets\n", g_num_ks_presets);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 128 * 1024) { fclose(f); return; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0'; fclose(f);

    g_num_ks_presets = 0;
    const char *p = buf;
    while (g_num_ks_presets < MAX_KEYSEQ_PRESETS) {
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *end = strchr(obj, '}');
        if (!end) break;
        int olen = (int)(end - obj + 1);
        char *oj = calloc(1, (size_t)(olen + 1));
        if (!oj) break;
        memcpy(oj, obj, (size_t)olen);
        KeySeqPreset *kp = &g_ks_presets[g_num_ks_presets];
        memset(kp, 0, sizeof(*kp));
        json_get_string(oj, "name", kp->name, PATCH_NAME_MAX);
        json_get_string(oj, "dsl", kp->dsl, sizeof(kp->dsl));
        if (kp->name[0]) g_num_ks_presets++;
        free(oj);
        p = end + 1;
    }
    free(buf);
    fprintf(stderr, "[miniwave] loaded %d keyseq presets\n", g_num_ks_presets);
}

static int ks_preset_find(const char *name) {
    for (int i = 0; i < g_num_ks_presets; i++)
        if (strcmp(g_ks_presets[i].name, name) == 0) return i;
    return -1;
}

static void sse_mark_dirty(void); /* forward decl */
static void state_mark_dirty(void) { g_state_dirty = 1; sse_mark_dirty(); }

/* SSE push-on-change flags */
static _Atomic int g_sse_rack_dirty = 1;    /* 1 = broadcast rack_status on next check */
static _Atomic int g_sse_detail_dirty = 1;  /* 1 = broadcast ch_status on next check */

static void sse_mark_dirty(void) {
    atomic_store(&g_sse_rack_dirty, 1);
    atomic_store(&g_sse_detail_dirty, 1);
}

static void state_init_path(void) {
    const char *cfg = getenv("XDG_CONFIG_HOME");
    if (cfg && cfg[0]) {
        snprintf(g_state_path, sizeof(g_state_path), "%s/miniwave/rack.json", cfg);
        snprintf(g_patches_path, sizeof(g_patches_path), "%s/miniwave/patches.json", cfg);
        snprintf(g_keyseq_presets_path, sizeof(g_keyseq_presets_path), "%s/miniwave/keyseq_presets.json", cfg);
    } else {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(g_state_path, sizeof(g_state_path), "%s/.config/miniwave/rack.json", home);
        snprintf(g_patches_path, sizeof(g_patches_path), "%s/.config/miniwave/patches.json", home);
        snprintf(g_keyseq_presets_path, sizeof(g_keyseq_presets_path), "%s/.config/miniwave/keyseq_presets.json", home);
    }
}

static void state_mkdir(void) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", g_state_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        char *s2 = strrchr(dir, '/');
        if (s2) { *s2 = '\0'; mkdir(dir, 0755); *s2 = '/'; }
        mkdir(dir, 0755);
    }
}

static void state_save(void) {
    if (!g_state_path[0]) return;
    state_mkdir();

    FILE *f = fopen(g_state_path, "w");
    if (!f) return;

    fprintf(f, "{\n  \"master_volume\": %.4f,\n  \"slots\": [\n",
            (double)g_rack.master_volume);

    for (int i = 0; i < MAX_SLOTS; i++) {
        RackSlot *slot = &g_rack.slots[i];
        fprintf(f, "    {");

        if (slot->active && slot->state && slot->type_idx >= 0) {
            InstrumentType *itype = g_type_registry[slot->type_idx];
            fprintf(f, "\"type\":\"%s\",\"volume\":%.4f,\"mute\":%d,\"solo\":%d",
                    itype->name, (double)slot->volume, slot->mute, slot->solo);

            if (itype->json_save) {
                char inst_buf[4096];
                int n = itype->json_save(slot->state, inst_buf, (int)sizeof(inst_buf));
                if (n > 0) fprintf(f, ",%s", inst_buf);
            }
            /* Save slot-level keyseq DSL */
            if (slot->keyseq && slot->keyseq->enabled && slot->keyseq->source[0]) {
                char esc[1024];
                json_escape(esc, sizeof(esc), slot->keyseq->source);
                fprintf(f, ",\"keyseq_dsl\":\"%s\"", esc);
            }
            /* Save slot-level seq DSL */
            if (slot->seq && slot->seq->source[0]) {
                char esc[1024];
                json_escape(esc, sizeof(esc), slot->seq->source);
                fprintf(f, ",\"seq_dsl\":\"%s\"", esc);
            }
        }

        fprintf(f, "}%s\n", (i < MAX_SLOTS - 1) ? "," : "");
    }

    fprintf(f, "  ]\n}\n");
    fclose(f);
}

static void state_load(void) {
    if (!g_state_path[0]) return;

    FILE *f = fopen(g_state_path, "r");
    if (!f) {
        fprintf(stderr, "[miniwave] no saved state at %s\n", g_state_path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0 || len > 65536) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);

    char *json = calloc(1, (size_t)(len + 1));
    if (!json) { fclose(f); return; }
    size_t nread = fread(json, 1, (size_t)len, f);
    fclose(f);
    if (nread == 0) { free(json); return; }

    float mv;
    if (json_get_float(json, "master_volume", &mv) == 0)
        g_rack.master_volume = mv;

    const char *slots_start = strstr(json, "\"slots\"");
    if (!slots_start) { free(json); return; }
    slots_start = strchr(slots_start, '[');
    if (!slots_start) { free(json); return; }
    slots_start++;

    for (int i = 0; i < MAX_SLOTS; i++) {
        const char *obj = strchr(slots_start, '{');
        if (!obj) break;

        int depth = 0;
        const char *end = obj;
        do {
            if (*end == '{') depth++;
            else if (*end == '}') depth--;
            end++;
        } while (depth > 0 && *end);

        int slen = (int)(end - obj);
        char *slot_json = calloc(1, (size_t)(slen + 1));
        if (!slot_json) break;
        memcpy(slot_json, obj, (size_t)slen);

        char type_name[64] = "";
        if (json_get_string(slot_json, "type", type_name, sizeof(type_name)) == 0 && type_name[0]) {
            if (rack_set_slot(i, type_name) == 0) {
                RackSlot *slot = &g_rack.slots[i];
                InstrumentType *itype = g_type_registry[slot->type_idx];

                float vol;
                int ival;
                if (json_get_float(slot_json, "volume", &vol) == 0) slot->volume = vol;
                if (json_get_int(slot_json, "mute", &ival) == 0) slot->mute = ival;
                if (json_get_int(slot_json, "solo", &ival) == 0) slot->solo = ival;

                if (itype->json_load) {
                    itype->json_load(slot->state, slot_json);
                }

                /* Restore slot-level keyseq DSL */
                char ks_dsl[512] = "";
                if (slot->keyseq && json_get_string(slot_json, "keyseq_dsl", ks_dsl, sizeof(ks_dsl)) == 0 && ks_dsl[0]) {
                    keyseq_parse(slot->keyseq, ks_dsl);
                }
                /* Restore slot-level seq DSL */
                char seq_dsl[512] = "";
                if (slot->seq && json_get_string(slot_json, "seq_dsl", seq_dsl, sizeof(seq_dsl)) == 0 && seq_dsl[0]) {
                    seq_parse(slot->seq, seq_dsl);
                }
            }
        }

        free(slot_json);
        slots_start = end;
    }

    free(json);
    fprintf(stderr, "[miniwave] state restored from %s\n", g_state_path);
}

/* ── MIDI dispatch (raw bytes → rack) ──────────────────────────────── */

/* Optional callback for broadcasting MIDI events (set by server after init) */
static void (*g_midi_broadcast)(int ch, int note, int vel, int is_on) = NULL;

static inline void midi_dispatch_raw(const uint8_t *data, int len) {
    if (len < 1) return;
    uint8_t status = data[0];
    int ch = status & 0x0F;
    if (ch < 0 || ch >= MAX_SLOTS) return;

    slot_read_begin();
    RackSlot *slot = &g_rack.slots[ch];
    if (!atomic_load(&slot->active)) { slot_read_end(); return; }
    void *st = slot->state;
    if (!st) { slot_read_end(); return; }
    InstrumentType *itype = g_type_registry[slot->type_idx];

    uint8_t type = status & 0xF0;
    uint8_t d1 = (len > 1) ? data[1] : 0;
    uint8_t d2 = (len > 2) ? data[2] : 0;

    /* Slot-level keyseq intercept — catches notes before instrument sees them */
    KeySeq *ks = slot->keyseq;
    if (ks && !ks->firing && ks->enabled && ks->num_steps > 0) {
        if (type == 0x90 && d2 > 0) {
            if (keyseq_note_on(ks, d1, d2)) {
                if (g_midi_broadcast) g_midi_broadcast(ch, d1, d2, 1);
                slot_read_end(); return;
            }
        } else if (type == 0x80 || (type == 0x90 && d2 == 0)) {
            if (keyseq_note_off(ks, d1)) {
                if (g_midi_broadcast) g_midi_broadcast(ch, d1, 0, 0);
                slot_read_end(); return;
            }
        }
    }

    switch (type) {
    case 0x90:
        if (d2 > 0 && slot->mono) {
            /* mono: kill previous note before playing new one */
            if (slot->last_note >= 0 && slot->last_note != d1)
                itype->midi(st, (uint8_t)(0x80 | (status & 0x0F)),
                            (uint8_t)slot->last_note, 0);
        }
        if (d2 > 0 && slot->legato && slot->last_note >= 0) {
            /* legato: set up portamento glide */
            slot->glide_from = 440.0f * powf(2.0f, (float)(slot->last_note - 69) / 12.0f);
            slot->glide_to   = 440.0f * powf(2.0f, (float)(d1 - 69) / 12.0f);
            slot->glide_pos  = 0.0f;
        }
        if (d2 > 0) slot->last_note = d1;
        else if (slot->mono) slot->last_note = -1;
        itype->midi(st, status, d1, d2);
        if (g_midi_broadcast) g_midi_broadcast(ch, d1, d2, d2 > 0 ? 1 : 0);
        atomic_store(&g_sse_detail_dirty, 1);
        break;
    case 0x80:
        if (slot->mono && d1 == slot->last_note) slot->last_note = -1;
        itype->midi(st, status, d1, d2);
        if (g_midi_broadcast) g_midi_broadcast(ch, d1, 0, 0);
        atomic_store(&g_sse_detail_dirty, 1);
        break;
    case 0xE0: {
        /* Pitch bend: 14-bit value, center=8192 → -1.0 to +1.0 */
        int bend14 = (int)d1 | ((int)d2 << 7);
        slot->pitch_bend = (float)(bend14 - 8192) / 8192.0f;
        itype->midi(st, status, d1, d2);
        break;
    }
    case 0xB0:
        /* CC1 = mod wheel → vibrato LFO depth */
        if (d1 == 1) {
            slot->mod_wheel = (float)d2 / 127.0f;
        }
        itype->midi(st, status, d1, d2);
        state_mark_dirty();
        break;
    case 0xC0: case 0xD0:
        itype->midi(st, status, d1, 0);
        state_mark_dirty();
        break;
    }
    slot_read_end();
}

/* ── Render Mixer ──────────────────────────────────────────────────── */

static void render_mix(float *mix_buf, float *slot_buf, int frames, int sample_rate) {
    /* Drain deferred free queue — safe here because render is the
     * thread that was holding pointers to old slot state. */
    deferred_free_drain();

    memset(mix_buf, 0, sizeof(float) * (size_t)(frames * CHANNELS));

    int any_solo = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (atomic_load(&g_rack.slots[i].active) && g_rack.slots[i].solo) {
            any_solo = 1;
            break;
        }
    }

    for (int i = 0; i < MAX_SLOTS; i++) {
        RackSlot *slot = &g_rack.slots[i];
        if (!atomic_load(&slot->active)) continue;
        void *st = slot->state;
        if (!st) continue;
        if (slot->mute) continue;
        if (any_solo && !slot->solo) continue;

        InstrumentType *itype = g_type_registry[slot->type_idx];

        /* Snapshot seq/keyseq pointers — stable for this render cycle */
        MiniSeq *s_seq = slot->seq;
        KeySeq  *s_ks  = slot->keyseq;

        /* Tick slot-level seq/keyseq per-sample before rendering.
         * Events fire at buffer boundaries (acceptable latency). */
        float tick_dt = 1.0f / (float)sample_rate;
        for (int si = 0; si < frames; si++) {
            if (s_seq) seq_tick(s_seq, tick_dt);
            if (s_ks)  keyseq_tick(s_ks, tick_dt);
        }

        /* Legato portamento glide → cents_mod */
        if (slot->legato && slot->glide_pos < 1.0f && slot->glide_to > 0.01f) {
            float glide_dt = (float)frames / (float)sample_rate;
            slot->glide_pos += glide_dt * slot->glide_rate;
            if (slot->glide_pos > 1.0f) slot->glide_pos = 1.0f;
            /* smoothstep for tasty curve */
            float t = slot->glide_pos;
            t = t * t * (3.0f - 2.0f * t);
            float freq = slot->glide_from + (slot->glide_to - slot->glide_from) * t;
            /* convert freq offset to cents relative to glide_to */
            float cents = 1200.0f * log2f(freq / slot->glide_to);
            slot->cents_mod += cents;
        }

        /* Pitch bend → cents */
        if (slot->pitch_bend != 0.0f) {
            slot->cents_mod += slot->pitch_bend * slot->pitch_bend_range * 100.0f;
        }

        /* Mod wheel → vibrato LFO (5.5 Hz sine, depth scaled by mod_wheel) */
        if (slot->mod_wheel > 0.001f) {
            float vib_rate = 5.5f; /* Hz */
            float vib_depth = slot->mod_wheel * 50.0f; /* max ±50 cents */
            float dt_total = (float)frames / (float)sample_rate;
            slot->vibrato_phase += vib_rate * dt_total * 6.2831853f;
            if (slot->vibrato_phase > 6.2831853f)
                slot->vibrato_phase -= 6.2831853f;
            slot->cents_mod += sinf(slot->vibrato_phase) * vib_depth;
        }

        /* Copy keyseq cents_mod to slot, then slot cents_mod to instrument */
        if (s_ks) slot->cents_mod += s_ks->cents_mod;
        {
            float cm = slot->cents_mod;
            if (strcmp(itype->name, "fm-synth") == 0) ((FMSynth *)st)->cents_mod = cm;
            else if (strcmp(itype->name, "sub-synth") == 0) ((SubSynth *)st)->cents_mod = cm;
            else if (strcmp(itype->name, "ym2413") == 0) ((YM2413State *)st)->cents_mod = cm;
            else if (strcmp(itype->name, "fm-drums") == 0) ((FMDrumState *)st)->cents_mod = cm;
            else if (strcmp(itype->name, "additive") == 0) ((AdditiveState *)st)->cents_mod = cm;
            else if (strcmp(itype->name, "phase-dist") == 0) ((PhaseDistState *)st)->cents_mod = cm;
        }
        slot->cents_mod = 0; /* reset for next frame */

        memset(slot_buf, 0, sizeof(float) * (size_t)(frames * CHANNELS));
        itype->render(st, slot_buf, frames, sample_rate);

        float vol = slot->volume;
        for (int j = 0; j < frames * CHANNELS; j++) {
            mix_buf[j] += slot_buf[j] * vol;
        }
    }

    float mv = g_rack.master_volume;
    for (int j = 0; j < frames * CHANNELS; j++) {
        mix_buf[j] *= mv;
        mix_buf[j] = master_limiter(mix_buf[j]);
    }
}

/* ── Bus Write ──────────────────────────────────────────────────────── */

static void bus_write(WaveosBus *bus, int slot, float *stereo_frames, int count) {
    WaveosBusSlot *s = &bus->slots[slot];
    int64_t wp = atomic_load(&s->write_pos);
    for (int i = 0; i < count; i++) {
        int idx = (int)((wp + i) % WAVEOS_BUS_RING_SIZE) * 2;
        s->ring[idx]     = stereo_frames[i * 2];
        s->ring[idx + 1] = stereo_frames[i * 2 + 1];
    }
    atomic_store(&s->write_pos, wp + count);
}

#endif
