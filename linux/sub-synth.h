#ifndef SUB_SYNTH_H
#define SUB_SYNTH_H

#include "instruments.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SUB_TAU              (2.0f * (float)M_PI)
#define SUB_MAX_VOICES       16
#define SUB_LIMITER_CEIL     0.95f
#define SUB_FADEIN_SAMPLES   48
#define SUB_FADEOUT_SAMPLES  96

/* ── Waveforms ────────────────────────────────────────────────────────── */

enum {
    SUB_WAVE_SAW = 0,
    SUB_WAVE_SQUARE,
    SUB_WAVE_PULSE,
    SUB_WAVE_TRI,
    SUB_WAVE_SINE,
    SUB_WAVE_NOISE,
    SUB_WAVE_COUNT
};

/* ── Envelope ─────────────────────────────────────────────────────────── */

typedef enum { SUB_ENV_ATTACK, SUB_ENV_DECAY, SUB_ENV_SUSTAIN, SUB_ENV_RELEASE } SubEnvState;

typedef struct {
    SubEnvState state;
    float       level;
    float       time;
    float       release_level;
} SubEnv;

/* ── Voice ────────────────────────────────────────────────────────────── */

typedef struct {
    int         active;
    int         note;
    float       freq;
    float       velocity;
    float       phase;          /* oscillator phase [0, TAU) */
    uint32_t    noise_state;    /* xorshift PRNG */
    SubEnv      amp_env;
    SubEnv      filt_env;
    /* State-variable filter (Cytomic SVF) */
    float       svf_ic1eq;
    float       svf_ic2eq;
    /* Anti-click */
    float       age;
    int         sample_count;
    int         killing;
    int         kill_pos;
    int         preset_idx;     /* for future use */
} SubVoice;

/* ── Parameters ───────────────────────────────────────────────────────── */

typedef struct {
    int   waveform;         /* SUB_WAVE_xxx */
    float pulse_width;      /* 0.05 - 0.95 */
    float filter_cutoff;    /* Hz, 20 - 20000 */
    float filter_reso;      /* 0.0 - 1.0 */
    float filter_env_depth; /* -1.0 to 1.0 (octaves of sweep) */
    float filt_attack;
    float filt_decay;
    float filt_sustain;
    float filt_release;
    float amp_attack;
    float amp_decay;
    float amp_sustain;
    float amp_release;
} SubSynthParams;

/* ── Synth state ──────────────────────────────────────────────────────── */

typedef struct {
    SubVoice       voices[SUB_MAX_VOICES];
    SubSynthParams params;
    float          volume;
} SubSynth;

/* ── Helpers ──────────────────────────────────────────────────────────── */

static inline float sub_midi_to_freq(int note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

/* Soft-knee limiter (same as fm-synth / yama-bruh) */
static inline float sub_limiter(float sample) {
    float absval = fabsf(sample);
    if (absval > 0.5f) {
        float over = absval - 0.5f;
        float gain = 0.5f + over / (1.0f + over * 2.0f);
        sample = (sample < 0.0f) ? -gain : gain;
    }
    if (sample > SUB_LIMITER_CEIL)       sample = SUB_LIMITER_CEIL;
    else if (sample < -SUB_LIMITER_CEIL) sample = -SUB_LIMITER_CEIL;
    return sample;
}

/* XorShift noise */
static inline float sub_noise(uint32_t *state) {
    uint32_t s = *state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    if (s == 0) s = 1;
    *state = s;
    return (float)(s & 0x7FFFFFFF) / (float)0x7FFFFFFF * 2.0f - 1.0f;
}

/* ── PolyBLEP anti-aliasing ───────────────────────────────────────────── */

static inline float polyblep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/* ── Oscillator ───────────────────────────────────────────────────────── */

static inline float sub_oscillator(SubVoice *v, const SubSynthParams *p, float dt) {
    float phase_norm = v->phase / SUB_TAU; /* [0, 1) */
    float freq_dt = v->freq * dt;          /* phase increment in [0,1) per sample */
    float out = 0.0f;

    switch (p->waveform) {
    case SUB_WAVE_SAW:
        out = 2.0f * phase_norm - 1.0f;
        out -= polyblep(phase_norm, freq_dt);
        break;

    case SUB_WAVE_SQUARE:
        out = (phase_norm < 0.5f) ? 1.0f : -1.0f;
        out += polyblep(phase_norm, freq_dt);
        out -= polyblep(fmodf(phase_norm + 0.5f, 1.0f), freq_dt);
        break;

    case SUB_WAVE_PULSE: {
        float pw = p->pulse_width;
        out = (phase_norm < pw) ? 1.0f : -1.0f;
        out += polyblep(phase_norm, freq_dt);
        out -= polyblep(fmodf(phase_norm + 1.0f - pw, 1.0f), freq_dt);
        break;
    }

    case SUB_WAVE_TRI:
        /* Integrated square → triangle */
        out = 2.0f * phase_norm - 1.0f;
        out -= polyblep(phase_norm, freq_dt);
        /* Leaky integrator would be ideal but naive triangle is fine */
        out = 2.0f * fabsf(out) - 1.0f;
        break;

    case SUB_WAVE_SINE:
        out = sinf(v->phase);
        break;

    case SUB_WAVE_NOISE:
        out = sub_noise(&v->noise_state);
        break;
    }

    /* Advance phase */
    v->phase += SUB_TAU * v->freq * dt;
    if (v->phase >= SUB_TAU) v->phase -= SUB_TAU;

    return out;
}

/* ── ADSR Envelope ────────────────────────────────────────────────────── */

static inline float sub_env_tick(SubEnv *env, float atk, float dec, float sus,
                                  float rel, float dt)
{
    switch (env->state) {
    case SUB_ENV_ATTACK: {
        float e = (atk > 0.0001f) ? env->time / atk : 1.0f;
        if (e >= 1.0f) {
            env->state = SUB_ENV_DECAY;
            env->time = 0.0f;
            env->level = 1.0f;
        } else {
            env->level = e;
        }
        env->time += dt;
        break;
    }
    case SUB_ENV_DECAY: {
        float t = (dec > 0.0001f) ? env->time / dec : 1.0f;
        if (t >= 1.0f) {
            env->state = SUB_ENV_SUSTAIN;
            env->level = sus;
        } else {
            env->level = 1.0f - (1.0f - sus) * t;
        }
        env->time += dt;
        break;
    }
    case SUB_ENV_SUSTAIN:
        env->level = sus;
        break;
    case SUB_ENV_RELEASE: {
        float t = (rel > 0.0001f) ? env->time / rel : 1.0f;
        if (t >= 1.0f) {
            env->level = 0.0f;
            return 0.0f; /* signal dead */
        }
        env->level = env->release_level * (1.0f - t);
        env->time += dt;
        break;
    }
    }
    return env->level;
}

/* ── State-Variable Filter (Cytomic / Andrew Simper) ──────────────────── */

static inline float sub_svf_lp(SubVoice *v, float input, float cutoff_hz,
                                 float reso, int sample_rate)
{
    /* Clamp cutoff */
    float nyquist = (float)sample_rate * 0.5f;
    if (cutoff_hz < 20.0f)    cutoff_hz = 20.0f;
    if (cutoff_hz > nyquist - 100.0f) cutoff_hz = nyquist - 100.0f;

    float g = tanf((float)M_PI * cutoff_hz / (float)sample_rate);
    float k = 2.0f - 2.0f * reso; /* reso 0→k=2 (no reso), reso 1→k=0 (self-osc) */
    if (k < 0.01f) k = 0.01f;     /* prevent division instability */

    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;

    float v3 = input - v->svf_ic2eq;
    float v1 = a1 * v->svf_ic1eq + a2 * v3;
    float v2 = v->svf_ic2eq + a2 * v->svf_ic1eq + a3 * v3;

    v->svf_ic1eq = 2.0f * v1 - v->svf_ic1eq;
    v->svf_ic2eq = 2.0f * v2 - v->svf_ic2eq;

    return v2; /* lowpass output */
}

/* ── Voice management ─────────────────────────────────────────────────── */

static void sub_note_on(SubSynth *s, int note, int vel) {
    float freq = sub_midi_to_freq(note);
    float velocity = (float)vel / 127.0f;

    /* Fade-out duplicate notes */
    for (int i = 0; i < SUB_MAX_VOICES; i++) {
        if (s->voices[i].active && s->voices[i].note == note && !s->voices[i].killing) {
            s->voices[i].killing = 1;
            s->voices[i].kill_pos = 0;
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < SUB_MAX_VOICES; i++) {
        if (!s->voices[i].active) { slot = i; break; }
    }
    /* Steal oldest */
    if (slot < 0) {
        float oldest = 0;
        slot = 0;
        for (int i = 0; i < SUB_MAX_VOICES; i++) {
            if (s->voices[i].age > oldest) {
                oldest = s->voices[i].age;
                slot = i;
            }
        }
    }

    SubVoice *v = &s->voices[slot];
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->note = note;
    v->freq = freq;
    v->velocity = velocity;
    v->noise_state = (uint32_t)(note * 1103515245 + 12345) | 1;
    v->amp_env.state = SUB_ENV_ATTACK;
    v->filt_env.state = SUB_ENV_ATTACK;
}

static void sub_note_off(SubSynth *s, int note) {
    for (int i = 0; i < SUB_MAX_VOICES; i++) {
        SubVoice *v = &s->voices[i];
        if (v->active && v->note == note && v->amp_env.state != SUB_ENV_RELEASE) {
            v->amp_env.state = SUB_ENV_RELEASE;
            v->amp_env.release_level = v->amp_env.level;
            v->amp_env.time = 0.0f;
            v->filt_env.state = SUB_ENV_RELEASE;
            v->filt_env.release_level = v->filt_env.level;
            v->filt_env.time = 0.0f;
        }
    }
}

/* ── InstrumentType interface ─────────────────────────────────────────── */

static void sub_synth_init(void *state) {
    SubSynth *s = (SubSynth *)state;
    memset(s, 0, sizeof(*s));
    s->volume = 1.0f;
    s->params.waveform = SUB_WAVE_SAW;
    s->params.pulse_width = 0.5f;
    s->params.filter_cutoff = 8000.0f;
    s->params.filter_reso = 0.0f;
    s->params.filter_env_depth = 0.5f;
    s->params.filt_attack = 0.01f;
    s->params.filt_decay = 0.2f;
    s->params.filt_sustain = 0.3f;
    s->params.filt_release = 0.3f;
    s->params.amp_attack = 0.005f;
    s->params.amp_decay = 0.2f;
    s->params.amp_sustain = 0.8f;
    s->params.amp_release = 0.3f;
}

static void sub_synth_destroy(void *state) { (void)state; }

static void sub_synth_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2) {
    SubSynth *s = (SubSynth *)state;
    uint8_t type = status & 0xF0;

    switch (type) {
    case 0x90:
        if (d2 > 0) sub_note_on(s, d1, d2);
        else        sub_note_off(s, d1);
        break;
    case 0x80:
        sub_note_off(s, d1);
        break;
    case 0xB0: /* CC */
        switch (d1) {
        case 70: /* waveform */
            s->params.waveform = d2 % SUB_WAVE_COUNT;
            break;
        case 71: /* resonance */
            s->params.filter_reso = (float)d2 / 127.0f;
            break;
        case 72: /* amp release */
            s->params.amp_release = 0.001f + (float)d2 / 127.0f * 5.0f;
            break;
        case 73: /* amp attack */
            s->params.amp_attack = 0.001f + (float)d2 / 127.0f * 5.0f;
            break;
        case 74: /* filter cutoff */
            /* Exponential: CC 0→20Hz, CC 127→20kHz */
            s->params.filter_cutoff = 20.0f * powf(1000.0f, (float)d2 / 127.0f);
            break;
        case 75: /* pulse width */
            s->params.pulse_width = 0.05f + (float)d2 / 127.0f * 0.9f;
            break;
        case 76: /* filter env depth */
            s->params.filter_env_depth = (float)d2 / 63.5f - 1.0f; /* -1 to +1 */
            break;
        case 77: s->params.filt_attack = 0.001f + (float)d2 / 127.0f * 5.0f; break;
        case 78: s->params.filt_decay  = 0.001f + (float)d2 / 127.0f * 5.0f; break;
        case 79: s->params.filt_sustain = (float)d2 / 127.0f; break;
        case 80: s->params.filt_release = 0.001f + (float)d2 / 127.0f * 5.0f; break;
        case 81: s->params.amp_decay   = 0.001f + (float)d2 / 127.0f * 5.0f; break;
        case 82: s->params.amp_sustain = (float)d2 / 127.0f; break;
        case 120: case 123: /* All Notes/Sound Off */
            for (int i = 0; i < SUB_MAX_VOICES; i++) s->voices[i].active = 0;
            break;
        }
        break;
    }
}

static void sub_synth_render(void *state, float *stereo_buf, int frames,
                              int sample_rate)
{
    SubSynth *s = (SubSynth *)state;
    const float dt = 1.0f / (float)sample_rate;
    const SubSynthParams *p = &s->params;

    /* Polyphony headroom */
    int active_count = 0;
    for (int v = 0; v < SUB_MAX_VOICES; v++) {
        if (s->voices[v].active) active_count++;
    }
    float poly_gain = (active_count <= 1) ? 1.0f : 1.0f / sqrtf((float)active_count);

    for (int i = 0; i < frames; i++) {
        float mix = 0.0f;

        for (int vi = 0; vi < SUB_MAX_VOICES; vi++) {
            SubVoice *v = &s->voices[vi];
            if (!v->active) continue;

            v->age += dt;
            if (v->age > 30.0f) { v->active = 0; continue; }

            /* Amp envelope */
            float amp = sub_env_tick(&v->amp_env, p->amp_attack, p->amp_decay,
                                      p->amp_sustain, p->amp_release, dt);

            /* Filter envelope */
            float fenv = sub_env_tick(&v->filt_env, p->filt_attack, p->filt_decay,
                                       p->filt_sustain, p->filt_release, dt);

            /* Dead voice */
            if (amp <= 0.0f && v->amp_env.state == SUB_ENV_RELEASE) {
                v->active = 0;
                continue;
            }

            /* Effective filter cutoff: base * 2^(depth * env * 4 octaves) */
            float cutoff = p->filter_cutoff * powf(2.0f, p->filter_env_depth * fenv * 4.0f);

            /* Oscillator */
            float osc = sub_oscillator(v, p, dt);

            /* Filter */
            float filtered = sub_svf_lp(v, osc, cutoff, p->filter_reso, sample_rate);

            /* Output */
            float sample = filtered * amp * v->velocity * 0.4f;

            /* Anti-click fade-in */
            if (v->sample_count < SUB_FADEIN_SAMPLES) {
                sample *= (float)v->sample_count / (float)SUB_FADEIN_SAMPLES;
            }
            v->sample_count++;

            /* Anti-click fade-out (voice kill) */
            if (v->killing) {
                float fade = 1.0f - (float)v->kill_pos / (float)SUB_FADEOUT_SAMPLES;
                if (fade <= 0.0f) { v->active = 0; continue; }
                sample *= fade;
                v->kill_pos++;
            }

            /* NaN guard */
            if (!isfinite(sample) || !isfinite(v->phase) ||
                !isfinite(v->svf_ic1eq) || !isfinite(v->svf_ic2eq)) {
                v->active = 0;
                continue;
            }

            mix += sample;
        }

        mix *= s->volume * poly_gain;
        mix = sub_limiter(mix);

        stereo_buf[i * 2]     = mix;
        stereo_buf[i * 2 + 1] = mix;
    }
}

/* ── OSC handler ──────────────────────────────────────────────────────── */

static void sub_synth_osc_handle(void *state, const char *sub_path,
                                  const int32_t *iargs, int ni,
                                  const float *fargs, int nf)
{
    SubSynth *s = (SubSynth *)state;

    if (strcmp(sub_path, "/volume") == 0 && nf >= 1) {
        float vol = fargs[0];
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 1.0f) vol = 1.0f;
        s->volume = vol;
    }
    else if (strncmp(sub_path, "/param/", 7) == 0) {
        const char *param = sub_path + 7;
        if (strcmp(param, "reset") == 0) {
            sub_synth_init(state);
        }
        else if (nf >= 1) {
            float val = fargs[0];
            if      (strcmp(param, "waveform")         == 0) s->params.waveform = (int)val % SUB_WAVE_COUNT;
            else if (strcmp(param, "pulse_width")       == 0) s->params.pulse_width = val;
            else if (strcmp(param, "filter_cutoff")     == 0) s->params.filter_cutoff = val;
            else if (strcmp(param, "filter_reso")       == 0) s->params.filter_reso = val;
            else if (strcmp(param, "filter_env_depth")  == 0) s->params.filter_env_depth = val;
            else if (strcmp(param, "filt_attack")       == 0) s->params.filt_attack = val;
            else if (strcmp(param, "filt_decay")        == 0) s->params.filt_decay = val;
            else if (strcmp(param, "filt_sustain")      == 0) s->params.filt_sustain = val;
            else if (strcmp(param, "filt_release")      == 0) s->params.filt_release = val;
            else if (strcmp(param, "amp_attack")        == 0) s->params.amp_attack = val;
            else if (strcmp(param, "amp_decay")         == 0) s->params.amp_decay = val;
            else if (strcmp(param, "amp_sustain")       == 0) s->params.amp_sustain = val;
            else if (strcmp(param, "amp_release")       == 0) s->params.amp_release = val;
        }
        else if (ni >= 1) {
            if (strcmp(param, "waveform") == 0) s->params.waveform = iargs[0] % SUB_WAVE_COUNT;
        }
    }
}

/* ── OSC status ───────────────────────────────────────────────────────── */

static int sub_synth_osc_status(void *state, uint8_t *buf, int max_len) {
    SubSynth *s = (SubSynth *)state;
    int pos = 0;
    int w;

    w = osc_write_string(buf + pos, max_len - pos, "/status");
    if (w < 0) return 0;
    pos += w;

    /* Type tags: i f i f f f f f f f f f f f i */
    w = osc_write_string(buf + pos, max_len - pos, ",ififfffffffffi");
    if (w < 0) return 0;
    pos += w;

    /* waveform (i) */
    if (pos + 4 > max_len) return 0;
    osc_write_i32(buf + pos, s->params.waveform); pos += 4;

    /* volume (f) */
    if (pos + 4 > max_len) return 0;
    osc_write_f32(buf + pos, s->volume); pos += 4;

    /* All params */
    if (pos + 4 > max_len) return 0;
    osc_write_i32(buf + pos, s->params.waveform); pos += 4;

    float params[] = {
        s->params.filter_cutoff, s->params.filter_reso,
        s->params.filter_env_depth, s->params.pulse_width,
        s->params.filt_attack, s->params.filt_decay,
        s->params.filt_sustain, s->params.filt_release,
        s->params.amp_attack, s->params.amp_decay,
        s->params.amp_sustain, s->params.amp_release,
    };
    for (int i = 0; i < 12; i++) {
        if (pos + 4 > max_len) return pos;
        osc_write_f32(buf + pos, params[i]); pos += 4;
    }

    /* active_voices (i) */
    if (pos + 4 > max_len) return pos;
    int active = 0;
    for (int v = 0; v < SUB_MAX_VOICES; v++)
        if (s->voices[v].active) active++;
    osc_write_i32(buf + pos, active); pos += 4;

    return pos;
}

/* ── Exported type descriptor ─────────────────────────────────────────── */

InstrumentType sub_synth_type = {
    .name         = "sub-synth",
    .display_name = "Subtractive Synth",
    .state_size   = sizeof(SubSynth),
    .init         = sub_synth_init,
    .destroy      = sub_synth_destroy,
    .midi         = sub_synth_midi,
    .render       = sub_synth_render,
    .osc_handle   = sub_synth_osc_handle,
    .osc_status   = sub_synth_osc_status,
};

#endif /* SUB_SYNTH_H */
