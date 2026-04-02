#ifndef FM_SYNTH_H
#define FM_SYNTH_H

#include "instruments.h"
#include "presets.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FM_TAU          (2.0f * (float)M_PI)
#define FM_MAX_VOICES   16
#define FM_LIMITER_THRESHOLD 0.7f
#define FM_LIMITER_CEILING   0.95f
#define FM_FADEIN_SAMPLES    48     /* ~1ms @ 48kHz — anti-click on note start */
#define FM_FADEOUT_SAMPLES   96     /* ~2ms @ 48kHz — anti-click on voice kill/steal */

/* ── Envelope state ─────────────────────────────────────────────────── */

typedef enum { FM_ENV_ATTACK, FM_ENV_DECAY, FM_ENV_SUSTAIN, FM_ENV_RELEASE } FMEnvState;

/* ── Voice ──────────────────────────────────────────────────────────── */

typedef struct {
    int         active;
    int         note;
    float       freq;
    float       velocity;
    float       cp, mp;         /* carrier / modulator phase */
    float       prev_mod;       /* feedback memory */
    FMEnvState  env_state;
    float       env_level;
    float       env_time;
    float       release_level;  /* level at note-off */
    float       age;
    int         preset_idx;
    int         sample_count;   /* samples since note-on (for fade-in) */
    int         killing;        /* 1 = voice being killed with fade-out */
    int         kill_pos;       /* current fade-out sample position */
} FMVoice;

/* ── Live parameter overrides ───────────────────────────────────────── */

typedef struct {
    float carrier_ratio, mod_ratio, mod_index;
    float attack, decay, sustain, release, feedback;
    int   override;  /* 1 = use these instead of preset table */
} FMSynthParams;

/* ── Synth state ────────────────────────────────────────────────────── */

typedef struct {
    FMVoice       voices[FM_MAX_VOICES];
    int           current_preset;
    FMSynthParams live_params;
    float         volume;        /* internal volume, default 1.0 */
    float         limiter_env;   /* per-instance limiter envelope */
    float         cents_mod;     /* written by slot layer before render */
} FMSynth;

/* ── Helpers ────────────────────────────────────────────────────────── */

static float fm_midi_to_freq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

/* Soft-knee limiter (matching yama-bruh worklet — instant, no envelope lag) */
static inline float fm_limiter(FMSynth *s, float sample) {
    (void)s;
    float absval = fabsf(sample);

    if (absval > 0.5f) {
        float over = absval - 0.5f;
        float gain = 0.5f + over / (1.0f + over * 2.0f);
        sample = (sample < 0.0f) ? -gain : gain;
    }

    if (sample > FM_LIMITER_CEILING)       sample = FM_LIMITER_CEILING;
    else if (sample < -FM_LIMITER_CEILING) sample = -FM_LIMITER_CEILING;

    return sample;
}

/* Copy preset values into live_params (without enabling override) */
static void fm_load_preset_params(FMSynth *s, int idx) {
    if (idx < 0 || idx >= NUM_PRESETS) return;
    const FMPreset *p = &PRESETS[idx];
    s->live_params.carrier_ratio = p->carrier_ratio;
    s->live_params.mod_ratio     = p->mod_ratio;
    s->live_params.mod_index     = p->mod_index;
    s->live_params.attack        = p->attack;
    s->live_params.decay         = p->decay;
    s->live_params.sustain       = p->sustain;
    s->live_params.release       = p->release;
    s->live_params.feedback      = p->feedback;
}

/* ── Voice management ───────────────────────────────────────────────── */

static void fm_note_on(FMSynth *s, int note, int vel) {
    float freq = fm_midi_to_freq(note);
    float velocity = vel / 127.0f;

    /* Kill duplicate note — fade out instead of hard cut */
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (s->voices[i].active && s->voices[i].note == note && !s->voices[i].killing) {
            s->voices[i].killing = 1;
            s->voices[i].kill_pos = 0;
        }
    }

    /* Find free slot (prefer already-dead, then fading-out) */
    int slot = -1;
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        if (!s->voices[i].active) { slot = i; break; }
    }
    /* Voice steal: oldest */
    if (slot < 0) {
        float oldest = 0;
        slot = 0;
        for (int i = 0; i < FM_MAX_VOICES; i++) {
            if (s->voices[i].age > oldest) {
                oldest = s->voices[i].age;
                slot = i;
            }
        }
    }

    FMVoice *v = &s->voices[slot];
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->note = note;
    v->freq = freq;
    v->velocity = velocity;
    v->env_state = FM_ENV_ATTACK;
    v->preset_idx = s->current_preset;
    v->sample_count = 0;
    v->killing = 0;
    v->kill_pos = 0;
}

static void fm_note_off(FMSynth *s, int note) {
    for (int i = 0; i < FM_MAX_VOICES; i++) {
        FMVoice *v = &s->voices[i];
        if (v->active && v->note == note && v->env_state != FM_ENV_RELEASE) {
            v->env_state = FM_ENV_RELEASE;
            v->release_level = v->env_level;
            v->env_time = 0;
        }
    }
}

/* ── InstrumentType interface ───────────────────────────────────────── */

static void fm_synth_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2);
static void fm_synth_set_param(void *state, const char *name, float value);
static void fm_synth_osc_handle(void *state, const char *sub_path,
                                const int32_t *iargs, int ni,
                                const float *fargs, int nf);

static void fm_synth_init(void *state) {
    FMSynth *s = (FMSynth *)state;
    memset(s, 0, sizeof(*s));
    s->current_preset = 0;
    s->volume = 1.0f;
    s->limiter_env = 0.0f;
    fm_load_preset_params(s, 0);
}

static void fm_synth_destroy(void *state) {
    (void)state;
    /* Nothing to free — all inline */
}

static void fm_synth_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2) {
    FMSynth *s = (FMSynth *)state;
    uint8_t type = status & 0xF0;

    switch (type) {
    case 0x90: /* Note On */
        if (d2 > 0) fm_note_on(s, d1, d2);
        else        fm_note_off(s, d1);
        break;
    case 0x80: /* Note Off */
        fm_note_off(s, d1);
        break;
    case 0xB0: { /* CC */
        float cc = (float)d2 / 127.0f;
        switch (d1) {
        case 0: case 32: /* bank select = preset */
            if (d2 < NUM_PRESETS) {
                s->current_preset = d2;
                fprintf(stderr, "[fm-synth] preset %d: %s\n", d2, PRESET_NAMES[d2]);
            }
            break;
        /* ── Macro knobs CC14-21 ── */
        case 14: /* mod index — exponential 0→30 */
            s->live_params.mod_index = 30.0f * cc * cc;
            s->live_params.override = 1;
            break;
        case 15: /* mod ratio — 0.25→16, log */
            s->live_params.mod_ratio = 0.25f * powf(64.0f, cc);
            s->live_params.override = 1;
            break;
        case 16: /* carrier ratio — 0.25→8, log */
            s->live_params.carrier_ratio = 0.25f * powf(32.0f, cc);
            s->live_params.override = 1;
            break;
        case 17: /* feedback — 0→2.5 */
            s->live_params.feedback = cc * 2.5f;
            s->live_params.override = 1;
            break;
        case 18: /* attack — 0.001→3s log */
            s->live_params.attack = 0.001f * powf(3000.0f, cc);
            s->live_params.override = 1;
            break;
        case 19: /* decay — 0.01→5s log */
            s->live_params.decay = 0.01f * powf(500.0f, cc);
            s->live_params.override = 1;
            break;
        case 20: /* sustain — 0→1 linear */
            s->live_params.sustain = cc;
            s->live_params.override = 1;
            break;
        case 21: /* release — 0.01→5s log */
            s->live_params.release = 0.01f * powf(500.0f, cc);
            s->live_params.override = 1;
            break;
        case 120: case 123:
            for (int i = 0; i < FM_MAX_VOICES; i++) s->voices[i].active = 0;
            break;
        }
        break;
    }
    case 0xC0: /* Program Change */
        if (d1 < NUM_PRESETS) {
            s->current_preset = d1;
            fprintf(stderr, "[fm-synth] preset %d: %s\n", d1, PRESET_NAMES[d1]);
        }
        break;
    }
}

static void fm_synth_render(void *state, float *stereo_buf, int frames, int sample_rate) {
    FMSynth *s = (FMSynth *)state;
    const float dt = 1.0f / (float)sample_rate;

    /* KeySeq cents detune → pitch multiplier */
    float pitch_mult = (s->cents_mod != 0.0f)
        ? powf(2.0f, s->cents_mod / 1200.0f) : 1.0f;

    /* Count active voices for headroom scaling */
    int active_count = 0;
    for (int v = 0; v < FM_MAX_VOICES; v++) {
        if (s->voices[v].active) active_count++;
    }
    /* Scale down as voices stack: 1 voice = full, 4+ voices = reduced */
    float poly_gain = (active_count <= 1) ? 1.0f
                    : 1.0f / sqrtf((float)active_count);

    for (int i = 0; i < frames; i++) {
        float mix = 0.0f;

        for (int v = 0; v < FM_MAX_VOICES; v++) {
            FMVoice *vc = &s->voices[v];
            if (!vc->active) continue;

            const FMPreset *p = &PRESETS[vc->preset_idx];
            float atk, dec, sus, rel, mi, fb, crf, mrf;
            if (s->live_params.override) {
                atk = s->live_params.attack;
                dec = s->live_params.decay;
                sus = s->live_params.sustain;
                rel = s->live_params.release;
                mi  = s->live_params.mod_index;
                fb  = s->live_params.feedback;
                crf = vc->freq * s->live_params.carrier_ratio;
                mrf = vc->freq * s->live_params.mod_ratio;
            } else {
                atk = p->attack;
                dec = p->decay;
                sus = p->sustain;
                rel = p->release;
                mi  = p->mod_index;
                fb  = p->feedback;
                crf = vc->freq * p->carrier_ratio;
                mrf = vc->freq * p->mod_ratio;
            }
            crf *= pitch_mult;
            mrf *= pitch_mult;

            vc->age += dt;
            vc->env_time += dt;

            /* ADSR */
            float env = 0.0f;
            int dead = 0;

            switch (vc->env_state) {
            case FM_ENV_ATTACK: {
                float e = (atk > 0.0001f) ? vc->env_time / atk : 1.0f;
                if (e >= 1.0f) {
                    vc->env_state = FM_ENV_DECAY;
                    vc->env_time = 0;
                    vc->env_level = 1.0f;
                    env = 1.0f;
                } else {
                    vc->env_level = e;
                    env = e;
                }
            } break;
            case FM_ENV_DECAY: {
                float t = (dec > 0.0001f) ? vc->env_time / dec : 1.0f;
                if (t >= 1.0f) {
                    vc->env_state = FM_ENV_SUSTAIN;
                    env = sus;
                } else {
                    env = 1.0f - (1.0f - sus) * t;
                }
                vc->env_level = env;
            } break;
            case FM_ENV_SUSTAIN:
                env = sus;
                vc->env_level = sus;
                break;
            case FM_ENV_RELEASE: {
                float t = (rel > 0.0001f) ? vc->env_time / rel : 1.0f;
                if (t >= 1.0f) {
                    dead = 1;
                    env = 0;
                } else {
                    env = vc->release_level * (1.0f - t);
                }
            } break;
            }

            if (dead || vc->age > 30.0f) {
                vc->active = 0;
                continue;
            }

            /* 2-op FM synthesis */
            float mod_out = sinf(vc->mp + fb * vc->prev_mod);
            vc->prev_mod = mod_out;
            float sample = sinf(vc->cp + mi * mod_out) * env * vc->velocity * 0.35f;

            /* Anti-click fade-in (~1ms ramp on note start) */
            if (vc->sample_count < FM_FADEIN_SAMPLES) {
                sample *= (float)vc->sample_count / (float)FM_FADEIN_SAMPLES;
            }
            vc->sample_count++;

            /* Anti-click fade-out (voice kill/steal) */
            if (vc->killing) {
                float fade = 1.0f - (float)vc->kill_pos / (float)FM_FADEOUT_SAMPLES;
                if (fade <= 0.0f) { vc->active = 0; continue; }
                sample *= fade;
                vc->kill_pos++;
            }

            /* NaN guard */
            if (!isfinite(sample) || !isfinite(vc->cp) || !isfinite(vc->mp)) {
                vc->active = 0;
                continue;
            }

            mix += sample;

            /* Phase increment */
            vc->cp += FM_TAU * crf * dt;
            vc->mp += FM_TAU * mrf * dt;
            if (vc->cp > FM_TAU) vc->cp -= FM_TAU;
            if (vc->mp > FM_TAU) vc->mp -= FM_TAU;
        }

        mix *= s->volume * poly_gain;
        mix = fm_limiter(s, mix);

        stereo_buf[i * 2]     = mix;
        stereo_buf[i * 2 + 1] = mix;
    }
}

/* ── OSC helpers ────────────────────────────────────────────────────── */

/* osc_pad4, osc_write_i32, osc_write_f32, osc_write_string are defined
 * in miniwave.c before this header is included. */

static void fm_synth_osc_handle(void *state, const char *sub_path,
                                const int32_t *iargs, int ni,
                                const float *fargs, int nf)
{
    FMSynth *s = (FMSynth *)state;

    if (strcmp(sub_path, "/preset") == 0 && ni >= 1) {
        int p = iargs[0];
        if (p >= 0 && p < NUM_PRESETS) {
            s->current_preset = p;
            fprintf(stderr, "[fm-synth] OSC preset %d: %s\n", p, PRESET_NAMES[p]);
        }
    }
    else if (strcmp(sub_path, "/volume") == 0 && nf >= 1) {
        float vol = fargs[0];
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 1.0f) vol = 1.0f;
        s->volume = vol;
        fprintf(stderr, "[fm-synth] OSC volume: %.2f\n", vol);
    }
    else if (strncmp(sub_path, "/param/", 7) == 0) {
        const char *param = sub_path + 7;
        if (strcmp(param, "reset") == 0) {
            fm_synth_set_param(state, "reset", 0);
        } else if (nf >= 1) {
            fm_synth_set_param(state, param, fargs[0]);
        }
    }
    else if (strcmp(sub_path, "/preset/load") == 0 && ni >= 1) {
        int p = iargs[0];
        if (p >= 0 && p < NUM_PRESETS) {
            s->current_preset = p;
            fm_load_preset_params(s, p);
            fprintf(stderr, "[fm-synth] OSC preset/load %d: %s\n", p, PRESET_NAMES[p]);
        }
    }
    /* seq paths handled at slot level in server/rack */
}

static int fm_synth_osc_status(void *state, uint8_t *buf, int max_len) {
    FMSynth *s = (FMSynth *)state;
    int pos = 0;
    int w;

    /* Address */
    w = osc_write_string(buf + pos, max_len - pos, "/status");
    if (w < 0) return 0;
    pos += w;

    /* Type tags: i s f i f f f f f f f f i */
    w = osc_write_string(buf + pos, max_len - pos, ",isfiffffffffi");
    if (w < 0) return 0;
    pos += w;

    /* preset_index (i) */
    if (pos + 4 > max_len) return 0;
    osc_write_i32(buf + pos, s->current_preset); pos += 4;

    /* preset_name (s) */
    const char *pname = (s->current_preset >= 0 && s->current_preset < NUM_PRESETS)
                        ? PRESET_NAMES[s->current_preset] : "Unknown";
    w = osc_write_string(buf + pos, max_len - pos, pname);
    if (w < 0) return 0;
    pos += w;

    /* volume (f) */
    if (pos + 4 > max_len) return 0;
    osc_write_f32(buf + pos, s->volume); pos += 4;

    /* override (i) */
    if (pos + 4 > max_len) return 0;
    osc_write_i32(buf + pos, s->live_params.override); pos += 4;

    /* 8 params (f) */
    const FMPreset *ep = &PRESETS[s->current_preset];
    float params[8];
    if (s->live_params.override) {
        params[0] = s->live_params.carrier_ratio;
        params[1] = s->live_params.mod_ratio;
        params[2] = s->live_params.mod_index;
        params[3] = s->live_params.attack;
        params[4] = s->live_params.decay;
        params[5] = s->live_params.sustain;
        params[6] = s->live_params.release;
        params[7] = s->live_params.feedback;
    } else {
        params[0] = ep->carrier_ratio;
        params[1] = ep->mod_ratio;
        params[2] = ep->mod_index;
        params[3] = ep->attack;
        params[4] = ep->decay;
        params[5] = ep->sustain;
        params[6] = ep->release;
        params[7] = ep->feedback;
    }
    for (int i = 0; i < 8; i++) {
        if (pos + 4 > max_len) return 0;
        osc_write_f32(buf + pos, params[i]); pos += 4;
    }

    /* active_voices (i) */
    if (pos + 4 > max_len) return 0;
    int active = 0;
    for (int v = 0; v < FM_MAX_VOICES; v++)
        if (s->voices[v].active) active++;
    osc_write_i32(buf + pos, active); pos += 4;

    return pos;
}

/* ── json_status — instrument-specific JSON fields ─────────────────── */

static int fm_synth_json_status(void *state, char *buf, int max) {
    FMSynth *s = (FMSynth *)state;
    const char *pname = (s->current_preset >= 0 && s->current_preset < NUM_PRESETS)
                        ? PRESET_NAMES[s->current_preset] : "Unknown";

    float params[8];
    if (s->live_params.override) {
        params[0] = s->live_params.carrier_ratio;
        params[1] = s->live_params.mod_ratio;
        params[2] = s->live_params.mod_index;
        params[3] = s->live_params.attack;
        params[4] = s->live_params.decay;
        params[5] = s->live_params.sustain;
        params[6] = s->live_params.release;
        params[7] = s->live_params.feedback;
    } else {
        const FMPreset *ep = &PRESETS[s->current_preset];
        params[0] = ep->carrier_ratio;
        params[1] = ep->mod_ratio;
        params[2] = ep->mod_index;
        params[3] = ep->attack;
        params[4] = ep->decay;
        params[5] = ep->sustain;
        params[6] = ep->release;
        params[7] = ep->feedback;
    }

    int active_voices = 0;
    for (int v = 0; v < FM_MAX_VOICES; v++)
        if (s->voices[v].active) active_voices++;

    return snprintf(buf, (size_t)max,
        "\"instrument_type\":\"fm-synth\","
        "\"preset_index\":%d,\"preset_name\":\"%s\","
        "\"volume\":%.4f,\"override\":%d,"
        "\"params\":{"
        "\"carrier_ratio\":%.4f,\"mod_ratio\":%.4f,\"mod_index\":%.4f,"
        "\"attack\":%.4f,\"decay\":%.4f,\"sustain\":%.4f,"
        "\"release\":%.4f,\"feedback\":%.4f},"
        "\"active_voices\":%d",
        s->current_preset, pname,
        (double)s->volume, s->live_params.override,
        (double)params[0], (double)params[1], (double)params[2],
        (double)params[3], (double)params[4], (double)params[5],
        (double)params[6], (double)params[7],
        active_voices);
}

/* ── json_save/json_load — state persistence ───────────────────────── */

static int fm_synth_json_save(void *state, char *buf, int max) {
    FMSynth *s = (FMSynth *)state;

    /* Always emit active param values (preset or override) */
    float p[8];
    if (s->live_params.override) {
        p[0]=s->live_params.carrier_ratio; p[1]=s->live_params.mod_ratio;
        p[2]=s->live_params.mod_index; p[3]=s->live_params.attack;
        p[4]=s->live_params.decay; p[5]=s->live_params.sustain;
        p[6]=s->live_params.release; p[7]=s->live_params.feedback;
    } else {
        const FMPreset *pr = &PRESETS[s->current_preset];
        p[0]=pr->carrier_ratio; p[1]=pr->mod_ratio; p[2]=pr->mod_index;
        p[3]=pr->attack; p[4]=pr->decay; p[5]=pr->sustain;
        p[6]=pr->release; p[7]=pr->feedback;
    }

    return snprintf(buf, (size_t)max,
        "\"preset\":%d,\"override\":%d,\"volume\":%.4f,"
        "\"carrier_ratio\":%.4f,\"mod_ratio\":%.4f,\"mod_index\":%.4f,"
        "\"attack\":%.4f,\"decay\":%.4f,\"sustain\":%.4f,"
        "\"release\":%.4f,\"feedback\":%.4f",
        s->current_preset, s->live_params.override, (double)s->volume,
        (double)p[0], (double)p[1], (double)p[2],
        (double)p[3], (double)p[4], (double)p[5],
        (double)p[6], (double)p[7]);
}

static int fm_synth_json_load(void *state, const char *json) {
    FMSynth *s = (FMSynth *)state;
    int preset;
    if (json_get_int(json, "preset", &preset) == 0 && preset >= 0 && preset < NUM_PRESETS) {
        s->current_preset = preset;
        fm_load_preset_params(s, preset);
    }
    int ovr;
    if (json_get_int(json, "override", &ovr) == 0 && ovr) {
        s->live_params.override = 1;
        const char *pp = strstr(json, "\"params\"");
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
    return 0;
}

/* ── set_param — named parameter setter ────────────────────────────── */

static void fm_synth_set_param(void *state, const char *name, float value) {
    FMSynth *s = (FMSynth *)state;

    if (strcmp(name, "reset") == 0) {
        s->live_params.override = 0;
        fprintf(stderr, "[fm-synth] param override disabled\n");
        return;
    }

    int matched = 1;
    if      (strcmp(name, "carrier_ratio") == 0) s->live_params.carrier_ratio = value;
    else if (strcmp(name, "mod_ratio")     == 0) s->live_params.mod_ratio     = value;
    else if (strcmp(name, "mod_index")     == 0) s->live_params.mod_index     = value;
    else if (strcmp(name, "attack")        == 0) s->live_params.attack        = value;
    else if (strcmp(name, "decay")         == 0) s->live_params.decay         = value;
    else if (strcmp(name, "sustain")       == 0) s->live_params.sustain       = value;
    else if (strcmp(name, "release")       == 0) s->live_params.release       = value;
    else if (strcmp(name, "feedback")      == 0) s->live_params.feedback      = value;
    else matched = 0;

    if (matched) {
        s->live_params.override = 1;
        fprintf(stderr, "[fm-synth] param %s = %.4f (override on)\n", name, value);
    }
}

/* ── Exported type descriptor ───────────────────────────────────────── */

InstrumentType fm_synth_type = {
    .name         = "fm-synth",
    .display_name = "FM Synth (yama-bruh)",
    .state_size   = sizeof(FMSynth),
    .init         = fm_synth_init,
    .destroy      = fm_synth_destroy,
    .midi         = fm_synth_midi,
    .render       = fm_synth_render,
    .set_param    = fm_synth_set_param,
    .json_status  = fm_synth_json_status,
    .json_save    = fm_synth_json_save,
    .json_load    = fm_synth_json_load,
    .osc_handle   = fm_synth_osc_handle,
    .osc_status   = fm_synth_osc_status,
};

#endif /* FM_SYNTH_H */
