/* miniwave — FM drum machine
 *
 * Per-note FM percussion — every MIDI note has its own drum sound.
 * Playing a note triggers it AND selects it for editing.
 * 2-operator FM with pitch sweep, noise, click transient.
 * Based on yama-bruh's drum engine.
 */

#ifndef FM_DRUMS_H
#define FM_DRUMS_H

#include "instruments.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FMD_TAU         (2.0f * (float)M_PI)
#define FMD_MAX_VOICES  12
#define FMD_NUM_PRESETS 16
#define FMD_NUM_NOTES   128

/* ── Drum parameters ───────────────────────────────────────────────── */

typedef struct {
    float carrier_freq;
    float mod_freq;
    float mod_index;
    float pitch_sweep;     /* Hz added at t=0, decays (negative = rise) */
    float pitch_decay;
    float decay;
    float noise_amt;       /* 0 = pure FM, 1 = pure noise */
    float click_amt;       /* transient click intensity */
    float feedback;
} FMDrumDef;

/* ── Presets ───────────────────────────────────────────────────────── */

static const FMDrumDef FMD_PRESETS[FMD_NUM_PRESETS] = {
    /* 0: Kick */
    { 60, 90, 3.0f, 160, 0.015f, 0.25f, 0, 0.3f, 0.1f },
    /* 1: Snare */
    { 200, 340, 2.5f, 60, 0.01f, 0.18f, 0.6f, 0.15f, 0.2f },
    /* 2: Clap */
    { 1200, 2400, 1.5f, 0, 0.01f, 0.2f, 0.85f, 0, 0 },
    /* 3: Closed HH */
    { 800, 5600, 4.0f, 0, 0.01f, 0.04f, 0.5f, 0, 0.3f },
    /* 4: Open HH */
    { 800, 5600, 4.0f, 0, 0.01f, 0.22f, 0.5f, 0, 0.3f },
    /* 5: Tom */
    { 165, 248, 2.0f, 83, 0.02f, 0.22f, 0, 0.1f, 0.08f },
    /* 6: Rimshot */
    { 500, 1600, 2.0f, 200, 0.005f, 0.06f, 0.2f, 0.5f, 0.15f },
    /* 7: Cowbell */
    { 587, 829, 1.8f, 0, 0.01f, 0.12f, 0, 0.1f, 0 },
    /* 8: Cymbal */
    { 940, 6580, 5.0f, 0, 0.01f, 0.8f, 0.4f, 0, 0.25f },
    /* 9: Zap */
    { 200, 600, 8.0f, 2000, 0.008f, 0.15f, 0, 0.6f, 0.3f },
    /* 10: Bomb */
    { 30, 45, 6.0f, 200, 0.05f, 0.8f, 0.5f, 0.4f, 0.4f },
    /* 11: Glitch */
    { 800, 5704, 12.0f, 1600, 0.003f, 0.04f, 0, 0.8f, 0.5f },
    /* 12: Scratch */
    { 600, 100, 15.0f, 400, 0.02f, 0.1f, 0.3f, 0.5f, 0.4f },
    /* 13: Shaker */
    { 1800, 4200, 2.4f, 0, 0.01f, 0.16f, 0.92f, 0.05f, 0 },
    /* 14: Blip */
    { 880, 1760, 0.5f, 0, 0.01f, 0.03f, 0, 0.4f, 0 },
    /* 15: Riser */
    { 100, 200, 4.0f, -400, 0.3f, 0.6f, 0.15f, 0, 0.1f },
};

static const char *FMD_PRESET_NAMES[FMD_NUM_PRESETS] = {
    "Kick", "Snare", "Clap", "CH", "OH", "Tom", "Rim", "Bell",
    "Cymbal", "Zap", "Bomb", "Glitch", "Scratch", "Shaker", "Blip", "Riser"
};

/* Per-note slot: which preset was loaded + current (editable) params */
typedef struct {
    int        preset;     /* which preset this note was initialized from (-1 = unset) */
    FMDrumDef  def;        /* current editable params */
} FMDrumNote;

/* Default note-to-preset mapping for GM drum range */
static int fmd_default_preset(int note) {
    switch (note) {
    case 35: case 36: return 0;   /* kick */
    case 38: case 40: return 1;   /* snare */
    case 39:          return 2;   /* clap */
    case 42: case 44: return 3;   /* closed hh */
    case 46:          return 4;   /* open hh */
    case 41: case 43: case 45: case 47: case 48: case 50: return 5; /* tom */
    case 37:          return 6;   /* rimshot */
    case 56:          return 7;   /* cowbell */
    case 49: case 51: case 52: case 57: return 8; /* cymbal */
    case 53:          return 9;   /* zap */
    case 54:          return 10;  /* bomb */
    case 55:          return 11;  /* glitch */
    case 58:          return 12;  /* scratch */
    case 59:          return 13;  /* shaker */
    case 60:          return 14;  /* blip */
    case 61:          return 15;  /* riser */
    default:          return 0;   /* fallback to kick */
    }
}

/* ── Voice ────────────────────────────────────────────────────────── */

typedef struct {
    int       active;
    int       note;
    float     velocity;
    float     time;
    float     car_phase;
    float     mod_phase;
    float     prev_mod;
    uint32_t  noise_state;
} FMDrumVoice;

/* ── State ────────────────────────────────────────────────────────── */

typedef struct {
    FMDrumVoice voices[FMD_MAX_VOICES];
    FMDrumNote  notes[FMD_NUM_NOTES];   /* per-note drum definitions */
    int         editing_note;            /* which MIDI note is selected for editing */
    float       volume;
    float       cents_mod;              /* written by slot layer before render */
} FMDrumState;

/* ── Helpers ──────────────────────────────────────────────────────── */

static inline float fmd_noise(uint32_t *state) {
    uint32_t s = *state;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    if (s == 0) s = 1;
    *state = s;
    return (float)(s & 0x7FFFFFFF) / (float)0x7FFFFFFF * 2.0f - 1.0f;
}

/* Ensure a note has been initialized with a preset */
static void fmd_ensure_note(FMDrumState *s, int note) {
    if (note < 0 || note >= FMD_NUM_NOTES) return;
    if (s->notes[note].preset < 0) {
        int p = fmd_default_preset(note);
        s->notes[note].preset = p;
        s->notes[note].def = FMD_PRESETS[p];
    }
}

/* ── InstrumentType interface ─────────────────────────────────────── */

static void fmd_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2);
static void fmd_set_param(void *state, const char *name, float value);
static void fmd_osc_handle(void *state, const char *sub_path,
                            const int32_t *iargs, int ni,
                            const float *fargs, int nf);

static void fmd_init(void *state) {
    FMDrumState *s = (FMDrumState *)state;
    memset(s, 0, sizeof(*s));
    s->volume = 1.0f;
    s->editing_note = 36; /* default to kick */
    /* Mark all notes as uninitialized — lazy-init on first use */
    for (int i = 0; i < FMD_NUM_NOTES; i++)
        s->notes[i].preset = -1;
    /* Pre-init the standard GM drum notes */
    for (int n = 35; n <= 61; n++)
        fmd_ensure_note(s, n);
}

static void fmd_destroy(void *state) { (void)state; }

static void fmd_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2) {
    FMDrumState *s = (FMDrumState *)state;
    uint8_t type = status & 0xF0;

    if (type == 0x90 && d2 > 0) {
        int note = d1;
        fmd_ensure_note(s, note);

        /* Auto-select this note for editing */
        s->editing_note = note;

        /* Find free voice or steal oldest */
        int slot = -1;
        for (int i = 0; i < FMD_MAX_VOICES; i++) {
            if (!s->voices[i].active) { slot = i; break; }
        }
        if (slot < 0) {
            float oldest = 0;
            slot = 0;
            for (int i = 0; i < FMD_MAX_VOICES; i++) {
                if (s->voices[i].time > oldest) {
                    oldest = s->voices[i].time;
                    slot = i;
                }
            }
        }

        FMDrumVoice *v = &s->voices[slot];
        memset(v, 0, sizeof(*v));
        v->active = 1;
        v->note = note;
        v->velocity = (float)d2 / 127.0f;
        v->noise_state = (uint32_t)(note * 1103515245 + d2 * 12345) | 1;
    }
    else if (type == 0xB0) {
        float cc = (float)d2 / 127.0f;
        int en = s->editing_note;
        fmd_ensure_note(s, en);
        FMDrumDef *def = &s->notes[en].def;
        switch (d1) {
        case 14: /* macro: carrier freq 20-2000 Hz log */
            def->carrier_freq = 20.0f * powf(100.0f, cc);
            break;
        case 15: /* macro: mod freq 20-8000 Hz log */
            def->mod_freq = 20.0f * powf(400.0f, cc);
            break;
        case 16: /* macro: mod index 0-10 */
            def->mod_index = cc * 10.0f;
            break;
        case 17: /* macro: pitch sweep -400 to +400 Hz */
            def->pitch_sweep = (cc - 0.5f) * 800.0f;
            break;
        case 18: /* macro: decay 0.01-2s log */
            def->decay = 0.01f * powf(200.0f, cc);
            break;
        case 19: /* macro: noise amount 0-1 */
            def->noise_amt = cc;
            break;
        case 20: /* macro: click amount 0-1 */
            def->click_amt = cc;
            break;
        case 21: /* macro: feedback 0-1 */
            def->feedback = cc;
            break;
        case 120: case 123:
            for (int i = 0; i < FMD_MAX_VOICES; i++) s->voices[i].active = 0;
            break;
        }
    }
}

static void fmd_render(void *state, float *stereo_buf, int frames, int sample_rate) {
    FMDrumState *s = (FMDrumState *)state;
    const float dt = 1.0f / (float)sample_rate;

    for (int i = 0; i < frames; i++) {
        float mix = 0.0f;

        for (int vi = 0; vi < FMD_MAX_VOICES; vi++) {
            FMDrumVoice *v = &s->voices[vi];
            if (!v->active) continue;

            const FMDrumDef *d = &s->notes[v->note].def;
            float t = v->time;

            /* Amplitude envelope */
            float env = expf(-t / (d->decay * 0.4f + 0.0001f)) * v->velocity;
            if (env < 0.001f) { v->active = 0; continue; }

            /* Pitch sweep + keyseq cents detune */
            float sweep = d->pitch_sweep * expf(-t / (d->pitch_decay + 0.0001f));
            float pm = (s->cents_mod != 0.0f)
                ? powf(2.0f, s->cents_mod / 1200.0f) : 1.0f;
            float c_freq = (d->carrier_freq + sweep) * pm;
            float m_freq = (d->mod_freq + sweep * 0.5f) * pm;

            /* FM synthesis */
            float fb = v->prev_mod * d->feedback;
            float mod_out = sinf(v->mod_phase + fb) * d->mod_index;
            v->prev_mod = mod_out;
            float carrier = sinf(v->car_phase + mod_out);

            /* Noise */
            float noise_val = 0.0f;
            if (d->noise_amt > 0.0f)
                noise_val = fmd_noise(&v->noise_state) * d->noise_amt * env;

            /* Click transient */
            float click = 0.0f;
            if (d->click_amt > 0.0f && t < 0.002f)
                click = (1.0f - t / 0.002f) * d->click_amt * v->velocity;

            float sample = (carrier * env * (1.0f - d->noise_amt)
                           + noise_val + click) * 0.5f;

            v->car_phase += FMD_TAU * c_freq * dt;
            v->mod_phase += FMD_TAU * m_freq * dt;
            if (v->car_phase >= FMD_TAU) v->car_phase -= FMD_TAU;
            if (v->mod_phase >= FMD_TAU) v->mod_phase -= FMD_TAU;
            v->time += dt;

            if (!isfinite(sample) || !isfinite(v->car_phase)) {
                v->active = 0; continue;
            }
            mix += sample;
        }

        mix *= s->volume;
        if (mix > 0.95f) mix = 0.95f;
        else if (mix < -0.95f) mix = -0.95f;
        stereo_buf[i * 2]     = mix;
        stereo_buf[i * 2 + 1] = mix;
    }
}

/* ── OSC handler ──────────────────────────────────────────────────── */

static void fmd_osc_handle(void *state, const char *sub_path,
                            const int32_t *iargs, int ni,
                            const float *fargs, int nf) {
    FMDrumState *s = (FMDrumState *)state;

    if (strcmp(sub_path, "/volume") == 0 && nf >= 1) {
        float vol = fargs[0];
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 1.0f) vol = 1.0f;
        s->volume = vol;
    }
    else if (strcmp(sub_path, "/select") == 0 && ni >= 1) {
        int note = iargs[0];
        if (note >= 0 && note < FMD_NUM_NOTES) {
            fmd_ensure_note(s, note);
            s->editing_note = note;
        }
    }
    else if (strcmp(sub_path, "/reset") == 0) {
        int note = s->editing_note;
        if (note >= 0 && note < FMD_NUM_NOTES) {
            int p = s->notes[note].preset;
            if (p >= 0 && p < FMD_NUM_PRESETS)
                s->notes[note].def = FMD_PRESETS[p];
        }
    }
    else if (strcmp(sub_path, "/load_preset") == 0 && ni >= 1) {
        int preset = iargs[0];
        int note = s->editing_note;
        if (preset >= 0 && preset < FMD_NUM_PRESETS &&
            note >= 0 && note < FMD_NUM_NOTES) {
            s->notes[note].preset = preset;
            s->notes[note].def = FMD_PRESETS[preset];
        }
    }
    else if (strncmp(sub_path, "/param/", 7) == 0 && nf >= 1) {
        fmd_set_param(state, sub_path + 7, fargs[0]);
    }
    /* seq paths handled at slot level in server/rack */
}

static int fmd_osc_status(void *state, uint8_t *buf, int max_len) {
    (void)state; (void)buf; (void)max_len;
    return 0;
}

/* ── json_status — instrument-specific JSON fields ─────────────────── */

static int fmd_json_status(void *state, char *buf, int max) {
    FMDrumState *ds = (FMDrumState *)state;
    int active_v = 0;
    for (int v = 0; v < FMD_MAX_VOICES; v++)
        if (ds->voices[v].active) active_v++;

    int en = ds->editing_note;
    if (en < 0 || en >= FMD_NUM_NOTES) en = 36;
    const FMDrumNote *dn = &ds->notes[en];
    const char *pname = (dn->preset >= 0 && dn->preset < FMD_NUM_PRESETS)
                        ? FMD_PRESET_NAMES[dn->preset] : "Custom";
    const FMDrumDef *ed = &dn->def;

    return snprintf(buf, (size_t)max,
        "\"instrument_type\":\"fm-drums\","
        "\"volume\":%.4f,"
        "\"editing_note\":%d,\"preset\":%d,\"preset_name\":\"%s\","
        "\"params\":{"
        "\"carrier_freq\":%.4f,\"mod_freq\":%.4f,\"mod_index\":%.4f,"
        "\"pitch_sweep\":%.4f,\"pitch_decay\":%.5f,"
        "\"decay\":%.4f,\"noise_amt\":%.4f,"
        "\"click_amt\":%.4f,\"feedback\":%.4f},"
        "\"active_voices\":%d",
        (double)ds->volume,
        en, dn->preset, pname,
        (double)ed->carrier_freq, (double)ed->mod_freq, (double)ed->mod_index,
        (double)ed->pitch_sweep, (double)ed->pitch_decay,
        (double)ed->decay, (double)ed->noise_amt,
        (double)ed->click_amt, (double)ed->feedback,
        active_v);
}

/* ── json_save/json_load — state persistence ──────────────────────── */

static int fmd_json_save(void *state, char *buf, int max) {
    FMDrumState *ds = (FMDrumState *)state;
    int en = ds->editing_note;
    if (en < 0 || en >= FMD_NUM_NOTES) en = 36;
    const FMDrumDef *ed = &ds->notes[en].def;

    int pos = 0;
    /* Flat params for current editing note — frontend renders these */
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "\"volume\":%.4f,\"editing_note\":%d,"
        "\"carrier_freq\":%.4f,\"mod_freq\":%.4f,\"mod_index\":%.4f,"
        "\"pitch_sweep\":%.4f,\"pitch_decay\":%.5f,"
        "\"decay\":%.4f,\"noise_amt\":%.4f,"
        "\"click_amt\":%.4f,\"feedback\":%.4f,",
        (double)ds->volume, en,
        (double)ed->carrier_freq, (double)ed->mod_freq, (double)ed->mod_index,
        (double)ed->pitch_sweep, (double)ed->pitch_decay,
        (double)ed->decay, (double)ed->noise_amt,
        (double)ed->click_amt, (double)ed->feedback);
    /* Full per-note data for persistence */
    pos += snprintf(buf + pos, (size_t)(max - pos), "\"notes\":{");
    int first = 1;
    for (int ni = 0; ni < FMD_NUM_NOTES; ni++) {
        if (ds->notes[ni].preset < 0) continue;
        FMDrumDef *dd = &ds->notes[ni].def;
        pos += snprintf(buf + pos, (size_t)(max - pos),
            "%s\"%d\":{\"p\":%d,\"cf\":%.4f,\"mf\":%.4f,\"mi\":%.4f,"
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
    pos += snprintf(buf + pos, (size_t)(max - pos), "}");
    return pos;
}

static int fmd_json_load(void *state, const char *json) {
    FMDrumState *ds = (FMDrumState *)state;
    const char *nobj = strstr(json, "\"notes\"");
    if (nobj) {
        nobj = strchr(nobj, '{');
        if (nobj) nobj++;
    }
    if (nobj) {
        const char *p = nobj;
        while (p && *p) {
            const char *q = strchr(p, '"');
            if (!q) break;
            int noteNum = atoi(q + 1);
            if (noteNum < 0 || noteNum >= FMD_NUM_NOTES) break;
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
    return 0;
}

/* ── set_param — named parameter setter ───────────────────────────── */

static void fmd_set_param(void *state, const char *name, float value) {
    FMDrumState *s = (FMDrumState *)state;
    int note = s->editing_note;
    if (note < 0 || note >= FMD_NUM_NOTES) return;
    FMDrumDef *d = &s->notes[note].def;

    if      (strcmp(name, "carrier_freq") == 0) d->carrier_freq = value;
    else if (strcmp(name, "mod_freq")     == 0) d->mod_freq     = value;
    else if (strcmp(name, "mod_index")    == 0) d->mod_index    = value;
    else if (strcmp(name, "pitch_sweep")  == 0) d->pitch_sweep  = value;
    else if (strcmp(name, "pitch_decay")  == 0) d->pitch_decay  = value;
    else if (strcmp(name, "decay")        == 0) d->decay        = value;
    else if (strcmp(name, "noise_amt")    == 0) d->noise_amt    = value;
    else if (strcmp(name, "click_amt")    == 0) d->click_amt    = value;
    else if (strcmp(name, "feedback")     == 0) d->feedback     = value;
}

/* ── Exported type descriptor ─────────────────────────────────────── */

InstrumentType fm_drums_type = {
    .name         = "fm-drums",
    .display_name = "FM Drums",
    .state_size   = sizeof(FMDrumState),
    .init         = fmd_init,
    .destroy      = fmd_destroy,
    .midi         = fmd_midi,
    .render       = fmd_render,
    .set_param    = fmd_set_param,
    .json_status  = fmd_json_status,
    .json_save    = fmd_json_save,
    .json_load    = fmd_json_load,
    .osc_handle   = fmd_osc_handle,
    .osc_status   = fmd_osc_status,
};

#endif /* FM_DRUMS_H */
