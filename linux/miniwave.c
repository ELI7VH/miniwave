/* miniwave — modular rack host for waveOS
 *
 * 16 instrument slots, MIDI-by-channel routing, stereo mix,
 * OSC control surface on UDP port 9000,
 * embedded HTTP + SSE server for browser-based WaveUI.
 */

#define _GNU_SOURCE
#define __USE_MISC
#define ALSA_PCM_NEW_HW_PARAMS_API

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <alsa/asoundlib.h>

/* ── OSC helpers (defined before fm-synth.h which uses them) ────────── */

#define OSC_BUF_SIZE 2048

static inline int osc_pad4(int n) { return (n + 3) & ~3; }

static inline int32_t osc_read_i32(const uint8_t *b) {
    return (int32_t)((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
}

static inline float osc_read_f32(const uint8_t *b) {
    union { uint32_t u; float f; } conv;
    conv.u = (uint32_t)((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
    return conv.f;
}

static inline void osc_write_i32(uint8_t *b, int32_t v) {
    b[0] = (v >> 24) & 0xFF;
    b[1] = (v >> 16) & 0xFF;
    b[2] = (v >> 8)  & 0xFF;
    b[3] =  v        & 0xFF;
}

static inline void osc_write_f32(uint8_t *b, float v) {
    union { uint32_t u; float f; } conv;
    conv.f = v;
    osc_write_i32(b, (int32_t)conv.u);
}

static int osc_write_string(uint8_t *buf, int max, const char *str) {
    int len = (int)strlen(str) + 1;
    int padded = osc_pad4(len);
    if (padded > max) return -1;
    memcpy(buf, str, (size_t)len);
    memset(buf + len, 0, (size_t)(padded - len));
    return padded;
}

/* ── Instrument headers ─────────────────────────────────────────────── */

#include "instruments.h"
#include "fm-synth.h"
#include "ym2413.h"

/* ── Constants ──────────────────────────────────────────────────────── */

#define SAMPLE_RATE      48000
#define CHANNELS         2
#define DEFAULT_PERIOD   64
#define DEFAULT_OSC_PORT 9000
#define DEFAULT_HTTP_PORT 8080

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LIMITER_THRESHOLD 0.7f
#define LIMITER_CEILING   0.95f

/* ── HTTP / SSE Server Constants ───────────────────────────────────── */

#define MAX_HTTP_CLIENTS  16
#define HTTP_BUF_SIZE     8192
#define SSE_BUF_SIZE      16384

/* ── Shared Memory Bus ──────────────────────────────────────────────── */

#define WAVEOS_BUS_SLOTS     8
#define WAVEOS_BUS_RING_SIZE 4096
#define WAVEOS_BUS_SHM_NAME  "/waveos-bus"

typedef struct {
    _Atomic int64_t write_pos;
    float           ring[WAVEOS_BUS_RING_SIZE * 2];
    char            name[32];
    _Atomic int     active;
} WaveosBusSlot;

typedef struct {
    uint32_t        magic;
    int             sample_rate;
    WaveosBusSlot   slots[WAVEOS_BUS_SLOTS];
} WaveosBus;

/* ── Globals ────────────────────────────────────────────────────────── */

static volatile int g_quit = 0;

static void sighandler(int sig) {
    (void)sig;
    g_quit = 1;
}

/* ── Master Limiter ─────────────────────────────────────────────────── */

static float g_master_limiter_env = 0.0f;

static inline float master_limiter(float sample) {
    float absval = fabsf(sample);

    if (absval > g_master_limiter_env)
        g_master_limiter_env += (absval - g_master_limiter_env) * 0.01f;
    else
        g_master_limiter_env += (absval - g_master_limiter_env) * 0.0001f;

    float gain = 1.0f;
    if (g_master_limiter_env > LIMITER_THRESHOLD) {
        gain = LIMITER_THRESHOLD / g_master_limiter_env;
    }

    sample *= gain;

    if (fabsf(sample) > LIMITER_THRESHOLD) {
        sample = tanhf(sample) * LIMITER_CEILING;
    }

    return sample;
}

/* ── Instrument Registry ────────────────────────────────────────────── */

#define MAX_INSTRUMENT_TYPES 32

static InstrumentType *g_type_registry[MAX_INSTRUMENT_TYPES];
static int             g_n_types = 0;

static void rack_register_type(InstrumentType *type) {
    if (g_n_types < MAX_INSTRUMENT_TYPES) {
        g_type_registry[g_n_types++] = type;
        fprintf(stderr, "[miniwave] registered type: %s (%s)\n",
                type->name, type->display_name);
    }
}

static int rack_find_type(const char *name) {
    for (int i = 0; i < g_n_types; i++) {
        if (strcmp(g_type_registry[i]->name, name) == 0) return i;
    }
    return -1;
}

/* ── Rack Management ────────────────────────────────────────────────── */

static Rack g_rack;
static char g_midi_device_name[128] = "";

static void rack_init(void) {
    memset(&g_rack, 0, sizeof(g_rack));
    g_rack.master_volume = 0.8f;
    for (int i = 0; i < MAX_SLOTS; i++) {
        g_rack.slots[i].active = 0;
        g_rack.slots[i].type_idx = -1;
        g_rack.slots[i].state = NULL;
        g_rack.slots[i].volume = 1.0f;
        g_rack.slots[i].mute = 0;
        g_rack.slots[i].solo = 0;
    }

    /* Register built-in types */
    rack_register_type(&fm_synth_type);
    rack_register_type(&ym2413_type);
}

static int rack_set_slot(int channel, const char *type_name) {
    if (channel < 0 || channel >= MAX_SLOTS) return -1;

    int tidx = rack_find_type(type_name);
    if (tidx < 0) {
        fprintf(stderr, "[miniwave] unknown instrument type: %s\n", type_name);
        return -1;
    }

    RackSlot *slot = &g_rack.slots[channel];

    /* Clear existing instrument if any */
    if (slot->active && slot->state) {
        InstrumentType *old_type = g_type_registry[slot->type_idx];
        old_type->destroy(slot->state);
        free(slot->state);
        slot->state = NULL;
        slot->active = 0;
    }

    InstrumentType *itype = g_type_registry[tidx];
    void *state = calloc(1, itype->state_size);
    if (!state) return -1;

    itype->init(state);

    slot->state = state;
    slot->type_idx = tidx;
    slot->volume = 1.0f;
    slot->mute = 0;
    slot->solo = 0;
    slot->active = 1;

    fprintf(stderr, "[miniwave] slot %d = %s\n", channel, itype->display_name);
    return 0;
}

static void rack_clear_slot(int channel) {
    if (channel < 0 || channel >= MAX_SLOTS) return;

    RackSlot *slot = &g_rack.slots[channel];
    if (slot->active && slot->state) {
        InstrumentType *itype = g_type_registry[slot->type_idx];
        itype->destroy(slot->state);
        free(slot->state);
    }
    slot->state = NULL;
    slot->type_idx = -1;
    slot->active = 0;
    slot->volume = 1.0f;
    slot->mute = 0;
    slot->solo = 0;

    fprintf(stderr, "[miniwave] slot %d cleared\n", channel);
}

/* ══════════════════════════════════════════════════════════════════════
 *  ALSA Sequencer MIDI (snd_seq)
 *  Uses the sequencer API so multiple apps can share the same device.
 *  Device addresses are "client:port" (e.g. "20:0") not "hw:1,0".
 * ══════════════════════════════════════════════════════════════════════ */

static snd_seq_t        *g_seq = NULL;
static int               g_seq_port = -1;   /* our input port */
static snd_seq_addr_t    g_seq_src = {0,0}; /* currently subscribed source */
static int               g_seq_connected = 0;

/* Open the sequencer client (once at startup) */
static int seq_init(void) {
    int err = snd_seq_open(&g_seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "[miniwave] can't open ALSA sequencer: %s\n", snd_strerror(err));
        return -1;
    }
    snd_seq_set_client_name(g_seq, "miniwave");

    g_seq_port = snd_seq_create_simple_port(g_seq, "MIDI In",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (g_seq_port < 0) {
        fprintf(stderr, "[miniwave] can't create seq port: %s\n", snd_strerror(g_seq_port));
        snd_seq_close(g_seq);
        g_seq = NULL;
        return -1;
    }

    fprintf(stderr, "[miniwave] ALSA seq client %d:%d (miniwave:MIDI In)\n",
            snd_seq_client_id(g_seq), g_seq_port);
    return 0;
}

/* Subscribe to a source port ("client:port" string) */
static int seq_connect(const char *addr_str) {
    if (!g_seq) return -1;

    /* Disconnect old */
    if (g_seq_connected) {
        snd_seq_disconnect_from(g_seq, g_seq_port, g_seq_src.client, g_seq_src.port);
        g_seq_connected = 0;
        g_midi_device_name[0] = '\0';
    }

    /* Parse "client:port" */
    snd_seq_addr_t addr;
    int err = snd_seq_parse_address(g_seq, &addr, addr_str);
    if (err < 0) {
        fprintf(stderr, "[miniwave] bad MIDI address '%s': %s\n",
                addr_str, snd_strerror(err));
        return -1;
    }

    err = snd_seq_connect_from(g_seq, g_seq_port, addr.client, addr.port);
    if (err < 0) {
        fprintf(stderr, "[miniwave] can't subscribe to %s: %s\n",
                addr_str, snd_strerror(err));
        return -1;
    }

    g_seq_src = addr;
    g_seq_connected = 1;

    /* Get client name for display */
    snd_seq_client_info_t *cinfo;
    snd_seq_client_info_alloca(&cinfo);
    if (snd_seq_get_any_client_info(g_seq, addr.client, cinfo) >= 0) {
        snprintf(g_midi_device_name, sizeof(g_midi_device_name), "%s (%d:%d)",
                 snd_seq_client_info_get_name(cinfo), addr.client, addr.port);
    } else {
        snprintf(g_midi_device_name, sizeof(g_midi_device_name), "%d:%d",
                 addr.client, addr.port);
    }

    fprintf(stderr, "[miniwave] MIDI connected: %s\n", g_midi_device_name);
    return 0;
}

/* Disconnect current source */
static void seq_disconnect(void) {
    if (!g_seq || !g_seq_connected) return;
    snd_seq_disconnect_from(g_seq, g_seq_port, g_seq_src.client, g_seq_src.port);
    g_seq_connected = 0;
    g_midi_device_name[0] = '\0';
    fprintf(stderr, "[miniwave] MIDI disconnected\n");
}

/* List all sequencer input ports. Returns count. */
static int list_midi_devices(char devices[][64], char names[][128], int max_devices) {
    if (!g_seq) return 0;
    int count = 0;

    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(g_seq, cinfo) >= 0 && count < max_devices) {
        int client = snd_seq_client_info_get_client(cinfo);
        const char *cname = snd_seq_client_info_get_name(cinfo);

        /* Skip ourselves and the System client */
        if (client == snd_seq_client_id(g_seq)) continue;
        if (client == 0) continue;

        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(g_seq, pinfo) >= 0 && count < max_devices) {
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            /* We want ports that can output (i.e. we can read from them) */
            if (!(caps & SND_SEQ_PORT_CAP_READ)) continue;
            if (!(caps & SND_SEQ_PORT_CAP_SUBS_READ)) continue;

            int port = snd_seq_port_info_get_port(pinfo);
            const char *pname = snd_seq_port_info_get_name(pinfo);

            /* Skip Midi Through */
            if (cname && strstr(cname, "Midi Through")) continue;

            snprintf(devices[count], 64, "%d:%d", client, port);
            snprintf(names[count], 128, "%s — %s",
                     cname ? cname : "?", pname ? pname : "?");
            count++;
        }
    }
    return count;
}

/* Dispatch a single sequencer event to the rack */
static inline void seq_dispatch(snd_seq_event_t *ev) {
    int ch = ev->data.note.channel; /* works for note, control, pgmchange */
    if (ch < 0 || ch >= MAX_SLOTS) return;

    RackSlot *slot = &g_rack.slots[ch];
    if (!slot->active || !slot->state) return;
    InstrumentType *itype = g_type_registry[slot->type_idx];

    switch (ev->type) {
    case SND_SEQ_EVENT_NOTEON:
        itype->midi(slot->state,
                    (uint8_t)(0x90 | ch),
                    (uint8_t)ev->data.note.note,
                    (uint8_t)ev->data.note.velocity);
        break;
    case SND_SEQ_EVENT_NOTEOFF:
        itype->midi(slot->state,
                    (uint8_t)(0x80 | ch),
                    (uint8_t)ev->data.note.note,
                    0);
        break;
    case SND_SEQ_EVENT_CONTROLLER:
        itype->midi(slot->state,
                    (uint8_t)(0xB0 | ch),
                    (uint8_t)ev->data.control.param,
                    (uint8_t)ev->data.control.value);
        break;
    case SND_SEQ_EVENT_PGMCHANGE:
        itype->midi(slot->state,
                    (uint8_t)(0xC0 | ch),
                    (uint8_t)ev->data.control.value,
                    0);
        break;
    case SND_SEQ_EVENT_PITCHBEND: {
        /* snd_seq pitch bend: -8192..8191 → 14-bit 0..16383 */
        int val = ev->data.control.value + 8192;
        if (val < 0) val = 0;
        if (val > 16383) val = 16383;
        itype->midi(slot->state,
                    (uint8_t)(0xE0 | ch),
                    (uint8_t)(val & 0x7F),
                    (uint8_t)((val >> 7) & 0x7F));
        break;
    }
    case SND_SEQ_EVENT_CHANPRESS:
        itype->midi(slot->state,
                    (uint8_t)(0xD0 | ch),
                    (uint8_t)ev->data.control.value,
                    0);
        break;
    default:
        break;
    }
}

/* ── MIDI Thread (sequencer event loop) ────────────────────────────── */

typedef struct {
    int dummy; /* seq handle is global */
} MidiThreadCtx;

static void *midi_thread_fn(void *arg) {
    (void)arg;
    if (!g_seq) return NULL;

    int npfds = snd_seq_poll_descriptors_count(g_seq, POLLIN);
    struct pollfd *pfds = calloc((size_t)npfds, sizeof(struct pollfd));
    if (!pfds) return NULL;
    snd_seq_poll_descriptors(g_seq, pfds, (unsigned int)npfds, POLLIN);

    while (!g_quit) {
        int ret = poll(pfds, (nfds_t)npfds, 50);
        if (ret <= 0) continue;

        snd_seq_event_t *ev = NULL;
        while (snd_seq_event_input(g_seq, &ev) >= 0 && ev) {
            seq_dispatch(ev);
        }
    }

    free(pfds);
    return NULL;
}

/* ── OSC Server Thread ──────────────────────────────────────────────── */

typedef struct {
    int          port;
} OscThreadCtx;

static void *osc_thread_fn(void *arg) {
    OscThreadCtx *ctx = (OscThreadCtx *)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "[miniwave] OSC: socket error: %s\n", strerror(errno));
        return NULL;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)ctx->port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[miniwave] OSC: bind error on port %d: %s\n",
                ctx->port, strerror(errno));
        close(sock);
        return NULL;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    fprintf(stderr, "[miniwave] OSC listening on port %d\n", ctx->port);

    uint8_t buf[OSC_BUF_SIZE];
    struct sockaddr_in sender;
    socklen_t sender_len = 0;

    while (!g_quit) {
        sender_len = sizeof(sender);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&sender, &sender_len);
        if (n <= 0) continue;

        /* Parse OSC address */
        if (buf[0] != '/') continue;
        int addr_end = 0;
        while (addr_end < n && buf[addr_end] != '\0') addr_end++;
        if (addr_end >= n) continue;
        const char *osc_addr = (const char *)buf;
        int pos = osc_pad4(addr_end + 1);

        /* Parse type tag string */
        const char *types = "";
        int n_args = 0;
        if (pos < n && buf[pos] == ',') {
            types = (const char *)&buf[pos + 1];
            int tag_end = pos;
            while (tag_end < n && buf[tag_end] != '\0') tag_end++;
            n_args = tag_end - pos - 1;
            pos = osc_pad4(tag_end + 1);
        }

        /* Read args */
        int32_t arg_i[8] = {0};
        float   arg_f[8] = {0};
        char    arg_s[4][128];
        int ai = 0, afi = 0, asi = 0;
        memset(arg_s, 0, sizeof(arg_s));

        for (int t = 0; t < n_args && pos < (int)n; t++) {
            switch (types[t]) {
            case 'i':
                if (pos + 4 <= (int)n && ai < 8)
                    arg_i[ai++] = osc_read_i32(&buf[pos]);
                pos += 4;
                break;
            case 'f':
                if (pos + 4 <= (int)n && afi < 8)
                    arg_f[afi++] = osc_read_f32(&buf[pos]);
                pos += 4;
                break;
            case 's': {
                int sstart = pos;
                while (pos < (int)n && buf[pos] != '\0') pos++;
                if (asi < 4) {
                    int slen = pos - sstart;
                    if (slen > 127) slen = 127;
                    memcpy(arg_s[asi], &buf[sstart], (size_t)slen);
                    arg_s[asi][slen] = '\0';
                    asi++;
                }
                pos = osc_pad4(pos + 1);
            } break;
            default:
                break;
            }
        }

        /* ── Dispatch ───────────────────────────────────────────── */

        /* /rack/slot/set  is  — set channel to instrument type */
        if (strcmp(osc_addr, "/rack/slot/set") == 0 && ai >= 1 && asi >= 1) {
            rack_set_slot(arg_i[0], arg_s[0]);
        }
        /* /rack/slot/clear  i */
        else if (strcmp(osc_addr, "/rack/slot/clear") == 0 && ai >= 1) {
            rack_clear_slot(arg_i[0]);
        }
        /* /rack/slot/volume  if */
        else if (strcmp(osc_addr, "/rack/slot/volume") == 0 && ai >= 1 && afi >= 1) {
            int ch = arg_i[0];
            if (ch >= 0 && ch < MAX_SLOTS) {
                float vol = arg_f[0];
                if (vol < 0.0f) vol = 0.0f;
                if (vol > 1.0f) vol = 1.0f;
                g_rack.slots[ch].volume = vol;
            }
        }
        /* /rack/slot/mute  ii */
        else if (strcmp(osc_addr, "/rack/slot/mute") == 0 && ai >= 2) {
            int ch = arg_i[0];
            if (ch >= 0 && ch < MAX_SLOTS) {
                g_rack.slots[ch].mute = arg_i[1] ? 1 : 0;
            }
        }
        /* /rack/slot/solo  ii */
        else if (strcmp(osc_addr, "/rack/slot/solo") == 0 && ai >= 2) {
            int ch = arg_i[0];
            if (ch >= 0 && ch < MAX_SLOTS) {
                g_rack.slots[ch].solo = arg_i[1] ? 1 : 0;
            }
        }
        /* /rack/master  f */
        else if (strcmp(osc_addr, "/rack/master") == 0 && afi >= 1) {
            float vol = arg_f[0];
            if (vol < 0.0f) vol = 0.0f;
            if (vol > 1.0f) vol = 1.0f;
            g_rack.master_volume = vol;
        }
        /* /rack/local_mute  i — mute ALSA output, bus-only mode */
        else if (strcmp(osc_addr, "/rack/local_mute") == 0 && ai >= 1) {
            g_rack.local_mute = arg_i[0] ? 1 : 0;
            fprintf(stderr, "[miniwave] local mute: %s (bus-only)\n",
                    g_rack.local_mute ? "ON" : "OFF");
        }
        /* /rack/status */
        else if (strcmp(osc_addr, "/rack/status") == 0) {
            uint8_t reply[OSC_BUF_SIZE];
            int rpos = 0;
            int w;

            w = osc_write_string(reply, (int)sizeof(reply), "/rack/status");
            if (w < 0) continue;
            rpos += w;

            /* Build type tag: for each slot isifi, then f (master_volume) s (midi_device) */
            char ttag[256] = ",";
            int tpos = 1;
            for (int i = 0; i < MAX_SLOTS; i++) {
                /* active(i) type_name(s) volume(f) mute(i) solo(i) */
                ttag[tpos++] = 'i';
                ttag[tpos++] = 's';
                ttag[tpos++] = 'f';
                ttag[tpos++] = 'i';
                ttag[tpos++] = 'i';
            }
            ttag[tpos++] = 'f'; /* master_volume */
            ttag[tpos++] = 's'; /* midi_device */
            ttag[tpos] = '\0';

            w = osc_write_string(reply + rpos, (int)sizeof(reply) - rpos, ttag);
            if (w < 0) continue;
            rpos += w;

            /* Slot data */
            for (int i = 0; i < MAX_SLOTS; i++) {
                RackSlot *slot = &g_rack.slots[i];
                if (rpos + 4 > (int)sizeof(reply)) break;
                osc_write_i32(reply + rpos, slot->active); rpos += 4;

                const char *tname = "";
                if (slot->active && slot->type_idx >= 0 && slot->type_idx < g_n_types)
                    tname = g_type_registry[slot->type_idx]->name;
                w = osc_write_string(reply + rpos, (int)sizeof(reply) - rpos, tname);
                if (w < 0) break;
                rpos += w;

                if (rpos + 12 > (int)sizeof(reply)) break;
                osc_write_f32(reply + rpos, slot->volume); rpos += 4;
                osc_write_i32(reply + rpos, slot->mute);   rpos += 4;
                osc_write_i32(reply + rpos, slot->solo);   rpos += 4;
            }

            if (rpos + 4 <= (int)sizeof(reply)) {
                osc_write_f32(reply + rpos, g_rack.master_volume); rpos += 4;
            }
            w = osc_write_string(reply + rpos, (int)sizeof(reply) - rpos, g_midi_device_name);
            if (w >= 0) rpos += w;

            sendto(sock, reply, (size_t)rpos, 0,
                   (struct sockaddr *)&sender, sender_len);
        }
        /* /rack/types */
        else if (strcmp(osc_addr, "/rack/types") == 0) {
            uint8_t reply[OSC_BUF_SIZE];
            int rpos = 0;
            int w;

            w = osc_write_string(reply, (int)sizeof(reply), "/rack/types");
            if (w < 0) continue;
            rpos += w;

            /* Type tag: one 's' per type */
            char ttag[128] = ",";
            int tpos = 1;
            for (int i = 0; i < g_n_types && tpos < 126; i++) {
                ttag[tpos++] = 's';
            }
            ttag[tpos] = '\0';

            w = osc_write_string(reply + rpos, (int)sizeof(reply) - rpos, ttag);
            if (w < 0) continue;
            rpos += w;

            for (int i = 0; i < g_n_types; i++) {
                w = osc_write_string(reply + rpos, (int)sizeof(reply) - rpos,
                                     g_type_registry[i]->name);
                if (w < 0) break;
                rpos += w;
            }

            sendto(sock, reply, (size_t)rpos, 0,
                   (struct sockaddr *)&sender, sender_len);
        }
        /* /midi/devices */
        else if (strcmp(osc_addr, "/midi/devices") == 0) {
            char devs[16][64];
            char devnames[16][128];
            int ndevs = list_midi_devices(devs, devnames, 16);

            uint8_t reply[OSC_BUF_SIZE];
            int rpos = 0;
            int w;

            w = osc_write_string(reply, (int)sizeof(reply), "/midi/devices");
            if (w < 0) continue;
            rpos += w;

            char ttag[128] = ",";
            int tpos = 1;
            for (int i = 0; i < ndevs && tpos < 124; i++) {
                ttag[tpos++] = 's';
                ttag[tpos++] = 's';
            }
            ttag[tpos] = '\0';

            w = osc_write_string(reply + rpos, (int)sizeof(reply) - rpos, ttag);
            if (w < 0) continue;
            rpos += w;

            for (int i = 0; i < ndevs; i++) {
                w = osc_write_string(reply + rpos, (int)sizeof(reply) - rpos, devs[i]);
                if (w < 0) break;
                rpos += w;
                w = osc_write_string(reply + rpos, (int)sizeof(reply) - rpos, devnames[i]);
                if (w < 0) break;
                rpos += w;
            }

            sendto(sock, reply, (size_t)rpos, 0,
                   (struct sockaddr *)&sender, sender_len);
        }
        /* /midi/device  s  — subscribe to MIDI source (client:port) */
        else if (strcmp(osc_addr, "/midi/device") == 0 && asi >= 1) {
            seq_connect(arg_s[0]);
        }
        /* /midi/disconnect — unsubscribe from current MIDI source */
        else if (strcmp(osc_addr, "/midi/disconnect") == 0) {
            seq_disconnect();
        }
        /* /ch/N/... — per-instrument OSC */
        else if (strncmp(osc_addr, "/ch/", 4) == 0) {
            /* Parse channel number */
            const char *p = osc_addr + 4;
            char *end = NULL;
            long ch = strtol(p, &end, 10);
            if (end && end != p && ch >= 0 && ch < MAX_SLOTS) {
                const char *sub_path = end; /* e.g. "/preset", "/param/mod_index", "/status" */
                RackSlot *slot = &g_rack.slots[ch];

                if (slot->active && slot->state) {
                    if (strcmp(sub_path, "/status") == 0) {
                        /* Call instrument's osc_status */
                        InstrumentType *itype = g_type_registry[slot->type_idx];
                        uint8_t reply[512];
                        int rlen = itype->osc_status(slot->state, reply, (int)sizeof(reply));
                        if (rlen > 0) {
                            sendto(sock, reply, (size_t)rlen, 0,
                                   (struct sockaddr *)&sender, sender_len);
                        }
                    } else {
                        /* Forward to instrument's osc_handle */
                        InstrumentType *itype = g_type_registry[slot->type_idx];
                        itype->osc_handle(slot->state, sub_path,
                                          arg_i, ai, arg_f, afi);
                    }
                }
            }
        }
    }

    close(sock);
    return NULL;
}

/* ── Bus Write ──────────────────────────────────────────────────────── */

static void bus_write(WaveosBus *bus, int slot, float *stereo_frames, int count) {
    WaveosBusSlot *s = &bus->slots[slot];
    int64_t wp = atomic_load(&s->write_pos);
    for (int i = 0; i < count; i++) {
        int idx = (int)((wp + i) % WAVEOS_BUS_RING_SIZE) * 2;
        s->ring[idx]     = stereo_frames[i * 2];
        s->ring[idx + 1] = stereo_frames[i * 2 + 1];
    }
    atomic_store(&s->write_pos, wp + count);
}

/* ── HTTP / SSE Server ──────────────────────────────────────────────── */

/* Loaded from disk at startup */
static char *g_html_content = NULL;
static size_t g_html_length = 0;

typedef struct {
    int  fd;
    int  is_sse;           /* 1 = SSE stream client */
    int  detail_channel;   /* which channel for detail polling, -1 = none */
} HttpClient;

static HttpClient g_http_clients[MAX_HTTP_CLIENTS];
static pthread_mutex_t g_http_lock = PTHREAD_MUTEX_INITIALIZER;

/* Resolve executable directory and load web/index.html */
static void http_load_html(void) {
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) {
        fprintf(stderr, "[http] can't resolve /proc/self/exe\n");
        return;
    }
    exe_path[len] = '\0';

    /* Strip binary name to get directory */
    char *slash = strrchr(exe_path, '/');
    if (slash) *(slash + 1) = '\0';

    char html_path[1280];
    snprintf(html_path, sizeof(html_path), "%sweb/index.html", exe_path);

    FILE *f = fopen(html_path, "r");
    if (!f) {
        fprintf(stderr, "[http] index.html not found at %s\n", html_path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 2 * 1024 * 1024) {
        fprintf(stderr, "[http] index.html bad size: %ld\n", sz);
        fclose(f);
        return;
    }

    g_html_content = malloc((size_t)sz + 1);
    if (!g_html_content) { fclose(f); return; }

    g_html_length = fread(g_html_content, 1, (size_t)sz, f);
    g_html_content[g_html_length] = '\0';
    fclose(f);

    fprintf(stderr, "[http] loaded index.html (%zu bytes)\n", g_html_length);
}

/* ── JSON helpers (simple string-based extraction) ─────────────────── */

static int json_get_string(const char *json, const char *key, char *out, int max) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < max - 1) {
        if (*p == '\\' && *(p + 1)) { p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int json_get_int(const char *json, const char *key, int *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') { /* string-encoded number */
        p++;
        *out = atoi(p);
        return 0;
    }
    if ((*p >= '0' && *p <= '9') || *p == '-') {
        *out = atoi(p);
        return 0;
    }
    return -1;
}

static int json_get_float(const char *json, const char *key, float *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
        *out = strtof(p, NULL);
        return 0;
    }
    return -1;
}

/* Extract first int from a JSON array field, e.g. "iargs":[42] */
static int json_get_iarray_first(const char *json, const char *key, int *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '[') return -1;
    p++;
    while (*p == ' ') p++;
    if ((*p >= '0' && *p <= '9') || *p == '-') {
        *out = atoi(p);
        return 0;
    }
    return -1;
}

static int json_get_farray_first(const char *json, const char *key, float *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '[') return -1;
    p++;
    while (*p == ' ') p++;
    if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
        *out = strtof(p, NULL);
        return 0;
    }
    return -1;
}

/* ── Build JSON responses from rack state ──────────────────────────── */

static int build_rack_status_json(char *buf, int max) {
    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "{\"type\":\"rack_status\",\"slots\":[");

    for (int i = 0; i < MAX_SLOTS; i++) {
        RackSlot *slot = &g_rack.slots[i];
        const char *tname = "";
        if (slot->active && slot->type_idx >= 0 && slot->type_idx < g_n_types)
            tname = g_type_registry[slot->type_idx]->name;

        pos += snprintf(buf + pos, (size_t)(max - pos),
            "%s{\"active\":%d,\"type\":\"%s\",\"volume\":%.4f,\"mute\":%d,\"solo\":%d}",
            i ? "," : "", slot->active, tname, (double)slot->volume,
            slot->mute, slot->solo);
    }

    pos += snprintf(buf + pos, (size_t)(max - pos),
        "],\"master_volume\":%.4f,\"midi_device\":\"%s\"}",
        (double)g_rack.master_volume, g_midi_device_name);

    return pos;
}

static int build_rack_types_json(char *buf, int max) {
    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(max - pos), "{\"type\":\"rack_types\",\"types\":[");
    for (int i = 0; i < g_n_types; i++) {
        pos += snprintf(buf + pos, (size_t)(max - pos),
            "%s\"%s\"", i ? "," : "", g_type_registry[i]->name);
    }
    pos += snprintf(buf + pos, (size_t)(max - pos), "]}");
    return pos;
}

static int build_midi_devices_json(char *buf, int max) {
    char devs[16][64];
    char devnames[16][128];
    int ndevs = list_midi_devices(devs, devnames, 16);

    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(max - pos), "{\"type\":\"midi_devices\",\"devices\":[");
    for (int i = 0; i < ndevs; i++) {
        /* Escape any special chars in name */
        pos += snprintf(buf + pos, (size_t)(max - pos),
            "%s{\"id\":\"%s\",\"name\":\"%s\"}", i ? "," : "",
            devs[i], devnames[i]);
    }
    pos += snprintf(buf + pos, (size_t)(max - pos), "]}");
    return pos;
}

static int build_ch_status_json(int ch, char *buf, int max) {
    RackSlot *slot = &g_rack.slots[ch];
    if (!slot->active || !slot->state || slot->type_idx < 0)
        return snprintf(buf, (size_t)max,
            "{\"type\":\"ch_status\",\"channel\":%d}", ch);

    InstrumentType *itype = g_type_registry[slot->type_idx];

    /* Dispatch by instrument type */
    if (strcmp(itype->name, "ym2413") == 0) {
        YM2413State *y = (YM2413State *)slot->state;
        const char *inst_names[] = {
            "Custom","Violin","Guitar","Piano","Flute","Clarinet","Oboe",
            "Trumpet","Organ","Horn","Synthesizer","Harpsichord",
            "Vibraphone","Synth Bass","Acoustic Bass","Electric Guitar"
        };
        const char *iname = (y->current_instrument >= 0 && y->current_instrument <= 15)
                            ? inst_names[y->current_instrument] : "Unknown";
        int active_ch = 0;
        int nch = y->rhythm_mode ? 6 : 9;
        for (int i = 0; i < nch; i++)
            if (y->channels[i].key_on) active_ch++;

        return snprintf(buf, (size_t)max,
            "{\"type\":\"ch_status\",\"channel\":%d,"
            "\"instrument_type\":\"ym2413\","
            "\"preset_index\":%d,\"preset_name\":\"%s\","
            "\"rhythm_mode\":%d,"
            "\"active_voices\":%d}",
            ch, y->current_instrument, iname,
            y->rhythm_mode, active_ch);
    }

    /* Default: FM synth */
    FMSynth *s = (FMSynth *)slot->state;
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
        "{\"type\":\"ch_status\",\"channel\":%d,"
        "\"instrument_type\":\"fm-synth\","
        "\"preset_index\":%d,\"preset_name\":\"%s\","
        "\"volume\":%.4f,\"override\":%d,"
        "\"params\":{"
        "\"carrier_ratio\":%.4f,\"mod_ratio\":%.4f,\"mod_index\":%.4f,"
        "\"attack\":%.4f,\"decay\":%.4f,\"sustain\":%.4f,"
        "\"release\":%.4f,\"feedback\":%.4f},"
        "\"active_voices\":%d}",
        ch, s->current_preset, pname,
        (double)s->volume, s->live_params.override,
        (double)params[0], (double)params[1], (double)params[2],
        (double)params[3], (double)params[4], (double)params[5],
        (double)params[6], (double)params[7],
        active_voices);
}

static const char *OSC_SPEC_JSON =
    "{\"name\":\"miniwave\",\"version\":\"1.0\","
    "\"transports\":[\"osc-udp\",\"http-sse\"],"
    "\"osc_port\":9000,\"http_port\":8080,"
    "\"endpoints\":["
    "{\"path\":\"/rack/slot/set\",\"args\":\"is\",\"desc\":\"Set slot instrument\"},"
    "{\"path\":\"/rack/slot/clear\",\"args\":\"i\",\"desc\":\"Clear slot\"},"
    "{\"path\":\"/rack/slot/volume\",\"args\":\"if\",\"desc\":\"Set slot volume\"},"
    "{\"path\":\"/rack/slot/mute\",\"args\":\"ii\",\"desc\":\"Mute slot\"},"
    "{\"path\":\"/rack/slot/solo\",\"args\":\"ii\",\"desc\":\"Solo slot\"},"
    "{\"path\":\"/rack/master\",\"args\":\"f\",\"desc\":\"Master volume\"},"
    "{\"path\":\"/rack/status\",\"args\":\"\",\"desc\":\"Get rack status\"},"
    "{\"path\":\"/rack/types\",\"args\":\"\",\"desc\":\"Get instrument types\"},"
    "{\"path\":\"/ch/N/preset\",\"args\":\"i\",\"desc\":\"Set preset\"},"
    "{\"path\":\"/ch/N/param/KEY\",\"args\":\"f\",\"desc\":\"Set parameter\"}"
    "]}";

/* ── HTTP response helpers ─────────────────────────────────────────── */

/* Suppress warn_unused_result for fire-and-forget network writes */
static inline ssize_t http_write(int fd, const void *buf, size_t len) {
    ssize_t r = write(fd, buf, len);
    return r; /* caller may ignore */
}

static void http_send_response(int fd, int status, const char *content_type,
                                const char *body, int body_len) {
    const char *status_text = (status == 200) ? "OK" :
                              (status == 204) ? "No Content" :
                              (status == 404) ? "Not Found" :
                              (status == 405) ? "Method Not Allowed" :
                              "Error";
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);

    http_write(fd, header, (size_t)hlen);
    if (body && body_len > 0)
        http_write(fd, body, (size_t)body_len);
}

static void http_send_sse_headers(int fd) {
    const char *resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    http_write(fd, resp, strlen(resp));
}

static int sse_send_event(int fd, const char *event, const char *data) {
    char buf[SSE_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf), "event: %s\ndata: %s\n\n", event, data);
    if (len <= 0 || len >= (int)sizeof(buf)) return -1;
    ssize_t w = write(fd, buf, (size_t)len);
    return (w > 0) ? 0 : -1;
}

/* Send SSE event to all connected SSE clients */
static void sse_broadcast(const char *event, const char *json_data) {
    pthread_mutex_lock(&g_http_lock);
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        if (g_http_clients[i].fd >= 0 && g_http_clients[i].is_sse) {
            if (sse_send_event(g_http_clients[i].fd, event, json_data) < 0) {
                close(g_http_clients[i].fd);
                g_http_clients[i].fd = -1;
                g_http_clients[i].is_sse = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_http_lock);
}

/* ── POST /api handler ─────────────────────────────────────────────── */

static void http_handle_api(int fd, const char *body) {
    char type_str[64] = "";
    json_get_string(body, "type", type_str, sizeof(type_str));

    char resp[SSE_BUF_SIZE];
    int rlen = 0;

    if (strcmp(type_str, "slot_set") == 0) {
        int ch = 0;
        char instr[64] = "";
        json_get_int(body, "channel", &ch);
        json_get_string(body, "instrument", instr, sizeof(instr));
        rack_set_slot(ch, instr);
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "slot_clear") == 0) {
        int ch = 0;
        json_get_int(body, "channel", &ch);
        rack_clear_slot(ch);
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "slot_volume") == 0) {
        int ch = 0;
        float val = 1.0f;
        json_get_int(body, "channel", &ch);
        json_get_float(body, "value", &val);
        if (ch >= 0 && ch < MAX_SLOTS) {
            if (val < 0.0f) val = 0.0f;
            if (val > 1.0f) val = 1.0f;
            g_rack.slots[ch].volume = val;
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "slot_mute") == 0) {
        int ch = 0, val = 0;
        json_get_int(body, "channel", &ch);
        json_get_int(body, "value", &val);
        if (ch >= 0 && ch < MAX_SLOTS)
            g_rack.slots[ch].mute = val ? 1 : 0;
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "slot_solo") == 0) {
        int ch = 0, val = 0;
        json_get_int(body, "channel", &ch);
        json_get_int(body, "value", &val);
        if (ch >= 0 && ch < MAX_SLOTS)
            g_rack.slots[ch].solo = val ? 1 : 0;
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "master_volume") == 0) {
        float val = 0.75f;
        json_get_float(body, "value", &val);
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;
        g_rack.master_volume = val;
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "local_mute") == 0) {
        int val = 0;
        json_get_int(body, "value", &val);
        g_rack.local_mute = val ? 1 : 0;
        fprintf(stderr, "[miniwave] local mute: %s (bus-only)\n",
                g_rack.local_mute ? "ON" : "OFF");
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "rack_status") == 0) {
        rlen = build_rack_status_json(resp, (int)sizeof(resp));
    }
    else if (strcmp(type_str, "rack_types") == 0) {
        rlen = build_rack_types_json(resp, (int)sizeof(resp));
    }
    else if (strcmp(type_str, "midi_devices") == 0) {
        rlen = build_midi_devices_json(resp, (int)sizeof(resp));
    }
    else if (strcmp(type_str, "ch_status") == 0) {
        int ch = 0;
        json_get_int(body, "channel", &ch);
        if (ch >= 0 && ch < MAX_SLOTS)
            rlen = build_ch_status_json(ch, resp, (int)sizeof(resp));
        else
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"invalid channel\"}");
    }
    else if (strcmp(type_str, "detail_close") == 0) {
        /* Client no longer wants detail updates — handled via SSE client state */
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "ch") == 0) {
        int ch = 0;
        char path[128] = "";
        json_get_int(body, "channel", &ch);
        json_get_string(body, "path", path, sizeof(path));

        if (ch >= 0 && ch < MAX_SLOTS) {
            RackSlot *slot = &g_rack.slots[ch];
            if (slot->active && slot->state) {
                InstrumentType *itype = g_type_registry[slot->type_idx];
                int32_t iargs[8] = {0};
                float fargs[8] = {0};
                int ni = 0, nf = 0;

                int ival;
                if (json_get_iarray_first(body, "iargs", &ival) == 0) {
                    iargs[0] = ival;
                    ni = 1;
                }
                float fval;
                if (json_get_farray_first(body, "fargs", &fval) == 0) {
                    fargs[0] = fval;
                    nf = 1;
                }

                itype->osc_handle(slot->state, path, iargs, ni, fargs, nf);
            }
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "note_on") == 0) {
        int ch = 0, note = 60, vel = 100;
        json_get_int(body, "channel", &ch);
        json_get_int(body, "note", &note);
        json_get_int(body, "velocity", &vel);
        if (ch >= 0 && ch < MAX_SLOTS) {
            RackSlot *slot = &g_rack.slots[ch];
            if (slot->active && slot->state) {
                InstrumentType *itype = g_type_registry[slot->type_idx];
                uint8_t status_byte = (uint8_t)(0x90 | (ch & 0x0F));
                itype->midi(slot->state, status_byte,
                            (uint8_t)note, (uint8_t)vel);
            }
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "note_off") == 0) {
        int ch = 0, note = 60;
        json_get_int(body, "channel", &ch);
        json_get_int(body, "note", &note);
        if (ch >= 0 && ch < MAX_SLOTS) {
            RackSlot *slot = &g_rack.slots[ch];
            if (slot->active && slot->state) {
                InstrumentType *itype = g_type_registry[slot->type_idx];
                uint8_t status_byte = (uint8_t)(0x80 | (ch & 0x0F));
                itype->midi(slot->state, status_byte, (uint8_t)note, 0);
            }
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "midi_device") == 0) {
        char dev[64] = "";
        json_get_string(body, "value", dev, sizeof(dev));
        if (dev[0]) {
            int err2 = seq_connect(dev);
            rlen = snprintf(resp, sizeof(resp), err2 == 0
                ? "{\"ok\":true}" : "{\"error\":\"can't connect\"}");
        } else {
            seq_disconnect();
            rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
        }
    }
    else {
        rlen = snprintf(resp, sizeof(resp), "{\"error\":\"unknown type: %s\"}", type_str);
    }

    http_send_response(fd, 200, "application/json", resp, rlen);
}

/* ── HTTP thread: accept, parse, route ─────────────────────────────── */

typedef struct {
    int port;
} HttpThreadCtx;

static void *http_thread_fn(void *arg) {
    HttpThreadCtx *ctx = (HttpThreadCtx *)arg;

    /* Init client slots */
    pthread_mutex_lock(&g_http_lock);
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        g_http_clients[i].fd = -1;
        g_http_clients[i].is_sse = 0;
        g_http_clients[i].detail_channel = -1;
    }
    pthread_mutex_unlock(&g_http_lock);

    /* Create listen socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "[http] socket error: %s\n", strerror(errno));
        return NULL;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)ctx->port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[http] bind error on port %d: %s\n",
                ctx->port, strerror(errno));
        close(listen_fd);
        return NULL;
    }

    if (listen(listen_fd, 8) < 0) {
        fprintf(stderr, "[http] listen error: %s\n", strerror(errno));
        close(listen_fd);
        return NULL;
    }

    /* Make listen socket non-blocking */
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);

    fprintf(stderr, "[http] WaveUI at http://0.0.0.0:%d\n", ctx->port);

    /* Polling loop: accept new connections + push SSE updates */
    struct timespec last_rack_poll = {0, 0};
    struct timespec last_detail_poll = {0, 0};

    while (!g_quit) {
        /* Accept new connections (non-blocking) */
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd >= 0) {
            /* Set recv timeout so we don't block forever reading the request */
            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            /* Disable Nagle for responsiveness */
            int flag = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            /* Read HTTP request */
            char reqbuf[HTTP_BUF_SIZE];
            memset(reqbuf, 0, sizeof(reqbuf));
            ssize_t nread = recv(client_fd, reqbuf, sizeof(reqbuf) - 1, 0);

            if (nread > 0) {
                reqbuf[nread] = '\0';

                /* Parse method and path */
                char method[8] = "";
                char path[256] = "";
                sscanf(reqbuf, "%7s %255s", method, path);

                /* Handle CORS preflight */
                if (strcmp(method, "OPTIONS") == 0) {
                    http_send_response(client_fd, 204, "text/plain", "", 0);
                    close(client_fd);
                }
                /* GET / — serve index.html */
                else if (strcmp(method, "GET") == 0 &&
                         (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
                    if (g_html_content) {
                        http_send_response(client_fd, 200, "text/html; charset=utf-8",
                                           g_html_content, (int)g_html_length);
                    } else {
                        const char *fallback =
                            "<html><body style='background:#111;color:#e0e0e0;"
                            "font-family:monospace;padding:40px'>"
                            "<h1>WaveUI not found</h1>"
                            "<p>Place web/index.html next to the miniwave binary.</p>"
                            "</body></html>";
                        http_send_response(client_fd, 200, "text/html",
                                           fallback, (int)strlen(fallback));
                    }
                    close(client_fd);
                }
                /* GET /osc-spec — serve OSC spec JSON */
                else if (strcmp(method, "GET") == 0 && strcmp(path, "/osc-spec") == 0) {
                    http_send_response(client_fd, 200, "application/json",
                                       OSC_SPEC_JSON, (int)strlen(OSC_SPEC_JSON));
                    close(client_fd);
                }
                /* GET /events — SSE stream */
                else if (strcmp(method, "GET") == 0 && strcmp(path, "/events") == 0) {
                    /* Make non-blocking for SSE */
                    fcntl(client_fd, F_SETFL, O_NONBLOCK);

                    http_send_sse_headers(client_fd);

                    /* Register as SSE client */
                    int registered = 0;
                    pthread_mutex_lock(&g_http_lock);
                    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
                        if (g_http_clients[i].fd < 0) {
                            g_http_clients[i].fd = client_fd;
                            g_http_clients[i].is_sse = 1;
                            g_http_clients[i].detail_channel = -1;
                            registered = 1;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&g_http_lock);

                    if (!registered) {
                        close(client_fd);
                    } else {
                        /* Send initial state immediately */
                        char json[SSE_BUF_SIZE];
                        build_rack_status_json(json, (int)sizeof(json));
                        sse_send_event(client_fd, "rack_status", json);

                        build_rack_types_json(json, (int)sizeof(json));
                        sse_send_event(client_fd, "rack_types", json);

                        build_midi_devices_json(json, (int)sizeof(json));
                        sse_send_event(client_fd, "midi_devices", json);
                    }
                    /* Don't close — keep alive for SSE */
                }
                /* POST /api — JSON command */
                else if (strcmp(method, "POST") == 0 && strcmp(path, "/api") == 0) {
                    /* Find body after \r\n\r\n */
                    const char *body = strstr(reqbuf, "\r\n\r\n");
                    if (body) {
                        body += 4;
                        http_handle_api(client_fd, body);
                    } else {
                        http_send_response(client_fd, 400, "application/json",
                                           "{\"error\":\"no body\"}", 18);
                    }
                    close(client_fd);
                }
                /* POST /api/detail — set SSE detail channel */
                else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/detail") == 0) {
                    const char *body = strstr(reqbuf, "\r\n\r\n");
                    int detail_ch = -1;
                    if (body) {
                        body += 4;
                        json_get_int(body, "channel", &detail_ch);
                    }

                    /* Update all SSE clients' detail channel */
                    pthread_mutex_lock(&g_http_lock);
                    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
                        if (g_http_clients[i].fd >= 0 && g_http_clients[i].is_sse) {
                            g_http_clients[i].detail_channel = detail_ch;
                        }
                    }
                    pthread_mutex_unlock(&g_http_lock);

                    http_send_response(client_fd, 200, "application/json",
                                       "{\"ok\":true}", 11);
                    close(client_fd);
                }
                else {
                    http_send_response(client_fd, 404, "text/plain",
                                       "Not Found", 9);
                    close(client_fd);
                }
            } else {
                close(client_fd);
            }
        }

        /* ── Periodic SSE push ─────────────────────────────────── */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        /* Rack status every 500ms */
        long rack_elapsed_ms = (now.tv_sec - last_rack_poll.tv_sec) * 1000 +
                               (now.tv_nsec - last_rack_poll.tv_nsec) / 1000000;
        if (rack_elapsed_ms >= 500) {
            last_rack_poll = now;
            char json[SSE_BUF_SIZE];
            build_rack_status_json(json, (int)sizeof(json));
            sse_broadcast("rack_status", json);
        }

        /* Detail channel status every 200ms */
        long detail_elapsed_ms = (now.tv_sec - last_detail_poll.tv_sec) * 1000 +
                                 (now.tv_nsec - last_detail_poll.tv_nsec) / 1000000;
        if (detail_elapsed_ms >= 200) {
            last_detail_poll = now;
            /* Find if any SSE client wants detail */
            int detail_ch = -1;
            pthread_mutex_lock(&g_http_lock);
            for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
                if (g_http_clients[i].fd >= 0 && g_http_clients[i].is_sse
                    && g_http_clients[i].detail_channel >= 0) {
                    detail_ch = g_http_clients[i].detail_channel;
                    break;
                }
            }
            pthread_mutex_unlock(&g_http_lock);

            if (detail_ch >= 0 && detail_ch < MAX_SLOTS) {
                char json[SSE_BUF_SIZE];
                build_ch_status_json(detail_ch, json, (int)sizeof(json));
                sse_broadcast("ch_status", json);
            }
        }

        /* Small sleep to avoid busy-spin (10ms) */
        usleep(10000);
    }

    /* Cleanup: close all SSE clients */
    pthread_mutex_lock(&g_http_lock);
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        if (g_http_clients[i].fd >= 0) {
            close(g_http_clients[i].fd);
            g_http_clients[i].fd = -1;
        }
    }
    pthread_mutex_unlock(&g_http_lock);

    close(listen_fd);
    return NULL;
}

/* ── Usage ──────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "miniwave — modular rack host for waveOS\n"
        "Usage: %s [options]\n"
        "  -m C:P    ALSA seq MIDI source client:port (e.g. 20:0)\n"
        "  -o DEV    ALSA audio output (default: default)\n"
        "  -c N      Pre-configure channels 1-N with FM synth (default: 0)\n"
        "  -O PORT   OSC port (default: 9000)\n"
        "  -W PORT   HTTP/SSE port for WaveUI (default: 8080, 0 to disable)\n"
        "  -P SIZE   Audio period size (default: 64)\n"
        "  -h        Help\n", prog);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    char midi_dev[64] = "";
    char audio_dev[64] = "default";
    int pre_config = 0;
    int osc_port = DEFAULT_OSC_PORT;
    int http_port = DEFAULT_HTTP_PORT;
    int period_size = DEFAULT_PERIOD;

    int opt;
    while ((opt = getopt(argc, argv, "m:o:c:O:W:P:h")) != -1) {
        switch (opt) {
        case 'm': strncpy(midi_dev, optarg, sizeof(midi_dev) - 1); break;
        case 'o': strncpy(audio_dev, optarg, sizeof(audio_dev) - 1); break;
        case 'c': pre_config = atoi(optarg); break;
        case 'O': osc_port = atoi(optarg); break;
        case 'W': http_port = atoi(optarg); break;
        case 'P': period_size = atoi(optarg); break;
        case 'h': /* fall through */
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    if (pre_config < 0) pre_config = 0;
    if (pre_config > MAX_SLOTS) pre_config = MAX_SLOTS;

    /* ── Init rack ─────────────────────────────────────────────── */

    rack_init();

    /* Pre-configure channels with FM synth */
    for (int i = 0; i < pre_config; i++) {
        rack_set_slot(i, "fm-synth");
    }

    /* ── ALSA Sequencer MIDI ─────────────────────────────────────── */
    /* Always open the sequencer client so we can list devices and
     * subscribe at runtime.  If -m is given, connect immediately. */

    if (seq_init() == 0) {
        if (midi_dev[0] != '\0') {
            seq_connect(midi_dev);
        } else {
            fprintf(stderr, "[miniwave] MIDI: ready (use -m client:port or OSC /midi/device)\n");
        }
    }

    /* ── ALSA Audio ────────────────────────────────────────────── */

    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, audio_dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "[miniwave] ERROR: can't open audio %s: %s\n",
                audio_dev, snd_strerror(err));
        if (g_seq) { snd_seq_close(g_seq); g_seq = NULL; }
        return 1;
    }

    {
        snd_pcm_hw_params_t *hw;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(pcm, hw);
        snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
        unsigned int rate = SAMPLE_RATE;
        snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
        snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS);
        snd_pcm_uframes_t ps = (snd_pcm_uframes_t)period_size;
        snd_pcm_hw_params_set_period_size_near(pcm, hw, &ps, NULL);
        snd_pcm_uframes_t bs = ps * 2;
        snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bs);

        err = snd_pcm_hw_params(pcm, hw);
        if (err < 0) {
            fprintf(stderr, "[miniwave] ERROR: hw_params: %s\n", snd_strerror(err));
            snd_pcm_close(pcm);
            if (g_seq) { snd_seq_close(g_seq); g_seq = NULL; }
            return 1;
        }

        snd_pcm_hw_params_get_period_size(hw, &ps, NULL);
        period_size = (int)ps;
        fprintf(stderr, "[miniwave] audio: %s @ %uHz period=%d\n",
                audio_dev, rate, period_size);
    }

    /* ── Shared memory bus ─────────────────────────────────────── */

    WaveosBus *bus = NULL;
    int bus_slot = -1;

    {
        int fd = shm_open(WAVEOS_BUS_SHM_NAME, O_RDWR, 0);
        if (fd >= 0) {
            bus = mmap(NULL, sizeof(WaveosBus), PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
            close(fd);
            if (bus == MAP_FAILED) {
                bus = NULL;
                fprintf(stderr, "[miniwave] WARN: bus mmap failed\n");
            } else if (bus->magic != 0x57415645) {
                fprintf(stderr, "[miniwave] WARN: bus magic mismatch\n");
                munmap(bus, sizeof(WaveosBus));
                bus = NULL;
            } else {
                for (int i = 0; i < WAVEOS_BUS_SLOTS; i++) {
                    int expected = 0;
                    if (atomic_compare_exchange_strong(&bus->slots[i].active,
                                                      &expected, 1)) {
                        bus_slot = i;
                        snprintf(bus->slots[i].name, 32, "miniwave");
                        fprintf(stderr, "[miniwave] bus slot %d claimed\n", i);
                        break;
                    }
                }
                if (bus_slot < 0) {
                    fprintf(stderr, "[miniwave] WARN: no free bus slots\n");
                    munmap(bus, sizeof(WaveosBus));
                    bus = NULL;
                }
            }
        } else {
            fprintf(stderr, "[miniwave] bus not available, ALSA only\n");
        }
    }

    /* ── Audio buffers ─────────────────────────────────────────── */

    int16_t *audio_buf = calloc((size_t)(period_size * CHANNELS), sizeof(int16_t));
    float   *mix_buf   = calloc((size_t)(period_size * CHANNELS), sizeof(float));
    float   *slot_buf  = calloc((size_t)(period_size * CHANNELS), sizeof(float));
    if (!audio_buf || !mix_buf || !slot_buf) {
        fprintf(stderr, "[miniwave] ERROR: alloc failed\n");
        goto cleanup;
    }

    fprintf(stderr, "[miniwave] running [ALSA]%s%s — %d types registered\n",
            (bus && bus_slot >= 0) ? " [BUS]" : "",
            (http_port > 0) ? " [HTTP]" : "",
            g_n_types);

    /* ── Start MIDI thread ─────────────────────────────────────── */

    pthread_t midi_tid = 0;

    if (g_seq) {
        if (pthread_create(&midi_tid, NULL, midi_thread_fn, NULL) != 0) {
            fprintf(stderr, "[miniwave] ERROR: can't create MIDI thread\n");
            goto cleanup;
        }
        fprintf(stderr, "[miniwave] MIDI seq thread started\n");
    }

    /* ── Start OSC thread ──────────────────────────────────────── */

    pthread_t osc_tid = 0;
    OscThreadCtx osc_ctx;
    memset(&osc_ctx, 0, sizeof(osc_ctx));
    osc_ctx.port = osc_port;

    if (osc_port > 0) {
        if (pthread_create(&osc_tid, NULL, osc_thread_fn, &osc_ctx) != 0) {
            fprintf(stderr, "[miniwave] WARN: can't create OSC thread\n");
        }
    }

    /* ── Start HTTP/SSE thread ────────────────────────────────── */

    pthread_t http_tid = 0;
    HttpThreadCtx http_ctx;
    memset(&http_ctx, 0, sizeof(http_ctx));
    http_ctx.port = http_port;

    if (http_port > 0) {
        http_load_html();
        if (pthread_create(&http_tid, NULL, http_thread_fn, &http_ctx) != 0) {
            fprintf(stderr, "[miniwave] WARN: can't create HTTP thread\n");
        }
    }

    /* ── Audio render loop ─────────────────────────────────────── */

    while (!g_quit) {
        /* Zero the mix buffer */
        memset(mix_buf, 0, sizeof(float) * (size_t)(period_size * CHANNELS));

        /* Check if any slot has solo enabled */
        int any_solo = 0;
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (g_rack.slots[i].active && g_rack.slots[i].solo) {
                any_solo = 1;
                break;
            }
        }

        /* Render each active slot */
        for (int i = 0; i < MAX_SLOTS; i++) {
            RackSlot *slot = &g_rack.slots[i];
            if (!slot->active || !slot->state) continue;
            if (slot->mute) continue;
            if (any_solo && !slot->solo) continue;

            InstrumentType *itype = g_type_registry[slot->type_idx];

            /* Clear slot render buffer */
            memset(slot_buf, 0, sizeof(float) * (size_t)(period_size * CHANNELS));

            /* Render instrument into slot buffer */
            itype->render(slot->state, slot_buf, period_size, SAMPLE_RATE);

            /* Mix into main buffer with slot volume */
            float vol = slot->volume;
            for (int j = 0; j < period_size * CHANNELS; j++) {
                mix_buf[j] += slot_buf[j] * vol;
            }
        }

        /* Apply master volume and limiter */
        float mv = g_rack.master_volume;
        for (int j = 0; j < period_size * CHANNELS; j++) {
            mix_buf[j] *= mv;
            mix_buf[j] = master_limiter(mix_buf[j]);
        }

        /* Write to bus if available */
        if (bus && bus_slot >= 0) {
            bus_write(bus, bus_slot, mix_buf, period_size);
        }

        /* Convert to S16 for ALSA (silence if local_mute — bus-only mode) */
        if (g_rack.local_mute) {
            memset(audio_buf, 0, (size_t)(period_size * CHANNELS) * sizeof(int16_t));
        } else {
            for (int j = 0; j < period_size * CHANNELS; j++) {
                float s = mix_buf[j];
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                audio_buf[j] = (int16_t)(s * 32000.0f);
            }
        }

        /* Write to ALSA */
        snd_pcm_sframes_t frames = snd_pcm_writei(pcm, audio_buf, (snd_pcm_uframes_t)period_size);
        if (frames < 0) {
            frames = snd_pcm_recover(pcm, (int)frames, 1);
            if (frames < 0) {
                fprintf(stderr, "[miniwave] audio write error: %s\n",
                        snd_strerror((int)frames));
            }
        }
    }

    /* ── Shutdown ───────────────────────────────────────────────── */

    fprintf(stderr, "[miniwave] shutting down...\n");

    if (midi_tid) pthread_join(midi_tid, NULL);
    if (osc_tid)  pthread_join(osc_tid, NULL);
    if (http_tid) pthread_join(http_tid, NULL);

cleanup:
    /* Destroy all active slots */
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_rack.slots[i].active && g_rack.slots[i].state) {
            InstrumentType *itype = g_type_registry[g_rack.slots[i].type_idx];
            itype->destroy(g_rack.slots[i].state);
            free(g_rack.slots[i].state);
            g_rack.slots[i].state = NULL;
            g_rack.slots[i].active = 0;
        }
    }

    if (bus && bus_slot >= 0) {
        atomic_store(&bus->slots[bus_slot].active, 0);
        memset(bus->slots[bus_slot].name, 0, 32);
        munmap(bus, sizeof(WaveosBus));
    }
    if (g_seq) {
        if (g_seq_connected) seq_disconnect();
        snd_seq_close(g_seq);
        g_seq = NULL;
    }
    if (pcm) snd_pcm_close(pcm);
    free(audio_buf);
    free(mix_buf);
    free(slot_buf);
    free(g_html_content);

    fprintf(stderr, "[miniwave] done\n");
    return 0;
}
