/* yama-bruh FM presets — ported from Rust/WASM
 * [carrier_ratio, mod_ratio, mod_index, attack, decay, sustain, release, feedback]
 */

#ifndef PRESETS_H
#define PRESETS_H

#define NUM_PRESETS 99

typedef struct {
    float carrier_ratio;
    float mod_ratio;
    float mod_index;
    float attack;
    float decay;
    float sustain;
    float release;
    float feedback;
} FMPreset;

static const FMPreset PRESETS[NUM_PRESETS] = {
    /* 00-09: Piano / Keys */
    { 1.0f,  1.0f,  1.8f,  0.001f, 0.8f,  0.15f, 0.4f,  0.0f  }, /*  0 Grand Piano    */
    { 1.0f,  2.0f,  2.5f,  0.001f, 0.6f,  0.2f,  0.35f, 0.0f  }, /*  1 Bright Piano   */
    { 1.0f,  3.0f,  3.5f,  0.001f, 0.4f,  0.25f, 0.3f,  0.05f }, /*  2 Honky-Tonk     */
    { 1.0f,  1.0f,  1.2f,  0.002f, 1.0f,  0.3f,  0.5f,  0.0f  }, /*  3 E.Piano 1      */
    { 1.0f,  7.0f,  2.0f,  0.001f, 0.7f,  0.12f, 0.6f,  0.0f  }, /*  4 E.Piano 2      */
    { 1.0f,  5.0f,  4.5f,  0.001f, 0.15f, 0.05f, 0.08f, 0.1f  }, /*  5 Clav           */
    { 1.0f,  3.0f,  3.0f,  0.001f, 0.3f,  0.0f,  0.2f,  0.02f }, /*  6 Harpsichord    */
    { 1.0f, 14.0f,  1.5f,  0.001f, 0.5f,  0.1f,  0.4f,  0.0f  }, /*  7 DX Piano       */
    { 1.0f,  2.0f,  1.5f,  0.001f, 0.9f,  0.25f, 0.45f, 0.0f  }, /*  8 Stage Piano    */
    { 1.0f,  4.0f,  2.8f,  0.002f, 0.7f,  0.18f, 0.5f,  0.03f }, /*  9 Vintage Keys   */

    /* 10-19: Organ */
    { 1.0f,  1.0f,  1.5f,  0.005f, 0.02f, 0.9f,  0.05f, 0.12f }, /* 10 Jazz Organ     */
    { 1.0f,  2.0f,  2.5f,  0.003f, 0.02f, 0.95f, 0.04f, 0.2f  }, /* 11 Rock Organ     */
    { 1.0f,  1.0f,  0.6f,  0.01f,  0.05f, 0.85f, 0.1f,  0.05f }, /* 12 Church Organ   */
    { 1.0f,  3.0f,  1.2f,  0.008f, 0.05f, 0.8f,  0.08f, 0.15f }, /* 13 Reed Organ     */
    { 1.0f,  1.0f,  0.4f,  0.015f, 0.08f, 0.75f, 0.15f, 0.03f }, /* 14 Pipe Organ     */
    { 1.0f,  2.0f,  1.8f,  0.003f, 0.02f, 0.9f,  0.04f, 0.08f }, /* 15 Drawbar 1      */
    { 1.0f,  4.0f,  2.2f,  0.003f, 0.02f, 0.88f, 0.04f, 0.1f  }, /* 16 Drawbar 2      */
    { 1.0f,  6.0f,  1.0f,  0.001f, 0.3f,  0.6f,  0.05f, 0.15f }, /* 17 Perc Organ     */
    { 1.0f,  3.0f,  1.5f,  0.005f, 0.02f, 0.92f, 0.06f, 0.3f  }, /* 18 Rotary Organ   */
    { 1.0f,  5.0f,  3.0f,  0.003f, 0.03f, 0.85f, 0.05f, 0.25f }, /* 19 Full Organ     */

    /* 20-29: Brass */
    { 1.0f,  1.0f,  4.0f,  0.04f,  0.15f, 0.7f,  0.2f,  0.2f  }, /* 20 Trumpet        */
    { 1.0f,  1.0f,  3.5f,  0.06f,  0.2f,  0.65f, 0.3f,  0.25f }, /* 21 Trombone       */
    { 1.0f,  1.0f,  2.5f,  0.08f,  0.25f, 0.6f,  0.4f,  0.15f }, /* 22 French Horn    */
    { 1.0f,  1.0f,  5.0f,  0.05f,  0.15f, 0.75f, 0.25f, 0.3f  }, /* 23 Brass Sect     */
    { 1.0f,  2.0f,  4.5f,  0.03f,  0.1f,  0.8f,  0.2f,  0.18f }, /* 24 Synth Brass 1  */
    { 1.0f,  3.0f,  6.0f,  0.02f,  0.08f, 0.85f, 0.15f, 0.22f }, /* 25 Synth Brass 2  */
    { 1.0f,  1.0f,  2.0f,  0.03f,  0.12f, 0.4f,  0.15f, 0.1f  }, /* 26 Mute Trumpet   */
    { 1.0f,  2.0f,  3.0f,  0.1f,   0.3f,  0.7f,  0.6f,  0.2f  }, /* 27 Brass Pad      */
    { 1.0f,  1.0f,  7.0f,  0.04f,  0.12f, 0.8f,  0.2f,  0.35f }, /* 28 Power Brass    */
    { 1.0f,  1.0f,  8.0f,  0.02f,  0.08f, 0.85f, 0.3f,  0.4f  }, /* 29 Fanfare        */

    /* 30-39: Strings / Pads */
    { 1.0f,  2.0f,  1.0f,  0.15f,  0.5f,  0.7f,  0.8f,  0.02f }, /* 30 Strings        */
    { 1.0f,  2.0f,  0.8f,  0.4f,   0.8f,  0.65f, 1.2f,  0.01f }, /* 31 Slow Strings   */
    { 1.0f,  3.0f,  1.5f,  0.08f,  0.4f,  0.75f, 0.6f,  0.05f }, /* 32 Syn Strings 1  */
    { 1.0f,  1.0f,  2.0f,  0.12f,  0.5f,  0.7f,  0.8f,  0.08f }, /* 33 Syn Strings 2  */
    { 1.0f,  2.0f,  0.5f,  0.2f,   0.6f,  0.8f,  1.5f,  0.03f }, /* 34 Warm Pad       */
    { 1.0f,  1.0f,  0.3f,  0.3f,   1.0f,  0.7f,  1.8f,  0.01f }, /* 35 Choir Pad      */
    { 1.0f,  5.0f,  1.2f,  0.25f,  0.8f,  0.6f,  2.0f,  0.06f }, /* 36 Atmosphere     */
    { 1.0f,  7.0f,  2.0f,  0.15f,  0.4f,  0.75f, 1.0f,  0.04f }, /* 37 Brightness Pad */
    { 1.0f,  3.0f,  3.0f,  0.5f,   1.5f,  0.5f,  2.5f,  0.1f  }, /* 38 Sweep Pad      */
    { 1.0f,  9.0f,  1.0f,  0.2f,   0.6f,  0.6f,  2.0f,  0.02f }, /* 39 Ice Pad        */

    /* 40-49: Bass */
    { 1.0f,  1.0f,  2.0f,  0.001f, 0.3f,  0.2f,  0.12f, 0.05f }, /* 40 Finger Bass    */
    { 1.0f,  3.0f,  3.5f,  0.001f, 0.15f, 0.1f,  0.08f, 0.1f  }, /* 41 Pick Bass      */
    { 1.0f,  1.0f,  5.0f,  0.001f, 0.08f, 0.05f, 0.06f, 0.2f  }, /* 42 Slap Bass      */
    { 1.0f,  1.0f,  0.8f,  0.005f, 0.4f,  0.35f, 0.2f,  0.0f  }, /* 43 Fretless       */
    { 0.5f,  1.0f,  4.0f,  0.001f, 0.2f,  0.25f, 0.1f,  0.15f }, /* 44 Synth Bass 1   */
    { 0.5f,  2.0f,  6.0f,  0.001f, 0.12f, 0.2f,  0.08f, 0.3f  }, /* 45 Synth Bass 2   */
    { 0.5f,  3.0f,  8.0f,  0.001f, 0.1f,  0.15f, 0.06f, 0.4f  }, /* 46 Acid Bass      */
    { 1.0f,  1.0f,  3.0f,  0.001f, 0.25f, 0.3f,  0.15f, 0.5f  }, /* 47 Rubber Bass    */
    { 0.5f,  0.5f,  1.5f,  0.001f, 0.5f,  0.4f,  0.2f,  0.0f  }, /* 48 Sub Bass       */
    { 0.5f,  1.0f,  7.0f,  0.001f, 0.08f, 0.3f,  0.1f,  0.6f  }, /* 49 Wobble Bass    */

    /* 50-59: Lead */
    { 1.0f,  1.0f,  0.5f,  0.01f,  0.08f, 0.8f,  0.15f, 0.0f  }, /* 50 Square Lead    */
    { 1.0f,  1.0f,  2.5f,  0.01f,  0.08f, 0.85f, 0.15f, 0.1f  }, /* 51 Saw Lead       */
    { 1.0f,  2.0f,  5.0f,  0.005f, 0.06f, 0.9f,  0.1f,  0.15f }, /* 52 Sync Lead      */
    { 2.0f,  1.0f,  1.5f,  0.01f,  0.1f,  0.7f,  0.2f,  0.0f  }, /* 53 Calliope       */
    { 1.0f,  4.0f,  3.0f,  0.001f, 0.05f, 0.6f,  0.1f,  0.08f }, /* 54 Chiffer        */
    { 1.0f,  1.0f,  6.0f,  0.005f, 0.06f, 0.85f, 0.12f, 0.25f }, /* 55 Charang        */
    { 1.0f,  1.0f,  1.0f,  0.02f,  0.15f, 0.75f, 0.3f,  0.02f }, /* 56 Solo Vox       */
    { 1.5f,  1.0f,  3.0f,  0.01f,  0.08f, 0.8f,  0.15f, 0.12f }, /* 57 Fifth Lead     */
    { 0.5f,  1.0f,  4.0f,  0.005f, 0.1f,  0.8f,  0.15f, 0.2f  }, /* 58 Bass+Lead      */
    { 1.0f,  3.0f,  2.0f,  0.01f,  0.08f, 0.85f, 0.12f, 0.08f }, /* 59 Poly Lead      */

    /* 60-69: Bell / Mallet */
    { 1.0f,  3.5f,  5.0f,  0.001f, 2.0f,  0.0f,  2.5f,  0.0f  }, /* 60 Tubular Bell   */
    { 1.0f,  5.4f,  3.0f,  0.001f, 1.0f,  0.0f,  1.5f,  0.0f  }, /* 61 Glockenspiel   */
    { 1.0f,  7.0f,  2.5f,  0.001f, 1.5f,  0.0f,  2.0f,  0.01f }, /* 62 Music Box      */
    { 1.0f,  4.0f,  2.0f,  0.001f, 2.5f,  0.05f, 3.0f,  0.0f  }, /* 63 Vibraphone     */
    { 1.0f,  4.0f,  4.0f,  0.001f, 0.6f,  0.0f,  0.5f,  0.02f }, /* 64 Marimba        */
    { 1.0f,  3.0f,  6.0f,  0.001f, 0.4f,  0.0f,  0.3f,  0.03f }, /* 65 Xylophone      */
    { 1.0f,  1.41f, 7.0f,  0.001f, 1.2f,  0.0f,  1.5f,  0.05f }, /* 66 Steel Drums    */
    { 1.0f, 13.0f,  1.5f,  0.001f, 3.0f,  0.0f,  3.5f,  0.0f  }, /* 67 Crystal        */
    { 1.0f,  5.19f, 3.5f,  0.001f, 0.8f,  0.0f,  0.6f,  0.02f }, /* 68 Kalimba        */
    { 1.0f, 11.0f,  2.0f,  0.001f, 2.0f,  0.0f,  2.5f,  0.01f }, /* 69 Tinkle Bell    */

    /* 70-79: Reed / Pipe */
    { 1.0f,  1.0f,  2.5f,  0.02f,  0.08f, 0.7f,  0.1f,  0.3f  }, /* 70 Harmonica      */
    { 1.0f,  2.0f,  2.0f,  0.02f,  0.08f, 0.75f, 0.1f,  0.25f }, /* 71 Accordion      */
    { 1.0f,  3.0f,  3.0f,  0.015f, 0.06f, 0.65f, 0.08f, 0.2f  }, /* 72 Clarinet       */
    { 1.0f,  2.0f,  4.0f,  0.02f,  0.08f, 0.6f,  0.1f,  0.15f }, /* 73 Oboe           */
    { 0.5f,  1.0f,  3.5f,  0.03f,  0.1f,  0.55f, 0.12f, 0.2f  }, /* 74 Bassoon        */
    { 2.0f,  1.0f,  1.0f,  0.02f,  0.05f, 0.7f,  0.1f,  0.05f }, /* 75 Flute          */
    { 2.0f,  1.0f,  1.5f,  0.015f, 0.05f, 0.65f, 0.08f, 0.08f }, /* 76 Recorder       */
    { 2.0f,  1.0f,  0.5f,  0.03f,  0.06f, 0.6f,  0.12f, 0.02f }, /* 77 Pan Flute      */
    { 2.0f,  1.0f,  0.3f,  0.04f,  0.08f, 0.5f,  0.15f, 0.01f }, /* 78 Bottle         */
    { 1.0f,  2.0f,  3.5f,  0.03f,  0.06f, 0.55f, 0.15f, 0.35f }, /* 79 Shakuhachi     */

    /* 80-89: SFX / Atmosphere */
    { 1.0f,  0.5f,  1.0f,  0.5f,   2.0f,  0.0f,  3.0f,  0.0f  }, /* 80 Rain           */
    { 1.0f,  1.41f, 2.0f,  0.3f,   1.5f,  0.4f,  2.5f,  0.1f  }, /* 81 Soundtrack     */
    { 1.0f,  7.0f,  8.0f,  0.01f,  0.5f,  0.6f,  1.5f,  0.5f  }, /* 82 Sci-Fi         */
    { 1.0f,  0.25f, 3.0f,  0.4f,   1.0f,  0.5f,  2.0f,  0.15f }, /* 83 Atmosphere 2   */
    { 1.0f,  0.5f, 10.0f,  0.2f,   0.8f,  0.3f,  1.5f,  0.7f  }, /* 84 Goblin         */
    { 1.0f,  3.0f,  2.0f,  0.1f,   3.0f,  0.0f,  4.0f,  0.05f }, /* 85 Echo Drop      */
    { 1.0f,  5.0f,  4.0f,  0.15f,  1.0f,  0.5f,  2.0f,  0.08f }, /* 86 Star Theme     */
    { 1.0f,  1.5f,  6.0f,  0.05f,  0.3f,  0.4f,  0.5f,  0.4f  }, /* 87 Sitar          */
    { 1.0f, 11.0f,  3.0f,  0.001f, 0.05f, 0.0f,  0.02f, 0.0f  }, /* 88 Telephone      */
    { 0.1f,  0.3f, 12.0f,  0.8f,   2.0f,  0.3f,  1.0f,  0.9f  }, /* 89 Helicopter     */

    /* 90-98: Retro / Digital */
    { 1.0f,  2.0f,  1.0f,  0.001f, 0.05f, 0.4f,  0.05f, 0.3f  }, /* 90 Chiptune 1     */
    { 1.0f,  1.0f,  0.3f,  0.001f, 0.04f, 0.5f,  0.04f, 0.0f  }, /* 91 Chiptune 2     */
    { 1.0f,  3.0f,  2.5f,  0.001f, 0.06f, 0.35f, 0.06f, 0.4f  }, /* 92 Chiptune 3     */
    { 1.0f,  8.0f,  1.5f,  0.001f, 0.03f, 0.0f,  0.03f, 0.0f  }, /* 93 Retro Beep     */
    { 1.0f,  0.1f, 15.0f,  0.001f, 0.15f, 0.3f,  0.1f,  0.8f  }, /* 94 Bit Crush      */
    { 1.0f,  4.0f,  3.0f,  0.001f, 0.08f, 0.2f,  0.05f, 0.5f  }, /* 95 Arcade         */
    { 1.0f,  1.0f,  5.0f,  0.001f, 1.5f,  0.0f,  2.0f,  0.3f  }, /* 96 Game Over      */
    { 2.0f,  1.0f,  2.0f,  0.001f, 0.4f,  0.0f,  0.3f,  0.0f  }, /* 97 Power Up       */
    { 1.0f,  7.0f,  3.0f,  0.01f,  0.1f,  0.6f,  0.15f, 0.15f }, /* 98 Digital Vox    */
};

static const char *PRESET_NAMES[NUM_PRESETS] = {
    "Grand Piano","Bright Piano","Honky-Tonk","E.Piano 1","E.Piano 2",
    "Clav","Harpsichord","DX Piano","Stage Piano","Vintage Keys",
    "Jazz Organ","Rock Organ","Church Organ","Reed Organ","Pipe Organ",
    "Drawbar 1","Drawbar 2","Perc Organ","Rotary Organ","Full Organ",
    "Trumpet","Trombone","French Horn","Brass Sect","Synth Brass 1",
    "Synth Brass 2","Mute Trumpet","Brass Pad","Power Brass","Fanfare",
    "Strings","Slow Strings","Syn Strings 1","Syn Strings 2","Warm Pad",
    "Choir Pad","Atmosphere","Brightness Pad","Sweep Pad","Ice Pad",
    "Finger Bass","Pick Bass","Slap Bass","Fretless","Synth Bass 1",
    "Synth Bass 2","Acid Bass","Rubber Bass","Sub Bass","Wobble Bass",
    "Square Lead","Saw Lead","Sync Lead","Calliope","Chiffer",
    "Charang","Solo Vox","Fifth Lead","Bass+Lead","Poly Lead",
    "Tubular Bell","Glockenspiel","Music Box","Vibraphone","Marimba",
    "Xylophone","Steel Drums","Crystal","Kalimba","Tinkle Bell",
    "Harmonica","Accordion","Clarinet","Oboe","Bassoon",
    "Flute","Recorder","Pan Flute","Bottle","Shakuhachi",
    "Rain","Soundtrack","Sci-Fi","Atmosphere 2","Goblin",
    "Echo Drop","Star Theme","Sitar","Telephone","Helicopter",
    "Chiptune 1","Chiptune 2","Chiptune 3","Retro Beep","Bit Crush",
    "Arcade","Game Over","Power Up","Digital Vox",
};

#endif /* PRESETS_H */
