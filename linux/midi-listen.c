/* midi-listen — dump all ALSA seq MIDI input to stderr */
#define _GNU_SOURCE
#define __USE_MISC
#include <stdio.h>
#include <signal.h>
#include <poll.h>
#include <alsa/asoundlib.h>

static volatile int quit = 0;
static void sig(int s) { (void)s; quit = 1; }

int main(void) {
    signal(SIGINT, sig);
    signal(SIGTERM, sig);

    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
        fprintf(stderr, "can't open ALSA sequencer\n");
        return 1;
    }
    snd_seq_set_client_name(seq, "midi-listen");

    int port = snd_seq_create_simple_port(seq, "in",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    /* subscribe to all readable ports */
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    int me = snd_seq_client_id(seq);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int c = snd_seq_client_info_get_client(cinfo);
        if (c == me || c == 0) continue;
        const char *cname = snd_seq_client_info_get_name(cinfo);

        snd_seq_port_info_set_client(pinfo, c);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            if (!(caps & SND_SEQ_PORT_CAP_READ)) continue;
            if (!(caps & SND_SEQ_PORT_CAP_SUBS_READ)) continue;
            int p = snd_seq_port_info_get_port(pinfo);
            const char *pname = snd_seq_port_info_get_name(pinfo);
            snd_seq_connect_from(seq, port, c, p);
            fprintf(stderr, "subscribed: %s — %s (%d:%d)\n", cname, pname, c, p);
        }
    }

    fprintf(stderr, "\nlistening... (ctrl+c to quit)\n\n");

    int npfds = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd *pfds = calloc((size_t)npfds, sizeof(struct pollfd));
    snd_seq_poll_descriptors(seq, pfds, (unsigned int)npfds, POLLIN);

    while (!quit) {
        if (poll(pfds, (nfds_t)npfds, 100) <= 0) continue;
        snd_seq_event_t *ev = NULL;
        while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
            int ch = ev->data.note.channel;
            switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:
                fprintf(stderr, "[%d:%d] NOTE ON  ch=%d note=%d vel=%d\n",
                    ev->source.client, ev->source.port,
                    ch, ev->data.note.note, ev->data.note.velocity);
                break;
            case SND_SEQ_EVENT_NOTEOFF:
                fprintf(stderr, "[%d:%d] NOTE OFF ch=%d note=%d\n",
                    ev->source.client, ev->source.port,
                    ch, ev->data.note.note);
                break;
            case SND_SEQ_EVENT_CONTROLLER:
                fprintf(stderr, "[%d:%d] CC       ch=%d cc=%d val=%d\n",
                    ev->source.client, ev->source.port,
                    ch, ev->data.control.param, ev->data.control.value);
                break;
            case SND_SEQ_EVENT_PGMCHANGE:
                fprintf(stderr, "[%d:%d] PRGM     ch=%d prog=%d\n",
                    ev->source.client, ev->source.port,
                    ch, ev->data.control.value);
                break;
            case SND_SEQ_EVENT_PITCHBEND:
                fprintf(stderr, "[%d:%d] PITCH    ch=%d val=%d\n",
                    ev->source.client, ev->source.port,
                    ch, ev->data.control.value);
                break;
            case SND_SEQ_EVENT_CHANPRESS:
                fprintf(stderr, "[%d:%d] ATOUCH   ch=%d val=%d\n",
                    ev->source.client, ev->source.port,
                    ch, ev->data.control.value);
                break;
            default:
                fprintf(stderr, "[%d:%d] type=%d\n",
                    ev->source.client, ev->source.port, ev->type);
                break;
            }
        }
    }

    free(pfds);
    snd_seq_close(seq);
    fprintf(stderr, "done\n");
    return 0;
}
