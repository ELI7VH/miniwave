/* miniwave — step sequencer
 *
 * Shared by all instrument types. Fires note_on/note_off via the
 * instrument's own MIDI handler — no coupling to specific synths.
 *
 * Two modes:
 *   1. DSL string:  "140L C4q E4e G#4s _h Bb3q."
 *   2. Seed-based:  deterministic random F#m pentatonic from hash
 *
 * DSL format:
 *   <BPM>[L] <NOTE><DUR> [<NOTE><DUR> ...]
 *
 *   BPM     = integer tempo, optional 'L' suffix for loop
 *   NOTE    = C D E F G A B with optional # or b, octave 0-9, or _ (rest)
 *   DUR     = w(4) h(2) q(1) e(0.5) s(0.25) t(0.125), optional . = dotted
 *   |       = ignored (visual separator)
 *
 * Examples:
 *   "120L C4q E4e G4e C5h"          — 120 BPM, looped, 4 notes
 *   "90  A3q _ e F#3e A3q C4h."     — 90 BPM, one-shot, dotted half at end
 *   "140L D4s D4s _s D4s _s D4e D4e" — fast 16th-note riff
 */

#ifndef MINIWAVE_SEQ_H
#define MINIWAVE_SEQ_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define SEQ_MAX_NOTES 64

/* ── Types ─────────────────────────────────────────────────────────────── */

typedef struct {
    int   midi_note;        /* 0-127, or -1 for rest */
    float duration_beats;   /* in beats */
    int   velocity;         /* 1-127 */
} SeqNote;

typedef struct MiniSeq {
    SeqNote  notes[SEQ_MAX_NOTES];
    int      num_notes;
    int      current_note;      /* playback cursor */
    float    note_time;         /* elapsed time in current note (seconds) */
    float    bpm;
    int      playing;
    int      loop;
    int      last_note_on;      /* MIDI note of last triggered note, -1 if none */

    /* Callback: fires MIDI into the instrument.
     * state = instrument state pointer, status/d1/d2 = raw MIDI bytes */
    void    *inst_state;
    void   (*midi_fn)(void *state, uint8_t status, uint8_t d1, uint8_t d2);
    uint8_t  midi_channel;      /* channel nibble for status byte (0-15) */

    /* Source string for status/save */
    char     source[256];
} MiniSeq;

/* ── Note name parsing ─────────────────────────────────────────────────── */

/* Returns MIDI note number or -1 for rest/_. Advances *p past the note name. */
static int seq_parse_note(const char **p) {
    const char *s = *p;

    /* Rest */
    if (*s == '_') { *p = s + 1; return -1; }

    /* Note letter */
    static const int base_note[] = {
        /* A  B  C  D  E  F  G */
           9, 11, 0, 2, 4, 5, 7
    };

    char letter = *s;
    if (letter >= 'a' && letter <= 'g') letter -= 32; /* uppercase */
    if (letter < 'A' || letter > 'G') return -2; /* error */
    s++;

    int note = base_note[letter - 'A'];

    /* Accidental */
    if (*s == '#')      { note++; s++; }
    else if (*s == 'b') { note--; s++; }

    /* Octave */
    if (*s >= '0' && *s <= '9') {
        int octave = *s - '0';
        note += (octave + 1) * 12; /* MIDI: C4 = 60 */
        s++;
    } else {
        note += 5 * 12; /* default octave 4 */
    }

    /* Clamp */
    if (note < 0) note = 0;
    if (note > 127) note = 127;

    *p = s;
    return note;
}

/* Returns duration in beats. Advances *p past the duration char(s). */
static float seq_parse_duration(const char **p) {
    const char *s = *p;
    float dur = 1.0f; /* default quarter */

    switch (*s) {
    case 'w': dur = 4.0f;   s++; break;
    case 'h': dur = 2.0f;   s++; break;
    case 'q': dur = 1.0f;   s++; break;
    case 'e': dur = 0.5f;   s++; break;
    case 's': dur = 0.25f;  s++; break;
    case 't': dur = 0.125f; s++; break;
    default:  break; /* no duration char = quarter */
    }

    /* Dotted */
    if (*s == '.') { dur *= 1.5f; s++; }

    *p = s;
    return dur;
}

/* ── DSL Parser ────────────────────────────────────────────────────────── */

static int seq_parse(MiniSeq *seq, const char *dsl) {
    memset(seq->notes, 0, sizeof(seq->notes));
    seq->num_notes = 0;
    seq->current_note = 0;
    seq->note_time = 0.0f;
    seq->playing = 0;
    seq->last_note_on = -1;
    seq->bpm = 120.0f;
    seq->loop = 0;

    if (!dsl || !*dsl) return -1;

    /* Copy source for status reporting */
    strncpy(seq->source, dsl, sizeof(seq->source) - 1);
    seq->source[sizeof(seq->source) - 1] = '\0';

    const char *p = dsl;

    /* Skip leading whitespace */
    while (*p == ' ' || *p == '\t') p++;

    /* Parse BPM */
    if (*p >= '0' && *p <= '9') {
        int bpm = 0;
        while (*p >= '0' && *p <= '9') {
            bpm = bpm * 10 + (*p - '0');
            p++;
        }
        if (bpm > 0 && bpm <= 999) seq->bpm = (float)bpm;

        /* Loop flag */
        if (*p == 'L' || *p == 'l') { seq->loop = 1; p++; }
    }

    /* Parse notes */
    while (*p && seq->num_notes < SEQ_MAX_NOTES) {
        /* Skip whitespace and pipe separators */
        while (*p == ' ' || *p == '\t' || *p == '|') p++;
        if (!*p) break;

        int note = seq_parse_note(&p);
        if (note == -2) {
            /* Unknown char — skip it */
            p++;
            continue;
        }

        float dur = seq_parse_duration(&p);

        /* Optional velocity: v followed by number */
        int vel = 100;
        if (*p == 'v' || *p == 'V') {
            p++;
            int v = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            if (v >= 1 && v <= 127) vel = v;
        }

        SeqNote *n = &seq->notes[seq->num_notes++];
        n->midi_note = note;
        n->duration_beats = dur;
        n->velocity = vel;
    }

    return seq->num_notes;
}

/* ── Seed-based random generation (from yama-bruh) ─────────────────────── */

static uint32_t seq_djb2(const char *str) {
    uint32_t h = 5381;
    while (*str) { h = ((h << 5) + h) + (uint8_t)*str; str++; }
    return h;
}

static uint32_t seq_rng(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static int seq_generate(MiniSeq *seq, const char *seed_str, float bpm, int loop_flag) {
    uint32_t seed = seq_djb2(seed_str);
    uint32_t rng = (seed == 0) ? 1 : seed;

    static const int movements[9] = { 0, 2, -2, 3, -3, 4, -4, 6, -6 };
    static const float durations[5] = { 0.125f, 0.25f, 0.5f, 1.0f, 2.0f };

    int num = 3 + (int)(seed % 3);
    if (num > SEQ_MAX_NOTES) num = SEQ_MAX_NOTES;

    int oct_off = (int)(seq_rng(&rng) % 3) * 12;
    int cur = 54 + oct_off; /* F#3, F#4, or F#5 */

    memset(seq->notes, 0, sizeof(seq->notes));
    for (int i = 0; i < num; i++) {
        cur += movements[seq_rng(&rng) % 9];
        if (cur < 42) cur += 12;
        if (cur > 84) cur -= 12;

        seq->notes[i].midi_note = cur;
        seq->notes[i].duration_beats = durations[seq_rng(&rng) % 5];
        seq->notes[i].velocity = 100;
    }

    seq->num_notes = num;
    seq->current_note = 0;
    seq->note_time = 0.0f;
    seq->bpm = (bpm > 0) ? bpm : 120.0f;
    seq->loop = loop_flag;
    seq->last_note_on = -1;
    snprintf(seq->source, sizeof(seq->source), "seed:%s", seed_str);
    return num;
}

/* ── Init / bind ───────────────────────────────────────────────────────── */

static void seq_init(MiniSeq *seq) {
    memset(seq, 0, sizeof(MiniSeq));
    seq->bpm = 120.0f;
    seq->last_note_on = -1;
    seq->source[0] = '\0';
}

static void seq_bind(MiniSeq *seq, void *inst_state,
                     void (*midi_fn)(void *, uint8_t, uint8_t, uint8_t),
                     uint8_t channel) {
    seq->inst_state = inst_state;
    seq->midi_fn = midi_fn;
    seq->midi_channel = channel & 0x0F;
}

/* ── Fire MIDI through bound instrument ────────────────────────────────── */

static inline void seq_note_on(MiniSeq *seq, int note, int vel) {
    if (!seq->midi_fn || note < 0) return;
    uint8_t status = (uint8_t)(0x90 | seq->midi_channel);
    seq->midi_fn(seq->inst_state, status, (uint8_t)note, (uint8_t)vel);
    seq->last_note_on = note;
}

static inline void seq_note_off(MiniSeq *seq, int note) {
    if (!seq->midi_fn || note < 0) return;
    uint8_t status = (uint8_t)(0x80 | seq->midi_channel);
    seq->midi_fn(seq->inst_state, status, (uint8_t)note, 0);
    if (seq->last_note_on == note) seq->last_note_on = -1;
}

/* ── Play / Stop ───────────────────────────────────────────────────────── */

static void seq_play(MiniSeq *seq) {
    if (seq->num_notes == 0) return;
    seq->current_note = 0;
    seq->note_time = 0.0f;
    seq->playing = 1;

    /* Release any hanging note */
    if (seq->last_note_on >= 0) seq_note_off(seq, seq->last_note_on);

    /* Trigger first note */
    seq_note_on(seq, seq->notes[0].midi_note, seq->notes[0].velocity);
}

static void seq_stop(MiniSeq *seq) {
    if (seq->last_note_on >= 0) seq_note_off(seq, seq->last_note_on);
    seq->playing = 0;
}

/* ── Tick — call once per sample from the render loop ──────────────────── */

static void seq_tick(MiniSeq *seq, float dt) {
    if (!seq->playing || seq->num_notes == 0) return;

    float beat_dur = 60.0f / seq->bpm;
    seq->note_time += dt;

    if (seq->current_note >= seq->num_notes) return;

    SeqNote *cur = &seq->notes[seq->current_note];
    float note_dur = cur->duration_beats * beat_dur;

    if (seq->note_time >= note_dur) {
        /* Release current note */
        if (cur->midi_note >= 0 && seq->last_note_on == cur->midi_note) {
            seq_note_off(seq, cur->midi_note);
        }

        seq->current_note++;
        seq->note_time -= note_dur; /* carry remainder for tight timing */

        if (seq->current_note < seq->num_notes) {
            /* Next note */
            SeqNote *next = &seq->notes[seq->current_note];
            seq_note_on(seq, next->midi_note, next->velocity);
        } else if (seq->loop) {
            seq->current_note = 0;
            SeqNote *first = &seq->notes[0];
            seq_note_on(seq, first->midi_note, first->velocity);
        } else {
            seq->playing = 0;
        }
    }
}

/* ── OSC handler — shared across all instruments ───────────────────────
 *
 * Call from the instrument's osc_handle when sub_path starts with "/seq/".
 * Handles:
 *   /seq/play    sargs[0] = DSL string
 *   /seq/seed    sargs[0] = seed string, fargs[0] = bpm, iargs[0] = loop
 *   /seq/stop
 *   /seq/bpm     fargs[0] = new bpm
 *   /seq/loop    iargs[0] = 0/1
 */
static int seq_osc_handle(MiniSeq *seq, const char *sub_path,
                          const int32_t *iargs, int ni,
                          const float *fargs, int nf) {
    if (strncmp(sub_path, "/seq/", 5) != 0) return 0; /* not ours */
    const char *cmd = sub_path + 4; /* skip "/seq" */

    if (strcmp(cmd, "/stop") == 0) {
        seq_stop(seq);
        fprintf(stderr, "[seq] stopped\n");
    }
    else if (strcmp(cmd, "/bpm") == 0 && nf >= 1) {
        if (fargs[0] > 0 && fargs[0] <= 999)
            seq->bpm = fargs[0];
        fprintf(stderr, "[seq] bpm = %.0f\n", seq->bpm);
    }
    else if (strcmp(cmd, "/loop") == 0 && ni >= 1) {
        seq->loop = iargs[0] ? 1 : 0;
        fprintf(stderr, "[seq] loop = %d\n", seq->loop);
    }
    else if (strcmp(cmd, "/play") == 0) {
        /* DSL string comes as the sub_path remainder after /seq/play/
         * or we check for a sargs-style interface. Since our OSC handler
         * doesn't pass string args directly, we use the path trick:
         * /seq/play/120L C4q E4e G4e
         * The caller should strip "/seq/play/" and pass the rest. */
        /* If no further path, treat iargs as: iargs[0]=seed_hash */
        if (ni >= 1) {
            /* Numeric seed mode */
            uint32_t seed = (uint32_t)iargs[0];
            float bpm = (nf >= 1) ? fargs[0] : seq->bpm;
            int loop = (ni >= 2) ? (iargs[1] ? 1 : 0) : seq->loop;

            uint32_t rng = (seed == 0) ? 1 : seed;
            static const int movements[9] = { 0, 2, -2, 3, -3, 4, -4, 6, -6 };
            static const float durations[5] = { 0.125f, 0.25f, 0.5f, 1.0f, 2.0f };

            int num = 3 + (int)(seed % 3);
            if (num > SEQ_MAX_NOTES) num = SEQ_MAX_NOTES;
            int oct_off = (int)(seq_rng(&rng) % 3) * 12;
            int cur = 54 + oct_off;
            for (int i = 0; i < num; i++) {
                cur += movements[seq_rng(&rng) % 9];
                if (cur < 42) cur += 12;
                if (cur > 84) cur -= 12;
                seq->notes[i].midi_note = cur;
                seq->notes[i].duration_beats = durations[seq_rng(&rng) % 5];
                seq->notes[i].velocity = 100;
            }
            seq->num_notes = num;
            seq->bpm = bpm;
            seq->loop = loop;
            snprintf(seq->source, sizeof(seq->source), "seed:%u", seed);
            seq_play(seq);
            fprintf(stderr, "[seq] play seed=%u notes=%d bpm=%.0f loop=%d\n",
                    seed, num, bpm, loop);
        }
    }

    return 1; /* handled */
}

/* ── HTTP/JSON handler — for DSL string from web UI ────────────────────── */

static int seq_handle_dsl(MiniSeq *seq, const char *dsl_string) {
    int n = seq_parse(seq, dsl_string);
    if (n > 0) {
        seq_play(seq);
        fprintf(stderr, "[seq] play dsl=\"%.60s%s\" notes=%d bpm=%.0f loop=%d\n",
                dsl_string, (strlen(dsl_string) > 60 ? "..." : ""),
                n, seq->bpm, seq->loop);
        return 0;
    }
    return -1;
}

#endif /* MINIWAVE_SEQ_H */
