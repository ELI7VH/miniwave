/* miniwave — YM2413 OPLL instrument (emu2413-backed)
 *
 * Wraps Mitsutaka Okazaki's cycle-accurate emu2413 emulator.
 * 9 melodic channels, rhythm mode with 5 percussion voices.
 * 15 built-in ROM patches + 1 custom user patch.
 *
 * MIDI mapping:
 *   Note on/off → F-number + block + key-on register writes
 *   Program change → instrument select (0-15)
 *   Pitch bend → F-number micro-adjustment
 */

#ifndef YM2413_H
#define YM2413_H

#include "instruments.h"
#include "emu2413.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Constants ────────────────────────────────────────────────────────── */

#define OPLL_CLOCK     3579545   /* NTSC standard clock */
#define OPLL_CHANNELS  9
#define OPLL_MAX_POLY  9

/* ── MIDI note → F-number/block lookup ────────────────────────────────── */

/* F-number table for one octave (C to B) at block 0.
 * F = freq * 2^(19-block) / (clock/72) */
static const uint16_t opll_fnums[12] = {
    172, 181, 192, 204, 216, 229, 242, 257, 272, 288, 305, 323
};

typedef struct {
    int fnum;
    int block;
} OPLLNote;

static OPLLNote opll_midi_to_note(int midi_note) {
    OPLLNote n;
    int oct = (midi_note / 12) - 1;
    int semi = midi_note % 12;
    if (oct < 0) oct = 0;
    if (oct > 7) oct = 7;
    n.fnum = opll_fnums[semi];
    n.block = oct;
    return n;
}

/* ── Internal key sequence (from yama-bruh) ───────────────────────────── */

#define OPLL_MAX_SEQ_NOTES 10
#define OPLL_SEQ_BPM_DEFAULT 140.0f

typedef struct {
    int   midi_note;
    float duration_beats;
} OPLLSeqNote;

typedef struct {
    OPLLSeqNote notes[OPLL_MAX_SEQ_NOTES];
    int         num_notes;
    int         current_note;
    float       note_time;
    float       bpm;
    int         preset_idx;
    int         playing;
    int         loop;
    uint32_t    seed;
} OPLLKeySequence;

/* ── Patch names ──────────────────────────────────────────────────────── */

static const char *OPLL_PATCH_NAMES[16] = {
    "Custom", "Violin", "Guitar", "Piano", "Flute",
    "Clarinet", "Oboe", "Trumpet", "Organ", "Horn",
    "Synthesizer", "Harpsichord", "Vibraphone", "Synth Bass",
    "Acoustic Bass", "Electric Guitar"
};

/* ── State ────────────────────────────────────────────────────────────── */

typedef struct {
    OPLL        *opll;
    int          sample_rate;

    /* Per-channel MIDI note tracking */
    int          ch_note[OPLL_CHANNELS];
    int          ch_vel[OPLL_CHANNELS];

    /* Settings */
    int          current_instrument;
    float        volume;
    int          rhythm_mode;
    float        cents_mod;

    /* Internal key sequence */
    OPLLKeySequence seq;
} YM2413State;

/* ── Channel allocation ───────────────────────────────────────────────── */

static int opll_alloc_channel(YM2413State *s) {
    int nch = s->rhythm_mode ? 6 : 9;
    for (int i = 0; i < nch; i++)
        if (s->ch_note[i] < 0) return i;
    return 0; /* steal first */
}

static void opll_key_on(YM2413State *s, int ch, int midi_note, int vel) {
    OPLLNote n = opll_midi_to_note(midi_note);
    int inst = s->current_instrument;
    int vol = 15 - (vel * 15 / 127);
    if (vol < 0) vol = 0;
    if (vol > 15) vol = 15;

    OPLL_writeReg(s->opll, (e_uint32)(0x30 + ch), (e_uint32)((inst << 4) | vol));
    OPLL_writeReg(s->opll, (e_uint32)(0x10 + ch), (e_uint32)(n.fnum & 0xFF));
    int hi = ((n.fnum >> 8) & 1) | (n.block << 1) | 0x10;
    OPLL_writeReg(s->opll, (e_uint32)(0x20 + ch), (e_uint32)hi);

    s->ch_note[ch] = midi_note;
    s->ch_vel[ch] = vel;
}

static void opll_key_off(YM2413State *s, int ch) {
    e_uint8 reg20 = s->opll->reg[0x20 + ch];
    OPLL_writeReg(s->opll, (e_uint32)(0x20 + ch), (e_uint32)(reg20 & ~0x10));
    s->ch_note[ch] = -1;
}

static void opll_note_on(YM2413State *s, int midi_note, int vel) {
    int ch = opll_alloc_channel(s);
    if (s->ch_note[ch] >= 0) opll_key_off(s, ch);
    opll_key_on(s, ch, midi_note, vel);
}

static void opll_note_off(YM2413State *s, int midi_note) {
    int nch = s->rhythm_mode ? 6 : 9;
    for (int i = 0; i < nch; i++) {
        if (s->ch_note[i] == midi_note) {
            opll_key_off(s, i);
            break;
        }
    }
}

/* ── Internal key sequence ────────────────────────────────────────────── */

static void opll_seq_tick(YM2413State *s, float dt) {
    OPLLKeySequence *seq = &s->seq;
    if (!seq->playing || seq->num_notes == 0) return;

    float beat_dur = 60.0f / seq->bpm;
    seq->note_time += dt;

    if (seq->current_note < seq->num_notes) {
        float note_dur = seq->notes[seq->current_note].duration_beats * beat_dur;
        if (seq->note_time >= note_dur) {
            opll_note_off(s, seq->notes[seq->current_note].midi_note);
            seq->current_note++;
            seq->note_time = 0.0f;
            if (seq->current_note < seq->num_notes) {
                opll_note_on(s, seq->notes[seq->current_note].midi_note, 100);
            } else if (seq->loop) {
                seq->current_note = 0;
                opll_note_on(s, seq->notes[0].midi_note, 100);
            } else {
                seq->playing = 0;
            }
        }
    }
}

static void opll_seq_stop(YM2413State *s) {
    OPLLKeySequence *seq = &s->seq;
    if (seq->playing && seq->current_note < seq->num_notes)
        opll_note_off(s, seq->notes[seq->current_note].midi_note);
    seq->playing = 0;
}

/* ── InstrumentType interface ─────────────────────────────────────────── */

static void ym2413_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2);
static void ym2413_set_param(void *state, const char *name, float value);
static void ym2413_osc_handle(void *state, const char *sub_path,
                               const int32_t *iargs, int ni,
                               const float *fargs, int nf);

static void ym2413_init(void *state) {
    YM2413State *s = (YM2413State *)state;
    memset(s, 0, sizeof(*s));
    s->sample_rate = 48000;
    s->opll = OPLL_new(OPLL_CLOCK, (e_uint32)s->sample_rate);
    OPLL_reset(s->opll);
    OPLL_reset_patch(s->opll, OPLL_2413_TONE);
    OPLL_set_quality(s->opll, 1);
    s->volume = 1.0f;
    s->current_instrument = 1;
    s->seq.bpm = OPLL_SEQ_BPM_DEFAULT;
    for (int i = 0; i < OPLL_CHANNELS; i++) s->ch_note[i] = -1;
}

static void ym2413_destroy(void *state) {
    YM2413State *s = (YM2413State *)state;
    if (s->opll) { OPLL_delete(s->opll); s->opll = NULL; }
}

static void ym2413_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2) {
    YM2413State *s = (YM2413State *)state;
    uint8_t type = status & 0xF0;
    switch (type) {
    case 0x90:
        if (d2 > 0) opll_note_on(s, d1, d2);
        else        opll_note_off(s, d1);
        break;
    case 0x80:
        opll_note_off(s, d1);
        break;
    case 0xC0:
        if (d1 < 16) {
            s->current_instrument = d1;
            fprintf(stderr, "[ym2413] instrument %d: %s\n", d1, OPLL_PATCH_NAMES[d1]);
        }
        break;
    case 0xB0: {
        float cc = (float)d2 / 127.0f;
        switch (d1) {
        case 14: /* macro: instrument select 0-15 */
            ym2413_set_param(state, "instrument", (float)(d2 % 16));
            break;
        case 15: /* macro: mod total level 0-63 */
            ym2413_set_param(state, "mod_tl", (float)(int)(cc * 63.0f));
            break;
        case 16: /* macro: feedback 0-7 */
            ym2413_set_param(state, "feedback", (float)(int)(cc * 7.0f));
            break;
        case 17: /* macro: mod attack 0-15 */
            ym2413_set_param(state, "mod_attack", (float)(int)(cc * 15.0f));
            break;
        case 18: /* macro: mod decay 0-15 */
            ym2413_set_param(state, "mod_decay", (float)(int)(cc * 15.0f));
            break;
        case 19: /* macro: car attack 0-15 */
            ym2413_set_param(state, "car_attack", (float)(int)(cc * 15.0f));
            break;
        case 20: /* macro: car decay 0-15 */
            ym2413_set_param(state, "car_decay", (float)(int)(cc * 15.0f));
            break;
        case 21: /* macro: mod multiplier 0-15 */
            ym2413_set_param(state, "mod_mult", (float)(int)(cc * 15.0f));
            break;
        case 120: case 123:
            for (int i = 0; i < OPLL_CHANNELS; i++)
                if (s->ch_note[i] >= 0) opll_key_off(s, i);
            break;
        }
        (void)cc;
        break;
    }
    }
}

static void ym2413_render(void *state, float *stereo_buf, int frames, int sample_rate) {
    YM2413State *s = (YM2413State *)state;
    float dt = 1.0f / (float)sample_rate;

    if (s->sample_rate != sample_rate) {
        s->sample_rate = sample_rate;
        OPLL_set_rate(s->opll, (e_uint32)sample_rate);
    }

    for (int i = 0; i < frames; i++) {
        opll_seq_tick(s, dt);
        e_int16 raw = OPLL_calc(s->opll);
        float sample = (float)raw / 32768.0f * s->volume;
        stereo_buf[i * 2]     = sample;
        stereo_buf[i * 2 + 1] = sample;
    }
}

/* ── OSC ──────────────────────────────────────────────────────────────── */

static void ym2413_osc_handle(void *state, const char *sub_path,
                               const int32_t *iargs, int ni,
                               const float *fargs, int nf) {
    YM2413State *s = (YM2413State *)state;

    if (strcmp(sub_path, "/instrument") == 0 && ni >= 1) {
        if (iargs[0] >= 0 && iargs[0] < 16) s->current_instrument = iargs[0];
    }
    else if (strcmp(sub_path, "/volume") == 0 && nf >= 1) {
        s->volume = fargs[0];
        if (s->volume < 0) s->volume = 0;
        if (s->volume > 1) s->volume = 1;
    }
    else if (strcmp(sub_path, "/rhythm") == 0 && ni >= 1) {
        s->rhythm_mode = iargs[0] ? 1 : 0;
        e_uint8 reg0e = s->opll->reg[0x0E];
        if (s->rhythm_mode) reg0e |= 0x20; else reg0e &= ~0x20;
        OPLL_writeReg(s->opll, 0x0E, reg0e);
    }
    else if (strncmp(sub_path, "/param/", 7) == 0) {
        ym2413_set_param(state, sub_path + 7, nf >= 1 ? fargs[0] : 0);
    }
    else if (strncmp(sub_path, "/reg/", 5) == 0 && ni >= 1) {
        int addr = (int)strtol(sub_path + 5, NULL, 16);
        if (addr >= 0 && addr < 0x40)
            OPLL_writeReg(s->opll, (e_uint32)addr, (e_uint32)(iargs[0] & 0xFF));
    }
    else if (strcmp(sub_path, "/seq/stop") == 0) {
        opll_seq_stop(s);
    }
    else if (strcmp(sub_path, "/seq/bpm") == 0 && nf >= 1) {
        s->seq.bpm = fargs[0];
    }
}

static int ym2413_osc_status(void *state, uint8_t *buf, int max_len) {
    (void)state; (void)buf; (void)max_len;
    return 0;
}

/* ── json_status ──────────────────────────────────────────────────────── */

static int ym2413_json_status(void *state, char *buf, int max) {
    YM2413State *s = (YM2413State *)state;
    e_uint8 regbuf[16]; /* patch2dump writes up to 14 bytes */
    if (s->current_instrument > 0) {
        OPLL_PATCH pair[2];
        OPLL_getDefaultPatch(OPLL_2413_TONE, s->current_instrument, pair);
        memset(regbuf, 0, sizeof(regbuf));
        OPLL_patch2dump(pair, regbuf);
    } else {
        for (int i = 0; i < 8; i++) regbuf[i] = s->opll->reg[i];
    }
    e_uint8 *r = regbuf;
    const char *iname = (s->current_instrument >= 0 && s->current_instrument < 16)
                        ? OPLL_PATCH_NAMES[s->current_instrument] : "Unknown";
    int active = 0;
    int nch = s->rhythm_mode ? 6 : 9;
    for (int i = 0; i < nch; i++)
        if (s->ch_note[i] >= 0) active++;

    return snprintf(buf, (size_t)max,
        "\"instrument_type\":\"ym2413\","
        "\"preset_index\":%d,\"preset_name\":\"%s\","
        "\"rhythm_mode\":%d,\"active_voices\":%d,\"volume\":%.4f,"
        "\"feedback\":%d,"
        "\"mod_mult\":%d,\"mod_am\":%d,\"mod_vibrato\":%d,"
        "\"mod_eg\":%d,\"mod_ksr\":%d,\"mod_tl\":%d,\"mod_ksl\":%d,"
        "\"mod_wave\":%d,\"mod_attack\":%d,\"mod_decay\":%d,"
        "\"mod_sustain\":%d,\"mod_release\":%d,"
        "\"car_mult\":%d,\"car_am\":%d,\"car_vibrato\":%d,"
        "\"car_eg\":%d,\"car_ksr\":%d,\"car_ksl\":%d,"
        "\"car_wave\":%d,\"car_attack\":%d,\"car_decay\":%d,"
        "\"car_sustain\":%d,\"car_release\":%d",
        s->current_instrument, iname, s->rhythm_mode, active, (double)s->volume,
        r[0x03] & 0x07,
        r[0x00] & 0x0F, (r[0x00] >> 7) & 1, (r[0x00] >> 6) & 1,
        (r[0x00] >> 5) & 1, (r[0x00] >> 4) & 1, r[0x02] & 0x3F, (r[0x02] >> 6) & 3,
        (r[0x03] >> 3) & 1, (r[0x04] >> 4) & 0x0F, r[0x04] & 0x0F,
        (r[0x06] >> 4) & 0x0F, r[0x06] & 0x0F,
        r[0x01] & 0x0F, (r[0x01] >> 7) & 1, (r[0x01] >> 6) & 1,
        (r[0x01] >> 5) & 1, (r[0x01] >> 4) & 1, (r[0x03] >> 6) & 3,
        (r[0x03] >> 4) & 1, (r[0x05] >> 4) & 0x0F, r[0x05] & 0x0F,
        (r[0x07] >> 4) & 0x0F, r[0x07] & 0x0F);
}

/* ── json_save / json_load ────────────────────────────────────────────── */

static int ym2413_json_save(void *state, char *buf, int max) {
    YM2413State *s = (YM2413State *)state;

    /* If using a ROM patch, dump its values so they survive save/load.
     * ROM patches aren't reflected in registers 0x00-0x07. */
    e_uint8 regbuf[16]; /* patch2dump writes up to 14 bytes */
    if (s->current_instrument > 0) {
        OPLL_PATCH pair[2];
        OPLL_getDefaultPatch(OPLL_2413_TONE, s->current_instrument, pair);
        memset(regbuf, 0, sizeof(regbuf));
        OPLL_patch2dump(pair, regbuf);
    } else {
        for (int i = 0; i < 8; i++) regbuf[i] = s->opll->reg[i];
    }
    e_uint8 *r = regbuf;
    return snprintf(buf, (size_t)max,
        "\"instrument\":%d,\"rhythm\":%d,\"volume\":%.4f,"
        "\"feedback\":%d,"
        "\"mod_mult\":%d,\"mod_am\":%d,\"mod_vibrato\":%d,"
        "\"mod_eg\":%d,\"mod_ksr\":%d,\"mod_tl\":%d,\"mod_ksl\":%d,"
        "\"mod_wave\":%d,\"mod_attack\":%d,\"mod_decay\":%d,"
        "\"mod_sustain\":%d,\"mod_release\":%d,"
        "\"car_mult\":%d,\"car_am\":%d,\"car_vibrato\":%d,"
        "\"car_eg\":%d,\"car_ksr\":%d,\"car_ksl\":%d,"
        "\"car_wave\":%d,\"car_attack\":%d,\"car_decay\":%d,"
        "\"car_sustain\":%d,\"car_release\":%d",
        s->current_instrument, s->rhythm_mode, (double)s->volume,
        r[0x03] & 0x07,
        r[0x00] & 0x0F, (r[0x00] >> 7) & 1, (r[0x00] >> 6) & 1,
        (r[0x00] >> 5) & 1, (r[0x00] >> 4) & 1, r[0x02] & 0x3F, (r[0x02] >> 6) & 3,
        (r[0x03] >> 3) & 1, (r[0x04] >> 4) & 0x0F, r[0x04] & 0x0F,
        (r[0x06] >> 4) & 0x0F, r[0x06] & 0x0F,
        r[0x01] & 0x0F, (r[0x01] >> 7) & 1, (r[0x01] >> 6) & 1,
        (r[0x01] >> 5) & 1, (r[0x01] >> 4) & 1, (r[0x03] >> 6) & 3,
        (r[0x03] >> 4) & 1, (r[0x05] >> 4) & 0x0F, r[0x05] & 0x0F,
        (r[0x07] >> 4) & 0x0F, r[0x07] & 0x0F);
}

static int ym2413_json_load(void *state, const char *json) {
    YM2413State *s = (YM2413State *)state;
    int ival;
    float fval;
    if (json_get_int(json, "instrument", &ival) == 0 && ival >= 0 && ival < 16)
        s->current_instrument = ival;
    if (json_get_int(json, "rhythm", &ival) == 0)
        s->rhythm_mode = ival ? 1 : 0;
    if (json_get_float(json, "volume", &fval) == 0)
        s->volume = fval;

    /* Restore custom patch registers */
    const char *params[] = {
        "feedback", "mod_mult", "mod_am", "mod_vibrato", "mod_eg", "mod_ksr",
        "mod_tl", "mod_ksl", "mod_wave", "mod_attack", "mod_decay",
        "mod_sustain", "mod_release",
        "car_mult", "car_am", "car_vibrato", "car_eg", "car_ksr", "car_ksl",
        "car_wave", "car_attack", "car_decay", "car_sustain", "car_release",
        NULL
    };
    for (int i = 0; params[i]; i++) {
        if (json_get_int(json, params[i], &ival) == 0)
            ym2413_set_param(state, params[i], (float)ival);
    }
    return 0;
}

/* ── set_param ────────────────────────────────────────────────────────── */

static void ym2413_set_param(void *state, const char *name, float value) {
    YM2413State *s = (YM2413State *)state;
    int iv = (int)value;

    if (strcmp(name, "instrument") == 0) {
        if (iv >= 0 && iv < 16) {
            s->current_instrument = iv;
            /* Copy ROM patch params into custom registers (0x00-0x07)
             * so they're editable. Switch to instrument 0 (custom). */
            if (iv > 0) {
                OPLL_PATCH pair[2]; /* [0]=mod, [1]=car */
                OPLL_getDefaultPatch(OPLL_2413_TONE, iv, pair);
                e_uint8 dump[16];
                OPLL_patch2dump(pair, dump);
                for (int r = 0; r < 8; r++)
                    OPLL_writeReg(s->opll, (e_uint32)r, dump[r]);
                OPLL_forceRefresh(s->opll);
            }
            s->current_instrument = 0;
        }
    }
    else if (strcmp(name, "volume") == 0) {
        s->volume = value < 0 ? 0 : value > 1 ? 1 : value;
    }
    else if (strcmp(name, "rhythm") == 0) {
        s->rhythm_mode = value != 0 ? 1 : 0;
        e_uint8 reg0e = s->opll->reg[0x0E];
        if (s->rhythm_mode) reg0e |= 0x20; else reg0e &= ~0x20;
        OPLL_writeReg(s->opll, 0x0E, reg0e);
    }
    /* ── Custom patch registers (instrument 0) ── */
    /* Modulator: reg 0x00 and 0x01 */
    else if (strcmp(name, "mod_mult") == 0)    { /* multiplier 0-15 */
        e_uint8 r = s->opll->reg[0x00]; r = (r & 0xF0) | (iv & 0x0F);
        OPLL_writeReg(s->opll, 0x00, r);
    }
    else if (strcmp(name, "mod_ksr") == 0)     { /* key scale rate 0-1 */
        e_uint8 r = s->opll->reg[0x00]; if (iv) r |= 0x10; else r &= ~0x10;
        OPLL_writeReg(s->opll, 0x00, r);
    }
    else if (strcmp(name, "mod_eg") == 0)      { /* EG type 0-1 */
        e_uint8 r = s->opll->reg[0x00]; if (iv) r |= 0x20; else r &= ~0x20;
        OPLL_writeReg(s->opll, 0x00, r);
    }
    else if (strcmp(name, "mod_vibrato") == 0) { /* vibrato 0-1 */
        e_uint8 r = s->opll->reg[0x00]; if (iv) r |= 0x40; else r &= ~0x40;
        OPLL_writeReg(s->opll, 0x00, r);
    }
    else if (strcmp(name, "mod_am") == 0)      { /* AM 0-1 */
        e_uint8 r = s->opll->reg[0x00]; if (iv) r |= 0x80; else r &= ~0x80;
        OPLL_writeReg(s->opll, 0x00, r);
    }
    else if (strcmp(name, "mod_tl") == 0)      { /* total level 0-63 */
        e_uint8 r = s->opll->reg[0x02]; r = (r & 0xC0) | (iv & 0x3F);
        OPLL_writeReg(s->opll, 0x02, r);
    }
    else if (strcmp(name, "mod_ksl") == 0)     { /* key scale level 0-3 */
        e_uint8 r = s->opll->reg[0x02]; r = (r & 0x3F) | ((iv & 3) << 6);
        OPLL_writeReg(s->opll, 0x02, r);
    }
    else if (strcmp(name, "mod_wave") == 0)    { /* waveform 0-1 */
        e_uint8 r = s->opll->reg[0x03]; if (iv) r |= 0x08; else r &= ~0x08;
        OPLL_writeReg(s->opll, 0x03, r);
    }
    else if (strcmp(name, "mod_attack") == 0)  { /* attack rate 0-15 */
        e_uint8 r = s->opll->reg[0x04]; r = (r & 0x0F) | ((iv & 0x0F) << 4);
        OPLL_writeReg(s->opll, 0x04, r);
    }
    else if (strcmp(name, "mod_decay") == 0)   { /* decay rate 0-15 */
        e_uint8 r = s->opll->reg[0x04]; r = (r & 0xF0) | (iv & 0x0F);
        OPLL_writeReg(s->opll, 0x04, r);
    }
    else if (strcmp(name, "mod_sustain") == 0) { /* sustain level 0-15 */
        e_uint8 r = s->opll->reg[0x06]; r = (r & 0x0F) | ((iv & 0x0F) << 4);
        OPLL_writeReg(s->opll, 0x06, r);
    }
    else if (strcmp(name, "mod_release") == 0) { /* release rate 0-15 */
        e_uint8 r = s->opll->reg[0x06]; r = (r & 0xF0) | (iv & 0x0F);
        OPLL_writeReg(s->opll, 0x06, r);
    }
    /* Carrier: reg 0x01 and 0x03 */
    else if (strcmp(name, "car_mult") == 0)    {
        e_uint8 r = s->opll->reg[0x01]; r = (r & 0xF0) | (iv & 0x0F);
        OPLL_writeReg(s->opll, 0x01, r);
    }
    else if (strcmp(name, "car_ksr") == 0)     {
        e_uint8 r = s->opll->reg[0x01]; if (iv) r |= 0x10; else r &= ~0x10;
        OPLL_writeReg(s->opll, 0x01, r);
    }
    else if (strcmp(name, "car_eg") == 0)      {
        e_uint8 r = s->opll->reg[0x01]; if (iv) r |= 0x20; else r &= ~0x20;
        OPLL_writeReg(s->opll, 0x01, r);
    }
    else if (strcmp(name, "car_vibrato") == 0) {
        e_uint8 r = s->opll->reg[0x01]; if (iv) r |= 0x40; else r &= ~0x40;
        OPLL_writeReg(s->opll, 0x01, r);
    }
    else if (strcmp(name, "car_am") == 0)      {
        e_uint8 r = s->opll->reg[0x01]; if (iv) r |= 0x80; else r &= ~0x80;
        OPLL_writeReg(s->opll, 0x01, r);
    }
    else if (strcmp(name, "car_wave") == 0)    {
        e_uint8 r = s->opll->reg[0x03]; if (iv) r |= 0x10; else r &= ~0x10;
        OPLL_writeReg(s->opll, 0x03, r);
    }
    else if (strcmp(name, "car_attack") == 0)  {
        e_uint8 r = s->opll->reg[0x05]; r = (r & 0x0F) | ((iv & 0x0F) << 4);
        OPLL_writeReg(s->opll, 0x05, r);
    }
    else if (strcmp(name, "car_decay") == 0)   {
        e_uint8 r = s->opll->reg[0x05]; r = (r & 0xF0) | (iv & 0x0F);
        OPLL_writeReg(s->opll, 0x05, r);
    }
    else if (strcmp(name, "car_sustain") == 0) {
        e_uint8 r = s->opll->reg[0x07]; r = (r & 0x0F) | ((iv & 0x0F) << 4);
        OPLL_writeReg(s->opll, 0x07, r);
    }
    else if (strcmp(name, "car_release") == 0) {
        e_uint8 r = s->opll->reg[0x07]; r = (r & 0xF0) | (iv & 0x0F);
        OPLL_writeReg(s->opll, 0x07, r);
    }
    /* Feedback */
    else if (strcmp(name, "feedback") == 0)    { /* feedback 0-7 */
        e_uint8 r = s->opll->reg[0x03]; r = (r & 0xF8) | (iv & 0x07);
        OPLL_writeReg(s->opll, 0x03, r);
    }
}

/* ── Type descriptor ──────────────────────────────────────────────────── */

InstrumentType ym2413_type = {
    .name         = "ym2413",
    .display_name = "YM2413 OPLL",
    .state_size   = sizeof(YM2413State),
    .init         = ym2413_init,
    .destroy      = ym2413_destroy,
    .midi         = ym2413_midi,
    .render       = ym2413_render,
    .set_param    = ym2413_set_param,
    .json_status  = ym2413_json_status,
    .json_save    = ym2413_json_save,
    .json_load    = ym2413_json_load,
    .osc_handle   = ym2413_osc_handle,
    .osc_status   = ym2413_osc_status,
};

#endif /* YM2413_H */
