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

static void deferred_free_drain(void) {
    int count = atomic_exchange(&g_deferred_free_count, 0);
    for (int i = 0; i < count && i < DEFERRED_FREE_MAX; i++) {
        DeferredFree *df = &g_deferred_free[i];
        if (df->ptr) {
            if (df->destroy) df->destroy(df->ptr);
            free(df->ptr);
            df->ptr = NULL;
            df->destroy = NULL;
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
}

static int rack_set_slot(int channel, const char *type_name) {
    if (channel < 0 || channel >= MAX_SLOTS) return -1;

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
static volatile int g_state_dirty = 0;

static void state_mark_dirty(void) { g_state_dirty = 1; }

static void state_init_path(void) {
    const char *cfg = getenv("XDG_CONFIG_HOME");
    if (cfg && cfg[0]) {
        snprintf(g_state_path, sizeof(g_state_path), "%s/miniwave/rack.json", cfg);
    } else {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(g_state_path, sizeof(g_state_path), "%s/.config/miniwave/rack.json", home);
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

    RackSlot *slot = &g_rack.slots[ch];
    if (!atomic_load(&slot->active)) return;
    void *st = slot->state;
    if (!st) return;
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
                return;
            }
        } else if (type == 0x80 || (type == 0x90 && d2 == 0)) {
            if (keyseq_note_off(ks, d1)) {
                if (g_midi_broadcast) g_midi_broadcast(ch, d1, 0, 0);
                return;
            }
        }
    }

    switch (type) {
    case 0x90:
        itype->midi(st, status, d1, d2);
        if (g_midi_broadcast) g_midi_broadcast(ch, d1, d2, d2 > 0 ? 1 : 0);
        break;
    case 0x80:
        itype->midi(st, status, d1, d2);
        if (g_midi_broadcast) g_midi_broadcast(ch, d1, 0, 0);
        break;
    case 0xE0:
        itype->midi(st, status, d1, d2);
        break;
    case 0xB0:
        itype->midi(st, status, d1, d2);
        state_mark_dirty();
        break;
    case 0xC0: case 0xD0:
        itype->midi(st, status, d1, 0);
        state_mark_dirty();
        break;
    }
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

        /* Tick slot-level seq/keyseq per-sample before rendering.
         * Events fire at buffer boundaries (acceptable latency). */
        float tick_dt = 1.0f / (float)sample_rate;
        for (int si = 0; si < frames; si++) {
            if (slot->seq) seq_tick(slot->seq, tick_dt);
            if (slot->keyseq) keyseq_tick(slot->keyseq, tick_dt);
        }

        /* Copy slot keyseq cents_mod → instrument's cents_mod field */
        if (slot->keyseq) {
            slot->cents_mod = slot->keyseq->cents_mod;
            if (strcmp(itype->name, "fm-synth") == 0) ((FMSynth *)st)->cents_mod = slot->cents_mod;
            else if (strcmp(itype->name, "sub-synth") == 0) ((SubSynth *)st)->cents_mod = slot->cents_mod;
            else if (strcmp(itype->name, "ym2413") == 0) ((YM2413State *)st)->cents_mod = slot->cents_mod;
            else if (strcmp(itype->name, "fm-drums") == 0) ((FMDrumState *)st)->cents_mod = slot->cents_mod;
        }

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
