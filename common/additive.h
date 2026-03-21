/* miniwave — additive synthesizer
 *
 * Three partial generation modes:
 *
 * 1. HARMONIC — integer multiples of fundamental
 *    amp(h) expression, phase(h) expression
 *    Baked to wavetable on change.
 *
 * 2. PARTIAL — explicit freq;amp pairs
 *    Each line: freq_expr; amp_expr
 *    Fixed partials bake to wavetable, time-varying compute per-sample.
 *
 * 3. CLUSTER — algorithmic partial generation
 *    freq(h) and amp(h) expressions, h = 1..count
 *    "220, 1.25, h => h + sin(t) + noise(h,t) * 1.5"
 *    Fundamental + ratio + rule function.
 *
 * Wavetable: 2048 samples, rebuilt on expression/param change.
 * Per-sample: table lookup + linear interpolation.
 * Live partials (time-varying): max 16, computed per-sample.
 */

#ifndef ADDITIVE_H
#define ADDITIVE_H

#include "instruments.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ADD_TAU (2.0f * (float)M_PI)
#define ADD_TABLE_SIZE 2048
#define ADD_MAX_HARMONICS 64
#define ADD_MAX_VOICES 8
#define ADD_FADEIN_SAMPLES 48
#define ADD_FADEOUT_SAMPLES 96

/* ── Modes ────────────────────────────────────────────────────────────── */

enum { ADD_MODE_HARMONIC = 0, ADD_MODE_PARTIAL = 1, ADD_MODE_CLUSTER = 2 };

/* ── Voice ────────────────────────────────────────────────────────────── */

typedef struct {
    int   active;
    int   note;
    float freq;
    float velocity;
    float phase;           /* wavetable phase [0, TABLE_SIZE) */
    float env_level;
    float env_time;
    int   env_state;       /* 0=atk 1=dec 2=sus 3=rel */
    float release_level;
    float age;
    int   sample_count;
    int   killing;
    int   kill_pos;
} AddVoice;

/* ── State ────────────────────────────────────────────────────────────── */

typedef struct {
    /* Wavetable */
    float table[ADD_TABLE_SIZE];
    int   table_dirty;      /* 1 = needs rebuild */

    /* Mode */
    int   mode;             /* ADD_MODE_* */

    /* Harmonic mode params */
    int   num_harmonics;    /* 1-64 */
    float harm_amps[ADD_MAX_HARMONICS];   /* amplitude per harmonic */
    float harm_phases[ADD_MAX_HARMONICS]; /* phase offset per harmonic */

    /* Cluster mode params */
    float cluster_fundamental;  /* base freq (overridden by MIDI note) */
    float cluster_ratio;        /* frequency ratio between partials */
    float cluster_spread;       /* detuning/noise amount */
    float cluster_rolloff;      /* amplitude rolloff per partial */

    /* Envelope */
    float attack, decay, sustain, release;

    /* Output */
    float volume;
    float cents_mod;

    /* Voices */
    AddVoice voices[ADD_MAX_VOICES];
} AdditiveState;

/* ── Wavetable generation ─────────────────────────────────────────────── */

static void additive_build_table(AdditiveState *s) {
    memset(s->table, 0, sizeof(s->table));

    if (s->mode == ADD_MODE_HARMONIC || s->mode == ADD_MODE_CLUSTER) {
        int nh = s->num_harmonics;
        if (nh < 1) nh = 1;
        if (nh > ADD_MAX_HARMONICS) nh = ADD_MAX_HARMONICS;

        for (int h = 0; h < nh; h++) {
            float amp = s->harm_amps[h];
            float phase_off = s->harm_phases[h];

            float freq_mult;
            if (s->mode == ADD_MODE_HARMONIC) {
                freq_mult = (float)(h + 1); /* integer harmonics */
            } else {
                /* Cluster: ratio-based spacing */
                freq_mult = powf(s->cluster_ratio, (float)h);
                /* Add spread/detuning */
                freq_mult += s->cluster_spread * sinf((float)h * 1.618f);
            }

            /* Apply rolloff for cluster mode */
            if (s->mode == ADD_MODE_CLUSTER && h > 0) {
                amp *= powf(s->cluster_rolloff, (float)h);
            }

            if (fabsf(amp) < 0.0001f) continue;

            for (int i = 0; i < ADD_TABLE_SIZE; i++) {
                float t = (float)i / (float)ADD_TABLE_SIZE;
                s->table[i] += amp * sinf(ADD_TAU * freq_mult * t + phase_off);
            }
        }
    }

    /* Normalize */
    float peak = 0;
    for (int i = 0; i < ADD_TABLE_SIZE; i++) {
        float a = fabsf(s->table[i]);
        if (a > peak) peak = a;
    }
    if (peak > 0.0001f) {
        float scale = 1.0f / peak;
        for (int i = 0; i < ADD_TABLE_SIZE; i++)
            s->table[i] *= scale;
    }

    s->table_dirty = 0;
}

/* ── Wavetable read (linear interpolation) ────────────────────────────── */

static inline float additive_read_table(const AdditiveState *s, float phase) {
    while (phase >= ADD_TABLE_SIZE) phase -= ADD_TABLE_SIZE;
    while (phase < 0) phase += ADD_TABLE_SIZE;
    int i0 = (int)phase;
    int i1 = (i0 + 1) % ADD_TABLE_SIZE;
    float frac = phase - (float)i0;
    return s->table[i0] + (s->table[i1] - s->table[i0]) * frac;
}

/* ── Envelope ─────────────────────────────────────────────────────────── */

static float additive_env_tick(AddVoice *v, float atk, float dec, float sus, float rel, float dt) {
    v->env_time += dt;
    switch (v->env_state) {
    case 0: /* attack */
        if (atk <= 0.001f) { v->env_level = 1.0f; v->env_state = 1; v->env_time = 0; }
        else {
            v->env_level = v->env_time / atk;
            if (v->env_level >= 1.0f) { v->env_level = 1.0f; v->env_state = 1; v->env_time = 0; }
        }
        break;
    case 1: /* decay */
        if (dec <= 0.001f) { v->env_level = sus; v->env_state = 2; }
        else {
            v->env_level = 1.0f - (1.0f - sus) * (v->env_time / dec);
            if (v->env_level <= sus) { v->env_level = sus; v->env_state = 2; }
        }
        break;
    case 2: /* sustain */
        v->env_level = sus;
        break;
    case 3: /* release */
        if (rel <= 0.001f) { v->env_level = 0; }
        else {
            v->env_level = v->release_level * (1.0f - v->env_time / rel);
            if (v->env_level < 0) v->env_level = 0;
        }
        break;
    }
    return v->env_level;
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static float additive_midi_to_freq(int note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

static void additive_note_on(AdditiveState *s, int note, int vel) {
    float freq = additive_midi_to_freq(note);
    float velocity = (float)vel / 127.0f;

    /* Find free voice or steal oldest */
    int vi = -1;
    for (int i = 0; i < ADD_MAX_VOICES; i++)
        if (!s->voices[i].active) { vi = i; break; }
    if (vi < 0) {
        /* Kill oldest with fadeout */
        vi = 0;
        s->voices[vi].killing = 1;
        s->voices[vi].kill_pos = 0;
    }

    AddVoice *v = &s->voices[vi];
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->note = note;
    v->freq = freq;
    v->velocity = velocity;
    v->env_state = 0;
}

static void additive_note_off(AdditiveState *s, int note) {
    for (int i = 0; i < ADD_MAX_VOICES; i++) {
        AddVoice *v = &s->voices[i];
        if (v->active && v->note == note && v->env_state != 3) {
            v->env_state = 3;
            v->release_level = v->env_level;
            v->env_time = 0;
        }
    }
}

/* ── InstrumentType interface ─────────────────────────────────────────── */

static void additive_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2);
static void additive_set_param(void *state, const char *name, float value);

static void additive_init(void *state) {
    AdditiveState *s = (AdditiveState *)state;
    memset(s, 0, sizeof(*s));

    s->mode = ADD_MODE_HARMONIC;
    s->num_harmonics = 16;
    s->volume = 1.0f;
    s->attack = 0.01f;
    s->decay = 0.1f;
    s->sustain = 0.7f;
    s->release = 0.3f;
    s->cluster_fundamental = 220.0f;
    s->cluster_ratio = 1.5f;
    s->cluster_spread = 0.0f;
    s->cluster_rolloff = 0.7f;

    /* Default: sawtooth (1/h) */
    for (int h = 0; h < ADD_MAX_HARMONICS; h++)
        s->harm_amps[h] = 1.0f / (float)(h + 1);

    additive_build_table(s);
}

static void additive_destroy(void *state) { (void)state; }

static void additive_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2) {
    AdditiveState *s = (AdditiveState *)state;
    uint8_t type = status & 0xF0;
    switch (type) {
    case 0x90:
        if (d2 > 0) additive_note_on(s, d1, d2);
        else        additive_note_off(s, d1);
        break;
    case 0x80:
        additive_note_off(s, d1);
        break;
    case 0xB0:
        if (d1 == 120 || d1 == 123)
            for (int i = 0; i < ADD_MAX_VOICES; i++) s->voices[i].active = 0;
        break;
    }
}

static void additive_render(void *state, float *stereo_buf, int frames, int sample_rate) {
    AdditiveState *s = (AdditiveState *)state;
    float dt = 1.0f / (float)sample_rate;

    if (s->table_dirty) additive_build_table(s);

    /* Pitch bend from cents_mod */
    float pitch_mult = (s->cents_mod != 0.0f)
        ? powf(2.0f, s->cents_mod / 1200.0f) : 1.0f;

    for (int i = 0; i < frames; i++) {
        float mix = 0.0f;

        for (int vi = 0; vi < ADD_MAX_VOICES; vi++) {
            AddVoice *v = &s->voices[vi];
            if (!v->active) continue;

            v->age += dt;
            if (v->age > 30.0f) { v->active = 0; continue; }

            float env = additive_env_tick(v, s->attack, s->decay, s->sustain, s->release, dt);
            if (env <= 0.0f && v->env_state == 3) { v->active = 0; continue; }

            /* Read wavetable */
            float sample = additive_read_table(s, v->phase);
            sample *= env * v->velocity * 0.5f;

            /* Anti-click */
            if (v->sample_count < ADD_FADEIN_SAMPLES)
                sample *= (float)v->sample_count / (float)ADD_FADEIN_SAMPLES;
            v->sample_count++;

            if (v->killing) {
                float fade = 1.0f - (float)v->kill_pos / (float)ADD_FADEOUT_SAMPLES;
                if (fade <= 0) { v->active = 0; continue; }
                sample *= fade;
                v->kill_pos++;
            }

            mix += sample;

            /* Phase advance */
            float phase_inc = v->freq * pitch_mult * (float)ADD_TABLE_SIZE / (float)sample_rate;
            v->phase += phase_inc;
            while (v->phase >= ADD_TABLE_SIZE) v->phase -= ADD_TABLE_SIZE;
        }

        mix *= s->volume;
        /* Simple soft clip */
        if (mix > 0.95f) mix = 0.95f;
        if (mix < -0.95f) mix = -0.95f;

        stereo_buf[i * 2]     = mix;
        stereo_buf[i * 2 + 1] = mix;
    }
}

/* ── set_param ────────────────────────────────────────────────────────── */

static void additive_set_param(void *state, const char *name, float value) {
    AdditiveState *s = (AdditiveState *)state;

    if (strcmp(name, "volume") == 0)    { s->volume = value < 0 ? 0 : value > 1 ? 1 : value; }
    else if (strcmp(name, "mode") == 0) { s->mode = (int)value % 3; s->table_dirty = 1; }
    else if (strcmp(name, "harmonics") == 0) { s->num_harmonics = (int)value; if (s->num_harmonics<1) s->num_harmonics=1; if (s->num_harmonics>ADD_MAX_HARMONICS) s->num_harmonics=ADD_MAX_HARMONICS; s->table_dirty=1; }
    else if (strcmp(name, "attack") == 0)    s->attack = value;
    else if (strcmp(name, "decay") == 0)     s->decay = value;
    else if (strcmp(name, "sustain") == 0)   s->sustain = value;
    else if (strcmp(name, "release") == 0)   s->release = value;
    /* Cluster params */
    else if (strcmp(name, "ratio") == 0)     { s->cluster_ratio = value; s->table_dirty = 1; }
    else if (strcmp(name, "spread") == 0)    { s->cluster_spread = value; s->table_dirty = 1; }
    else if (strcmp(name, "rolloff") == 0)   { s->cluster_rolloff = value; s->table_dirty = 1; }
    /* Per-harmonic amplitude: harm_0, harm_1, ... harm_63 */
    else if (strncmp(name, "harm_", 5) == 0) {
        int h = atoi(name + 5);
        if (h >= 0 && h < ADD_MAX_HARMONICS) { s->harm_amps[h] = value; s->table_dirty = 1; }
    }
    /* Per-harmonic phase: phase_0, phase_1, ... */
    else if (strncmp(name, "phase_", 6) == 0) {
        int h = atoi(name + 6);
        if (h >= 0 && h < ADD_MAX_HARMONICS) { s->harm_phases[h] = value; s->table_dirty = 1; }
    }
}

/* ── OSC ──────────────────────────────────────────────────────────────── */

static void additive_osc_handle(void *state, const char *sub_path,
                                 const int32_t *iargs, int ni,
                                 const float *fargs, int nf) {
    AdditiveState *s = (AdditiveState *)state;

    if (strcmp(sub_path, "/volume") == 0 && nf >= 1) {
        s->volume = fargs[0];
    }
    else if (strncmp(sub_path, "/param/", 7) == 0) {
        additive_set_param(state, sub_path + 7, nf >= 1 ? fargs[0] : 0);
    }
    /* Set all harmonics from a rule: /harmonics/sawtooth, /harmonics/square, etc. */
    else if (strcmp(sub_path, "/harmonics/sawtooth") == 0) {
        for (int h = 0; h < ADD_MAX_HARMONICS; h++) s->harm_amps[h] = 1.0f / (float)(h + 1);
        s->table_dirty = 1;
    }
    else if (strcmp(sub_path, "/harmonics/square") == 0) {
        for (int h = 0; h < ADD_MAX_HARMONICS; h++) s->harm_amps[h] = (h % 2 == 0) ? 1.0f / (float)(h + 1) : 0;
        s->table_dirty = 1;
    }
    else if (strcmp(sub_path, "/harmonics/triangle") == 0) {
        for (int h = 0; h < ADD_MAX_HARMONICS; h++) {
            if (h % 2 == 0) s->harm_amps[h] = (((h/2) % 2 == 0) ? 1.0f : -1.0f) / ((float)(h+1) * (float)(h+1));
            else s->harm_amps[h] = 0;
        }
        s->table_dirty = 1;
    }
    (void)iargs; (void)ni;
}

static int additive_osc_status(void *state, uint8_t *buf, int max_len) {
    (void)state; (void)buf; (void)max_len;
    return 0;
}

/* ── json_status ──────────────────────────────────────────────────────── */

static int additive_json_status(void *state, char *buf, int max) {
    AdditiveState *s = (AdditiveState *)state;
    int active = 0;
    for (int i = 0; i < ADD_MAX_VOICES; i++)
        if (s->voices[i].active) active++;

    static const char *mode_names[] = {"harmonic", "partial", "cluster"};
    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "\"instrument_type\":\"additive\","
        "\"mode\":\"%s\",\"mode_index\":%d,"
        "\"harmonics\":%d,\"active_voices\":%d",
        mode_names[s->mode], s->mode, s->num_harmonics, active);
    return pos;
}

/* ── json_save / json_load ────────────────────────────────────────────── */

static int additive_json_save(void *state, char *buf, int max) {
    AdditiveState *s = (AdditiveState *)state;
    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "\"mode\":%d,\"harmonics\":%d,\"volume\":%.4f,"
        "\"attack\":%.4f,\"decay\":%.4f,\"sustain\":%.4f,\"release\":%.4f,"
        "\"ratio\":%.4f,\"spread\":%.4f,\"rolloff\":%.4f",
        s->mode, s->num_harmonics, (double)s->volume,
        (double)s->attack, (double)s->decay, (double)s->sustain, (double)s->release,
        (double)s->cluster_ratio, (double)s->cluster_spread, (double)s->cluster_rolloff);

    /* Save harmonic amplitudes */
    pos += snprintf(buf + pos, (size_t)(max - pos), ",\"amps\":[");
    for (int h = 0; h < s->num_harmonics; h++)
        pos += snprintf(buf + pos, (size_t)(max - pos), "%s%.4f", h?",":"", (double)s->harm_amps[h]);
    pos += snprintf(buf + pos, (size_t)(max - pos), "]");

    return pos;
}

static int additive_json_load(void *state, const char *json) {
    AdditiveState *s = (AdditiveState *)state;
    int ival; float fval;
    if (json_get_int(json, "mode", &ival) == 0) s->mode = ival % 3;
    if (json_get_int(json, "harmonics", &ival) == 0) s->num_harmonics = ival;
    if (json_get_float(json, "volume", &fval) == 0) s->volume = fval;
    if (json_get_float(json, "attack", &fval) == 0) s->attack = fval;
    if (json_get_float(json, "decay", &fval) == 0) s->decay = fval;
    if (json_get_float(json, "sustain", &fval) == 0) s->sustain = fval;
    if (json_get_float(json, "release", &fval) == 0) s->release = fval;
    if (json_get_float(json, "ratio", &fval) == 0) s->cluster_ratio = fval;
    if (json_get_float(json, "spread", &fval) == 0) s->cluster_spread = fval;
    if (json_get_float(json, "rolloff", &fval) == 0) s->cluster_rolloff = fval;
    s->table_dirty = 1;
    return 0;
}

/* ── Type descriptor ──────────────────────────────────────────────────── */

InstrumentType additive_type = {
    .name         = "additive",
    .display_name = "Additive Synth",
    .state_size   = sizeof(AdditiveState),
    .init         = additive_init,
    .destroy      = additive_destroy,
    .midi         = additive_midi,
    .render       = additive_render,
    .set_param    = additive_set_param,
    .json_status  = additive_json_status,
    .json_save    = additive_json_save,
    .json_load    = additive_json_load,
    .osc_handle   = additive_osc_handle,
    .osc_status   = additive_osc_status,
};

#endif /* ADDITIVE_H */
