#ifndef INSTRUMENTS_H
#define INSTRUMENTS_H

#include <stdint.h>

#define MAX_SLOTS 16

/* Every instrument type implements this interface */
typedef struct InstrumentType {
    const char *name;           /* "fm-synth", "drum-machine", etc */
    const char *display_name;   /* "FM Synth (yama-bruh)" */
    size_t state_size;          /* sizeof the instrument's state struct */
    void (*init)(void *state);
    void (*destroy)(void *state);
    void (*midi)(void *state, uint8_t status, uint8_t d1, uint8_t d2);
    void (*render)(void *state, float *stereo_buf, int frames, int sample_rate);
    /* OSC: handle a sub-path like "/param/mod_index" with raw OSC args */
    void (*osc_handle)(void *state, const char *sub_path,
                       const int32_t *iargs, int ni,
                       const float *fargs, int nf);
    /* OSC: write status response into buf, return length */
    int  (*osc_status)(void *state, uint8_t *buf, int max_len);
} InstrumentType;

/* A slot in the rack */
typedef struct {
    int              active;        /* 0 = empty slot */
    int              type_idx;      /* index into registered instrument types (-1 = none) */
    void            *state;         /* instrument state (malloc'd) */
    float            volume;        /* 0.0 - 1.0 */
    int              mute;
    int              solo;
} RackSlot;

/* The rack */
typedef struct {
    RackSlot         slots[MAX_SLOTS];
    float            master_volume;
    int              local_mute;    /* 1 = silence ALSA output, only send to bus */
    int              n_types;       /* number of registered instrument types */
    InstrumentType  *types;         /* array of registered types */
} Rack;

#endif
