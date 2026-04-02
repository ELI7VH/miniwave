/* miniwave — phase distortion synthesizer
 *
 * Inspired by the Casio CZ series. A sine oscillator with a
 * phase transfer function that reshapes the waveform.
 *
 * Modes:
 *   0. Resonant — phase pinch creates resonant peak
 *   1. Sawtooth — asymmetric phase ramp
 *   2. Pulse   — variable width via phase jump
 *   3. Cosine  — phase-distorted cosine (warm)
 *   4. Sync    — hard sync effect via phase reset
 *   5. Wavefold — phase wrapping creates harmonics
 *
 * Key params:
 *   distortion — amount of phase distortion (0=sine, 1=max)
 *   timbre     — character of distortion (mode-dependent)
 *   color      — spectral tilt / brightness
 */

#ifndef PHASE_DIST_H
#define PHASE_DIST_H

#include "instruments.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PD_TAU (2.0f * (float)M_PI)
#define PD_MAX_VOICES 8
#define PD_FADEIN 48
#define PD_FADEOUT 96

enum {
    PD_MODE_RESONANT = 0,
    PD_MODE_SAW      = 1,
    PD_MODE_PULSE    = 2,
    PD_MODE_COSINE   = 3,
    PD_MODE_SYNC     = 4,
    PD_MODE_WAVEFOLD = 5,
    PD_MODE_COUNT
};

typedef struct {
    int   active;
    int   note;
    float freq;
    float velocity;
    float phase;
    float env_level, env_time;
    int   env_state;
    float release_level;
    float age;
    int   sample_count;
    int   killing, kill_pos;
    float lpf_z;  /* one-pole LPF state */
} PDVoice;

typedef struct {
    int     mode;
    float   distortion;    /* 0-1 */
    float   timbre;        /* 0-1, mode-dependent */
    float   color;         /* 0-1, brightness */
    float   attack, decay, sustain, release;
    float   volume;
    float   cents_mod;
    PDVoice voices[PD_MAX_VOICES];
} PhaseDistState;

/* ── Phase distortion functions ───────────────────────────────────────── */

static float pd_midi_to_freq(int note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

/* Apply phase distortion based on mode */
static float pd_distort_phase(float phase, int mode, float dist, float timbre) {
    /* phase is 0-1 (one cycle) */
    float p = phase;

    switch (mode) {
    case PD_MODE_RESONANT: {
        /* Pinch phase near the peak — creates resonant formant */
        float d = dist * 0.9f;
        if (p < 0.5f) {
            float t = p * 2.0f;
            p = (1.0f - d) * t + d * t * t;
            p *= 0.5f;
        } else {
            float t = (p - 0.5f) * 2.0f;
            p = 0.5f + ((1.0f - d) * t + d * (1.0f - (1.0f - t) * (1.0f - t))) * 0.5f;
        }
        break;
    }
    case PD_MODE_SAW: {
        /* Asymmetric ramp — more distortion = sharper rise */
        float d = 0.01f + dist * 0.98f;
        if (p < d) p = p / d * 0.5f;
        else       p = 0.5f + (p - d) / (1.0f - d) * 0.5f;
        break;
    }
    case PD_MODE_PULSE: {
        /* Phase jump creates pulse width modulation */
        float width = 0.1f + timbre * 0.8f;
        if (p < width) p = p / width * 0.5f;
        else           p = 0.5f + (p - width) / (1.0f - width) * 0.5f;
        /* Add distortion as edge sharpness */
        float d = dist * 4.0f;
        p = p + d * sinf(p * PD_TAU) * 0.05f;
        break;
    }
    case PD_MODE_COSINE: {
        /* Phase-distorted cosine — warm analog character */
        float d = dist * 0.5f;
        p = p + d * sinf(p * PD_TAU * 2.0f) * 0.15f;
        break;
    }
    case PD_MODE_SYNC: {
        /* Hard sync effect — phase wraps at a rate controlled by timbre */
        float sync_ratio = 1.0f + timbre * 7.0f;
        p = fmodf(p * sync_ratio, 1.0f);
        /* Distortion controls the blend between synced and original */
        p = p * dist + phase * (1.0f - dist);
        break;
    }
    case PD_MODE_WAVEFOLD: {
        /* Phase wrapping — creates dense harmonics */
        float folds = 1.0f + dist * 7.0f;
        p = p * folds;
        /* Triangle fold */
        p = fmodf(p, 2.0f);
        if (p > 1.0f) p = 2.0f - p;
        break;
    }
    }

    return p;
}

/* ── Envelope ─────────────────────────────────────────────────────────── */

static float pd_env_tick(PDVoice *v, float atk, float dec, float sus, float rel, float dt) {
    v->env_time += dt;
    switch (v->env_state) {
    case 0:
        if (atk <= 0.001f) { v->env_level = 1; v->env_state = 1; v->env_time = 0; }
        else { v->env_level = v->env_time / atk; if (v->env_level >= 1) { v->env_level = 1; v->env_state = 1; v->env_time = 0; } }
        break;
    case 1:
        if (dec <= 0.001f) { v->env_level = sus; v->env_state = 2; }
        else { v->env_level = 1 - (1 - sus) * (v->env_time / dec); if (v->env_level <= sus) { v->env_level = sus; v->env_state = 2; } }
        break;
    case 2: v->env_level = sus; break;
    case 3:
        if (rel <= 0.001f) v->env_level = 0;
        else { v->env_level = v->release_level * (1 - v->env_time / rel); if (v->env_level < 0) v->env_level = 0; }
        break;
    }
    return v->env_level;
}

/* ── InstrumentType interface ─────────────────────────────────────────── */

static void pd_set_param(void *state, const char *name, float value);

static void pd_init(void *state) {
    PhaseDistState *s = (PhaseDistState *)state;
    memset(s, 0, sizeof(*s));
    s->mode = PD_MODE_RESONANT;
    s->distortion = 0.5f;
    s->timbre = 0.5f;
    s->color = 0.5f;
    s->volume = 1.0f;
    s->attack = 0.01f;
    s->decay = 0.2f;
    s->sustain = 0.6f;
    s->release = 0.3f;
}

static void pd_destroy(void *state) { (void)state; }

static void pd_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2) {
    PhaseDistState *s = (PhaseDistState *)state;
    uint8_t type = status & 0xF0;
    switch (type) {
    case 0x90:
        if (d2 > 0) {
            int vi = -1;
            for (int i = 0; i < PD_MAX_VOICES; i++) if (!s->voices[i].active) { vi = i; break; }
            if (vi < 0) { vi = 0; s->voices[vi].killing = 1; s->voices[vi].kill_pos = 0; }
            PDVoice *v = &s->voices[vi];
            memset(v, 0, sizeof(*v));
            v->active = 1; v->note = d1;
            v->freq = pd_midi_to_freq(d1);
            v->velocity = (float)d2 / 127.0f;
            fprintf(stderr, "[phase-dist] note_on %d vel=%d freq=%.1f voice=%d\n", d1, d2, (double)v->freq, vi);
        } else {
            for (int i = 0; i < PD_MAX_VOICES; i++) {
                PDVoice *v = &s->voices[i];
                if (v->active && v->note == d1 && v->env_state != 3) {
                    v->env_state = 3; v->release_level = v->env_level; v->env_time = 0;
                }
            }
        }
        break;
    case 0x80:
        for (int i = 0; i < PD_MAX_VOICES; i++) {
            PDVoice *v = &s->voices[i];
            if (v->active && v->note == d1 && v->env_state != 3) {
                v->env_state = 3; v->release_level = v->env_level; v->env_time = 0;
            }
        }
        break;
    case 0xB0: {
        float cc = (float)d2 / 127.0f;
        switch (d1) {
        case 14: s->distortion = cc; break;
        case 15: s->timbre = cc; break;
        case 16: s->mode = (int)(cc * 5.99f) % PD_MODE_COUNT; break;
        case 17: s->color = cc; break;
        case 18: s->attack = 0.001f * powf(3000.0f, cc); break;
        case 19: s->decay = 0.01f * powf(500.0f, cc); break;
        case 20: s->sustain = cc; break;
        case 21: s->release = 0.01f * powf(500.0f, cc); break;
        case 120: case 123:
            for (int i = 0; i < PD_MAX_VOICES; i++) s->voices[i].active = 0;
            break;
        }
        break;
    }
    }
}

static void pd_render(void *state, float *stereo_buf, int frames, int sample_rate) {
    PhaseDistState *s = (PhaseDistState *)state;
    float dt = 1.0f / (float)sample_rate;

    float pitch_mult = (s->cents_mod != 0.0f) ? powf(2.0f, s->cents_mod / 1200.0f) : 1.0f;

    for (int i = 0; i < frames; i++) {
        float mix = 0;

        for (int vi = 0; vi < PD_MAX_VOICES; vi++) {
            PDVoice *v = &s->voices[vi];
            if (!v->active) continue;
            v->age += dt;
            if (v->age > 30.0f) { v->active = 0; continue; }

            float env = pd_env_tick(v, s->attack, s->decay, s->sustain, s->release, dt);
            if (env <= 0 && v->env_state == 3) { v->active = 0; continue; }

            /* Phase distortion synthesis */
            float raw_phase = v->phase; /* 0-1 */
            float dist_phase = pd_distort_phase(raw_phase, s->mode, s->distortion, s->timbre);

            /* Generate sample from distorted phase */
            float sample = sinf(dist_phase * PD_TAU);

            /* Color: one-pole LPF */
            if (s->color < 0.99f) {
                float cutoff_hz = 20.0f * powf(800.0f, s->color); /* 20→16kHz */
                float rc = 1.0f / (PD_TAU * cutoff_hz);
                float alpha = dt / (rc + dt);
                v->lpf_z += alpha * (sample - v->lpf_z);
                sample = v->lpf_z;
            }

            sample *= env * v->velocity * 0.5f;

            /* Anti-click */
            if (v->sample_count < PD_FADEIN) sample *= (float)v->sample_count / PD_FADEIN;
            v->sample_count++;
            if (v->killing) {
                float fade = 1.0f - (float)v->kill_pos / PD_FADEOUT;
                if (fade <= 0) { v->active = 0; continue; }
                sample *= fade; v->kill_pos++;
            }

            mix += sample;

            /* Advance phase */
            v->phase += v->freq * pitch_mult * dt;
            while (v->phase >= 1.0f) v->phase -= 1.0f;
        }

        mix *= s->volume;
        if (mix > 0.95f) mix = 0.95f;
        if (mix < -0.95f) mix = -0.95f;
        stereo_buf[i * 2] = mix;
        stereo_buf[i * 2 + 1] = mix;
    }
}

/* ── set_param ────────────────────────────────────────────────────────── */

static void pd_set_param(void *state, const char *name, float value) {
    PhaseDistState *s = (PhaseDistState *)state;
    if (strcmp(name, "mode") == 0)        { s->mode = (int)value % PD_MODE_COUNT; }
    else if (strcmp(name, "distortion") == 0) s->distortion = value < 0 ? 0 : value > 1 ? 1 : value;
    else if (strcmp(name, "timbre") == 0)     s->timbre = value < 0 ? 0 : value > 1 ? 1 : value;
    else if (strcmp(name, "color") == 0)      s->color = value < 0 ? 0 : value > 1 ? 1 : value;
    else if (strcmp(name, "volume") == 0)     s->volume = value < 0 ? 0 : value > 1 ? 1 : value;
    else if (strcmp(name, "attack") == 0)     s->attack = value;
    else if (strcmp(name, "decay") == 0)      s->decay = value;
    else if (strcmp(name, "sustain") == 0)    s->sustain = value;
    else if (strcmp(name, "release") == 0)    s->release = value;
    fprintf(stderr, "[phase-dist] set_param %s = %.4f\n", name, (double)value);
}

/* ── OSC ──────────────────────────────────────────────────────────────── */

static void pd_osc_handle(void *state, const char *sub_path,
                           const int32_t *iargs, int ni,
                           const float *fargs, int nf) {
    (void)iargs; (void)ni;
    if (strncmp(sub_path, "/param/", 7) == 0 && nf >= 1)
        pd_set_param(state, sub_path + 7, fargs[0]);
    else if (strcmp(sub_path, "/volume") == 0 && nf >= 1)
        pd_set_param(state, "volume", fargs[0]);
}

static int pd_osc_status(void *state, uint8_t *buf, int max_len) {
    (void)state; (void)buf; (void)max_len; return 0;
}

/* ── JSON ─────────────────────────────────────────────────────────────── */

static int pd_json_status(void *state, char *buf, int max) {
    PhaseDistState *s = (PhaseDistState *)state;
    static const char *modes[] = {"resonant","saw","pulse","cosine","sync","wavefold"};
    int active = 0;
    for (int i = 0; i < PD_MAX_VOICES; i++) if (s->voices[i].active) active++;
    return snprintf(buf, (size_t)max,
        "\"instrument_type\":\"phase-dist\","
        "\"mode\":\"%s\",\"mode_index\":%d,\"active_voices\":%d",
        modes[s->mode], s->mode, active);
}

static int pd_json_save(void *state, char *buf, int max) {
    PhaseDistState *s = (PhaseDistState *)state;
    return snprintf(buf, (size_t)max,
        "\"mode\":%d,\"distortion\":%.4f,\"timbre\":%.4f,\"color\":%.4f,"
        "\"volume\":%.4f,\"attack\":%.4f,\"decay\":%.4f,\"sustain\":%.4f,\"release\":%.4f",
        s->mode, (double)s->distortion, (double)s->timbre, (double)s->color,
        (double)s->volume, (double)s->attack, (double)s->decay,
        (double)s->sustain, (double)s->release);
}

static int pd_json_load(void *state, const char *json) {
    PhaseDistState *s = (PhaseDistState *)state;
    int ival; float fval;
    if (json_get_int(json, "mode", &ival) == 0) s->mode = ival % PD_MODE_COUNT;
    if (json_get_float(json, "distortion", &fval) == 0) s->distortion = fval;
    if (json_get_float(json, "timbre", &fval) == 0) s->timbre = fval;
    if (json_get_float(json, "color", &fval) == 0) s->color = fval;
    if (json_get_float(json, "volume", &fval) == 0) s->volume = fval;
    if (json_get_float(json, "attack", &fval) == 0) s->attack = fval;
    if (json_get_float(json, "decay", &fval) == 0) s->decay = fval;
    if (json_get_float(json, "sustain", &fval) == 0) s->sustain = fval;
    if (json_get_float(json, "release", &fval) == 0) s->release = fval;
    return 0;
}

/* ── Type descriptor ──────────────────────────────────────────────────── */

InstrumentType phase_dist_type = {
    .name         = "phase-dist",
    .display_name = "Phase Distortion",
    .state_size   = sizeof(PhaseDistState),
    .init         = pd_init,
    .destroy      = pd_destroy,
    .midi         = pd_midi,
    .render       = pd_render,
    .set_param    = pd_set_param,
    .json_status  = pd_json_status,
    .json_save    = pd_json_save,
    .json_load    = pd_json_load,
    .osc_handle   = pd_osc_handle,
    .osc_status   = pd_osc_status,
};

#endif /* PHASE_DIST_H */
