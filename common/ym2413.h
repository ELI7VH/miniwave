#ifndef YM2413_H
#define YM2413_H

#include "instruments.h"
#include "seq.h"
#include "keyseq.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Constants ────────────────────────────────────────────────────────── */

#define OPLL_TAU            (2.0f * (float)M_PI)
#define OPLL_CHANNELS       9
#define OPLL_LIMITER_THRESH 0.7f
#define OPLL_LIMITER_CEIL   0.95f
#define OPLL_SINE_BITS      10
#define OPLL_SINE_LEN       (1 << OPLL_SINE_BITS)
#define OPLL_PHASE_BITS     19
#define OPLL_PHASE_MASK     ((1 << OPLL_PHASE_BITS) - 1)

/* DAC simulation */
#define OPLL_DAC_BITS       9
#define OPLL_DAC_LEVELS     (1 << OPLL_DAC_BITS)
#define OPLL_NOISE_FLOOR    0.0012f

/* Envelope */
#define OPLL_ENV_FLOOR      1e-5f
#define OPLL_ENV_SNAP       2e-4f
#define OPLL_VOICE_DONE     0.004f

/* Chip LFO rates (Hz) */
#define OPLL_TREM_RATE      3.7f
#define OPLL_TREM_DEPTH     0.28f
#define OPLL_CHIPVIB_RATE   6.4f
#define OPLL_CHIPVIB_DEPTH  0.008f

/* Portamento / choke */
#define OPLL_PORTA_TIME     0.08f
#define OPLL_CHOKE_FADE     0.006f
#define OPLL_FADEIN_SAMPLES 48      /* ~1ms @ 48kHz — anti-click on note start */

/* Drum engine */
#define OPLL_MAX_DRUM_HITS  32
#define OPLL_NUM_DRUM_BANKS 8

/* Key sequence */
#define OPLL_MAX_SEQ_NOTES  10
#define OPLL_SEQ_BPM_DEFAULT 140.0f

/* ══════════════════════════════════════════════════════════════════════════
 *  100 YAMA-BRUH PRESETS (YM2413 Extended, 21-element format)
 *
 *  [0]  carrier_ratio    [1]  mod_ratio       [2]  mod_index
 *  [3]  carrier_attack   [4]  carrier_decay   [5]  carrier_sustain
 *  [6]  carrier_release  [7]  feedback        [8]  carrier_wave
 *  [9]  mod_wave         [10] tremolo_depth   [11] chip_vibrato
 *  [12] ksr              [13] ksl             [14] mod_level
 *  [15] eg_type          [16] mod_attack      [17] mod_decay
 *  [18] mod_sustain      [19] mod_release     [20] mod_eg_type
 * ══════════════════════════════════════════════════════════════════════════ */

#define YB_PRESET_PARAMS 21
#define YB_NUM_PRESETS   100

static const float YB_PRESETS[YB_NUM_PRESETS][YB_PRESET_PARAMS] = {
    /* 00 Piano 1           */ {1,3,0.515,0.003,1.0,0.3,0.4,0,0,0,0,0,0.3,1,1,1,0.001,0.6,0.708,0.3,1},
    /* 01 Piano 2           */ {1,3,0.942,0.003,1.0,0.3,0.4,0,0,0,0,0,0.3,1,1,1,0.001,0.7,0.708,0.3,1},
    /* 02 Honky-Tonk Piano  */ {1,3,1.582,0.004,0.9,0.3,0.35,0,0,0,0,0,0.3,1,1,1,0.001,0.5,0.708,0.25,1},
    /* 03 Electric Piano 1  */ {1,1,0.334,0.001,1.0,0.4,0.4,0.393,0,0,0.5,0,0.3,1,1,1,0.001,0.6,0.708,0.3,1},
    /* 04 Electric Piano 2  */ {1,1,0.793,0.001,1.0,0.4,0.4,0.785,0,0,0.5,0,0.3,1,1,1,0.001,0.6,0.708,0.3,1},
    /* 05 Harpsichord 1     */ {1,3,3.157,0.003,0.3,0.08,0.1,0,0,0,0,0,0.3,1,1,1,0.001,0.2,0.5,0.1,1},
    /* 06 Harpsichord 2     */ {1,3,9.699,0.003,0.25,0.08,0.1,0,0,0,0,0,0.3,1,1,1,0.001,0.2,0.5,0.1,1},
    /* 07 Harpsichord 3     */ {1,3,9.699,0.003,0.3,0.08,0.1,0,0,0,0,0,0.3,1,1,1,0.001,0.2,0.5,0.1,1},
    /* 08 Honky-Tonk Clavi  */ {1,3,5.299,0.002,0.1,0.006,0.1,0,0,0,0,0,0.3,1.5,1,0,0.002,0.15,0.708,0.1,0},
    /* 09 Glass Celesta     */ {1,7,3.157,0.001,1.0,0.05,0.5,0,0,0,0.5,0,0.3,1.5,1,1,0.005,0.6,0.3,0.4,1},
    /* 10 Reed Organ        */ {1,3,0.281,0.04,0.25,1,0.1,0.393,0,1,0,0,0.3,1.5,1,0,0.03,0.25,1,0.1,0},
    /* 11 Pipe Organ 1      */ {1,0.5,0.055,0.04,0.25,1,0.1,0,0,0,0,0,0.3,1.5,1,0,0.001,0.3,1,0.1,0},
    /* 12 Pipe Organ 2      */ {1,3,0.258,0.04,0.25,1,0.1,0.393,0,1,0,0,0.3,1.5,1,0,0.03,0.25,1,0.1,0},
    /* 13 Electronic Organ 1*/ {1,1,0.365,0.04,0.25,1,0.1,0.785,1,0,0,0,0.3,1.5,1,0,0.04,0.3,1,0.12,0},
    /* 14 Electronic Organ 2*/ {1,3,0.258,0.04,0.25,1,0.1,0.393,0,1,0,0,0.3,1.5,1,0,0.03,0.25,1,0.1,0},
    /* 15 Jazz Organ        */ {1,3,2.235,0.03,0.2,0.708,0.1,0.393,2,0,0,0,0.3,1.5,1,0,0.001,0.2,0.5,0.1,0},
    /* 16 Accordion         */ {1,3,2.05,0.04,0.25,1,0.1,0.785,0,1,0,0,0.3,1.5,1,0,0.04,0.3,1,0.1,0},
    /* 17 Vibraphone        */ {1,7,0.561,0.001,0.9,0.1,0.5,0.785,0,0,0.5,0,0.3,1,1,1,0.001,0.6,0.4,0.4,1},
    /* 18 Marimba 1         */ {2,0.5,12.566,0.001,0.35,0.02,0.5,0.785,0,0,0,0,0.3,1,1,1,0.001,0.2,0.1,0.4,1},
    /* 19 Marimba 2         */ {2,0.5,12.566,0.001,0.35,0.02,0.5,0.785,0,0,0,0,0.3,1,1,1,0.001,0.2,0.1,0.4,1},
    /* 20 Trumpet           */ {1,1,1.12,0.04,0.2,1,0.1,0.785,0,1,0,0,0.3,1.5,1,0,0.03,0.15,0.708,0.1,0},
    /* 21 Mute Trumpet      */ {1,1,1.12,0.04,0.2,1,0.1,0.785,1,0,0,0,0.3,1.5,1,0,0.02,0.15,0.501,0.1,0},
    /* 22 Trombone          */ {0.5,0.5,2.546,0.001,0.3,1,0.2,0.785,0,0,0,0,0.3,2,1,0,0.049,0.18,0.708,0.9896,0},
    /* 23 Soft Trombone     */ {0.5,0.5,1.451,0.05,0.2,1,0.1,0.785,0,0,0,0,0.3,1.5,1,0,0.04,0.15,0.708,0.1,0},
    /* 24 Horn              */ {1,1,1.027,0.05,0.2,1,0.1,0.785,0,0,0,0,0.3,1.5,1,0,0.04,0.15,0.708,0.1,0},
    /* 25 Alpenhorn         */ {1,1,0.667,0.06,0.2,1,0.1,0,0,0,0,0,0.3,1.5,1,0,0.05,0.15,0.708,0.1,0},
    /* 26 Tuba              */ {0.5,0.5,1.725,0.05,0.18,1,0.1,0.785,0,0,0,0,0.3,1.5,1,0,0.04,0.15,0.708,0.1,0},
    /* 27 Brass Ensemble 1  */ {1,1,1.12,0.05,0.2,1,0.1,0.785,0,0,0,0,0.3,1.5,1,0,0.04,0.15,0.708,0.1,0},
    /* 28 Brass Ensemble 2  */ {1,1,1.12,0.05,0.2,1,0.1,0.785,0,1,0,0,0.3,1.5,1,0,0.03,0.15,0.708,0.1,0},
    /* 29 Brass Ensemble 3  */ {0.5,0.5,1.725,0.05,0.18,1,0.1,0.785,0,0,0,0,0.3,1.5,1,0,0.04,0.15,0.708,0.1,0},
    /* 30 Flute             */ {1,1,3.752,0.04,0.2,0.7,0.1,0.785,0,0,0,0,0.3,1,1,1,0.02,0.15,0.5,0.1,1},
    /* 31 Panflute          */ {1,6,0.793,0.04,0.2,1,0.1,0.785,0,0,0,0,0.3,1.5,1,0,0.003,0.2,0.7,0.1,0},
    /* 32 Piccolo           */ {4,4,0.199,0.04,0.2,1,0.1,0.785,0,0,0,0,0.3,1.5,1,0,0.02,0.15,0.708,0.1,0},
    /* 33 Clarinet          */ {1,2,0.942,0.04,0.2,0.8,0.1,0.785,0,2,0,0,0.3,1,1,0,0.003,0.2,1,0.1,0},
    /* 34 Bass Clarinet     */ {0.5,1,1.027,0.05,0.2,1,0.12,0.785,0,2,0,0,0.3,1.5,1,0,0.03,0.2,1,0.12,0},
    /* 35 Oboe              */ {2,1,1.88,0.04,0.2,0.8,0.1,0.393,0,1,0,0,0.3,1,1,0,0.003,0.15,1,0.1,0},
    /* 36 Bassoon           */ {1,0.5,1.88,0.05,0.2,1,0.1,0.393,0,0,0,0,0.3,1.5,1,0,0.003,0.15,1,0.1,0},
    /* 37 Saxophone         */ {1,0.5,1.451,0.05,0.2,1,0.1,0.785,0,1,0,0,0.3,1.5,1,0,0.01,0.15,1,0.1,0},
    /* 38 Bagpipe           */ {1,0.5,8.896,0.06,0.2,1,0.1,0.196,0,0,0.5,0,0.3,1.5,1,0,0.001,0.15,1,0.1,0},
    /* 39 Woodwinds         */ {1,0.5,1.88,0.05,0.2,1,0.1,0.393,0,0,0,0,0.3,1.5,1,0,0.003,0.15,1,0.1,0},
    /* 40 Violin 1          */ {1,1,0.942,0.1,0.25,0.85,0.25,0.785,0,0,0,0,0.3,1,1,0,0.05,0.3,1,0.4,0},
    /* 41 Violin 2          */ {1,1,1.331,0.1,0.25,1,0.25,0.785,0,0,0,0,0.3,1.5,1,0,0.05,0.3,1,0.4,0},
    /* 42 Cello             */ {1,0.5,1.027,0.12,0.25,1,0.3,0.785,0,0,0,0,0.3,1.5,1,0,0.06,0.3,1,0.4,0},
    /* 43 Strings           */ {1,0.5,1.027,0.15,0.25,1,0.3,0.785,0,0,0,0,0.3,1.5,1,0,0.07,0.3,1,0.4,0},
    /* 44 Electric Bass     */ {0.5,0.5,2.05,0.002,0.3,0.5,0.1,0.196,0,0,0,0,0.3,1,1,1,0.001,0.2,0.501,0.1,1},
    /* 45 Slap Bass         */ {0.5,0.5,7.485,0.001,0.25,0.2,0.08,0.393,0,0,0,0,0.3,1,1,1,0.001,0.15,0.251,0.08,1},
    /* 46 Wood Bass         */ {0.5,0.5,2.05,0.003,0.3,0.5,0.1,0.196,0,0,0,0,0.3,1,1,1,0.002,0.2,0.6,0.1,1},
    /* 47 Synth Bass        */ {0.5,0.5,3.157,0.001,0.3,0.35,0.1,0.393,0,0,0,0,0.3,1,1,1,0.001,0.15,0.501,0.1,1},
    /* 48 Banjo             */ {1,3,2.05,0.001,0.4,0.2,0.15,0.785,0,3,0,0,0.3,1,1,1,0.001,0.2,0.3,0.15,1},
    /* 49 Mandolin          */ {1,3,1.582,0.001,0.35,0.2,0.12,0.393,0,0,0,0,0.3,1,1,1,0.001,0.18,0.4,0.12,1},
    /* 50 Classic Guitar    */ {1,0.5,8.16,0.002,0.4,0.2,0.15,0.785,0,0,0,0,0.3,1,1,1,0.001,0.2,0.126,0.15,1},
    /* 51 Jazz Guitar       */ {1,3,0.515,0.002,0.35,0.25,0.15,0,0,0,0.5,0,0.3,1,1,1,0.001,0.18,0.5,0.15,1},
    /* 52 Folk Guitar       */ {1,3,9.699,0.001,0.4,0.15,0.15,0,0,0,0,0,0.3,1,1,1,0.001,0.15,0.178,0.15,1},
    /* 53 Hawaiian Guitar   */ {1,3,1.451,0.001,0.5,0.3,0.2,0.785,0,0,0.5,0,0.3,1,1,1,0.001,0.4,1,0.2,1},
    /* 54 Ukulele           */ {2,1,5.777,0.002,0.35,0.3,0.15,0.393,0,0,0,0,0.3,1,1,1,0.001,0.2,1,0.15,1},
    /* 55 Koto              */ {1,3,3.157,0.001,0.4,0.15,0.15,0.785,0,3,0,0,0.3,1,1,1,0.001,0.2,0.355,0.15,1},
    /* 56 Shamisen          */ {1,3,12.566,0.001,0.3,0.05,0.12,0.785,0,3,0,0,0.3,1,1,1,0.001,0.15,0.089,0.12,1},
    /* 57 Harp              */ {1,0.5,1.221,0.001,0.5,0.3,0.2,0,0,0,0,0,0.3,1,1,1,0.001,0.35,1,0.2,1},
    /* 58 Harmonica         */ {1,3,1.582,0.04,0.25,1,0.1,0.393,0,0,0.5,0,0.3,1.5,1,0,0.03,0.2,1,0.1,0},
    /* 59 Music Box         */ {2,9,3.441,0.001,1.0,0.02,0.5,0.196,0,0,0.5,0,0.3,1,1,1,0.001,0.5,0.05,0.4,1},
    /* 60 Brass & Marimba   */ {3,1,1.027,0.04,0.25,0.3,0.1,12.566,0,0,0,0,0.3,1.5,1,0,0.04,0.2,1,0.1,0},
    /* 61 Flute & Harpsi.   */ {1,3,9.699,0.04,0.25,0.08,0.1,0,0,1,0,0,0.3,1,1,1,0.001,0.2,0.5,0.1,1},
    /* 62 Oboe & Vibra.     */ {2,1,1.88,0.04,0.2,0.708,0.1,0.393,0,0,0,0,0.3,1.5,1,0,0.003,0.2,1,0.1,0},
    /* 63 Clarinet & Harp   */ {1,0.5,1.221,0.04,0.4,0.4,0.15,0,0,0,0,0,0.3,1,1,1,0.001,0.3,1,0.15,1},
    /* 64 Violin & Steel Dr.*/ {0.5,3,5.299,0.04,0.3,0.2,0.12,0,0,0,0.5,0,0.3,1,1,1,0.02,0.2,0.3,0.12,0},
    /* 65 Handsaw           */ {5,0.5,0.793,0.04,0.3,0.7,0.1,0.785,2,0,0,0,0.3,1.5,1,1,0.001,0.2,0.708,0.1,1},
    /* 66 Synth Brass       */ {1,1,1.725,0.04,0.25,0.7,0.1,0,1,0,0,0,0.3,1.5,1,0,0.001,0.15,0.251,0.1,0},
    /* 67 Metallic Synth    */ {1,1,4.861,0.04,0.2,0.6,0.1,0,2,1,0,0,0.3,1,1,1,0.05,0.15,0.3,0.1,1},
    /* 68 Sine Wave         */ {1,1,0.055,0.12,0.99,1,0.2,0,0,0,0,0,0.3,1.5,1,0,99,0.00005,0.006,0.2,0},
    /* 69 Reverse           */ {1,0.5,0.793,0.3,0.3,0.3,0.4,0,0,0,0.5,0,0.3,1.5,1,0,0.001,0.99,1,0.99,0},
    /* 70 Human Voice 1     */ {3,1,3.157,0.08,0.4,0.6,0.2,0,0,1,0,0,0.3,1.5,1,0,0.05,0.3,0.5,0.2,0},
    /* 71 Human Voice 2     */ {1,2,1.725,0.08,0.4,0.6,0.2,0,0,1,0,0,0.3,1.5,1,0,0.001,0.3,1,0.2,0},
    /* 72 Human Voice 3     */ {4,1,1.221,0.1,0.4,0.6,0.2,0.785,0,1,0,0,0.3,1.5,1,0,0.001,0.2,0.5,0.2,0},
    /* 73 Whisper           */ {1,2,0.793,0.1,0.5,1,0.2,0,0,0,0,0,0.3,1.5,1,0,0.001,0.3,1,0.2,0},
    /* 74 Whistle           */ {4,6,0.793,0.05,0.3,1,0.15,0.785,0,0,0,0,0.3,1.5,1,0,0.003,0.3,0.7,0.15,0},
    /* 75 Gurgle            */ {1,0.5,12.566,1,0.3,0.5,0.3,0,0,5,0.5,0,0.3,1,1,1,3,0.99,1,0.2,0},
    /* 76 Bubble            */ {1,0.5,1.582,0.04,0.2,0.6,0.2,0.785,0,3,0,0,0.3,1,1,1,0.001,0.99,1,0.99,1},
    /* 77 Raindrop          */ {1,3,0.055,0.04,0.15,0.1,0.2,0.785,0,0,0,0,0.3,1,1,1,0.001,0.1,0.1,0.15,1},
    /* 78 Popcorn           */ {1,1,2.436,0.001,0.15,0.01,0.1,0.393,3,3,0,0,0.3,1.5,1,0,0.6,0.99,0.1,0.1,0},
    /* 79 Drip              */ {1,0.5,3.157,0.001,0.15,0.01,0.1,0.393,0,3,0,0,0.3,1.5,1,0,1,0.2,0.1,0.1,0},
    /* 80 Dog Pianist       */ {4,1,1.221,0.01,0.2,0.5,0.08,0.785,0,1,0,0,0.3,1.5,1,0,0.001,0.2,0.5,0.08,0},
    /* 81 Duck              */ {1,1,4.861,0.001,0.3,0.6,0.2,0.393,0,0,0,0,0.3,1.5,1,0,0.2,0.2,0.5,0.2,0},
    /* 82 Baby Doll         */ {4,8,2.895,0.02,0.3,0.4,0.1,0.196,0,0,0,0,0.3,1.5,1,0,0.02,0.3,0.708,0.1,0},
    /* 83 Telephone Bell    */ {1,2,2.895,0.001,1.5,0.05,0.8,0,1,0,0,0,0.3,1,1,1,0.001,0.8,0.3,0.6,1},
    /* 84 Emergency Alarm   */ {1,1,1.027,0.01,0.2,1,0.1,0.785,0,0,0,0,0.3,1.5,1,0,0.02,0.15,0.708,0.1,0},
    /* 85 Leaf Spring       */ {8,0.5,2.656,0.005,0.4,0.5,0.2,0.196,2,0,0.5,0,0.3,1.5,1,1,0.001,0.3,0.251,0.99,1},
    /* 86 Comet             */ {1,2,2.895,0.02,0.4,0.3,0.1,0,1,1,0,0,0.3,1.5,1,0,0.02,0.3,0.708,0.1,0},
    /* 87 Fireworks         */ {1,3,12.566,0.001,0.3,1,0.3,0.785,0,5,0,0,0.3,1,1,1,5,0.3,1,0.99,1},
    /* 88 Crystal           */ {1,2,2.895,0.001,1.0,0.02,0.5,0,1,0,0,0,0.3,1.5,1,1,0.001,0.7,0.1,0.4,1},
    /* 89 Ghost             */ {1,2,1.725,0.3,0.6,0.3,0.5,0,1,0,0,0,0.3,1.5,1,0,0.001,0.99,1,0.99,0},
    /* 90 Hand Bell         */ {1,3,3.157,0.001,2.0,0.02,0.8,0,0,0,0,0,0.3,1,1,1,5,0.5,0.05,0.8,1},
    /* 91 Chimes            */ {1,2,2.895,0.001,2.0,0.02,0.5,0,1,2,0,0,0.3,1.5,1,1,0.001,0.7,0.05,0.5,1},
    /* 92 Bell              */ {1,2,8.16,0.001,2.0,0.02,0.5,0,1,0,0,0,0.3,1.5,1,1,0.001,0.7,0.05,0.5,1},
    /* 93 Steel Drum        */ {0.5,3,5.299,0.001,0.7,0.05,0.3,0,0,2,0.5,0,0.3,1,1,1,0.04,0.3,0.1,0.3,1},
    /* 94 Cowbell            */ {1,5,0.472,0.001,0.4,0.3,0.2,0.785,0,3,0,0,0.3,1,1,1,0.001,0.2,0.5,0.2,1},
    /* 95 Synth Tom 1       */ {1,1,3.157,0.001,0.15,0.05,0.1,0.785,0,0,0,0,0.3,1,1,1,0.001,0.08,0.1,0.1,1},
    /* 96 Synth Tom 2       */ {0.5,0.5,3.157,0.001,0.15,0.03,0.1,0.785,0,0,0,0,0.3,1,1,1,0.001,0.08,0.05,0.1,1},
    /* 97 Snare Drum        */ {3,1,12.566,0.001,0.12,0.05,0.08,0.785,5,1,0.5,0,0.3,1.5,1,1,0.001,0.06,0.1,0.08,1},
    /* 98 Machine Gun       */ {1,4,12.57,0.001,0.05,0.1,0.0015,0.785,4,0,0,0,0.3,1,1,1,0.001,0.08,0.3,0.06005,1},
    /* 99 Wave              */ {8,9,10.57,0.318,0.3115,0.086,0.3695,10.52,4,4,0,0,0.3,1.5,1,0,1.815,0.5474,1,0.9896,0},
};

static const char *YB_PRESET_NAMES[YB_NUM_PRESETS] = {
    "Piano 1","Piano 2","Honky-Tonk Piano","Electric Piano 1","Electric Piano 2",
    "Harpsichord 1","Harpsichord 2","Harpsichord 3","Honky-Tonk Clavi","Glass Celesta",
    "Reed Organ","Pipe Organ 1","Pipe Organ 2","Electronic Organ 1","Electronic Organ 2",
    "Jazz Organ","Accordion","Vibraphone","Marimba 1","Marimba 2",
    "Trumpet","Mute Trumpet","Trombone","Soft Trombone","Horn",
    "Alpenhorn","Tuba","Brass Ensemble 1","Brass Ensemble 2","Brass Ensemble 3",
    "Flute","Panflute","Piccolo","Clarinet","Bass Clarinet",
    "Oboe","Bassoon","Saxophone","Bagpipe","Woodwinds",
    "Violin 1","Violin 2","Cello","Strings","Electric Bass",
    "Slap Bass","Wood Bass","Synth Bass","Banjo","Mandolin",
    "Classic Guitar","Jazz Guitar","Folk Guitar","Hawaiian Guitar","Ukulele",
    "Koto","Shamisen","Harp","Harmonica","Music Box",
    "Brass & Marimba","Flute & Harpsichord","Oboe & Vibraphone","Clarinet & Harp","Violin & Steel Drum",
    "Handsaw","Synth Brass","Metallic Synth","Sine Wave","Reverse",
    "Human Voice 1","Human Voice 2","Human Voice 3","Whisper","Whistle",
    "Gurgle","Bubble","Raindrop","Popcorn","Drip",
    "Dog Pianist","Duck","Baby Doll","Telephone Bell","Emergency Alarm",
    "Leaf Spring","Comet","Fireworks","Crystal","Ghost",
    "Hand Bell","Chimes","Bell","Steel Drum","Cowbell",
    "Synth Tom 1","Synth Tom 2","Snare Drum","Machine Gun","Wave",
};

/* ══════════════════════════════════════════════════════════════════════════
 *  KEY SEQUENCE GENERATOR
 *  DJB2 hash + XorShift PRNG → F#m pentatonic melody (3-5 notes)
 *  Deterministic: same seed string → identical sequence on every device
 * ══════════════════════════════════════════════════════════════════════════ */

/* DJB2 hash — matches yama-bruh Rust/JS implementation */
static uint32_t opll_djb2_hash(const char *str) {
    uint32_t h = 5381;
    while (*str) {
        h = ((h << 5) + h) + (uint8_t)*str;
        str++;
    }
    return h;
}

/* XorShift PRNG — matches yama-bruh Rust implementation */
typedef struct {
    uint32_t state;
} OPLLRng;

static inline void opll_rng_init(OPLLRng *rng, uint32_t seed) {
    rng->state = (seed == 0) ? 1 : seed;
}

static inline uint32_t opll_rng_next(OPLLRng *rng) {
    rng->state ^= rng->state << 13;
    rng->state ^= rng->state >> 17;
    rng->state ^= rng->state << 5;
    return rng->state;
}

static inline uint32_t opll_rng_range(OPLLRng *rng, uint32_t n) {
    return opll_rng_next(rng) % n;
}

/* Sequence note */
typedef struct {
    int   midi_note;
    float duration_beats;  /* in beats: 0.125, 0.25, 0.5, 1.0, 2.0 */
} OPLLSeqNote;

/* Key sequence state */
typedef struct {
    OPLLSeqNote notes[OPLL_MAX_SEQ_NOTES];
    int         num_notes;
    int         current_note;       /* playback cursor */
    float       note_time;          /* elapsed time in current note */
    float       bpm;
    int         preset_idx;         /* yama-bruh preset index (0-99) */
    int         playing;
    int         loop;               /* 0 = one-shot, 1 = loop */
    uint32_t    seed;               /* hash of seed string */
} OPLLKeySequence;

/*
 * Generate a deterministic F#m pentatonic melody from a seed.
 *
 * Algorithm (matching yama-bruh):
 *   - Start on F# in random octave (F#3=54, F#4=66, F#5=78)
 *   - Move by intervals: [0, ±2, ±3, ±4, ±6] semitones
 *   - Clamp to MIDI 42-84 with octave wrapping
 *   - 3-5 notes, with random durations from [1/8, 1/4, 1/2, 1, 2] beats
 */
static void opll_generate_sequence(OPLLKeySequence *seq, uint32_t seed) {
    static const int movements[9] = { 0, 2, -2, 3, -3, 4, -4, 6, -6 };
    static const float durations[5] = { 0.125f, 0.25f, 0.5f, 1.0f, 2.0f };

    OPLLRng rng;
    opll_rng_init(&rng, seed);

    int num_notes = 3 + (int)(seed % 3);
    if (num_notes > OPLL_MAX_SEQ_NOTES) num_notes = OPLL_MAX_SEQ_NOTES;

    int octave_offset = (int)opll_rng_range(&rng, 3) * 12;
    int current_note = 54 + octave_offset; /* F#3, F#4, or F#5 */

    for (int i = 0; i < num_notes; i++) {
        current_note += movements[opll_rng_range(&rng, 9)];
        if (current_note < 42) current_note += 12;
        if (current_note > 84) current_note -= 12;

        seq->notes[i].midi_note = current_note;
        seq->notes[i].duration_beats = durations[opll_rng_range(&rng, 5)];
    }

    seq->num_notes = num_notes;
    seq->current_note = 0;
    seq->note_time = 0.0f;
    seq->seed = seed;
}

/* ── Envelope states ──────────────────────────────────────────────────── */

enum {
    OPLL_ENV_OFF     = 0,
    OPLL_ENV_ATTACK  = 1,
    OPLL_ENV_DECAY   = 2,
    OPLL_ENV_SUSTAIN = 3,
    OPLL_ENV_RELEASE = 4
};

/* ── ROM patch data (15 instruments + 5 rhythm) ──────────────────────── */

static const uint8_t OPLL_ROM_PATCHES[15][8] = {
    /* 1  Violin       */ { 0x71, 0x61, 0x1E, 0x17, 0xD0, 0x78, 0x00, 0x17 },
    /* 2  Guitar       */ { 0x13, 0x41, 0x1A, 0x0D, 0xD8, 0xF7, 0x23, 0x13 },
    /* 3  Piano        */ { 0x13, 0x01, 0x99, 0x00, 0xF2, 0xC4, 0x11, 0x23 },
    /* 4  Flute        */ { 0x31, 0x61, 0x0E, 0x07, 0xA8, 0x64, 0x70, 0x27 },
    /* 5  Clarinet     */ { 0x32, 0x21, 0x1E, 0x06, 0xE0, 0x76, 0x00, 0x28 },
    /* 6  Oboe         */ { 0x31, 0x22, 0x16, 0x05, 0xE0, 0x71, 0x00, 0x18 },
    /* 7  Trumpet      */ { 0x21, 0x61, 0x1D, 0x07, 0x82, 0x81, 0x10, 0x07 },
    /* 8  Organ        */ { 0x23, 0x21, 0x2D, 0x14, 0xA2, 0x72, 0x00, 0x07 },
    /* 9  Horn         */ { 0x61, 0x61, 0x1B, 0x06, 0x64, 0x65, 0x10, 0x17 },
    /* 10 Synthesizer  */ { 0x41, 0x61, 0x0B, 0x18, 0x85, 0xF7, 0x71, 0x07 },
    /* 11 Harpsichord  */ { 0x13, 0x01, 0x83, 0x11, 0xFA, 0xE4, 0x10, 0x04 },
    /* 12 Vibraphone   */ { 0x17, 0xC1, 0x24, 0x07, 0xF8, 0xF8, 0x22, 0x12 },
    /* 13 Synth Bass   */ { 0x61, 0x50, 0x0C, 0x05, 0xC2, 0xF5, 0x20, 0x42 },
    /* 14 Acoustic Bass */ { 0x01, 0x01, 0x55, 0x03, 0xC9, 0x95, 0x03, 0x02 },
    /* 15 Elec Guitar  */ { 0x61, 0x41, 0x89, 0x03, 0xF1, 0xE4, 0x40, 0x13 },
};

static const uint8_t OPLL_RHYTHM_PATCHES[5][8] = {
    /* BD (Bass Drum)  */ { 0x01, 0x01, 0x18, 0x0F, 0xDF, 0xF8, 0x6A, 0x6D },
    /* HH (Hi-Hat)     */ { 0x01, 0x01, 0x00, 0x00, 0xC8, 0xD8, 0xA7, 0x48 },
    /* SD (Snare)      */ { 0x01, 0x01, 0x00, 0x00, 0xC8, 0xD8, 0xA7, 0x48 },
    /* TM (Tom-Tom)    */ { 0x05, 0x01, 0x00, 0x00, 0xF8, 0xAA, 0x59, 0x55 },
    /* TC (Top Cymbal) */ { 0x05, 0x01, 0x00, 0x00, 0xF8, 0xAA, 0x59, 0x55 },
};

static const char *OPLL_PATCH_NAMES[16] = {
    "Custom", "Violin", "Guitar", "Piano", "Flute",
    "Clarinet", "Oboe", "Trumpet", "Organ", "Horn",
    "Synthesizer", "Harpsichord", "Vibraphone",
    "Synth Bass", "Acoustic Bass", "Elec Guitar"
};

static const float OPLL_MUL_TABLE[16] = {
    0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
    8.0f, 9.0f, 10.0f, 10.0f, 12.0f, 12.0f, 15.0f, 15.0f
};

/* KSL (Key Scale Level) attenuation in dB, indexed by fnum>>7 (8 entries) */
static const float OPLL_KSL_TABLE[8] = {
    0.0f, 18.0f, 24.0f, 27.75f, 30.0f, 32.25f, 33.75f, 35.25f
};

/* Feedback scaling table matching YM2413 hardware:
 * Index 0 = off, 1..7 = pi/16, pi/8, pi/4, pi/2, pi, 2*pi, 4*pi */
static const float OPLL_FB_TABLE[8] = {
    0.0f,
    (float)M_PI / 16.0f,
    (float)M_PI / 8.0f,
    (float)M_PI / 4.0f,
    (float)M_PI / 2.0f,
    (float)M_PI,
    (float)M_PI * 2.0f,
    (float)M_PI * 4.0f,
};

/* KSL dB/octave table for the extended (yama-bruh-style) path */
static const float OPLL_KSL_DB_PER_OCT[4] = { 0.0f, 1.5f, 3.0f, 6.0f };

/* ── Per-operator envelope state (exponential approach model) ─────────── */

typedef struct {
    int   state;        /* OPLL_ENV_xxx */
    float level;        /* linear amplitude 0..1 */
} OPLLEnvelope;

/* ── Per-channel state ────────────────────────────────────────────────── */

typedef struct {
    float       cp;             /* carrier phase (radians) */
    float       mp;             /* modulator phase (radians) */
    float       prev_mod;       /* modulator feedback memory (raw, pre-envelope) */
    int         key_on;
    int         instrument;     /* 0-15: 0 = custom, 1-15 = ROM patches; negative = rhythm */
    int         fnum;           /* 10-bit F-number */
    int         block;          /* 3-bit octave block */
    int         volume;         /* 4-bit channel volume: 0 = loudest, 15 = silent */
    int         midi_note;      /* currently sounding MIDI note (-1 = none) */
    float       freq;           /* actual frequency in Hz */
    float       cur_freq;       /* portamento current frequency */
    float       velocity;       /* 0..1 */
    float       age;            /* voice age in seconds */
    int         sample_count;   /* samples since note-on (for anti-click fade-in) */
    OPLLEnvelope mod_env;
    OPLLEnvelope car_env;
} OPLLChannel;

/* ── FM drum hit ──────────────────────────────────────────────────────── */

typedef struct {
    float t;            /* elapsed time */
    float vel;          /* velocity 0..1 */
    float cp, mp;       /* carrier / modulator phase */
    float carrier_freq;
    float mod_freq;
    float mod_index;
    float pitch_sweep;
    float pitch_decay;
    float decay;
    float noise_amt;
    float click_amt;
    int   clap_mode;
    int   clap_count;
    float clap_gap;
    int   active;
} OPLLDrumHit;

/* ── Drum bank ────────────────────────────────────────────────────────── */

typedef struct {
    float carrier_freq;  /* 0 = use default */
    float mod_freq;
    float mod_index;
    float pitch_sweep;
    float pitch_decay;
    float decay;
    float noise_amt;
    float click_amt;
    int   has_carrier_freq;
    int   has_mod_freq;
    int   has_mod_index;
    int   has_pitch_sweep;
    int   has_pitch_decay;
    int   has_decay;
    int   has_noise_amt;
    int   has_click_amt;
} OPLLDrumBankMod;

/* ── Main synth state ─────────────────────────────────────────────────── */

typedef struct {
    /* OPLL melodic channels */
    OPLLChannel channels[OPLL_CHANNELS];
    uint8_t     custom_patch[8];    /* user-definable patch 0 */
    int         rhythm_mode;        /* 0 = 9 melody, 1 = 6 melody + 5 rhythm */
    int         current_instrument; /* default ROM patch for new notes */

    /* Shared LFOs (hardware-accurate: one LFO for all voices) */
    float       trem_phase;         /* 3.7 Hz tremolo */
    float       chipvib_phase;      /* 6.4 Hz chip vibrato */

    /* User vibrato (separate from chip vibrato) */
    float       user_vib_phase;
    float       user_vib_rate;      /* Hz, default 5.5 */
    float       user_vib_depth;     /* default 0.004 */
    int         user_vib_on;

    /* 23-bit LFSR noise generator (shared like hardware) */
    uint32_t    noise_lfsr;
    int         noise_out;

    /* Pitch bend (frequency multiplier, 1.0 = no bend) */
    float       pitch_bend;
    int         pitch_bend_range;   /* semitones, default 2 */

    /* Mod wheel */
    float       mod_wheel;          /* 0..1 normalized */
    int         mod_target;         /* 0=vibrato, 1=modIndex, 2=tremolo */

    /* Sustain pedal */
    int         sustain_on;
    float       sustain_mult;       /* release time multiplier when sustained */

    /* Portamento */
    int         porta_on;
    float       porta_time;         /* seconds */
    float       last_freq;

    /* Mono mode */
    int         mono_mode;

    /* FM drum engine */
    OPLLDrumHit drum_hits[OPLL_MAX_DRUM_HITS];
    int         drum_bank;          /* 0..7 */
    uint32_t    drum_noise_seed;    /* XorShift PRNG for drum noise */

    /* Key sequence engine */
    OPLLKeySequence seq;
    int         yb_preset;          /* current yama-bruh extended preset (0-99) */
    int         use_yb_preset;      /* 1 = use yama-bruh preset, 0 = use ROM patch */

    /* Output processing */
    float       limiter_env;
    float       volume;             /* 0.0..1.0 */

    /* Precomputed waveform tables */
    float       sine_table[OPLL_SINE_LEN];
    float       halfsine_table[OPLL_SINE_LEN];
    float       abssine_table[OPLL_SINE_LEN];
    float       quartersine_table[OPLL_SINE_LEN];

    /* Shared step sequencer (DSL-driven) */
    MiniSeq     mini_seq;
    KeySeq      keyseq;
} YM2413State;

/* ── Waveform table init ──────────────────────────────────────────────── */

static void opll_build_tables(YM2413State *s) {
    for (int i = 0; i < OPLL_SINE_LEN; i++) {
        float phase = (float)i / (float)OPLL_SINE_LEN * OPLL_TAU;
        float raw = sinf(phase);

        /* Quantize to ~8-bit amplitude for OPLL lo-fi character */
        float quantized = roundf(raw * 255.0f) / 255.0f;
        s->sine_table[i] = quantized;

        /* Waveform 1: half-sine (rectified positive half) */
        s->halfsine_table[i] = (raw > 0.0f) ? quantized : 0.0f;

        /* Waveform 2: abs-sine (full-wave rectified) */
        s->abssine_table[i] = fabsf(quantized);

        /* Waveform 3: quarter-sine (first quarter only) */
        s->quartersine_table[i] = (i < OPLL_SINE_LEN / 4) ? quantized : 0.0f;
    }
}

/* ── Waveform lookup (4 types + 2 noise variants) ─────────────────────── */

static inline float opll_waveform(const YM2413State *s, float phase, int type, int noise) {
    switch (type) {
    case 0: { /* Sine */
        int idx = (int)(phase / OPLL_TAU * (float)OPLL_SINE_LEN) & (OPLL_SINE_LEN - 1);
        return s->sine_table[idx];
    }
    case 1: { /* Half-sine */
        int idx = (int)(phase / OPLL_TAU * (float)OPLL_SINE_LEN) & (OPLL_SINE_LEN - 1);
        return s->halfsine_table[idx];
    }
    case 2: { /* Abs-sine */
        int idx = (int)(phase / OPLL_TAU * (float)OPLL_SINE_LEN) & (OPLL_SINE_LEN - 1);
        return s->abssine_table[idx];
    }
    case 3: { /* Quarter-sine */
        int idx = (int)(phase / OPLL_TAU * (float)OPLL_SINE_LEN) & (OPLL_SINE_LEN - 1);
        return s->quartersine_table[idx];
    }
    case 4: /* Pure noise (YM2413 percussion LFSR) */
        return noise ? 1.0f : -1.0f;
    case 5: { /* Tone + noise mix */
        int idx = (int)(phase / OPLL_TAU * (float)OPLL_SINE_LEN) & (OPLL_SINE_LEN - 1);
        return (s->sine_table[idx] + (noise ? 1.0f : -1.0f)) * 0.5f;
    }
    default: {
        int idx = (int)(phase / OPLL_TAU * (float)OPLL_SINE_LEN) & (OPLL_SINE_LEN - 1);
        return s->sine_table[idx];
    }
    }
}

/* ── Patch data access ────────────────────────────────────────────────── */

static const uint8_t *opll_get_patch(const YM2413State *s, int instrument) {
    if (instrument <= 0 || instrument > 15)
        return s->custom_patch;
    return OPLL_ROM_PATCHES[instrument - 1];
}

/* Decode patch byte fields */
static inline int opll_patch_mod_am(const uint8_t *p)   { return (p[0] >> 7) & 1; }
static inline int opll_patch_mod_vib(const uint8_t *p)  { return (p[0] >> 6) & 1; }
static inline int opll_patch_mod_eg(const uint8_t *p)   { return (p[0] >> 5) & 1; }
static inline int opll_patch_mod_ksr(const uint8_t *p)  { return (p[0] >> 4) & 1; }
static inline int opll_patch_mod_mul(const uint8_t *p)  { return p[0] & 0x0F; }

static inline int opll_patch_car_am(const uint8_t *p)   { return (p[1] >> 7) & 1; }
static inline int opll_patch_car_vib(const uint8_t *p)  { return (p[1] >> 6) & 1; }
static inline int opll_patch_car_eg(const uint8_t *p)   { return (p[1] >> 5) & 1; }
static inline int opll_patch_car_ksr(const uint8_t *p)  { return (p[1] >> 4) & 1; }
static inline int opll_patch_car_mul(const uint8_t *p)  { return p[1] & 0x0F; }

static inline int opll_patch_mod_ksl(const uint8_t *p)  { return (p[2] >> 6) & 3; }
static inline int opll_patch_mod_tl(const uint8_t *p)   { return p[2] & 0x3F; }

static inline int opll_patch_car_ksl(const uint8_t *p)  { return (p[3] >> 6) & 3; }
static inline int opll_patch_car_wf(const uint8_t *p)   { return (p[3] >> 4) & 1; }
static inline int opll_patch_mod_wf(const uint8_t *p)   { return (p[3] >> 3) & 1; }
static inline int opll_patch_fb(const uint8_t *p)       { return p[3] & 0x07; }

static inline int opll_patch_mod_ar(const uint8_t *p)   { return (p[4] >> 4) & 0x0F; }
static inline int opll_patch_mod_dr(const uint8_t *p)   { return p[4] & 0x0F; }
static inline int opll_patch_car_ar(const uint8_t *p)   { return (p[5] >> 4) & 0x0F; }
static inline int opll_patch_car_dr(const uint8_t *p)   { return p[5] & 0x0F; }

static inline int opll_patch_mod_sl(const uint8_t *p)   { return (p[6] >> 4) & 0x0F; }
static inline int opll_patch_mod_rr(const uint8_t *p)   { return p[6] & 0x0F; }
static inline int opll_patch_car_sl(const uint8_t *p)   { return (p[7] >> 4) & 0x0F; }
static inline int opll_patch_car_rr(const uint8_t *p)   { return p[7] & 0x0F; }

/* ── Envelope processing (exponential approach, matching yama-bruh) ──── */

/*
 * Envelope uses 1-pole exponential approach (same as yama-bruh worklet).
 * This gives smooth, natural-sounding transitions:
 *   output = current + (target - current) * coeff
 *   coeff  = 1 - exp(-5 / (time_seconds * sample_rate))
 */

static inline float opll_env_step(float current, float target,
                                   float time_seconds, float sr)
{
    if (time_seconds <= 0.00001f) return target;
    float samples = time_seconds * sr;
    if (samples < 1.0f) samples = 1.0f;
    float coeff = 1.0f - expf(-5.0f / samples);
    return current + (target - current) * coeff;
}

/*
 * Convert 4-bit ROM rate (0-15) to time in seconds.
 * Uses lookup tables matching YM2413 hardware timing.
 * KSR scales rate by pitch: higher notes = faster envelopes.
 */

/* Attack times (seconds) indexed by 4-bit rate value */
static const float OPLL_ATTACK_TIMES[16] = {
    99.0f, 5.0f, 3.0f, 1.8f, 1.0f, 0.6f, 0.35f, 0.2f,
    0.12f, 0.07f, 0.04f, 0.02f, 0.01f, 0.005f, 0.003f, 0.001f
};

/* Decay/release times (seconds) indexed by 4-bit rate value */
static const float OPLL_DECAY_TIMES[16] = {
    99.0f, 30.0f, 18.0f, 10.0f, 6.0f, 3.5f, 2.0f, 1.0f,
    0.5f, 0.3f, 0.15f, 0.08f, 0.04f, 0.02f, 0.01f, 0.005f
};

static inline float opll_rate_to_attack_time(int rate, int ksr_bit, float freq) {
    if (rate <= 0) return 99.0f;
    if (rate >= 15) return 0.001f;
    float base = OPLL_ATTACK_TIMES[rate];
    if (ksr_bit && freq > 0.0f) {
        float octave = log2f(freq / 440.0f);
        base *= powf(2.0f, -octave);
    }
    return base;
}

static inline float opll_rate_to_decay_time(int rate, int ksr_bit, float freq) {
    if (rate <= 0) return 99.0f;
    if (rate >= 15) return 0.005f;
    float base = OPLL_DECAY_TIMES[rate];
    if (ksr_bit && freq > 0.0f) {
        float octave = log2f(freq / 440.0f);
        base *= powf(2.0f, -octave);
    }
    return base;
}

/* ── Tremolo gain (matching yama-bruh) ────────────────────────────────── */

static inline float opll_tremolo_gain(int enabled, float trem_val,
                                       float mod_wheel, int mod_target)
{
    if (!enabled && mod_target != 2) return 1.0f;
    float depth = OPLL_TREM_DEPTH;
    if (mod_target == 2) depth += mod_wheel * 0.18f;
    return 1.0f - depth * (1.0f + trem_val) * 0.5f;
}

/* ── Vibrato depth (matching yama-bruh) ───────────────────────────────── */

static inline float opll_vibrato_depth(int enabled, float mod_wheel, int mod_target) {
    if (!enabled && mod_target != 0) return 0.0f;
    float depth = OPLL_CHIPVIB_DEPTH;
    if (mod_target == 0) depth += mod_wheel * 0.004f;
    return depth;
}

/* ── KSL attenuation (linear gain, matching yama-bruh) ────────────────── */

static inline float opll_ksl_gain(int ksl, float freq) {
    float db_per_oct = OPLL_KSL_DB_PER_OCT[ksl & 3];
    if (db_per_oct == 0.0f || freq <= 440.0f) return 1.0f;
    float octave = log2f(freq / 440.0f);
    return powf(10.0f, -(db_per_oct * octave) / 20.0f);
}

/* ── Noise LFSR (23-bit, XOR taps at bits 0 and 14) ──────────────────── */

static inline int opll_noise_clock(uint32_t *lfsr) {
    int bit = ((*lfsr >> 0) ^ (*lfsr >> 14)) & 1;
    *lfsr = ((*lfsr >> 1) | (bit << 22)) & 0x7FFFFF;
    return *lfsr & 1;
}

/* ── Drum noise (XorShift PRNG for FM percussion) ─────────────────────── */

static inline float opll_drum_noise(uint32_t *seed) {
    uint32_t s = *seed;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    if (s == 0) s = 1;
    *seed = s;
    return (float)(s & 0x7FFFFFFF) / (float)0x7FFFFFFF * 2.0f - 1.0f;
}

/* ── Per-instance limiter ─────────────────────────────────────────────── */

static inline float opll_limiter(float *env, float sample) {
    float absval = fabsf(sample);

    if (absval > *env)
        *env += (absval - *env) * 0.01f;
    else
        *env += (absval - *env) * 0.0001f;

    /* Soft-knee limiter (matching yama-bruh) */
    if (absval > 0.5f) {
        float over = absval - 0.5f;
        float gain = 0.5f + over / (1.0f + over * 2.0f);
        sample = (sample < 0.0f) ? -gain : gain;
    }

    if (sample > OPLL_LIMITER_CEIL)       sample = OPLL_LIMITER_CEIL;
    else if (sample < -OPLL_LIMITER_CEIL) sample = -OPLL_LIMITER_CEIL;

    return sample;
}

/* ── F-number / block from MIDI note ──────────────────────────────────── */

static void opll_midi_note_to_fnum(int note, int sample_rate,
                                    int *out_fnum, int *out_block)
{
    float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
    float fmaster = (float)sample_rate;

    int best_block = 0;
    int best_fnum = 0;

    for (int b = 0; b < 8; b++) {
        float fn = freq * (float)(1 << (20 - b)) / fmaster;
        int fnum = (int)(fn + 0.5f);

        if (fnum >= 0 && fnum <= 1023) {
            if (fnum >= 256 || b == 7) {
                best_block = b;
                best_fnum = fnum;
                break;
            }
            best_block = b;
            best_fnum = fnum;
        }
    }

    if (best_fnum > 1023) best_fnum = 1023;
    if (best_fnum < 0) best_fnum = 0;

    *out_fnum = best_fnum;
    *out_block = best_block;
}

static inline float opll_midi_to_freq(int note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  FM DRUM ENGINE
 *  Full FM percussion synthesis matching yama-bruh's drum-worklet.js
 * ══════════════════════════════════════════════════════════════════════════ */

/* Drum sound types */
enum {
    DRUM_KICK = 0, DRUM_SNARE, DRUM_HIHAT_C, DRUM_HIHAT_O,
    DRUM_CLAP, DRUM_TOM, DRUM_RIMSHOT, DRUM_COWBELL, DRUM_CYMBAL,
    /* Extended / SFX */
    DRUM_ZAP, DRUM_RISER, DRUM_GLITCH, DRUM_BOMB, DRUM_SCRATCH,
    DRUM_CHIRP, DRUM_METALLIC, DRUM_NOISE_BURST, DRUM_BLIP,
    DRUM_WHOOSH, DRUM_THUD, DRUM_SHAKER, DRUM_FM_POP,
    DRUM_COUNT
};

static const char *OPLL_DRUM_NAMES[DRUM_COUNT] = {
    "kick", "snare", "hihat_c", "hihat_o",
    "clap", "tom", "rimshot", "cowbell", "cymbal",
    "zap", "riser", "glitch", "bomb", "scratch",
    "chirp", "metallic", "noise_burst", "blip",
    "whoosh", "thud", "shaker", "fm_pop"
};

static const char *OPLL_DRUM_BANK_NAMES[OPLL_NUM_DRUM_BANKS] = {
    "Standard", "Electronic", "Power", "Brush",
    "Orchestra", "Synth", "Latin", "Lo-Fi"
};

static void opll_make_drum(OPLLDrumHit *h, int type, float vel, int midi_note,
                            int bank_idx, uint32_t *noise_seed)
{
    memset(h, 0, sizeof(*h));
    h->vel = vel;
    h->active = 1;
    h->clap_count = 3;
    h->clap_gap = 0.012f;
    h->pitch_decay = 0.01f;

    float freq = (midi_note > 0) ? opll_midi_to_freq(midi_note) : 0.0f;

    switch (type) {
    case DRUM_KICK:
        h->carrier_freq = 60; h->mod_freq = 90; h->mod_index = 3.0f;
        h->pitch_sweep = 160; h->pitch_decay = 0.015f;
        h->decay = 0.25f; h->click_amt = 0.3f;
        break;
    case DRUM_SNARE:
        h->carrier_freq = 200; h->mod_freq = 340; h->mod_index = 2.5f;
        h->pitch_sweep = 60; h->pitch_decay = 0.01f;
        h->decay = 0.18f; h->noise_amt = 0.6f; h->click_amt = 0.15f;
        break;
    case DRUM_HIHAT_C:
        h->carrier_freq = 800; h->mod_freq = 5600; h->mod_index = 4.0f;
        h->decay = 0.04f; h->noise_amt = 0.5f;
        break;
    case DRUM_HIHAT_O:
        h->carrier_freq = 800; h->mod_freq = 5600; h->mod_index = 4.0f;
        h->decay = 0.22f; h->noise_amt = 0.5f;
        break;
    case DRUM_CLAP:
        h->carrier_freq = 1200; h->mod_freq = 2400; h->mod_index = 1.5f;
        h->decay = 0.2f; h->noise_amt = 0.85f; h->clap_mode = 1;
        break;
    case DRUM_TOM:
        if (freq <= 0.0f) freq = 165.0f;
        h->carrier_freq = freq; h->mod_freq = freq * 1.5f; h->mod_index = 2.0f;
        h->pitch_sweep = freq * 0.5f; h->pitch_decay = 0.02f;
        h->decay = 0.22f; h->click_amt = 0.1f;
        break;
    case DRUM_RIMSHOT:
        h->carrier_freq = 500; h->mod_freq = 1600; h->mod_index = 2.0f;
        h->pitch_sweep = 200; h->pitch_decay = 0.005f;
        h->decay = 0.06f; h->noise_amt = 0.2f; h->click_amt = 0.5f;
        break;
    case DRUM_COWBELL:
        h->carrier_freq = 587; h->mod_freq = 829; h->mod_index = 1.8f;
        h->decay = 0.12f; h->click_amt = 0.1f;
        break;
    case DRUM_CYMBAL:
        h->carrier_freq = 940; h->mod_freq = 6580; h->mod_index = 5.0f;
        h->decay = 0.8f; h->noise_amt = 0.4f;
        break;
    case DRUM_ZAP:
        if (freq <= 0.0f) freq = 2000.0f; else freq *= 2.0f;
        h->carrier_freq = freq * 0.1f; h->mod_freq = freq * 0.3f; h->mod_index = 8.0f;
        h->pitch_sweep = freq; h->pitch_decay = 0.008f;
        h->decay = 0.15f; h->click_amt = 0.6f;
        break;
    case DRUM_RISER:
        if (freq <= 0.0f) freq = 400.0f;
        h->carrier_freq = freq * 0.25f; h->mod_freq = freq * 0.5f; h->mod_index = 4.0f;
        h->pitch_sweep = -freq; h->pitch_decay = 0.3f;
        h->decay = 0.6f; h->noise_amt = 0.15f;
        break;
    case DRUM_GLITCH:
        if (freq <= 0.0f) freq = 800.0f;
        h->carrier_freq = freq; h->mod_freq = freq * 7.13f; h->mod_index = 12.0f;
        h->pitch_sweep = freq * 2.0f; h->pitch_decay = 0.003f;
        h->decay = 0.04f; h->click_amt = 0.8f;
        break;
    case DRUM_BOMB:
        h->carrier_freq = 30; h->mod_freq = 45; h->mod_index = 6.0f;
        h->pitch_sweep = 200; h->pitch_decay = 0.05f;
        h->decay = 0.8f; h->noise_amt = 0.5f; h->click_amt = 0.4f;
        break;
    case DRUM_SCRATCH:
        h->carrier_freq = 600; h->mod_freq = 100; h->mod_index = 15.0f;
        h->pitch_sweep = 400; h->pitch_decay = 0.02f;
        h->decay = 0.1f; h->noise_amt = 0.3f; h->click_amt = 0.5f;
        break;
    case DRUM_CHIRP:
        if (freq <= 0.0f) freq = 1200.0f;
        h->carrier_freq = freq * 0.5f; h->mod_freq = freq; h->mod_index = 3.0f;
        h->pitch_sweep = -freq * 0.8f; h->pitch_decay = 0.015f;
        h->decay = 0.08f; h->click_amt = 0.3f;
        break;
    case DRUM_METALLIC:
        if (freq <= 0.0f) freq = 500.0f;
        h->carrier_freq = freq; h->mod_freq = freq * 1.41f; h->mod_index = 5.0f;
        h->decay = 0.5f; h->click_amt = 0.15f;
        break;
    case DRUM_NOISE_BURST:
        h->carrier_freq = 1000; h->mod_freq = 3000; h->mod_index = 2.0f;
        h->decay = 0.12f; h->noise_amt = 0.95f; h->click_amt = 0.2f;
        break;
    case DRUM_BLIP:
        if (freq <= 0.0f) freq = 880.0f;
        h->carrier_freq = freq; h->mod_freq = freq * 2.0f; h->mod_index = 0.5f;
        h->decay = 0.03f; h->click_amt = 0.4f;
        break;
    case DRUM_WHOOSH:
        h->carrier_freq = 200; h->mod_freq = 1500; h->mod_index = 8.0f;
        h->pitch_sweep = -800; h->pitch_decay = 0.15f;
        h->decay = 0.4f; h->noise_amt = 0.6f;
        break;
    case DRUM_THUD:
        if (freq <= 0.0f) freq = 70.0f; else freq = fmaxf(30.0f, freq * 0.33f);
        h->carrier_freq = freq; h->mod_freq = freq * 1.2f; h->mod_index = 1.1f;
        h->pitch_sweep = freq * 0.8f; h->pitch_decay = 0.03f;
        h->decay = 0.28f; h->click_amt = 0.18f;
        break;
    case DRUM_SHAKER:
        h->carrier_freq = 1800; h->mod_freq = 4200; h->mod_index = 2.4f;
        h->decay = 0.16f; h->noise_amt = 0.92f; h->click_amt = 0.05f;
        break;
    case DRUM_FM_POP:
        if (freq <= 0.0f) freq = 520.0f;
        h->carrier_freq = freq; h->mod_freq = freq * 2.8f; h->mod_index = 5.5f;
        h->pitch_sweep = freq * 1.1f; h->pitch_decay = 0.012f;
        h->decay = 0.09f; h->click_amt = 0.45f;
        break;
    default:
        h->active = 0;
        return;
    }

    /* Apply drum bank overrides */
    (void)bank_idx; /* Bank mods applied below */
    (void)noise_seed;

    /* Bank parameter overrides — matching yama-bruh's DRUM_BANKS */
    if (bank_idx >= 0 && bank_idx < OPLL_NUM_DRUM_BANKS) {
        /* Bank-specific overrides for core sounds (types 0..8) */
        switch (bank_idx) {
        case 1: /* Electronic — 808/TR style */
            switch (type) {
            case DRUM_KICK:     h->decay = 0.4f; h->pitch_sweep = 220; h->mod_index = 1.5f; h->click_amt = 0.1f; break;
            case DRUM_SNARE:    h->noise_amt = 0.75f; h->decay = 0.22f; h->mod_index = 1.5f; h->click_amt = 0.05f; break;
            case DRUM_HIHAT_C:  h->decay = 0.025f; h->carrier_freq = 1200; h->mod_freq = 7800; h->mod_index = 5.0f; break;
            case DRUM_HIHAT_O:  h->decay = 0.35f; h->carrier_freq = 1200; h->mod_freq = 7800; h->mod_index = 5.0f; break;
            case DRUM_TOM:      h->mod_index = 1.0f; h->pitch_sweep = 40; h->decay = 0.35f; h->noise_amt = 0; break;
            case DRUM_CLAP:     h->decay = 0.25f; h->noise_amt = 0.9f; break;
            default: break;
            }
            break;
        case 2: /* Power — big rock kit */
            switch (type) {
            case DRUM_KICK:     h->decay = 0.35f; h->pitch_sweep = 200; h->mod_index = 4.0f; h->click_amt = 0.5f; break;
            case DRUM_SNARE:    h->decay = 0.25f; h->mod_index = 3.5f; h->click_amt = 0.3f; h->noise_amt = 0.5f; break;
            case DRUM_TOM:      h->decay = 0.3f; h->mod_index = 3.0f; h->pitch_sweep = 100; h->click_amt = 0.2f; break;
            case DRUM_CYMBAL:   h->decay = 1.2f; break;
            default: break;
            }
            break;
        case 3: /* Brush — jazz/light */
            switch (type) {
            case DRUM_KICK:     h->decay = 0.15f; h->pitch_sweep = 80; h->mod_index = 1.5f; h->click_amt = 0.1f; break;
            case DRUM_SNARE:    h->noise_amt = 0.85f; h->decay = 0.3f; h->mod_index = 0.8f; h->click_amt = 0.0f; break;
            case DRUM_HIHAT_C:  h->decay = 0.06f; h->noise_amt = 0.7f; h->mod_index = 2.5f; break;
            case DRUM_HIHAT_O:  h->decay = 0.3f; h->noise_amt = 0.7f; h->mod_index = 2.5f; break;
            case DRUM_TOM:      h->mod_index = 1.2f; h->decay = 0.2f; h->noise_amt = 0.1f; break;
            case DRUM_RIMSHOT:  h->noise_amt = 0.4f; h->click_amt = 0.3f; break;
            default: break;
            }
            break;
        case 4: /* Orchestra — timpani, concert perc */
            switch (type) {
            case DRUM_KICK:     h->carrier_freq = 50; h->decay = 0.5f; h->pitch_sweep = 30; h->mod_index = 1.0f; h->click_amt = 0.05f; break;
            case DRUM_SNARE:    h->carrier_freq = 280; h->decay = 0.15f; h->mod_index = 1.8f; h->noise_amt = 0.3f; h->click_amt = 0.2f; break;
            case DRUM_TOM:      h->mod_index = 1.0f; h->pitch_sweep = 20; h->decay = 0.45f; h->click_amt = 0.05f; break;
            case DRUM_CYMBAL:   h->decay = 1.5f; h->carrier_freq = 700; h->mod_index = 6.0f; break;
            default: break;
            }
            break;
        case 5: /* Synth — digital, punchy */
            switch (type) {
            case DRUM_KICK:     h->carrier_freq = 55; h->decay = 0.18f; h->pitch_sweep = 300; h->pitch_decay = 0.008f; h->mod_index = 5.0f; h->click_amt = 0.6f; break;
            case DRUM_SNARE:    h->carrier_freq = 250; h->mod_index = 4.0f; h->noise_amt = 0.4f; h->decay = 0.12f; h->click_amt = 0.4f; break;
            case DRUM_HIHAT_C:  h->decay = 0.02f; h->carrier_freq = 1500; h->mod_freq = 9000; h->mod_index = 6.0f; break;
            case DRUM_HIHAT_O:  h->decay = 0.15f; h->carrier_freq = 1500; h->mod_freq = 9000; h->mod_index = 6.0f; break;
            case DRUM_TOM:      h->mod_index = 3.5f; h->pitch_sweep = 120; h->decay = 0.15f; h->click_amt = 0.3f; break;
            case DRUM_CLAP:     h->decay = 0.12f; h->noise_amt = 0.95f; h->clap_gap = 0.008f; break;
            case DRUM_COWBELL:  h->carrier_freq = 700; h->mod_freq = 1000; h->mod_index = 2.5f; break;
            default: break;
            }
            break;
        case 6: /* Latin — conga, bongo, timbale */
            switch (type) {
            case DRUM_KICK:     h->carrier_freq = 80; h->decay = 0.2f; h->pitch_sweep = 50; h->mod_index = 1.5f; h->click_amt = 0.15f; break;
            case DRUM_SNARE:    h->carrier_freq = 300; h->decay = 0.1f; h->mod_index = 1.5f; h->noise_amt = 0.2f; h->click_amt = 0.35f; break;
            case DRUM_TOM:      h->mod_index = 1.5f; h->pitch_sweep = 30; h->pitch_decay = 0.01f; h->decay = 0.15f; h->click_amt = 0.25f; break;
            case DRUM_RIMSHOT:  h->decay = 0.04f; h->click_amt = 0.7f; h->noise_amt = 0.1f; break;
            case DRUM_COWBELL:  h->decay = 0.08f; break;
            default: break;
            }
            break;
        case 7: /* Lo-Fi — gritty, bit-crushed */
            switch (type) {
            case DRUM_KICK:     h->decay = 0.2f; h->mod_index = 6.0f; h->click_amt = 0.15f; break;
            case DRUM_SNARE:    h->mod_index = 5.0f; h->noise_amt = 0.7f; h->decay = 0.15f; break;
            case DRUM_HIHAT_C:  h->mod_index = 7.0f; h->decay = 0.03f; break;
            case DRUM_HIHAT_O:  h->mod_index = 7.0f; h->decay = 0.18f; break;
            case DRUM_TOM:      h->mod_index = 4.0f; h->decay = 0.2f; break;
            default: break;
            }
            break;
        default: break;
        }
    }
}

/* Render a single drum hit, return sample value */
static inline float opll_drum_render_hit(OPLLDrumHit *h, float dt,
                                          uint32_t *noise_seed)
{
    /* Exponential decay envelope */
    float decay_tc = h->decay * 0.4f;
    if (decay_tc < 0.0001f) decay_tc = 0.0001f;
    float env = expf(-h->t / decay_tc) * h->vel;

    if (env < 0.001f) {
        h->active = 0;
        return 0.0f;
    }

    /* Pitch sweep */
    float pd = h->pitch_decay;
    if (pd < 0.001f) pd = 0.001f;
    float sweep = h->pitch_sweep * expf(-h->t / pd);
    float c_freq = h->carrier_freq + sweep;
    float m_freq = h->mod_freq + sweep * 0.5f;

    /* FM synthesis */
    float mod = sinf(h->mp) * h->mod_index;
    float carrier = sinf(h->cp + mod);

    /* Noise component */
    float noise_val = 0.0f;
    if (h->noise_amt > 0.0f) {
        noise_val = opll_drum_noise(noise_seed) * h->noise_amt * env;
    }

    /* Click transient (first ~2ms) */
    float click = 0.0f;
    if (h->click_amt > 0.0f && h->t < 0.002f) {
        click = (1.0f - h->t / 0.002f) * h->click_amt * h->vel;
    }

    /* Clap mode: re-trigger envelope */
    float clap_env = 1.0f;
    if (h->clap_mode) {
        float gap = h->clap_gap;
        if (h->t < gap * (float)h->clap_count) {
            int clap_idx = (int)(h->t / gap);
            float clap_t = h->t - (float)clap_idx * gap;
            clap_env = expf(-clap_t / 0.008f);
        }
    }

    float sample = (carrier * env * (1.0f - h->noise_amt) + noise_val + click) * clap_env * 0.5f;

    /* Advance phases */
    h->cp += OPLL_TAU * c_freq * dt;
    h->mp += OPLL_TAU * m_freq * dt;
    if (h->cp > OPLL_TAU) h->cp -= OPLL_TAU;
    if (h->mp > OPLL_TAU) h->mp -= OPLL_TAU;
    h->t += dt;

    return sample;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  INSTRUMENT TYPE INTERFACE
 * ══════════════════════════════════════════════════════════════════════════ */

static void ym2413_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2);
static void ym2413_osc_handle(void *state, const char *sub_path,
                               const int32_t *iargs, int ni,
                               const float *fargs, int nf);

static void ym2413_param_fn(void *state, const char *name, float value) {
    char path[64];
    snprintf(path, sizeof(path), "/%s", name);  /* ym2413 uses /instrument, /volume etc. */
    float fargs[1] = {value};
    ym2413_osc_handle(state, path, NULL, 0, fargs, 1);
}

static void ym2413_init(void *state) {
    YM2413State *s = (YM2413State *)state;
    memset(s, 0, sizeof(*s));

    s->volume = 1.0f;
    s->limiter_env = 0.0f;
    s->current_instrument = 1; /* Default to Violin */
    s->noise_lfsr = 1;
    s->drum_noise_seed = 1;
    s->pitch_bend = 1.0f;
    s->pitch_bend_range = 2;
    s->mod_target = 0;  /* vibrato */
    s->sustain_mult = 3.0f;
    s->porta_time = OPLL_PORTA_TIME;
    s->user_vib_rate = 5.5f;
    s->user_vib_depth = 0.004f;
    s->yb_preset = 0;
    s->use_yb_preset = 0;
    s->seq.bpm = OPLL_SEQ_BPM_DEFAULT;
    s->seq.playing = 0;
    seq_init(&s->mini_seq);
    seq_bind(&s->mini_seq, state, ym2413_midi, 0);
    keyseq_init(&s->keyseq);
    keyseq_bind(&s->keyseq, state, ym2413_midi, ym2413_param_fn, 0);

    for (int i = 0; i < OPLL_CHANNELS; i++) {
        OPLLChannel *ch = &s->channels[i];
        ch->instrument = 1;
        ch->volume = 0;
        ch->midi_note = -1;
        ch->mod_env.state = OPLL_ENV_OFF;
        ch->mod_env.level = OPLL_ENV_FLOOR;
        ch->car_env.state = OPLL_ENV_OFF;
        ch->car_env.level = OPLL_ENV_FLOOR;
    }

    memcpy(s->custom_patch, OPLL_ROM_PATCHES[0], 8);
    opll_build_tables(s);
}

static void ym2413_destroy(void *state) {
    (void)state;
}

/* ── Note management ──────────────────────────────────────────────────── */

static void opll_note_on(YM2413State *s, int note, int vel, int sample_rate) {
    float freq = opll_midi_to_freq(note);
    float velocity = (float)vel / 127.0f;

    /* Mono mode: release all sounding notes */
    if (s->mono_mode) {
        for (int i = 0; i < OPLL_CHANNELS; i++) {
            OPLLChannel *ch = &s->channels[i];
            if (s->rhythm_mode && i >= 6) continue;
            if (ch->key_on) {
                ch->key_on = 0;
                ch->car_env.state = OPLL_ENV_RELEASE;
                ch->mod_env.state = OPLL_ENV_RELEASE;
            }
        }
    }

    /* Find a free channel (or steal the quietest) */
    int slot = -1;
    int steal = -1;
    float quietest = 2.0f;

    for (int i = 0; i < OPLL_CHANNELS; i++) {
        OPLLChannel *ch = &s->channels[i];
        if (s->rhythm_mode && i >= 6) continue;

        /* Kill duplicate note */
        if (ch->key_on && ch->midi_note == note) {
            slot = i;
            break;
        }

        /* Free channel */
        if (!ch->key_on && ch->car_env.state == OPLL_ENV_OFF) {
            if (slot < 0) slot = i;
            continue;
        }

        /* Track best steal candidate (quietest carrier envelope) */
        if (ch->car_env.level < quietest) {
            quietest = ch->car_env.level;
            steal = i;
        }
    }

    if (slot < 0) slot = steal;
    if (slot < 0) slot = 0;

    OPLLChannel *ch = &s->channels[slot];

    opll_midi_note_to_fnum(note, sample_rate, &ch->fnum, &ch->block);

    ch->instrument = s->current_instrument;
    ch->key_on = 1;
    ch->midi_note = note;
    ch->freq = freq;
    ch->velocity = velocity;
    ch->age = 0.0f;

    /* Portamento: start from last frequency */
    if (s->porta_on && s->last_freq > 0.0f) {
        ch->cur_freq = s->last_freq;
    } else {
        ch->cur_freq = freq;
    }
    s->last_freq = freq;

    /* Reset phase and anti-click counter */
    ch->cp = 0.0f;
    ch->mp = 0.0f;
    ch->prev_mod = 0.0f;
    ch->sample_count = 0;

    /* Velocity to 4-bit volume (for legacy register-level compat) */
    ch->volume = 15 - (vel * 15 / 127);
    if (ch->volume < 0) ch->volume = 0;
    if (ch->volume > 15) ch->volume = 15;

    /* Trigger envelopes */
    ch->car_env.state = OPLL_ENV_ATTACK;
    ch->car_env.level = OPLL_ENV_FLOOR;
    ch->mod_env.state = OPLL_ENV_ATTACK;
    ch->mod_env.level = OPLL_ENV_FLOOR;
}

static void opll_note_off(YM2413State *s, int note) {
    for (int i = 0; i < OPLL_CHANNELS; i++) {
        OPLLChannel *ch = &s->channels[i];
        if (ch->key_on && ch->midi_note == note) {
            if (s->sustain_on) {
                /* Sustain pedal held: don't release yet, mark for deferred release */
                continue;
            }
            ch->key_on = 0;
            if (ch->car_env.state < OPLL_ENV_RELEASE) {
                ch->car_env.state = OPLL_ENV_RELEASE;
            }
            if (ch->mod_env.state < OPLL_ENV_RELEASE) {
                ch->mod_env.state = OPLL_ENV_RELEASE;
            }
        }
    }
}

/* Release all sustained notes (called when sustain pedal lifts) */
static void opll_sustain_off(YM2413State *s) {
    for (int i = 0; i < OPLL_CHANNELS; i++) {
        OPLLChannel *ch = &s->channels[i];
        /* Notes that are still key_on but should be releasing */
        if (ch->key_on && ch->car_env.state == OPLL_ENV_SUSTAIN) {
            /* Check if the physical key is actually released (not re-held).
             * We don't track this separately, so just let them sustain
             * until natural decay or next pedal event. */
        }
    }
}

/* ── FM Drum trigger via MIDI ─────────────────────────────────────────── */

static int opll_gm_drum_map(int note) {
    /* GM drum map to our drum types */
    switch (note) {
    case 35: case 36: return DRUM_KICK;
    case 37:          return DRUM_RIMSHOT;
    case 38: case 40: return DRUM_SNARE;
    case 39:          return DRUM_CLAP;
    case 41: case 43: case 45: case 47: case 48: case 50: return DRUM_TOM;
    case 42: case 44: return DRUM_HIHAT_C;
    case 46:          return DRUM_HIHAT_O;
    case 49: case 57: return DRUM_CYMBAL;
    case 51: case 59: return DRUM_CYMBAL;  /* ride */
    case 52:          return DRUM_CYMBAL;   /* chinese */
    case 53:          return DRUM_COWBELL;  /* ride bell */
    case 54:          return DRUM_SHAKER;   /* tambourine */
    case 55:          return DRUM_CYMBAL;   /* splash */
    case 56:          return DRUM_COWBELL;
    case 58:          return DRUM_SHAKER;   /* vibraslap → shaker */
    case 60: case 61: return DRUM_TOM;     /* hi bongo */
    case 62: case 63: return DRUM_TOM;     /* lo conga */
    case 64: case 65: return DRUM_TOM;     /* timbales */
    case 66: case 67: return DRUM_COWBELL;  /* agogo */
    case 68: case 69: return DRUM_SHAKER;   /* cabasa/maracas */
    case 70: case 71: return DRUM_SHAKER;   /* short/long whistle → shaker */
    case 72: case 73: return DRUM_SHAKER;   /* guiro */
    case 74:          return DRUM_SHAKER;   /* claves */
    case 75: case 76: case 77: return DRUM_BLIP; /* woodblocks */
    case 78: case 79: return DRUM_CHIRP;    /* cuica */
    case 80: case 81: return DRUM_BLIP;    /* triangle */
    default:          return DRUM_BLIP;
    }
}

static void opll_drum_trigger(YM2413State *s, int drum_type, float vel,
                                int midi_note)
{
    /* Find free slot */
    int slot = -1;
    float oldest_t = 0.0f;
    int oldest = 0;

    for (int i = 0; i < OPLL_MAX_DRUM_HITS; i++) {
        if (!s->drum_hits[i].active) {
            slot = i;
            break;
        }
        if (s->drum_hits[i].t > oldest_t) {
            oldest_t = s->drum_hits[i].t;
            oldest = i;
        }
    }
    if (slot < 0) slot = oldest;

    opll_make_drum(&s->drum_hits[slot], drum_type, vel, midi_note,
                    s->drum_bank, &s->drum_noise_seed);
}

/* ── MIDI handler ─────────────────────────────────────────────────────── */

static void ym2413_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2) {
    YM2413State *s = (YM2413State *)state;
    uint8_t type = status & 0xF0;

    if (!s->keyseq.firing && s->keyseq.enabled && s->keyseq.num_steps > 0) {
        if (type == 0x90 && d2 > 0) {
            if (keyseq_note_on(&s->keyseq, d1, d2)) return;
        } else if (type == 0x80 || (type == 0x90 && d2 == 0)) {
            if (keyseq_note_off(&s->keyseq, d1)) return;
        }
    }

    switch (type) {
    case 0x90: /* Note On */
        if (d2 > 0) {
            if (s->rhythm_mode && d1 >= 35 && d1 <= 81) {
                int drum_type = opll_gm_drum_map(d1);
                opll_drum_trigger(s, drum_type, (float)d2 / 127.0f, d1);
            } else {
                opll_note_on(s, d1, d2, 48000);
            }
        } else {
            opll_note_off(s, d1);
        }
        break;

    case 0x80: /* Note Off */
        opll_note_off(s, d1);
        break;

    case 0xC0: /* Program Change: select ROM instrument 0-14 → patches 1-15 */
        if (d1 < 16) {
            s->current_instrument = d1 + 1;
            if (s->current_instrument > 15) s->current_instrument = 15;
            fprintf(stderr, "[ym2413] program %d: %s\n",
                    s->current_instrument,
                    OPLL_PATCH_NAMES[s->current_instrument]);
        }
        break;

    case 0xE0: { /* Pitch Bend (14-bit) */
        int bend_val = ((int)d2 << 7) | (int)d1; /* 0..16383, 8192 = center */
        float centered = (float)(bend_val - 8192) / 8192.0f;
        s->pitch_bend = powf(2.0f, centered * (float)s->pitch_bend_range / 12.0f);
        break;
    }

    case 0xB0: /* CC */
        switch (d1) {
        case 1: /* Mod wheel */
            s->mod_wheel = (float)d2 / 127.0f;
            break;
        case 5: /* Portamento time */
            s->porta_time = (float)d2 / 127.0f * 0.5f;
            break;
        case 7: /* Channel volume */
            s->volume = (float)d2 / 127.0f;
            break;
        case 64: /* Sustain pedal */
            if (d2 >= 64) {
                s->sustain_on = 1;
            } else {
                s->sustain_on = 0;
                opll_sustain_off(s);
            }
            break;
        case 65: /* Portamento on/off */
            s->porta_on = (d2 >= 64) ? 1 : 0;
            break;
        case 74: /* Brightness: adjust feedback of custom patch */
            {
                int fb = d2 * 7 / 127;
                s->custom_patch[3] = (s->custom_patch[3] & 0xF8) | (fb & 0x07);
            }
            break;
        case 80: /* Toggle rhythm mode */
            s->rhythm_mode = (d2 >= 64) ? 1 : 0;
            fprintf(stderr, "[ym2413] rhythm mode: %s\n",
                    s->rhythm_mode ? "ON" : "OFF");
            if (s->rhythm_mode) {
                for (int i = 6; i < 9; i++) {
                    s->channels[i].key_on = 0;
                    s->channels[i].car_env.state = OPLL_ENV_RELEASE;
                    s->channels[i].mod_env.state = OPLL_ENV_RELEASE;
                }
            }
            break;
        case 81: /* Drum bank select */
            if (d2 < OPLL_NUM_DRUM_BANKS) {
                s->drum_bank = d2;
                fprintf(stderr, "[ym2413] drum bank %d: %s\n",
                        d2, OPLL_DRUM_BANK_NAMES[d2]);
            }
            break;
        case 82: /* Mono mode toggle */
            s->mono_mode = (d2 >= 64) ? 1 : 0;
            break;
        case 83: /* Mod target: 0=vibrato, 1=modIndex, 2=tremolo */
            s->mod_target = d2 % 3;
            break;
        case 84: /* User vibrato on/off */
            s->user_vib_on = (d2 >= 64) ? 1 : 0;
            break;
        case 120: /* All Sound Off */
        case 123: /* All Notes Off */
            for (int i = 0; i < OPLL_CHANNELS; i++) {
                s->channels[i].key_on = 0;
                s->channels[i].car_env.state = OPLL_ENV_OFF;
                s->channels[i].car_env.level = OPLL_ENV_FLOOR;
                s->channels[i].mod_env.state = OPLL_ENV_OFF;
                s->channels[i].mod_env.level = OPLL_ENV_FLOOR;
            }
            for (int i = 0; i < OPLL_MAX_DRUM_HITS; i++) {
                s->drum_hits[i].active = 0;
            }
            break;
        case 121: /* Reset All Controllers */
            s->pitch_bend = 1.0f;
            s->mod_wheel = 0.0f;
            s->sustain_on = 0;
            s->porta_on = 0;
            break;
        }
        break;
    }
}

/* ── Key sequence tick (called once per render block) ──────────────────── */

static void opll_seq_tick(YM2413State *s, float dt, int sample_rate) {
    OPLLKeySequence *seq = &s->seq;
    if (!seq->playing || seq->num_notes == 0) return;

    float beat_duration = 60.0f / seq->bpm;

    seq->note_time += dt;

    /* Check if current note duration has elapsed */
    if (seq->current_note < seq->num_notes) {
        float note_dur = seq->notes[seq->current_note].duration_beats * beat_duration;

        if (seq->note_time >= note_dur) {
            /* Release current note */
            opll_note_off(s, seq->notes[seq->current_note].midi_note);
            seq->current_note++;
            seq->note_time = 0.0f;

            /* Trigger next note */
            if (seq->current_note < seq->num_notes) {
                opll_note_on(s, seq->notes[seq->current_note].midi_note, 100, sample_rate);
            } else if (seq->loop) {
                /* Loop: restart */
                seq->current_note = 0;
                seq->note_time = 0.0f;
                opll_note_on(s, seq->notes[0].midi_note, 100, sample_rate);
            } else {
                seq->playing = 0;
            }
        }
    }
}

/* Start key sequence playback from a seed string */
static void opll_seq_play(YM2413State *s, const char *seed_str,
                            int preset_idx, float bpm, int loop,
                            int sample_rate)
{
    OPLLKeySequence *seq = &s->seq;

    /* Stop any current sequence */
    if (seq->playing && seq->current_note < seq->num_notes) {
        opll_note_off(s, seq->notes[seq->current_note].midi_note);
    }

    uint32_t seed = opll_djb2_hash(seed_str);
    opll_generate_sequence(seq, seed);

    seq->bpm = (bpm > 0.0f) ? bpm : OPLL_SEQ_BPM_DEFAULT;
    seq->preset_idx = (preset_idx >= 0 && preset_idx < YB_NUM_PRESETS) ? preset_idx : 88;
    seq->loop = loop;
    seq->playing = 1;

    /* Set the yama-bruh preset for the channels */
    s->yb_preset = seq->preset_idx;
    s->use_yb_preset = 1;

    /* Trigger first note */
    if (seq->num_notes > 0) {
        opll_note_on(s, seq->notes[0].midi_note, 100, sample_rate);
    }

    fprintf(stderr, "[ym2413] seq play: seed='%s' hash=%u notes=%d preset=%d bpm=%.0f\n",
            seed_str, seed, seq->num_notes, seq->preset_idx, seq->bpm);
}

static void opll_seq_stop(YM2413State *s) {
    OPLLKeySequence *seq = &s->seq;
    if (seq->playing && seq->current_note < seq->num_notes) {
        opll_note_off(s, seq->notes[seq->current_note].midi_note);
    }
    seq->playing = 0;
}

/* ── Render ────────────────────────────────────────────────────────────── */

static void ym2413_render(void *state, float *stereo_buf, int frames,
                           int sample_rate)
{
    YM2413State *s = (YM2413State *)state;
    const float dt = 1.0f / (float)sample_rate;
    const float sr = (float)sample_rate;

    /* Portamento smoothing coefficient */
    const float porta_coeff = (s->porta_time > 0.001f)
        ? expf(-dt / s->porta_time) : 0.0f;

    /* Tick key sequences (once per sample for timing) */
    for (int si = 0; si < frames; si++) {
        opll_seq_tick(s, dt, sample_rate);
        seq_tick(&s->mini_seq, dt);
        keyseq_tick(&s->keyseq, dt);
    }

    /*
     * When using yama-bruh extended presets, the channel render path
     * reads from YB_PRESETS[yb_preset] for tremolo depth, chip vibrato,
     * KSR, KSL, feedback, waveforms, and per-operator envelope times.
     * This overrides the ROM patch decode for those channels.
     */

    for (int i = 0; i < frames; i++) {
        /* ── Clock noise LFSR (once per sample, full-bandwidth) ── */
        int noise = opll_noise_clock(&s->noise_lfsr);

        /* ── Shared LFOs (hardware-accurate: single LFO for all voices) ── */
        float trem_val = sinf(s->trem_phase);
        s->trem_phase += OPLL_TAU * OPLL_TREM_RATE * dt;
        if (s->trem_phase > OPLL_TAU) s->trem_phase -= OPLL_TAU;

        float chipvib_val = sinf(s->chipvib_phase);
        s->chipvib_phase += OPLL_TAU * OPLL_CHIPVIB_RATE * dt;
        if (s->chipvib_phase > OPLL_TAU) s->chipvib_phase -= OPLL_TAU;

        /* User vibrato */
        float user_vib = 0.0f;
        if (s->user_vib_on) {
            user_vib = sinf(s->user_vib_phase) * s->user_vib_depth;
            s->user_vib_phase += OPLL_TAU * s->user_vib_rate * dt;
            if (s->user_vib_phase > OPLL_TAU) s->user_vib_phase -= OPLL_TAU;
        }

        float mix = 0.0f;

        /* ── OPLL melodic channels ── */
        for (int c = 0; c < OPLL_CHANNELS; c++) {
            OPLLChannel *ch = &s->channels[c];

            /* Skip fully silent */
            if (!ch->key_on &&
                ch->car_env.state == OPLL_ENV_OFF &&
                ch->mod_env.state == OPLL_ENV_OFF)
                continue;

            ch->age += dt;
            if (ch->age > 30.0f) {
                ch->car_env.state = OPLL_ENV_OFF;
                ch->car_env.level = OPLL_ENV_FLOOR;
                ch->mod_env.state = OPLL_ENV_OFF;
                ch->mod_env.level = OPLL_ENV_FLOOR;
                continue;
            }

            /* Get patch data — two paths: ROM patch or yama-bruh extended */
            const uint8_t *patch = NULL;
            const float *yb = NULL;
            int is_rhythm = 0;

            if (ch->instrument < 0) {
                int ridx = -(ch->instrument) - 1;
                if (ridx < 0 || ridx > 4) ridx = 0;
                patch = OPLL_RHYTHM_PATCHES[ridx];
                is_rhythm = 1;
            } else if (s->use_yb_preset && s->yb_preset >= 0 && s->yb_preset < YB_NUM_PRESETS) {
                yb = YB_PRESETS[s->yb_preset];
            } else {
                patch = opll_get_patch(s, ch->instrument);
            }

            /* ── Decode into unified variables ── */
            float car_ratio, mod_ratio, mod_index_val, feedback_val;
            int car_wf, mod_wf;
            float c_trem_depth, m_trem_depth, chipvib_depth_val;
            float ksr_val;
            int c_ksl_bits, m_ksl_bits;
            float mod_level_val;
            int car_eg_type, mod_eg_type;
            float c_atk_t, c_dec_t, c_sus_level, c_rel_t;
            float m_atk_t, m_dec_t, m_sus_level, m_rel_t;

            if (yb) {
                /* ── Yama-bruh extended preset path ── */
                car_ratio       = yb[0];
                mod_ratio       = yb[1];
                mod_index_val   = yb[2];
                feedback_val    = yb[7];
                car_wf          = (int)yb[8];
                mod_wf          = (int)yb[9];
                c_trem_depth    = yb[10];
                m_trem_depth    = 0.0f; /* modulator tremolo not in preset */
                chipvib_depth_val = yb[11];
                ksr_val         = yb[12];
                c_ksl_bits      = (int)yb[13];
                m_ksl_bits      = 0;
                mod_level_val   = yb[14];
                car_eg_type     = (int)yb[15];
                mod_eg_type     = (int)yb[20];

                /* Carrier envelope times (direct seconds) */
                c_atk_t         = yb[3];
                c_dec_t         = yb[4];
                c_sus_level     = yb[5];
                c_rel_t         = yb[6];

                /* Modulator envelope times */
                m_atk_t         = yb[16];
                m_dec_t         = yb[17];
                m_sus_level     = yb[18];
                m_rel_t         = yb[19];
            } else {
                /* ── ROM patch decode path ── */
                int mod_mul_idx = opll_patch_mod_mul(patch);
                int car_mul_idx = opll_patch_car_mul(patch);
                int mod_tl      = opll_patch_mod_tl(patch);
                int fb_idx      = opll_patch_fb(patch);

                car_ratio       = OPLL_MUL_TABLE[car_mul_idx];
                mod_ratio       = OPLL_MUL_TABLE[mod_mul_idx];
                mod_index_val   = 2.0f; /* ROM patches use TL for mod depth */
                feedback_val    = OPLL_FB_TABLE[fb_idx & 7];
                car_wf          = opll_patch_car_wf(patch);
                mod_wf          = opll_patch_mod_wf(patch);
                c_trem_depth    = opll_patch_car_am(patch) ? OPLL_TREM_DEPTH : 0.0f;
                m_trem_depth    = opll_patch_mod_am(patch) ? OPLL_TREM_DEPTH : 0.0f;
                chipvib_depth_val = (opll_patch_car_vib(patch) || opll_patch_mod_vib(patch))
                                    ? OPLL_CHIPVIB_DEPTH : 0.0f;
                ksr_val         = opll_patch_car_ksr(patch) ? 0.3f : 0.0f;
                c_ksl_bits      = opll_patch_car_ksl(patch);
                m_ksl_bits      = opll_patch_mod_ksl(patch);
                mod_level_val   = powf(10.0f, -(float)mod_tl * 0.75f / 20.0f);
                car_eg_type     = opll_patch_car_eg(patch) ? 0 : 1; /* ROM: eg=1→sustained=0→percussive */
                mod_eg_type     = opll_patch_mod_eg(patch) ? 0 : 1;

                /* ROM rates → times */
                int mod_ksr_bit = opll_patch_mod_ksr(patch);
                c_atk_t = opll_rate_to_attack_time(opll_patch_car_ar(patch), opll_patch_car_ksr(patch), ch->freq);
                c_dec_t = opll_rate_to_decay_time(opll_patch_car_dr(patch), opll_patch_car_ksr(patch), ch->freq);
                c_rel_t = opll_rate_to_decay_time(opll_patch_car_rr(patch), opll_patch_car_ksr(patch), ch->freq);
                m_atk_t = opll_rate_to_attack_time(opll_patch_mod_ar(patch), mod_ksr_bit, ch->freq);
                m_dec_t = opll_rate_to_decay_time(opll_patch_mod_dr(patch), mod_ksr_bit, ch->freq);
                m_rel_t = opll_rate_to_decay_time(opll_patch_mod_rr(patch), mod_ksr_bit, ch->freq);

                c_sus_level = powf(10.0f, -3.0f * (float)opll_patch_car_sl(patch) / 20.0f);
                m_sus_level = powf(10.0f, -3.0f * (float)opll_patch_mod_sl(patch) / 20.0f);
            }

            /* Clamp sustain levels */
            if (c_sus_level < OPLL_ENV_FLOOR) c_sus_level = OPLL_ENV_FLOOR;
            if (m_sus_level < OPLL_ENV_FLOOR) m_sus_level = OPLL_ENV_FLOOR;

            /* ── Portamento ── */
            if (s->porta_on && ch->cur_freq != ch->freq) {
                ch->cur_freq = ch->freq + (ch->cur_freq - ch->freq) * porta_coeff;
                if (fabsf(ch->cur_freq - ch->freq) < 0.1f) ch->cur_freq = ch->freq;
            } else {
                ch->cur_freq = ch->freq;
            }

            /* Apply pitch bend + keyseq cents detune */
            float ks_pm = (s->keyseq.cents_mod != 0.0f)
                ? powf(2.0f, s->keyseq.cents_mod / 1200.0f) : 1.0f;
            float base_freq = ch->cur_freq * s->pitch_bend * ks_pm;

            /* Mod wheel can boost mod depth */
            float mod_depth_mult = 1.0f;
            if (s->mod_target == 1) mod_depth_mult = 1.0f + s->mod_wheel * 3.0f;

            /* KSR: scale envelope times with pitch */
            if (ksr_val > 0.0f && base_freq > 0.0f) {
                float octave = log2f(base_freq / 440.0f);
                float ksr_factor = powf(2.0f, -ksr_val * octave);
                c_atk_t *= ksr_factor;
                c_dec_t *= ksr_factor;
                c_rel_t *= ksr_factor;
                m_atk_t *= ksr_factor;
                m_dec_t *= ksr_factor;
                m_rel_t *= ksr_factor;
            }

            /* Chip vibrato on frequency */
            float vib_depth_c = chipvib_depth_val;
            float vib_depth_m = chipvib_depth_val;
            float c_freq_val = base_freq * (1.0f + vib_depth_c * chipvib_val + user_vib);
            float m_freq_val = base_freq * (1.0f + vib_depth_m * chipvib_val + user_vib);

            /* Apply multipliers */
            float crf, mrf;
            if (yb) {
                crf = c_freq_val * car_ratio;
                mrf = m_freq_val * mod_ratio;
            } else {
                /* ROM path: car_ratio/mod_ratio already contain MUL_TABLE values */
                crf = c_freq_val * car_ratio;
                mrf = m_freq_val * mod_ratio;
            }

            /* Tremolo gains */
            float c_trem = 1.0f, m_trem = 1.0f;
            if (c_trem_depth > 0.0f)
                c_trem = 1.0f - c_trem_depth * (1.0f + trem_val) * 0.5f;
            if (m_trem_depth > 0.0f)
                m_trem = 1.0f - m_trem_depth * (1.0f + trem_val) * 0.5f;

            /* KSL gains */
            float c_ksl_g = opll_ksl_gain(c_ksl_bits, base_freq);
            float m_ksl_g = opll_ksl_gain(m_ksl_bits, base_freq);

            /* Sustain pedal extends release */
            if (s->sustain_on) {
                c_rel_t *= s->sustain_mult;
                m_rel_t *= s->sustain_mult;
            }

            /* ── Modulator Envelope ── */
            float m_env;
            switch (ch->mod_env.state) {
            case OPLL_ENV_ATTACK:
                ch->mod_env.level = opll_env_step(ch->mod_env.level, 1.0f, m_atk_t, sr);
                if (ch->mod_env.level >= 1.0f - OPLL_ENV_SNAP) {
                    ch->mod_env.level = 1.0f;
                    ch->mod_env.state = OPLL_ENV_DECAY;
                }
                break;
            case OPLL_ENV_DECAY:
                ch->mod_env.level = opll_env_step(ch->mod_env.level, m_sus_level, m_dec_t, sr);
                if (fabsf(ch->mod_env.level - m_sus_level) <= OPLL_ENV_SNAP) {
                    ch->mod_env.level = m_sus_level;
                    ch->mod_env.state = OPLL_ENV_SUSTAIN;
                }
                break;
            case OPLL_ENV_SUSTAIN:
                if (mod_eg_type > 0) {
                    /* Percussive: continue decaying */
                    ch->mod_env.level = opll_env_step(ch->mod_env.level, OPLL_ENV_FLOOR, m_rel_t, sr);
                    if (ch->mod_env.level <= OPLL_ENV_FLOOR + OPLL_ENV_SNAP) {
                        ch->mod_env.level = OPLL_ENV_FLOOR;
                        ch->mod_env.state = OPLL_ENV_OFF;
                    }
                }
                /* Sustained (eg=1): hold at sustain level */
                break;
            case OPLL_ENV_RELEASE:
                ch->mod_env.level = opll_env_step(ch->mod_env.level, OPLL_ENV_FLOOR, m_rel_t, sr);
                if (ch->mod_env.level <= OPLL_ENV_FLOOR + OPLL_ENV_SNAP) {
                    ch->mod_env.level = OPLL_ENV_FLOOR;
                    ch->mod_env.state = OPLL_ENV_OFF;
                }
                break;
            case OPLL_ENV_OFF:
            default:
                ch->mod_env.level = OPLL_ENV_FLOOR;
                break;
            }
            m_env = ch->mod_env.level;

            /* ── Carrier Envelope ── */
            float c_env;
            switch (ch->car_env.state) {
            case OPLL_ENV_ATTACK:
                ch->car_env.level = opll_env_step(ch->car_env.level, 1.0f, c_atk_t, sr);
                if (ch->car_env.level >= 1.0f - OPLL_ENV_SNAP) {
                    ch->car_env.level = 1.0f;
                    ch->car_env.state = OPLL_ENV_DECAY;
                }
                break;
            case OPLL_ENV_DECAY:
                ch->car_env.level = opll_env_step(ch->car_env.level, c_sus_level, c_dec_t, sr);
                if (fabsf(ch->car_env.level - c_sus_level) <= OPLL_ENV_SNAP) {
                    ch->car_env.level = c_sus_level;
                    ch->car_env.state = OPLL_ENV_SUSTAIN;
                }
                break;
            case OPLL_ENV_SUSTAIN:
                if (car_eg_type > 0) {
                    /* Percussive: continue decaying */
                    ch->car_env.level = opll_env_step(ch->car_env.level, OPLL_ENV_FLOOR, c_rel_t, sr);
                    if (ch->car_env.level <= OPLL_ENV_FLOOR + OPLL_ENV_SNAP) {
                        ch->car_env.level = OPLL_ENV_FLOOR;
                        ch->car_env.state = OPLL_ENV_OFF;
                    }
                }
                break;
            case OPLL_ENV_RELEASE:
                ch->car_env.level = opll_env_step(ch->car_env.level, OPLL_ENV_FLOOR, c_rel_t, sr);
                if (ch->car_env.level <= OPLL_ENV_FLOOR + OPLL_ENV_SNAP) {
                    ch->car_env.level = OPLL_ENV_FLOOR;
                    ch->car_env.state = OPLL_ENV_OFF;
                }
                break;
            case OPLL_ENV_OFF:
            default:
                ch->car_env.level = OPLL_ENV_FLOOR;
                break;
            }
            c_env = ch->car_env.level;

            /* Cull dead voices */
            if (c_env <= OPLL_VOICE_DONE && ch->car_env.state >= OPLL_ENV_SUSTAIN) {
                ch->car_env.state = OPLL_ENV_OFF;
                ch->car_env.level = OPLL_ENV_FLOOR;
                ch->mod_env.state = OPLL_ENV_OFF;
                ch->mod_env.level = OPLL_ENV_FLOOR;
                continue;
            }

            /* ── 2-op FM synthesis ── */

            /* Feedback: raw (pre-envelope) modulator — hardware-accurate */
            float mod_raw = opll_waveform(s, ch->mp + feedback_val * ch->prev_mod,
                                           mod_wf, noise);
            ch->prev_mod = mod_raw; /* feedback stays alive regardless of mod envelope */

            /* Modulator output: apply envelope, level, tremolo, KSL */
            float mod_out = mod_raw * mod_level_val * m_env * m_trem * m_ksl_g;

            /* Apply mod depth multiplier (mod wheel → mod index) */
            mod_out *= mod_depth_mult;

            /* Carrier: phase modulated by modulator output */
            float car_phase = ch->cp + mod_index_val * mod_out * OPLL_TAU;
            float car_out = opll_waveform(s, car_phase, car_wf, noise);

            /* Carrier output: apply envelope, velocity, tremolo, KSL */
            car_out *= c_env * ch->velocity * 0.35f * c_trem * c_ksl_g;

            /* Anti-click fade-in (~1ms ramp on note start) */
            if (ch->sample_count < OPLL_FADEIN_SAMPLES) {
                car_out *= (float)ch->sample_count / (float)OPLL_FADEIN_SAMPLES;
            }
            ch->sample_count++;

            /* Rhythm noise injection for OPLL percussion channels */
            if (is_rhythm && s->rhythm_mode) {
                int ridx = -(ch->instrument) - 1;
                float noise_val = noise ? 1.0f : -1.0f;

                switch (ridx) {
                case 1: /* HH: mostly noise */
                    car_out = noise_val * c_env * ch->velocity * 0.35f * 0.8f
                            + car_out * 0.2f;
                    break;
                case 2: /* SD: noise + tone */
                    car_out = (noise_val * 0.5f + car_out * 0.5f);
                    break;
                case 4: /* TC: metallic */
                    car_out = (noise_val * 0.3f + car_out * 0.7f);
                    break;
                default:
                    break;
                }
            }

            /* NaN guard */
            if (!isfinite(car_out) || !isfinite(ch->cp) || !isfinite(ch->mp)) {
                ch->cp = 0.0f;
                ch->mp = 0.0f;
                ch->prev_mod = 0.0f;
                continue;
            }

            mix += car_out;

            /* Advance phases (floating-point, TAU-wrapped) */
            ch->cp += OPLL_TAU * crf * dt;
            ch->mp += OPLL_TAU * mrf * dt;
            if (ch->cp > OPLL_TAU) ch->cp -= OPLL_TAU;
            if (ch->mp > OPLL_TAU) ch->mp -= OPLL_TAU;
        }

        /* ── FM drum engine ── */
        for (int d = 0; d < OPLL_MAX_DRUM_HITS; d++) {
            OPLLDrumHit *h = &s->drum_hits[d];
            if (!h->active) continue;

            float dsample = opll_drum_render_hit(h, dt, &s->drum_noise_seed);

            if (!isfinite(dsample)) {
                h->active = 0;
                continue;
            }

            mix += dsample;
        }

        /* ── YM2413 DAC simulation (9-bit quantization + noise floor) ── */
        mix *= s->volume;
        mix = roundf(mix * (float)OPLL_DAC_LEVELS) / (float)OPLL_DAC_LEVELS;
        mix += ((float)(s->noise_lfsr & 0xFF) / 255.0f - 0.5f) * OPLL_NOISE_FLOOR;

        /* ── Soft-knee limiter ── */
        mix = opll_limiter(&s->limiter_env, mix);

        stereo_buf[i * 2]     = mix;
        stereo_buf[i * 2 + 1] = mix;
    }
}

/* ── OSC handler ──────────────────────────────────────────────────────── */

static void ym2413_osc_handle(void *state, const char *sub_path,
                               const int32_t *iargs, int ni,
                               const float *fargs, int nf)
{
    YM2413State *s = (YM2413State *)state;

    if (strcmp(sub_path, "/instrument") == 0 && ni >= 1) {
        int inst = iargs[0];
        if (inst >= 0 && inst <= 15) {
            s->current_instrument = inst;
            fprintf(stderr, "[ym2413] OSC instrument %d: %s\n",
                    inst, OPLL_PATCH_NAMES[inst]);
        }
    }
    else if (strcmp(sub_path, "/rhythm") == 0 && ni >= 1) {
        s->rhythm_mode = iargs[0] ? 1 : 0;
        fprintf(stderr, "[ym2413] OSC rhythm mode: %s\n",
                s->rhythm_mode ? "ON" : "OFF");
    }
    else if (strcmp(sub_path, "/volume") == 0 && nf >= 1) {
        float vol = fargs[0];
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 1.0f) vol = 1.0f;
        s->volume = vol;
    }
    else if (strcmp(sub_path, "/mono") == 0 && ni >= 1) {
        s->mono_mode = iargs[0] ? 1 : 0;
    }
    else if (strcmp(sub_path, "/portamento") == 0 && ni >= 1) {
        s->porta_on = iargs[0] ? 1 : 0;
        if (nf >= 1) s->porta_time = fargs[0];
    }
    else if (strcmp(sub_path, "/pitchbend") == 0 && nf >= 1) {
        s->pitch_bend = fargs[0];
    }
    else if (strcmp(sub_path, "/pitchbend/range") == 0 && ni >= 1) {
        s->pitch_bend_range = iargs[0];
    }
    else if (strcmp(sub_path, "/modwheel") == 0 && nf >= 1) {
        s->mod_wheel = fargs[0];
        if (s->mod_wheel < 0.0f) s->mod_wheel = 0.0f;
        if (s->mod_wheel > 1.0f) s->mod_wheel = 1.0f;
    }
    else if (strcmp(sub_path, "/modtarget") == 0 && ni >= 1) {
        s->mod_target = iargs[0] % 3;
    }
    else if (strcmp(sub_path, "/vibrato") == 0 && ni >= 1) {
        s->user_vib_on = iargs[0] ? 1 : 0;
        if (nf >= 1) s->user_vib_rate = fargs[0];
        if (nf >= 2) s->user_vib_depth = fargs[1];
    }
    else if (strcmp(sub_path, "/drum/bank") == 0 && ni >= 1) {
        int bank = iargs[0];
        if (bank >= 0 && bank < OPLL_NUM_DRUM_BANKS) {
            s->drum_bank = bank;
            fprintf(stderr, "[ym2413] OSC drum bank %d: %s\n",
                    bank, OPLL_DRUM_BANK_NAMES[bank]);
        }
    }
    else if (strcmp(sub_path, "/drum/trigger") == 0 && ni >= 1) {
        int drum_type = iargs[0];
        float vel = (nf >= 1) ? fargs[0] : 0.8f;
        int note = (ni >= 2) ? iargs[1] : 0;
        if (drum_type >= 0 && drum_type < DRUM_COUNT) {
            opll_drum_trigger(s, drum_type, vel, note);
        }
    }
    else if (strncmp(sub_path, "/custom/", 8) == 0 && ni >= 1) {
        int reg = atoi(sub_path + 8);
        if (reg >= 0 && reg < 8) {
            s->custom_patch[reg] = (uint8_t)(iargs[0] & 0xFF);
        }
    }
    else if (strncmp(sub_path, "/channel/", 9) == 0) {
        const char *rest = sub_path + 9;
        int ch_num = atoi(rest);
        if (ch_num < 0 || ch_num >= OPLL_CHANNELS) return;

        const char *slash = strchr(rest, '/');
        if (!slash) return;

        OPLLChannel *ch = &s->channels[ch_num];

        if (strcmp(slash, "/fnum") == 0 && ni >= 1)
            ch->fnum = iargs[0] & 0x3FF;
        else if (strcmp(slash, "/block") == 0 && ni >= 1)
            ch->block = iargs[0] & 0x07;
        else if (strcmp(slash, "/volume") == 0 && ni >= 1)
            ch->volume = iargs[0] & 0x0F;
        else if (strcmp(slash, "/keyon") == 0 && ni >= 1) {
            if (iargs[0]) {
                ch->key_on = 1;
                ch->car_env.state = OPLL_ENV_ATTACK;
                ch->car_env.level = OPLL_ENV_FLOOR;
                ch->mod_env.state = OPLL_ENV_ATTACK;
                ch->mod_env.level = OPLL_ENV_FLOOR;
            } else {
                ch->key_on = 0;
                ch->car_env.state = OPLL_ENV_RELEASE;
                ch->mod_env.state = OPLL_ENV_RELEASE;
            }
        }
    }
    else if (strcmp(sub_path, "/note/on") == 0 && ni >= 2) {
        int note = iargs[0];
        int vel = iargs[1];
        if (vel > 0) opll_note_on(s, note, vel, 48000);
        else opll_note_off(s, note);
    }
    else if (strcmp(sub_path, "/note/off") == 0 && ni >= 1) {
        opll_note_off(s, iargs[0]);
    }
    else if (strcmp(sub_path, "/param/reset") == 0) {
        ym2413_init(state);
        fprintf(stderr, "[ym2413] OSC reset\n");
    }
    /* ── Yama-bruh preset selection ── */
    else if (strcmp(sub_path, "/yb/preset") == 0 && ni >= 1) {
        int p = iargs[0];
        if (p >= 0 && p < YB_NUM_PRESETS) {
            s->yb_preset = p;
            s->use_yb_preset = 1;
            fprintf(stderr, "[ym2413] yb preset %d: %s\n", p, YB_PRESET_NAMES[p]);
        }
    }
    else if (strcmp(sub_path, "/yb/off") == 0) {
        s->use_yb_preset = 0;
        fprintf(stderr, "[ym2413] yb preset mode OFF (ROM patches)\n");
    }
    /* ── Key sequence ── */
    else if (strncmp(sub_path, "/seq/play", 9) == 0) {
        /* /seq/play  seed_string (passed as first iarg = hash, or use sub-path) */
        /* For OSC: /seq/play with iargs[0]=seed_hash, fargs[0]=bpm, iargs[1]=preset */
        uint32_t seed = (ni >= 1) ? (uint32_t)iargs[0] : 12345;
        float bpm = (nf >= 1) ? fargs[0] : OPLL_SEQ_BPM_DEFAULT;
        int preset = (ni >= 2) ? iargs[1] : s->yb_preset;
        int loop = (ni >= 3) ? iargs[2] : 0;

        /* Generate from hash directly */
        OPLLKeySequence *seq = &s->seq;
        opll_generate_sequence(seq, seed);
        seq->bpm = bpm;
        seq->preset_idx = (preset >= 0 && preset < YB_NUM_PRESETS) ? preset : 88;
        seq->loop = loop;
        seq->playing = 1;
        s->yb_preset = seq->preset_idx;
        s->use_yb_preset = 1;

        if (seq->num_notes > 0) {
            opll_note_on(s, seq->notes[0].midi_note, 100, 48000);
        }
        fprintf(stderr, "[ym2413] seq play: seed=%u notes=%d preset=%d bpm=%.0f loop=%d\n",
                seed, seq->num_notes, seq->preset_idx, bpm, loop);
    }
    else if (strcmp(sub_path, "/seq/play/string") == 0) {
        /* Play from a seed string via OSC string arg — requires string parsing.
         * For simplicity, hash the sub_path beyond /seq/play/string/ */
        /* Use a default if no args */
        const char *seed_str = "default";
        float bpm = (nf >= 1) ? fargs[0] : OPLL_SEQ_BPM_DEFAULT;
        int preset = (ni >= 1) ? iargs[0] : s->yb_preset;
        int loop = (ni >= 2) ? iargs[1] : 0;
        opll_seq_play(s, seed_str, preset, bpm, loop, 48000);
    }
    else if (strcmp(sub_path, "/seq/stop") == 0) {
        opll_seq_stop(s);
    }
    else if (strcmp(sub_path, "/seq/bpm") == 0 && nf >= 1) {
        s->seq.bpm = fargs[0];
    }
    else if (strcmp(sub_path, "/seq/loop") == 0 && ni >= 1) {
        s->seq.loop = iargs[0] ? 1 : 0;
    }
    else {
        seq_osc_handle(&s->mini_seq, sub_path, iargs, ni, fargs, nf);
    }
}

/* ── OSC status ───────────────────────────────────────────────────────── */

static int ym2413_osc_status(void *state, uint8_t *buf, int max_len) {
    YM2413State *s = (YM2413State *)state;
    int pos = 0;
    int w;

    w = osc_write_string(buf + pos, max_len - pos, "/status");
    if (w < 0) return 0;
    pos += w;

    /* Type tags: i s i i i i f f */
    w = osc_write_string(buf + pos, max_len - pos, ",isiiiiiffisii");
    if (w < 0) return 0;
    pos += w;

    /* current_instrument (i) */
    if (pos + 4 > max_len) return 0;
    osc_write_i32(buf + pos, s->current_instrument); pos += 4;

    /* instrument_name (s) */
    const char *name = (s->current_instrument >= 0 && s->current_instrument <= 15)
                       ? OPLL_PATCH_NAMES[s->current_instrument] : "Unknown";
    w = osc_write_string(buf + pos, max_len - pos, name);
    if (w < 0) return 0;
    pos += w;

    /* rhythm_mode (i) */
    if (pos + 4 > max_len) return 0;
    osc_write_i32(buf + pos, s->rhythm_mode); pos += 4;

    /* active melodic channels (i) */
    if (pos + 4 > max_len) return 0;
    int active = 0;
    for (int c = 0; c < OPLL_CHANNELS; c++) {
        if (s->channels[c].key_on ||
            s->channels[c].car_env.state != OPLL_ENV_OFF)
            active++;
    }
    osc_write_i32(buf + pos, active); pos += 4;

    /* active drum hits (i) */
    if (pos + 4 > max_len) return 0;
    int drum_active = 0;
    for (int d = 0; d < OPLL_MAX_DRUM_HITS; d++) {
        if (s->drum_hits[d].active) drum_active++;
    }
    osc_write_i32(buf + pos, drum_active); pos += 4;

    /* drum bank (i) */
    if (pos + 4 > max_len) return 0;
    osc_write_i32(buf + pos, s->drum_bank); pos += 4;

    /* mono mode (i) */
    if (pos + 4 > max_len) return 0;
    osc_write_i32(buf + pos, s->mono_mode); pos += 4;

    /* pitch bend (f) */
    if (pos + 4 > max_len) return 0;
    osc_write_f32(buf + pos, s->pitch_bend); pos += 4;

    /* mod wheel (f) */
    if (pos + 4 > max_len) return 0;
    osc_write_f32(buf + pos, s->mod_wheel); pos += 4;

    /* yb_preset (i) */
    if (pos + 4 > max_len) return pos;
    osc_write_i32(buf + pos, s->use_yb_preset ? s->yb_preset : -1); pos += 4;

    /* yb_preset_name (s) */
    const char *yb_name = (s->use_yb_preset && s->yb_preset >= 0 && s->yb_preset < YB_NUM_PRESETS)
                           ? YB_PRESET_NAMES[s->yb_preset] : "ROM";
    w = osc_write_string(buf + pos, max_len - pos, yb_name);
    if (w < 0) return pos;
    pos += w;

    /* seq_playing (i) */
    if (pos + 4 > max_len) return pos;
    osc_write_i32(buf + pos, s->seq.playing); pos += 4;

    /* seq_note (i) — current note index */
    if (pos + 4 > max_len) return pos;
    osc_write_i32(buf + pos, s->seq.current_note); pos += 4;

    return pos;
}

/* ── Exported type descriptor ─────────────────────────────────────────── */

InstrumentType ym2413_type = {
    .name         = "ym2413",
    .display_name = "YM2413 OPLL",
    .state_size   = sizeof(YM2413State),
    .init         = ym2413_init,
    .destroy      = ym2413_destroy,
    .midi         = ym2413_midi,
    .render       = ym2413_render,
    .osc_handle   = ym2413_osc_handle,
    .osc_status   = ym2413_osc_status,
};

#endif /* YM2413_H */
