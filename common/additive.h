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

/* json_escape may not be defined yet (additive.h included before rack.h) */
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

enum {
    ADD_MODE_HARMONIC = 0,  /* integer multiples, classic additive */
    ADD_MODE_CLUSTER  = 1,  /* ratio-based exponential spacing */
    ADD_MODE_FORMANT  = 2,  /* resonant peaks at formant frequencies */
    ADD_MODE_METALLIC = 3,  /* inharmonic ratios (bells, gongs) */
    ADD_MODE_NOISE    = 4,  /* noise-shaped spectrum */
    ADD_MODE_EXPR     = 5,  /* fully expression-driven amp + freq */
    ADD_MODE_COUNT
};

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

    /* Shared mode params */
    float cluster_ratio;        /* frequency ratio between partials */
    float cluster_spread;       /* detuning/noise amount */
    float cluster_rolloff;      /* amplitude rolloff per partial */

    /* Formant mode */
    float formant_center;       /* center freq in Hz */
    float formant_width;        /* bandwidth in Hz */

    /* Metallic mode */
    float metal_inharmonicity;  /* 0=harmonic, 1=maximally inharmonic */

    /* Expression mode — compiled expressions for amp(h) and freq(h) */
    KeySeqExpr amp_expr;        /* amplitude expression, var h = harmonic index */
    KeySeqExpr freq_expr;       /* freq multiplier expression, var h */
    KeySeqExpr phase_expr;      /* phase offset expression, var h */
    char amp_expr_src[256];
    char freq_expr_src[256];
    char phase_expr_src[256];

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
    static const char *mode_names[] = {"harmonic","cluster","formant","metallic","noise","expr"};
    fprintf(stderr, "[additive] build_table: mode=%s harmonics=%d\n",
            s->mode < ADD_MODE_COUNT ? mode_names[s->mode] : "?", s->num_harmonics);
    memset(s->table, 0, sizeof(s->table));

    int nh = s->num_harmonics;
    if (nh < 1) nh = 1;
    if (nh > ADD_MAX_HARMONICS) nh = ADD_MAX_HARMONICS;

    for (int h = 0; h < nh; h++) {
        float amp, freq_mult, phase_off;
        float hf = (float)(h + 1);  /* 1-based harmonic index */

        switch (s->mode) {
        case ADD_MODE_HARMONIC:
            freq_mult = hf * s->cluster_ratio;
            freq_mult += s->cluster_spread * sinf((float)h * 1.618f);
            amp = s->harm_amps[h];
            if (h > 0) amp *= powf(s->cluster_rolloff, (float)h);
            phase_off = s->harm_phases[h];
            break;

        case ADD_MODE_CLUSTER:
            freq_mult = powf(s->cluster_ratio, (float)h);
            freq_mult += s->cluster_spread * sinf((float)h * 1.618f);
            amp = s->harm_amps[h];
            if (h > 0) amp *= powf(s->cluster_rolloff, (float)h);
            phase_off = s->harm_phases[h];
            break;

        case ADD_MODE_FORMANT: {
            /* Gaussian peak around formant_center */
            float f = hf * 110.0f; /* approximate freq at A2 fundamental */
            float dist = (f - s->formant_center) / (s->formant_width + 1.0f);
            amp = expf(-0.5f * dist * dist);
            if (h > 0) amp *= powf(s->cluster_rolloff, (float)h * 0.3f);
            freq_mult = hf;
            phase_off = 0;
            break;
        }

        case ADD_MODE_METALLIC: {
            /* Inharmonic: stretch partials away from integer ratios */
            float stretch = 1.0f + s->metal_inharmonicity * (float)h * 0.02f;
            freq_mult = hf * stretch;
            /* Add some pseudo-random detuning for bell-like quality */
            freq_mult += sinf(hf * 2.718f) * s->metal_inharmonicity * 0.5f;
            amp = 1.0f / (1.0f + (float)h * 0.5f);
            if (h > 0) amp *= powf(s->cluster_rolloff, (float)h);
            phase_off = sinf(hf * 1.414f) * (float)M_PI; /* random-ish phase */
            break;
        }

        case ADD_MODE_NOISE: {
            /* Noise-shaped: amplitude from noise function, frequencies stretched */
            ke_ensure_fnl();
            float nv = fnlGetNoise2D(&g_fnl, hf * 0.7f + 13.37f, hf * 0.3f);
            amp = (nv + 1.0f) * 0.5f; /* 0-1 */
            amp *= 1.0f / (1.0f + (float)h * 0.3f); /* rolloff */
            freq_mult = hf + s->cluster_spread * nv;
            phase_off = nv * (float)M_PI;
            break;
        }

        case ADD_MODE_EXPR: {
            /* Expression-driven: evaluate amp(h) and freq(h) */
            KeySeqCtx ctx = {0};
            ctx.n = hf;        /* h as 'n' variable */
            ctx.i = hf;        /* h as 'i' variable */
            ctx.root = (float)nh;  /* total harmonics as 'root' */
            ctx.v = 1.0f;

            amp = s->amp_expr.valid ? ke_eval(&s->amp_expr, &ctx) : (1.0f / hf);
            freq_mult = s->freq_expr.valid ? ke_eval(&s->freq_expr, &ctx) : hf;
            phase_off = s->phase_expr.valid ? ke_eval(&s->phase_expr, &ctx) : 0;
            break;
        }

        default:
            amp = 0; freq_mult = hf; phase_off = 0;
            break;
        }

        if (fabsf(amp) < 0.0001f || freq_mult < 0.01f) continue;

        for (int i = 0; i < ADD_TABLE_SIZE; i++) {
            float t = (float)i / (float)ADD_TABLE_SIZE;
            s->table[i] += amp * sinf(ADD_TAU * freq_mult * t + phase_off);
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

    int vi = -1;
    for (int i = 0; i < ADD_MAX_VOICES; i++)
        if (!s->voices[i].active) { vi = i; break; }
    if (vi < 0) {
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

    int active = 0;
    for (int i = 0; i < ADD_MAX_VOICES; i++) if (s->voices[i].active) active++;
    fprintf(stderr, "[additive] note_on %d vel=%d freq=%.1fHz voice=%d (%d active)\n",
            note, vel, (double)freq, vi, active);
}

static void additive_note_off(AdditiveState *s, int note) {
    int found = 0;
    for (int i = 0; i < ADD_MAX_VOICES; i++) {
        AddVoice *v = &s->voices[i];
        if (v->active && v->note == note && v->env_state != 3) {
            v->env_state = 3;
            v->release_level = v->env_level;
            v->env_time = 0;
            fprintf(stderr, "[additive] note_off %d voice=%d (env=%.3f → release)\n",
                    note, i, (double)v->release_level);
            found = 1;
        }
    }
    if (!found) fprintf(stderr, "[additive] note_off %d — no active voice found\n", note);
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
    s->cluster_ratio = 1.0f;
    s->cluster_spread = 0.0f;
    s->cluster_rolloff = 0.7f;
    s->formant_center = 800.0f;
    s->formant_width = 200.0f;
    s->metal_inharmonicity = 0.5f;

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
    case 0xB0: {
        float cc = (float)d2 / 127.0f;
        switch (d1) {
        case 14: /* mode — 6 zones */
            s->mode = (int)(cc * 5.99f) % ADD_MODE_COUNT;
            s->table_dirty = 1;
            break;
        case 15: /* harmonics — 1→64 */
            s->num_harmonics = 1 + (int)(cc * 63.0f);
            s->table_dirty = 1;
            break;
        case 16: /* ratio — 0.25→4.0 log */
            s->cluster_ratio = 0.25f * powf(16.0f, cc);
            s->table_dirty = 1;
            break;
        case 17: /* spread — 0→3 */
            s->cluster_spread = cc * 3.0f;
            s->table_dirty = 1;
            break;
        case 18: /* rolloff — 0.05→1.0 */
            s->cluster_rolloff = 0.05f + cc * 0.95f;
            s->table_dirty = 1;
            break;
        case 19: /* character — inharmonicity + formant center */
            s->metal_inharmonicity = cc;
            s->formant_center = 200.0f + cc * 4800.0f;
            s->table_dirty = 1;
            break;
        case 20: { /* shape — combined attack/decay/sustain macro */
            /* 0=sharp pluck, 0.5=balanced, 1=slow pad */
            s->attack = 0.001f * powf(3000.0f, cc);
            s->decay = 0.05f + (1.0f - cc) * 2.0f;
            s->sustain = cc * cc;
            break;
        }
        case 21: /* release — 0.01→8s log */
            s->release = 0.01f * powf(800.0f, cc);
            break;
        case 120: case 123:
            for (int i = 0; i < ADD_MAX_VOICES; i++) s->voices[i].active = 0;
            break;
        }
        break;
    }
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

    fprintf(stderr, "[additive] set_param %s = %.4f\n", name, (double)value);

    if (strcmp(name, "volume") == 0)    { s->volume = value < 0 ? 0 : value > 1 ? 1 : value; }
    else if (strcmp(name, "mode") == 0) { s->mode = (int)value % ADD_MODE_COUNT; s->table_dirty = 1; }
    else if (strcmp(name, "harmonics") == 0) { s->num_harmonics = (int)value; if (s->num_harmonics<1) s->num_harmonics=1; if (s->num_harmonics>ADD_MAX_HARMONICS) s->num_harmonics=ADD_MAX_HARMONICS; s->table_dirty=1; }
    else if (strcmp(name, "attack") == 0)    s->attack = value;
    else if (strcmp(name, "decay") == 0)     s->decay = value;
    else if (strcmp(name, "sustain") == 0)   s->sustain = value;
    else if (strcmp(name, "release") == 0)   s->release = value;
    /* Cluster params */
    else if (strcmp(name, "ratio") == 0)     { s->cluster_ratio = value; s->table_dirty = 1; }
    else if (strcmp(name, "spread") == 0)    { s->cluster_spread = value; s->table_dirty = 1; }
    else if (strcmp(name, "rolloff") == 0)    { s->cluster_rolloff = value; s->table_dirty = 1; }
    /* Formant mode */
    else if (strcmp(name, "formant_center") == 0) { s->formant_center = value; s->table_dirty = 1; }
    else if (strcmp(name, "formant_width") == 0)  { s->formant_width = value; s->table_dirty = 1; }
    /* Metallic mode */
    else if (strcmp(name, "inharmonicity") == 0)  { s->metal_inharmonicity = value; s->table_dirty = 1; }
    /* Expression mode */
    else if (strcmp(name, "amp_expr") == 0) {
        /* value is ignored — expression set via osc_handle string path */
    }
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
    /* Expression strings via /expr/amp, /expr/freq, /expr/phase
     * Pass the expression as the remaining path after /expr/<type>/ */
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

    static const char *mode_names[] = {"harmonic","cluster","formant","metallic","noise","expr"};
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
    fprintf(stderr, "[additive] json_save: mode=%d harmonics=%d vol=%.2f ratio=%.2f spread=%.2f rolloff=%.2f\n",
            s->mode, s->num_harmonics, (double)s->volume,
            (double)s->cluster_ratio, (double)s->cluster_spread, (double)s->cluster_rolloff);
    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "\"mode\":%d,\"harmonics\":%d,\"volume\":%.4f,"
        "\"attack\":%.4f,\"decay\":%.4f,\"sustain\":%.4f,\"release\":%.4f,"
        "\"ratio\":%.4f,\"spread\":%.4f,\"rolloff\":%.4f,"
        "\"formant_center\":%.1f,\"formant_width\":%.1f,\"inharmonicity\":%.4f",
        s->mode, s->num_harmonics, (double)s->volume,
        (double)s->attack, (double)s->decay, (double)s->sustain, (double)s->release,
        (double)s->cluster_ratio, (double)s->cluster_spread, (double)s->cluster_rolloff,
        (double)s->formant_center, (double)s->formant_width, (double)s->metal_inharmonicity);

    /* Save harmonic amplitudes */
    pos += snprintf(buf + pos, (size_t)(max - pos), ",\"amps\":[");
    for (int h = 0; h < s->num_harmonics; h++)
        pos += snprintf(buf + pos, (size_t)(max - pos), "%s%.4f", h?",":"", (double)s->harm_amps[h]);
    pos += snprintf(buf + pos, (size_t)(max - pos), "]");

    /* Expression sources */
    if (s->amp_expr_src[0]) {
        char esc[512]; json_escape(esc, sizeof(esc), s->amp_expr_src);
        pos += snprintf(buf + pos, (size_t)(max - pos), ",\"amp_expr\":\"%s\"", esc);
    }
    if (s->freq_expr_src[0]) {
        char esc[512]; json_escape(esc, sizeof(esc), s->freq_expr_src);
        pos += snprintf(buf + pos, (size_t)(max - pos), ",\"freq_expr\":\"%s\"", esc);
    }
    if (s->phase_expr_src[0]) {
        char esc[512]; json_escape(esc, sizeof(esc), s->phase_expr_src);
        pos += snprintf(buf + pos, (size_t)(max - pos), ",\"phase_expr\":\"%s\"", esc);
    }

    return pos;
}

static int additive_json_load(void *state, const char *json) {
    AdditiveState *s = (AdditiveState *)state;
    int ival; float fval;
    fprintf(stderr, "[additive] json_load: parsing...\n");
    if (json_get_int(json, "mode", &ival) == 0) { s->mode = ival % ADD_MODE_COUNT; fprintf(stderr, "[additive]   mode=%d\n", s->mode); }
    if (json_get_int(json, "harmonics", &ival) == 0) { s->num_harmonics = ival; fprintf(stderr, "[additive]   harmonics=%d\n", ival); }
    if (json_get_float(json, "volume", &fval) == 0) { s->volume = fval; fprintf(stderr, "[additive]   volume=%.4f\n", (double)fval); }
    if (json_get_float(json, "attack", &fval) == 0) s->attack = fval;
    if (json_get_float(json, "decay", &fval) == 0) s->decay = fval;
    if (json_get_float(json, "sustain", &fval) == 0) s->sustain = fval;
    if (json_get_float(json, "release", &fval) == 0) s->release = fval;
    if (json_get_float(json, "ratio", &fval) == 0) { s->cluster_ratio = fval; fprintf(stderr, "[additive]   ratio=%.4f\n", (double)fval); }
    if (json_get_float(json, "spread", &fval) == 0) { s->cluster_spread = fval; fprintf(stderr, "[additive]   spread=%.4f\n", (double)fval); }
    if (json_get_float(json, "rolloff", &fval) == 0) { s->cluster_rolloff = fval; }
    if (json_get_float(json, "formant_center", &fval) == 0) s->formant_center = fval;
    if (json_get_float(json, "formant_width", &fval) == 0) s->formant_width = fval;
    if (json_get_float(json, "inharmonicity", &fval) == 0) s->metal_inharmonicity = fval;
    /* Restore expressions */
    char expr_buf[256];
    if (json_get_string(json, "amp_expr", expr_buf, sizeof(expr_buf)) == 0 && expr_buf[0]) {
        strncpy(s->amp_expr_src, expr_buf, 255);
        ke_compile(&s->amp_expr, expr_buf);
    }
    if (json_get_string(json, "freq_expr", expr_buf, sizeof(expr_buf)) == 0 && expr_buf[0]) {
        strncpy(s->freq_expr_src, expr_buf, 255);
        ke_compile(&s->freq_expr, expr_buf);
    }
    if (json_get_string(json, "phase_expr", expr_buf, sizeof(expr_buf)) == 0 && expr_buf[0]) {
        strncpy(s->phase_expr_src, expr_buf, 255);
        ke_compile(&s->phase_expr, expr_buf);
    }
    s->table_dirty = 1;
    fprintf(stderr, "[additive] json_load done: mode=%d harmonics=%d vol=%.2f\n",
            s->mode, s->num_harmonics, (double)s->volume);
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
