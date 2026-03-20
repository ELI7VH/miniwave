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
#include "instruments.h"
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

    /* Deactivate slot so render thread skips it */
    slot->active = 0;
    __sync_synchronize(); /* memory barrier — ensure render thread sees active=0 */
    usleep(2000); /* 2ms — let any in-flight render callback finish */

    /* Swap in new instrument */
    slot->state = new_state;
    slot->type_idx = tidx;
    slot->volume = 1.0f;
    slot->mute = 0;
    slot->solo = 0;
    __sync_synchronize();
    slot->active = 1;

    /* Now safe to destroy old state */
    if (was_active && old_state && old_type_idx >= 0) {
        InstrumentType *old_type = g_type_registry[old_type_idx];
        old_type->destroy(old_state);
        free(old_state);
    }

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

    slot->active = 0;
    __sync_synchronize();
    usleep(2000);

    slot->state = NULL;
    slot->type_idx = -1;
    slot->volume = 1.0f;
    slot->mute = 0;
    slot->solo = 0;

    if (was_active && old_state && old_type_idx >= 0) {
        InstrumentType *itype = g_type_registry[old_type_idx];
        itype->destroy(old_state);
        free(old_state);
    }

    fprintf(stderr, "[miniwave] slot %d cleared\n", channel);
    state_mark_dirty();
}

/* ── JSON helpers (used by state persistence and server) ───────────── */

static int json_get_string(const char *json, const char *key, char *out, int max) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = json;
    /* Find exact key — must be preceded by { , or whitespace, not a letter */
    while ((p = strstr(p, pattern)) != NULL) {
        if (p == json || p[-1] == '{' || p[-1] == ',' || p[-1] == ' ' || p[-1] == '\n')
            break;
        p += strlen(pattern);
    }
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < max - 1) {
        if (*p == '\\' && *(p + 1)) { p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int json_get_int(const char *json, const char *key, int *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') { /* string-encoded number */
        p++;
        *out = atoi(p);
        return 0;
    }
    if ((*p >= '0' && *p <= '9') || *p == '-') {
        *out = atoi(p);
        return 0;
    }
    return -1;
}

static int json_get_float(const char *json, const char *key, float *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
        *out = strtof(p, NULL);
        return 0;
    }
    return -1;
}

static int json_get_iarray_first(const char *json, const char *key, int *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '[') return -1;
    p++;
    while (*p == ' ') p++;
    if ((*p >= '0' && *p <= '9') || *p == '-') {
        *out = atoi(p);
        return 0;
    }
    return -1;
}

static int json_get_farray_first(const char *json, const char *key, float *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '[') return -1;
    p++;
    while (*p == ' ') p++;
    if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
        *out = strtof(p, NULL);
        return 0;
    }
    return -1;
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

            if (strcmp(itype->name, "fm-synth") == 0) {
                FMSynth *s = (FMSynth *)slot->state;
                fprintf(f, ",\"preset\":%d,\"override\":%d", s->current_preset, s->live_params.override);
                if (s->live_params.override) {
                    fprintf(f, ",\"params\":{\"carrier_ratio\":%.4f,\"mod_ratio\":%.4f,"
                            "\"mod_index\":%.4f,\"attack\":%.4f,\"decay\":%.4f,"
                            "\"sustain\":%.4f,\"release\":%.4f,\"feedback\":%.4f}",
                            (double)s->live_params.carrier_ratio,
                            (double)s->live_params.mod_ratio,
                            (double)s->live_params.mod_index,
                            (double)s->live_params.attack,
                            (double)s->live_params.decay,
                            (double)s->live_params.sustain,
                            (double)s->live_params.release,
                            (double)s->live_params.feedback);
                }
            }
            else if (strcmp(itype->name, "sub-synth") == 0) {
                SubSynth *s = (SubSynth *)slot->state;
                fprintf(f, ",\"params\":{\"waveform\":%d,\"pulse_width\":%.4f,"
                        "\"filter_cutoff\":%.4f,\"filter_reso\":%.4f,"
                        "\"filter_env_depth\":%.4f,"
                        "\"filt_attack\":%.4f,\"filt_decay\":%.4f,"
                        "\"filt_sustain\":%.4f,\"filt_release\":%.4f,"
                        "\"amp_attack\":%.4f,\"amp_decay\":%.4f,"
                        "\"amp_sustain\":%.4f,\"amp_release\":%.4f}",
                        s->params.waveform,
                        (double)s->params.pulse_width,
                        (double)s->params.filter_cutoff,
                        (double)s->params.filter_reso,
                        (double)s->params.filter_env_depth,
                        (double)s->params.filt_attack,
                        (double)s->params.filt_decay,
                        (double)s->params.filt_sustain,
                        (double)s->params.filt_release,
                        (double)s->params.amp_attack,
                        (double)s->params.amp_decay,
                        (double)s->params.amp_sustain,
                        (double)s->params.amp_release);
            }
            else if (strcmp(itype->name, "ym2413") == 0) {
                YM2413State *y = (YM2413State *)slot->state;
                fprintf(f, ",\"instrument\":%d,\"rhythm_mode\":%d",
                        y->current_instrument, y->rhythm_mode);
            }
            else if (strcmp(itype->name, "fm-drums") == 0) {
                FMDrumState *ds = (FMDrumState *)slot->state;
                fprintf(f, ",\"notes\":{");
                int first = 1;
                for (int ni = 0; ni < FMD_NUM_NOTES; ni++) {
                    if (ds->notes[ni].preset < 0) continue;
                    FMDrumDef *dd = &ds->notes[ni].def;
                    fprintf(f, "%s\"%d\":{\"p\":%d,\"cf\":%.4f,\"mf\":%.4f,\"mi\":%.4f,"
                            "\"sw\":%.4f,\"pd\":%.5f,\"dc\":%.4f,"
                            "\"na\":%.4f,\"ca\":%.4f,\"fb\":%.4f}",
                            first ? "" : ",", ni, ds->notes[ni].preset,
                            (double)dd->carrier_freq, (double)dd->mod_freq,
                            (double)dd->mod_index, (double)dd->pitch_sweep,
                            (double)dd->pitch_decay, (double)dd->decay,
                            (double)dd->noise_amt, (double)dd->click_amt,
                            (double)dd->feedback);
                    first = 0;
                }
                fprintf(f, "}");
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

                if (strcmp(itype->name, "fm-synth") == 0) {
                    FMSynth *s = (FMSynth *)slot->state;
                    int preset;
                    if (json_get_int(slot_json, "preset", &preset) == 0 && preset >= 0 && preset < NUM_PRESETS) {
                        s->current_preset = preset;
                        fm_load_preset_params(s, preset);
                    }
                    int ovr;
                    if (json_get_int(slot_json, "override", &ovr) == 0 && ovr) {
                        s->live_params.override = 1;
                        const char *pp = strstr(slot_json, "\"params\"");
                        if (pp) {
                            float fv;
                            if (json_get_float(pp, "carrier_ratio", &fv) == 0) s->live_params.carrier_ratio = fv;
                            if (json_get_float(pp, "mod_ratio", &fv) == 0) s->live_params.mod_ratio = fv;
                            if (json_get_float(pp, "mod_index", &fv) == 0) s->live_params.mod_index = fv;
                            if (json_get_float(pp, "attack", &fv) == 0) s->live_params.attack = fv;
                            if (json_get_float(pp, "decay", &fv) == 0) s->live_params.decay = fv;
                            if (json_get_float(pp, "sustain", &fv) == 0) s->live_params.sustain = fv;
                            if (json_get_float(pp, "release", &fv) == 0) s->live_params.release = fv;
                            if (json_get_float(pp, "feedback", &fv) == 0) s->live_params.feedback = fv;
                        }
                    }
                }
                else if (strcmp(itype->name, "sub-synth") == 0) {
                    SubSynth *s = (SubSynth *)slot->state;
                    const char *pp = strstr(slot_json, "\"params\"");
                    if (pp) {
                        int wf;
                        float fv;
                        if (json_get_int(pp, "waveform", &wf) == 0) s->params.waveform = wf % SUB_WAVE_COUNT;
                        if (json_get_float(pp, "pulse_width", &fv) == 0) s->params.pulse_width = fv;
                        if (json_get_float(pp, "filter_cutoff", &fv) == 0) s->params.filter_cutoff = fv;
                        if (json_get_float(pp, "filter_reso", &fv) == 0) s->params.filter_reso = fv;
                        if (json_get_float(pp, "filter_env_depth", &fv) == 0) s->params.filter_env_depth = fv;
                        if (json_get_float(pp, "filt_attack", &fv) == 0) s->params.filt_attack = fv;
                        if (json_get_float(pp, "filt_decay", &fv) == 0) s->params.filt_decay = fv;
                        if (json_get_float(pp, "filt_sustain", &fv) == 0) s->params.filt_sustain = fv;
                        if (json_get_float(pp, "filt_release", &fv) == 0) s->params.filt_release = fv;
                        if (json_get_float(pp, "amp_attack", &fv) == 0) s->params.amp_attack = fv;
                        if (json_get_float(pp, "amp_decay", &fv) == 0) s->params.amp_decay = fv;
                        if (json_get_float(pp, "amp_sustain", &fv) == 0) s->params.amp_sustain = fv;
                        if (json_get_float(pp, "amp_release", &fv) == 0) s->params.amp_release = fv;
                    }
                }
                else if (strcmp(itype->name, "ym2413") == 0) {
                    YM2413State *y = (YM2413State *)slot->state;
                    int inst, rhy;
                    if (json_get_int(slot_json, "instrument", &inst) == 0 && inst >= 0 && inst <= 15)
                        y->current_instrument = inst;
                    if (json_get_int(slot_json, "rhythm_mode", &rhy) == 0)
                        y->rhythm_mode = rhy;
                }
                else if (strcmp(itype->name, "fm-drums") == 0) {
                    FMDrumState *ds = (FMDrumState *)slot->state;
                    /* Parse per-note "notes":{"36":{...},"38":{...}} */
                    const char *nobj = strstr(slot_json, "\"notes\"");
                    if (nobj) {
                        nobj = strchr(nobj, '{');
                        if (nobj) nobj++; /* skip opening { of notes object */
                    }
                    if (nobj) {
                        /* Scan for "N":{ entries */
                        const char *p = nobj;
                        while (p && *p) {
                            /* Find next key like "36" */
                            const char *q = strchr(p, '"');
                            if (!q) break;
                            int noteNum = atoi(q + 1);
                            if (noteNum < 0 || noteNum >= FMD_NUM_NOTES) break;
                            /* Find the { for this note's object */
                            const char *brace = strchr(q + 1, '{');
                            if (!brace) break;
                            const char *bend = strchr(brace, '}');
                            if (!bend) break;
                            int nlen = (int)(bend - brace + 1);
                            char *njson = calloc(1, (size_t)(nlen + 1));
                            if (!njson) break;
                            memcpy(njson, brace, (size_t)nlen);
                            int preset;
                            float fv;
                            if (json_get_int(njson, "p", &preset) == 0 &&
                                preset >= 0 && preset < FMD_NUM_PRESETS) {
                                ds->notes[noteNum].preset = preset;
                                ds->notes[noteNum].def = FMD_PRESETS[preset];
                            }
                            if (json_get_float(njson, "cf", &fv) == 0) ds->notes[noteNum].def.carrier_freq = fv;
                            if (json_get_float(njson, "mf", &fv) == 0) ds->notes[noteNum].def.mod_freq = fv;
                            if (json_get_float(njson, "mi", &fv) == 0) ds->notes[noteNum].def.mod_index = fv;
                            if (json_get_float(njson, "sw", &fv) == 0) ds->notes[noteNum].def.pitch_sweep = fv;
                            if (json_get_float(njson, "pd", &fv) == 0) ds->notes[noteNum].def.pitch_decay = fv;
                            if (json_get_float(njson, "dc", &fv) == 0) ds->notes[noteNum].def.decay = fv;
                            if (json_get_float(njson, "na", &fv) == 0) ds->notes[noteNum].def.noise_amt = fv;
                            if (json_get_float(njson, "ca", &fv) == 0) ds->notes[noteNum].def.click_amt = fv;
                            if (json_get_float(njson, "fb", &fv) == 0) ds->notes[noteNum].def.feedback = fv;
                            free(njson);
                            p = bend + 1;
                        }
                    }
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
    if (!slot->active || !slot->state) return;
    InstrumentType *itype = g_type_registry[slot->type_idx];

    uint8_t type = status & 0xF0;
    uint8_t d1 = (len > 1) ? data[1] : 0;
    uint8_t d2 = (len > 2) ? data[2] : 0;

    switch (type) {
    case 0x90:
        itype->midi(slot->state, status, d1, d2);
        if (g_midi_broadcast) g_midi_broadcast(ch, d1, d2, d2 > 0 ? 1 : 0);
        break;
    case 0x80:
        itype->midi(slot->state, status, d1, d2);
        if (g_midi_broadcast) g_midi_broadcast(ch, d1, 0, 0);
        break;
    case 0xE0:
        itype->midi(slot->state, status, d1, d2);
        break;
    case 0xB0:
        itype->midi(slot->state, status, d1, d2);
        state_mark_dirty();
        break;
    case 0xC0: case 0xD0:
        itype->midi(slot->state, status, d1, 0);
        state_mark_dirty();
        break;
    }
}

/* ── Render Mixer ──────────────────────────────────────────────────── */

static void render_mix(float *mix_buf, float *slot_buf, int frames, int sample_rate) {
    memset(mix_buf, 0, sizeof(float) * (size_t)(frames * CHANNELS));

    int any_solo = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_rack.slots[i].active && g_rack.slots[i].solo) {
            any_solo = 1;
            break;
        }
    }

    for (int i = 0; i < MAX_SLOTS; i++) {
        RackSlot *slot = &g_rack.slots[i];
        if (!slot->active || !slot->state) continue;
        if (slot->mute) continue;
        if (any_solo && !slot->solo) continue;

        InstrumentType *itype = g_type_registry[slot->type_idx];
        memset(slot_buf, 0, sizeof(float) * (size_t)(frames * CHANNELS));
        itype->render(slot->state, slot_buf, frames, sample_rate);

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
