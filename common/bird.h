#ifndef BIRD_H
#define BIRD_H

/* bird — single-sine bird phrase synthesizer
 *
 * A single sine oscillator mangled with pitch envelopes, vibrato,
 * and harmonic injection to simulate bird calls.
 *
 * Knob philosophy: CC14-21 cover chirp timing, pitch sweep,
 * vibrato (the warble), harmonic buzz, and envelope shape.
 *
 * TODO: seeded randomized phrase generation, multi-chirp patterns,
 *       breath noise layer, species presets.
 */

#include "instruments.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BIRD_TAU         (2.0f * (float)M_PI)
#define BIRD_MAX_VOICES  4

/* ── Voice ────────────────────────────────────────────────────────── */

typedef struct {
    int     active;
    int     note;
    float   root_freq;
    float   velocity;
    float   phase;
    float   vib_phase;    /* vibrato LFO phase */
    float   time;         /* time within current chirp cycle */
    float   chirp_dur;
    float   gap_dur;
    int     releasing;
    float   release_gain;
} BirdVoice;

/* ── State ────────────────────────────────────────────────────────── */

typedef struct {
    BirdVoice voices[BIRD_MAX_VOICES];
    float     volume;

    /* chirp timing */
    float     chirp_dur;   /* seconds per chirp */
    float     gap_dur;     /* silence between chirps */

    /* pitch sweep */
    float     drop_semi;   /* pitch sweep in semitones (negative = rising) */
    float     curve;       /* envelope curve (1=linear, >1=exponential) */

    /* vibrato */
    float     vib_depth;   /* vibrato depth in semitones */
    float     vib_rate;    /* vibrato rate in Hz */

    /* timbre */
    float     buzz;        /* harmonic content 0=pure sine, 1=raspy */
    float     chirp_shape; /* amp envelope: 0=percussive, 1=smooth swell */
} BirdState;

/* ── Helpers ──────────────────────────────────────────────────────── */

static inline float bird_midi_to_freq(int note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

/* ── Init / Destroy ───────────────────────────────────────────────── */

static void bird_init(void *state) {
    BirdState *s = (BirdState *)state;
    memset(s, 0, sizeof(*s));
    s->volume = 0.8f;
    s->chirp_dur = 0.12f;
    s->gap_dur = 0.08f;
    s->drop_semi = 12.0f;
    s->curve = 2.0f;
    s->vib_depth = 0.0f;
    s->vib_rate = 12.0f;
    s->buzz = 0.0f;
    s->chirp_shape = 0.3f;
}

static void bird_destroy(void *state) { (void)state; }

/* ── MIDI ─────────────────────────────────────────────────────────── */

static void bird_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2) {
    BirdState *s = (BirdState *)state;
    uint8_t cmd = status & 0xF0;

    if (cmd == 0x90 && d2 > 0) {
        int vi = -1;
        for (int i = 0; i < BIRD_MAX_VOICES; i++)
            if (!s->voices[i].active) { vi = i; break; }
        if (vi < 0) vi = 0;

        BirdVoice *v = &s->voices[vi];
        v->active = 1;
        v->note = d1;
        v->root_freq = bird_midi_to_freq(d1);
        v->velocity = (float)d2 / 127.0f;
        v->phase = 0.0f;
        v->vib_phase = 0.0f;
        v->time = 0.0f;
        v->chirp_dur = s->chirp_dur;
        v->gap_dur = s->gap_dur;
        v->releasing = 0;
        v->release_gain = 1.0f;
    }
    else if (cmd == 0xB0) {
        float cc = (float)d2 / 127.0f;
        switch (d1) {
        case 14: /* rate — chirp speed */
            s->chirp_dur = 0.5f * powf(0.04f, cc); /* 0.5s → 0.02s */
            s->gap_dur = s->chirp_dur * 0.6f;
            break;
        case 15: /* drop — pitch sweep, bipolar */
            s->drop_semi = (cc - 0.5f) * 48.0f; /* -24 to +24 semi */
            break;
        case 16: /* curve — sweep shape */
            s->curve = 0.2f + cc * 5.8f; /* 0.2 → 6.0 */
            break;
        case 17: /* vibrato depth */
            s->vib_depth = cc * 4.0f; /* 0 → 4 semitones */
            break;
        case 18: /* vibrato rate */
            s->vib_rate = 2.0f * powf(30.0f, cc); /* 2 → 60 Hz */
            break;
        case 19: /* buzz — harmonic content */
            s->buzz = cc;
            break;
        case 20: /* chirp shape — amp envelope */
            s->chirp_shape = cc;
            break;
        case 21: /* gap — pause between chirps */
            s->gap_dur = cc * 2.0f; /* 0 → 2 seconds */
            break;
        case 120: case 123:
            for (int i = 0; i < BIRD_MAX_VOICES; i++)
                s->voices[i].active = 0;
            break;
        }
    }
    else if (cmd == 0x80 || (cmd == 0x90 && d2 == 0)) {
        for (int i = 0; i < BIRD_MAX_VOICES; i++)
            if (s->voices[i].active && s->voices[i].note == d1)
                s->voices[i].releasing = 1;
    }
}

/* ── Render ────────────────────────────────────────────────────────── */

static void bird_render(void *state, float *buf, int frames, int sample_rate) {
    BirdState *s = (BirdState *)state;
    float dt = 1.0f / (float)sample_rate;
    float cycle_dur = s->chirp_dur + s->gap_dur;
    if (cycle_dur < 0.01f) cycle_dur = 0.01f;

    for (int i = 0; i < frames; i++) {
        float mix = 0.0f;

        for (int vi = 0; vi < BIRD_MAX_VOICES; vi++) {
            BirdVoice *v = &s->voices[vi];
            if (!v->active) continue;

            float t_in_cycle = fmodf(v->time, cycle_dur);
            float sample = 0.0f;

            if (t_in_cycle < v->chirp_dur && v->chirp_dur > 0.001f) {
                float t_norm = t_in_cycle / v->chirp_dur;

                /* pitch sweep */
                float curve_t = powf(t_norm, s->curve);
                float sweep_mult = powf(2.0f, -s->drop_semi * curve_t / 12.0f);
                float freq = v->root_freq * sweep_mult;

                /* vibrato */
                if (s->vib_depth > 0.001f) {
                    float vib = sinf(v->vib_phase) * s->vib_depth;
                    freq *= powf(2.0f, vib / 12.0f);
                    v->vib_phase += BIRD_TAU * s->vib_rate * dt;
                    if (v->vib_phase >= BIRD_TAU) v->vib_phase -= BIRD_TAU;
                }

                /* phase advance */
                v->phase += BIRD_TAU * freq * dt;
                if (v->phase >= BIRD_TAU) v->phase -= BIRD_TAU;

                /* oscillator: sine + harmonics for buzz */
                sample = sinf(v->phase);
                if (s->buzz > 0.001f) {
                    /* add odd harmonics progressively for raspy quality */
                    sample += s->buzz * 0.5f * sinf(v->phase * 3.0f);
                    sample += s->buzz * 0.3f * sinf(v->phase * 5.0f);
                    sample += s->buzz * 0.15f * sinf(v->phase * 7.0f);
                    sample /= (1.0f + s->buzz * 0.95f); /* normalize */
                }

                /* amplitude envelope within chirp */
                float amp;
                float shape = s->chirp_shape;
                /* shape 0 = sharp percussive (fast attack, fast decay)
                 * shape 0.5 = natural bird (quick attack, gradual fade)
                 * shape 1 = smooth swell (slow attack, slow decay) */
                float atk_t = 0.02f + shape * 0.4f;  /* 2-42% of chirp is attack */
                float dec_t = 0.3f + shape * 0.5f;    /* decay starts 30-80% in */

                if (t_norm < atk_t)
                    amp = t_norm / atk_t;
                else if (t_norm > dec_t)
                    amp = (1.0f - t_norm) / (1.0f - dec_t);
                else
                    amp = 1.0f;

                /* extra smoothing for high shape values */
                if (shape > 0.5f) {
                    float s2 = (shape - 0.5f) * 2.0f;
                    amp = amp * (1.0f - s2) + (0.5f - 0.5f * cosf(t_norm * (float)M_PI)) * s2;
                }

                sample *= amp;
            }

            /* release fade */
            if (v->releasing) {
                v->release_gain -= dt * 10.0f;
                if (v->release_gain <= 0.0f) {
                    v->active = 0;
                    continue;
                }
            }

            mix += sample * v->velocity * v->release_gain;
            v->time += dt;
        }

        mix *= s->volume;
        buf[i * 2]     += mix;
        buf[i * 2 + 1] += mix;
    }
}

/* ── Params ───────────────────────────────────────────────────────── */

static void bird_set_param(void *state, const char *name, float value) {
    BirdState *s = (BirdState *)state;
    if (strcmp(name, "volume") == 0)       s->volume = value < 0 ? 0 : value > 1 ? 1 : value;
    else if (strcmp(name, "drop") == 0)    s->drop_semi = value;
    else if (strcmp(name, "chirp") == 0)   s->chirp_dur = value < 0.01f ? 0.01f : value > 1 ? 1 : value;
    else if (strcmp(name, "gap") == 0)     s->gap_dur = value < 0 ? 0 : value > 2 ? 2 : value;
    else if (strcmp(name, "curve") == 0)   s->curve = value < 0.1f ? 0.1f : value > 8 ? 8 : value;
    else if (strcmp(name, "vib_depth") == 0) s->vib_depth = value < 0 ? 0 : value > 8 ? 8 : value;
    else if (strcmp(name, "vib_rate") == 0)  s->vib_rate = value < 0.1f ? 0.1f : value > 100 ? 100 : value;
    else if (strcmp(name, "buzz") == 0)    s->buzz = value < 0 ? 0 : value > 1 ? 1 : value;
    else if (strcmp(name, "chirp_shape") == 0) s->chirp_shape = value < 0 ? 0 : value > 1 ? 1 : value;
}

/* ── JSON ─────────────────────────────────────────────────────────── */

static int bird_json_status(void *state, char *buf, int max) {
    BirdState *s = (BirdState *)state;
    return snprintf(buf, (size_t)max,
        "{\"volume\":%.3f,\"drop\":%.1f,\"chirp\":%.3f,"
        "\"gap\":%.3f,\"curve\":%.1f,"
        "\"vib_depth\":%.2f,\"vib_rate\":%.1f,"
        "\"buzz\":%.2f,\"chirp_shape\":%.2f}",
        (double)s->volume, (double)s->drop_semi,
        (double)s->chirp_dur, (double)s->gap_dur,
        (double)s->curve, (double)s->vib_depth,
        (double)s->vib_rate, (double)s->buzz,
        (double)s->chirp_shape);
}

static int bird_json_save(void *state, char *buf, int max) {
    return bird_json_status(state, buf, max);
}

static int bird_json_load(void *state, const char *json) {
    BirdState *s = (BirdState *)state;
    const char *p;
    if ((p = strstr(json, "\"volume\":")) != NULL)      s->volume = (float)atof(p + 9);
    if ((p = strstr(json, "\"drop\":")) != NULL)        s->drop_semi = (float)atof(p + 7);
    if ((p = strstr(json, "\"chirp\":")) != NULL)       s->chirp_dur = (float)atof(p + 8);
    if ((p = strstr(json, "\"gap\":")) != NULL)         s->gap_dur = (float)atof(p + 6);
    if ((p = strstr(json, "\"curve\":")) != NULL)       s->curve = (float)atof(p + 8);
    if ((p = strstr(json, "\"vib_depth\":")) != NULL)   s->vib_depth = (float)atof(p + 12);
    if ((p = strstr(json, "\"vib_rate\":")) != NULL)    s->vib_rate = (float)atof(p + 11);
    if ((p = strstr(json, "\"buzz\":")) != NULL)        s->buzz = (float)atof(p + 7);
    if ((p = strstr(json, "\"chirp_shape\":")) != NULL) s->chirp_shape = (float)atof(p + 14);
    return 0;
}

/* ── OSC ──────────────────────────────────────────────────────────── */

static void bird_osc_handle(void *state, const char *sub_path,
                            const int32_t *iargs, int ni,
                            const float *fargs, int nf) {
    (void)iargs; (void)ni;
    if (strcmp(sub_path, "/volume") == 0 && nf >= 1)
        bird_set_param(state, "volume", fargs[0]);
    else if (strncmp(sub_path, "/param/", 7) == 0 && nf >= 1)
        bird_set_param(state, sub_path + 7, fargs[0]);
}

static int bird_osc_status(void *state, uint8_t *buf, int max_len) {
    (void)state; (void)buf; (void)max_len;
    return 0;
}

/* ── Type registration ────────────────────────────────────────────── */

static InstrumentType bird_type = {
    .name         = "bird",
    .display_name = "Bird",
    .state_size   = sizeof(BirdState),
    .init         = bird_init,
    .destroy      = bird_destroy,
    .midi         = bird_midi,
    .render       = bird_render,
    .calc         = NULL,
    .set_param    = bird_set_param,
    .json_status  = bird_json_status,
    .json_save    = bird_json_save,
    .json_load    = bird_json_load,
    .osc_handle   = bird_osc_handle,
    .osc_status   = bird_osc_status,
};

#endif
