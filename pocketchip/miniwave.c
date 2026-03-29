/* miniwave — PocketCHIP platform (ALSA-only, no JACK)
 *
 * Stripped-down Linux build for the C.H.I.P. / PocketCHIP.
 * ARM Cortex-A8 @ 1GHz, 512MB RAM, ALSA sun4i-codec.
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

/* ── Common code ───────────────────────────────────────────────────── */

#include "../common/rack.h"
#include "../common/server.h"

/* ── Stub out JACK symbols (not available on PocketCHIP) ───────────── */

static int jack_init(void) { return -1; }
static int jack_start(void) { return -1; }
static void jack_cleanup(void) {}

typedef struct { void *bus; int bus_slot; } JackCtx;
static JackCtx g_jack = {0};

/* ══════════════════════════════════════════════════════════════════════
 *  Platform: ALSA Sequencer MIDI
 * ══════════════════════════════════════════════════════════════════════ */

static snd_seq_t        *g_seq = NULL;
static int               g_seq_port = -1;
static snd_seq_addr_t    g_seq_src = {0,0};
static int               g_seq_connected = 0;

static int platform_midi_init(void) {
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

static int platform_midi_connect(const char *addr_str) {
    if (!g_seq) return -1;

    if (g_seq_connected) {
        snd_seq_disconnect_from(g_seq, g_seq_port, g_seq_src.client, g_seq_src.port);
        g_seq_connected = 0;
        g_midi_device_name[0] = '\0';
    }

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

static void platform_midi_disconnect(void) {
    if (!g_seq || !g_seq_connected) return;
    snd_seq_disconnect_from(g_seq, g_seq_port, g_seq_src.client, g_seq_src.port);
    g_seq_connected = 0;
    g_midi_device_name[0] = '\0';
    fprintf(stderr, "[miniwave] MIDI disconnected\n");
}

static int platform_midi_list_devices(char devices[][64], char names[][128], int max_devices) {
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

        if (client == snd_seq_client_id(g_seq)) continue;
        if (client == 0) continue;

        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(g_seq, pinfo) >= 0 && count < max_devices) {
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            if (!(caps & SND_SEQ_PORT_CAP_READ)) continue;
            if (!(caps & SND_SEQ_PORT_CAP_SUBS_READ)) continue;

            int port = snd_seq_port_info_get_port(pinfo);
            const char *pname = snd_seq_port_info_get_name(pinfo);

            if (cname && strstr(cname, "Midi Through")) continue;

            snprintf(devices[count], 64, "%d:%d", client, port);
            snprintf(names[count], 128, "%s — %s",
                     cname ? cname : "?", pname ? pname : "?");
            count++;
        }
    }
    return count;
}

/* Dispatch ALSA sequencer event to rack */
static inline void seq_dispatch(snd_seq_event_t *ev) {
    int ch = ev->data.note.channel;
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

static void *platform_midi_thread(void *arg) {
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

static void platform_midi_cleanup(void) {
    if (g_seq) {
        if (g_seq_connected) platform_midi_disconnect();
        snd_seq_close(g_seq);
        g_seq = NULL;
    }
}

/* ── Platform helpers ──────────────────────────────────────────────── */

static int platform_exe_dir(char *buf, int max) {
    ssize_t len = readlink("/proc/self/exe", buf, (size_t)(max - 1));
    if (len <= 0) return -1;
    buf[len] = '\0';
    char *slash = strrchr(buf, '/');
    if (slash) *(slash + 1) = '\0';
    return 0;
}

static const char *platform_audio_fallback_name(void) {
    return "ALSA";
}

/* ── Usage ──────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "miniwave — PocketCHIP synth rack\n"
        "Usage: %s [options]\n"
        "  -m C:P    ALSA seq MIDI source client:port (e.g. 20:0)\n"
        "  -o DEV    ALSA audio output (default: default)\n"
        "  -c N      Pre-configure channels 1-N with FM synth (default: 0)\n"
        "  -O PORT   OSC port (default: 9000)\n"
        "  -W PORT   HTTP/SSE port for WaveUI (default: 8080, 0 to disable)\n"
        "  -P SIZE   Audio period size (default: 256)\n"
        "  -h        Help\n", prog);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGPIPE, SIG_IGN);

    char midi_dev[64] = "";
    char audio_dev[64] = "default";
    int pre_config = 0;
    int osc_port = DEFAULT_OSC_PORT;
    int http_port = DEFAULT_HTTP_PORT;
    int period_size = 256;  /* larger default for ARM stability */

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
    state_init_path();
    state_load();
    keyseq_wire_graph_broadcast();

    for (int i = 0; i < pre_config; i++) {
        if (!g_rack.slots[i].active)
            rack_set_slot(i, "fm-synth");
    }

    /* ── ALSA Sequencer MIDI ─────────────────────────────────────── */

    if (platform_midi_init() == 0) {
        if (midi_dev[0] != '\0') {
            platform_midi_connect(midi_dev);
        } else {
            fprintf(stderr, "[miniwave] MIDI: ready (use -m client:port or OSC /midi/device)\n");
        }
    }

    /* ── Audio: ALSA only ─────────────────────────────────────────── */

    snd_pcm_t *pcm = NULL;

    int err = snd_pcm_open(&pcm, audio_dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "[miniwave] ERROR: can't open audio %s: %s\n",
                audio_dev, snd_strerror(err));
        platform_midi_cleanup();
        return 1;
    }

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
    snd_pcm_uframes_t bs = ps * 4;  /* bigger buffer for ARM */
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bs);

    err = snd_pcm_hw_params(pcm, hw);
    if (err < 0) {
        fprintf(stderr, "[miniwave] ERROR: hw_params: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        platform_midi_cleanup();
        return 1;
    }

    snd_pcm_hw_params_get_period_size(hw, &ps, NULL);
    period_size = (int)ps;
    fprintf(stderr, "[miniwave] audio: ALSA %s @ %uHz period=%d\n",
            audio_dev, rate, period_size);

    /* ── Audio buffers ────────────────────────────────────────────── */

    int16_t *audio_buf = calloc((size_t)(period_size * CHANNELS), sizeof(int16_t));
    float   *mix_buf   = calloc((size_t)(period_size * CHANNELS), sizeof(float));
    float   *slot_buf  = calloc((size_t)(period_size * CHANNELS), sizeof(float));
    if (!audio_buf || !mix_buf || !slot_buf) {
        fprintf(stderr, "[miniwave] ERROR: alloc failed\n");
        goto cleanup;
    }

    fprintf(stderr, "[miniwave] running [ALSA]%s — %d types registered\n",
            (http_port > 0) ? " [HTTP]" : "",
            g_n_types);

    /* ── Start MIDI thread ─────────────────────────────────────── */

    pthread_t midi_tid = 0;

    if (g_seq) {
        if (pthread_create(&midi_tid, NULL, platform_midi_thread, NULL) != 0) {
            fprintf(stderr, "[miniwave] ERROR: can't create MIDI thread\n");
            goto cleanup;
        }
        fprintf(stderr, "[miniwave] MIDI seq thread started\n");
    }

    /* ── Start multicast broadcast thread ─────────────────────── */

    pthread_t mcast_tid = 0;
    if (mcast_init() == 0) {
        if (pthread_create(&mcast_tid, NULL, mcast_thread_fn, NULL) != 0) {
            fprintf(stderr, "[miniwave] WARN: can't create multicast thread\n");
        }
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
        render_mix(mix_buf, slot_buf, period_size, SAMPLE_RATE);

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
    state_save();

    if (midi_tid)  pthread_join(midi_tid, NULL);
    if (mcast_tid) pthread_join(mcast_tid, NULL);
    if (osc_tid)   pthread_join(osc_tid, NULL);
    if (http_tid)  pthread_join(http_tid, NULL);

cleanup:
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_rack.slots[i].active && g_rack.slots[i].state) {
            InstrumentType *itype = g_type_registry[g_rack.slots[i].type_idx];
            itype->destroy(g_rack.slots[i].state);
            free(g_rack.slots[i].state);
            g_rack.slots[i].state = NULL;
            g_rack.slots[i].active = 0;
        }
    }

    platform_midi_cleanup();
    if (pcm) snd_pcm_close(pcm);
    free(audio_buf);
    free(mix_buf);
    free(slot_buf);
    free(g_html_content);

    fprintf(stderr, "[miniwave] done\n");
    return 0;
}
