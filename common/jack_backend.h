/* miniwave — JACK audio backend (optional, cross-platform)
 *
 * Requires: jack/jack.h, jack/midiport.h
 * Provides: JackCtx, g_jack, jack_init/start/cleanup
 */

#ifndef MINIWAVE_JACK_BACKEND_H
#define MINIWAVE_JACK_BACKEND_H

typedef struct {
    jack_client_t *client;
    jack_port_t   *out_L;
    jack_port_t   *out_R;
    jack_port_t   *midi_in;
    float         *mix_buf;
    float         *slot_buf;
    int            buf_frames;
    WaveosBus     *bus;
    int            bus_slot;
} JackCtx;

static JackCtx g_jack = {0};

/* JACK process callback — realtime thread, no malloc/printf/mutex.
 * Reads JACK MIDI events and renders audio in sample-accurate segments. */
static int jack_process_cb(jack_nframes_t nframes, void *arg) {
    JackCtx *jctx = (JackCtx *)arg;

    float *out_L = (float *)jack_port_get_buffer(jctx->out_L, nframes);
    float *out_R = (float *)jack_port_get_buffer(jctx->out_R, nframes);

    int total_frames = (int)nframes;
    if (total_frames > jctx->buf_frames) total_frames = jctx->buf_frames;
    int sample_rate = (int)jack_get_sample_rate(jctx->client);

    void *midi_buf = jctx->midi_in
        ? jack_port_get_buffer(jctx->midi_in, nframes) : NULL;
    uint32_t midi_count = midi_buf ? jack_midi_get_event_count(midi_buf) : 0;

    int pos = 0;
    uint32_t midi_idx = 0;

    while (pos < total_frames) {
        int seg_end = total_frames;
        while (midi_idx < midi_count) {
            jack_midi_event_t ev;
            if (jack_midi_event_get(&ev, midi_buf, midi_idx) != 0) {
                midi_idx++;
                continue;
            }
            int ev_frame = (int)ev.time;
            if (ev_frame < pos) ev_frame = pos;

            if (ev_frame == pos) {
                midi_dispatch_raw(ev.buffer, (int)ev.size);
                midi_idx++;
                continue;
            }
            seg_end = ev_frame;
            break;
        }

        int seg_len = seg_end - pos;
        if (seg_len > 0) {
            render_mix(jctx->mix_buf, jctx->slot_buf, seg_len, sample_rate);

            if (g_rack.local_mute) {
                memset(out_L + pos, 0, sizeof(float) * (size_t)seg_len);
                memset(out_R + pos, 0, sizeof(float) * (size_t)seg_len);
            } else {
                for (int i = 0; i < seg_len; i++) {
                    out_L[pos + i] = jctx->mix_buf[i * 2];
                    out_R[pos + i] = jctx->mix_buf[i * 2 + 1];
                }
            }
        }

        pos = seg_end;
    }

    if (jctx->bus && jctx->bus_slot >= 0) {
        for (int i = 0; i < total_frames; i++) {
            jctx->mix_buf[i * 2]     = out_L[i];
            jctx->mix_buf[i * 2 + 1] = out_R[i];
        }
        bus_write(jctx->bus, jctx->bus_slot, jctx->mix_buf, total_frames);
    }

    return 0;
}

static void jack_shutdown_cb(void *arg) {
    (void)arg;
    g_jack.client = NULL;
    fprintf(stderr, "[miniwave] JACK server shut down\n");
    g_quit = 1;
}

static int jack_init(void) {
    jack_status_t status;
    g_jack.client = jack_client_open("miniwave", JackNoStartServer, &status);
    if (!g_jack.client) {
        fprintf(stderr, "[miniwave] JACK not available (status=0x%x)\n",
                (unsigned)status);
        return -1;
    }

    jack_set_process_callback(g_jack.client, jack_process_cb, &g_jack);
    jack_on_shutdown(g_jack.client, jack_shutdown_cb, NULL);

    g_jack.out_L = jack_port_register(g_jack.client, "output_L",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_jack.out_R = jack_port_register(g_jack.client, "output_R",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_jack.midi_in = jack_port_register(g_jack.client, "midi_in",
        JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);

    if (!g_jack.out_L || !g_jack.out_R) {
        fprintf(stderr, "[miniwave] can't register JACK ports\n");
        jack_client_close(g_jack.client);
        g_jack.client = NULL;
        return -1;
    }
    if (!g_jack.midi_in) {
        fprintf(stderr, "[miniwave] WARN: can't register JACK MIDI port\n");
    }

    g_jack.buf_frames = (int)jack_get_buffer_size(g_jack.client);
    g_jack.mix_buf  = calloc((size_t)(g_jack.buf_frames * CHANNELS), sizeof(float));
    g_jack.slot_buf = calloc((size_t)(g_jack.buf_frames * CHANNELS), sizeof(float));

    if (!g_jack.mix_buf || !g_jack.slot_buf) {
        fprintf(stderr, "[miniwave] can't alloc JACK buffers\n");
        jack_client_close(g_jack.client);
        g_jack.client = NULL;
        return -1;
    }

    fprintf(stderr, "[miniwave] JACK client '%s' @ %uHz buf=%d%s\n",
            jack_get_client_name(g_jack.client),
            jack_get_sample_rate(g_jack.client),
            g_jack.buf_frames,
            g_jack.midi_in ? " [JACK MIDI]" : "");
    return 0;
}

static int jack_start(void) {
    if (!g_jack.client) return -1;

    if (jack_activate(g_jack.client) != 0) {
        fprintf(stderr, "[miniwave] can't activate JACK client\n");
        return -1;
    }

    const char **ports = jack_get_ports(g_jack.client, NULL, NULL,
        JackPortIsPhysical | JackPortIsInput);
    if (ports) {
        if (ports[0])
            jack_connect(g_jack.client, jack_port_name(g_jack.out_L), ports[0]);
        if (ports[1])
            jack_connect(g_jack.client, jack_port_name(g_jack.out_R), ports[1]);
        jack_free(ports);
    }

    return 0;
}

static void jack_cleanup(void) {
    if (g_jack.client) {
        jack_deactivate(g_jack.client);
        jack_client_close(g_jack.client);
        g_jack.client = NULL;
    }
    free(g_jack.mix_buf);
    free(g_jack.slot_buf);
    g_jack.mix_buf = NULL;
    g_jack.slot_buf = NULL;
}

#endif
