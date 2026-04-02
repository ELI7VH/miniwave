/* pocketwave-ui — ANSI terminal UI for miniwave on PocketCHIP
 *
 * Talks to miniwave over OSC (localhost:9000).
 * Render-on-demand: only redraws when state changes.
 */

#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

/* ── OSC helpers ───────────────────────────────────────────────────── */

static int g_osc_fd = -1;
static struct sockaddr_in g_osc_addr;

static int osc_init(int port) {
    g_osc_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_osc_fd < 0) return -1;
    memset(&g_osc_addr, 0, sizeof(g_osc_addr));
    g_osc_addr.sin_family = AF_INET;
    g_osc_addr.sin_port = htons((uint16_t)port);
    g_osc_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return 0;
}

static int osc_build(uint8_t *buf, int max, const char *addr, const char *types, ...) {
    int pos = 0;
    int alen = (int)strlen(addr) + 1;
    int apad = (4 - (alen % 4)) % 4;
    if (pos + alen + apad > max) return -1;
    memcpy(buf + pos, addr, (size_t)alen);
    pos += alen;
    memset(buf + pos, 0, (size_t)apad);
    pos += apad;

    char ttag[32];
    snprintf(ttag, sizeof(ttag), ",%s", types);
    int tlen = (int)strlen(ttag) + 1;
    int tpad = (4 - (tlen % 4)) % 4;
    if (pos + tlen + tpad > max) return -1;
    memcpy(buf + pos, ttag, (size_t)tlen);
    pos += tlen;
    memset(buf + pos, 0, (size_t)tpad);
    pos += tpad;

    __builtin_va_list ap;
    __builtin_va_start(ap, types);
    for (const char *t = types; *t; t++) {
        if (*t == 'i') {
            int32_t v = __builtin_va_arg(ap, int32_t);
            int32_t nv = htonl((uint32_t)v);
            if (pos + 4 > max) { __builtin_va_end(ap); return -1; }
            memcpy(buf + pos, &nv, 4);
            pos += 4;
        } else if (*t == 'f') {
            float v = (float)__builtin_va_arg(ap, double);
            uint32_t uv;
            memcpy(&uv, &v, 4);
            uv = htonl(uv);
            if (pos + 4 > max) { __builtin_va_end(ap); return -1; }
            memcpy(buf + pos, &uv, 4);
            pos += 4;
        } else if (*t == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            int slen = (int)strlen(s) + 1;
            int spad = (4 - (slen % 4)) % 4;
            if (pos + slen + spad > max) { __builtin_va_end(ap); return -1; }
            memcpy(buf + pos, s, (size_t)slen);
            pos += slen;
            memset(buf + pos, 0, (size_t)spad);
            pos += spad;
        }
    }
    __builtin_va_end(ap);
    return pos;
}

static void osc_send(const uint8_t *buf, int len) {
    if (g_osc_fd >= 0 && len > 0)
        sendto(g_osc_fd, buf, (size_t)len, 0,
               (struct sockaddr *)&g_osc_addr, sizeof(g_osc_addr));
}

/* ── CPU usage ─────────────────────────────────────────────────────── */

static long g_prev_total = 0, g_prev_idle = 0;

static int get_cpu_pct(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    long user, nice, sys, idle, iow, irq, sirq;
    if (fscanf(f, "cpu %ld %ld %ld %ld %ld %ld %ld",
               &user, &nice, &sys, &idle, &iow, &irq, &sirq) != 7) {
        fclose(f); return -1;
    }
    fclose(f);
    long total = user + nice + sys + idle + iow + irq + sirq;
    long dt = total - g_prev_total;
    long di = idle - g_prev_idle;
    g_prev_total = total;
    g_prev_idle = idle;
    if (dt <= 0) return 0;
    return (int)(100 * (dt - di) / dt);
}

/* ── Battery ───────────────────────────────────────────────────────── */

static int g_batt_pct = -1;   /* -1 = unknown */
static int g_batt_charging = 0;

static void update_battery(void) {
    FILE *f = fopen("/usr/lib/pocketchip-batt/voltage", "r");
    if (f) {
        int mv = 0;
        if (fscanf(f, "%d", &mv) == 1 && mv > 0) {
            /* LiPo: 3300mV = 0%, 4200mV = 100% */
            int pct = (mv - 3300) * 100 / 900;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            g_batt_pct = pct;
        }
        fclose(f);
    }
    f = fopen("/usr/lib/pocketchip-batt/charging", "r");
    if (f) {
        int c = fgetc(f);
        g_batt_charging = (c == '1');
        fclose(f);
    }
}

/* ── UI modes ──────────────────────────────────────────────────────── */

enum {
    MODE_MAIN,
    MODE_HELP,
    MODE_CONFIG,
    MODE_MIDI_MON,
    MODE_QUIT
};

/* ── State ─────────────────────────────────────────────────────────── */

static const char *SYNTH_TYPES[] = {
    "fm-synth", "ym2413", "sub-synth", "fm-drums", "additive", "phase-dist", "bird"
};
static const char *SYNTH_LABELS[] = {
    "FM SYNTH", "YM2413", "SUB SYNTH", "FM DRUMS", "ADDITIVE", "PHASE DIST", "BIRD"
};
#define NUM_TYPES 7

/* knob labels per synth, 8 knobs each (CC14-21) */
static const char *KNOB_LABELS[][8] = {
    /* fm-synth */  {"MOD", "RATIO", "CARR", "FDBK", "ATK", "DEC", "SUS", "REL"},
    /* ym2413 */    {"INST", "MODLV", "FDBK", "MATK", "MDEC", "CATK", "CDEC", "MMUL"},
    /* sub-synth */ {"CUT", "RES", "WAVE", "PW", "FENV", "ATK", "SUS", "REL"},
    /* fm-drums */  {"FREQ", "MFRQ", "MIDX", "SWEP", "DECY", "NOIS", "CLIK", "FDBK"},
    /* additive */  {"MODE", "HARM", "RATIO", "SPRD", "ROLL", "CHAR", "SHPE", "REL"},
    /* phase-dist */{"DIST", "TMBR", "MODE", "COLR", "ATK", "DEC", "SUS", "REL"},
    /* bird */      {"RATE", "DROP", "CURV", "VDEP", "VRAT", "BUZZ", "SHPE", "GAP"},
};

static int g_type_idx = 0;
static int g_preset = 0;
static int g_volume = 80;
static int g_cpu = 0;
static int g_channel = 0;
static int g_mono = 0;
static int g_legato = 0;
static int g_mode = MODE_MAIN;
static int g_flash_ticks = 0;   /* countdown for flash message */
static char g_flash_msg[32] = "";
static volatile int g_quit = 0;

/* config menu */
#define CFG_AUDIO   0
#define CFG_BUFFER  1
#define CFG_MIDI    2
#define CFG_RESTART 3
#define CFG_MIDMON  4
#define CFG_COUNT   5

static int g_cfg_cursor = 0;

/* audio devices */
#define MAX_ADEVS 8
static char g_adev_ids[MAX_ADEVS][32];
static char g_adev_names[MAX_ADEVS][64];
static int g_adev_count = 0;
static int g_adev_sel = 0;

/* buffer sizes */
static const int BUF_SIZES[] = { 64, 128, 256, 512, 1024 };
#define NUM_BUFS 5
static int g_buf_sel = 2; /* default 256 */

/* midi devices */
#define MAX_MDEVS 8
static char g_mdev_ids[MAX_MDEVS][32];
static char g_mdev_names[MAX_MDEVS][64];
static int g_mdev_count = 0;
static int g_mdev_sel = 0;

/* midi monitor */
#define MIDI_LOG_LINES 8
static char g_midi_log[MIDI_LOG_LINES][32];
static int g_midi_log_pos = 0;
static snd_seq_t *g_midi_seq = NULL;
static int g_midi_seq_port = -1;

/* ── Terminal ──────────────────────────────────────────────────────── */

static struct termios g_orig_termios;
static int g_cols = 19, g_rows = 10;

static void term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        g_cols = ws.ws_col;
        g_rows = ws.ws_row;
    }
}

static void term_raw(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    raw = g_orig_termios;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG);
    raw.c_iflag &= (tcflag_t)~(IXON | IXOFF);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    write(STDOUT_FILENO, "\033[?25l", 6);
}

static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    write(STDOUT_FILENO, "\033[?25h\033[0m\033[2J\033[H", 18);
}

static void sighandler(int sig) {
    (void)sig;
    g_quit = 1;
}

/* ── Device scanning ──────────────────────────────────────────────── */

static void scan_audio_devices(void) {
    g_adev_count = 0;
    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0 && g_adev_count < MAX_ADEVS) {
        char name[32];
        snprintf(name, sizeof(name), "hw:%d,0", card);

        snd_pcm_t *pcm;
        if (snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0)
            continue;
        snd_pcm_close(pcm);

        char *cname = NULL;
        snd_card_get_name(card, &cname);

        snprintf(g_adev_ids[g_adev_count], 32, "hw:%d,0", card);
        snprintf(g_adev_names[g_adev_count], 64, "%s",
                 cname ? cname : name);
        free(cname);
        g_adev_count++;
    }
}

static void scan_midi_devices(void) {
    g_mdev_count = 0;
    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0)
        return;

    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0 && g_mdev_count < MAX_MDEVS) {
        int client = snd_seq_client_info_get_client(cinfo);
        const char *cname = snd_seq_client_info_get_name(cinfo);
        if (client == 0) continue;
        if (cname && strstr(cname, "Midi Through")) continue;
        if (cname && strstr(cname, "miniwave")) continue;

        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0 && g_mdev_count < MAX_MDEVS) {
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            if (!(caps & SND_SEQ_PORT_CAP_READ)) continue;
            if (!(caps & SND_SEQ_PORT_CAP_SUBS_READ)) continue;

            int port = snd_seq_port_info_get_port(pinfo);
            snprintf(g_mdev_ids[g_mdev_count], 32, "%d:%d", client, port);
            snprintf(g_mdev_names[g_mdev_count], 64, "%s",
                     cname ? cname : "?");
            g_mdev_count++;
        }
    }
    snd_seq_close(seq);
}

/* ── MIDI monitor ─────────────────────────────────────────────────── */

static void midi_mon_open(void) {
    if (g_midi_seq) return;
    if (snd_seq_open(&g_midi_seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
        g_midi_seq = NULL;
        return;
    }
    snd_seq_set_client_name(g_midi_seq, "pocketwave-mon");
    g_midi_seq_port = snd_seq_create_simple_port(g_midi_seq, "Monitor",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    /* subscribe to all available output ports */
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(g_midi_seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == snd_seq_client_id(g_midi_seq)) continue;
        if (client == 0) continue;
        const char *cn = snd_seq_client_info_get_name(cinfo);
        if (cn && strstr(cn, "Midi Through")) continue;

        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(g_midi_seq, pinfo) >= 0) {
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            if (!(caps & SND_SEQ_PORT_CAP_READ)) continue;
            if (!(caps & SND_SEQ_PORT_CAP_SUBS_READ)) continue;
            snd_seq_connect_from(g_midi_seq, g_midi_seq_port,
                                 client, snd_seq_port_info_get_port(pinfo));
        }
    }
    memset(g_midi_log, 0, sizeof(g_midi_log));
    g_midi_log_pos = 0;
}

static void midi_mon_close(void) {
    if (g_midi_seq) {
        snd_seq_close(g_midi_seq);
        g_midi_seq = NULL;
    }
}

static void midi_log_push(const char *msg) {
    int idx = g_midi_log_pos % MIDI_LOG_LINES;
    snprintf(g_midi_log[idx], 32, "%s", msg);
    g_midi_log_pos++;
}

static int midi_mon_poll(void) {
    if (!g_midi_seq) return 0;
    int got = 0;
    snd_seq_event_t *ev = NULL;
    while (snd_seq_event_input(g_midi_seq, &ev) >= 0 && ev) {
        char msg[32];
        switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            snprintf(msg, sizeof(msg), "ON  %3d v%3d c%d",
                     ev->data.note.note, ev->data.note.velocity,
                     ev->data.note.channel);
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            snprintf(msg, sizeof(msg), "OFF %3d      c%d",
                     ev->data.note.note, ev->data.note.channel);
            break;
        case SND_SEQ_EVENT_CONTROLLER:
            snprintf(msg, sizeof(msg), "CC  %3d=%3d  c%d",
                     ev->data.control.param, ev->data.control.value,
                     ev->data.control.channel);
            break;
        case SND_SEQ_EVENT_PITCHBEND:
            snprintf(msg, sizeof(msg), "PB  %+5d    c%d",
                     ev->data.control.value, ev->data.control.channel);
            break;
        case SND_SEQ_EVENT_PGMCHANGE:
            snprintf(msg, sizeof(msg), "PGM %3d      c%d",
                     ev->data.control.value, ev->data.control.channel);
            break;
        default:
            snprintf(msg, sizeof(msg), "??? type=%d", ev->type);
            break;
        }
        midi_log_push(msg);
        got = 1;
    }
    return got;
}

/* ── Volume ────────────────────────────────────────────────────────── */

static void set_master_volume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_volume = pct;
    float vol = (float)pct / 100.0f;
    uint8_t buf[256];
    int len = osc_build(buf, (int)sizeof(buf), "/rack/master", "f", (double)vol);
    osc_send(buf, len);
}

/* ── Actions ───────────────────────────────────────────────────────── */

static void action_set_type(int idx) {
    if (idx < 0) idx = NUM_TYPES - 1;
    if (idx >= NUM_TYPES) idx = 0;
    g_type_idx = idx;
    g_preset = 0;
    uint8_t buf[256];
    int len = osc_build(buf, (int)sizeof(buf), "/rack/slot/set", "is",
                        (int32_t)g_channel, SYNTH_TYPES[g_type_idx]);
    osc_send(buf, len);
}

static void action_set_preset(int p) {
    if (p < 0) p = 98;
    if (p > 98) p = 0;
    g_preset = p;
    uint8_t buf[256];
    char addr[64];
    snprintf(addr, sizeof(addr), "/ch/%d/preset", g_channel);
    int len = osc_build(buf, (int)sizeof(buf), addr, "i", (int32_t)p);
    osc_send(buf, len);
}

static void action_apply_audio(void) {
    /* miniwave doesn't support live audio switch, so we restart it.
     * try /usr/local/bin first (OS install), fall back to local path */
    const char *bin = "/usr/local/bin/miniwave";
    if (access(bin, X_OK) != 0)
        bin = "~/miniwave/pocketchip/miniwave";

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "pkill miniwave; sleep 1; nohup "
        "%s -c 1 -o %s -P %d "
        "%s%s"
        "> /tmp/miniwave.log 2>&1 &",
        bin,
        g_adev_ids[g_adev_sel], BUF_SIZES[g_buf_sel],
        g_mdev_count > 0 ? "-m " : "",
        g_mdev_count > 0 ? g_mdev_ids[g_mdev_sel] : "");
    system(cmd);
}

static void action_apply_midi(void) {
    /* send OSC /midi/device to miniwave */
    if (g_mdev_count > 0) {
        uint8_t buf[256];
        int len = osc_build(buf, (int)sizeof(buf), "/midi/device", "s",
                            g_mdev_ids[g_mdev_sel]);
        osc_send(buf, len);
    }
}

/* ── Render ────────────────────────────────────────────────────────── */

static void render(void) {
    char out[8192];
    int p = 0;

    term_size();
    p += snprintf(out + p, sizeof(out) - (size_t)p, "\033[2J\033[H");

    switch (g_mode) {

    case MODE_QUIT:
        p += snprintf(out + p, sizeof(out) - (size_t)p,
            "\n \033[1;31mQUIT?\033[0m y/n\n");
        break;

    case MODE_HELP:
        p += snprintf(out + p, sizeof(out) - (size_t)p,
            "\033[1;36m KEYS\033[0m\n\n"
            " \033[33mup/dn\033[0m synth\n"
            " \033[33mlf/rt\033[0m preset\n"
            " \033[33m+ -\033[0m   vol\n"
            " \033[33mtab\033[0m   config\n"
            " \033[33m?\033[0m     help\n"
            " \033[33mesc\033[0m   back/quit\n");
        break;

    case MODE_CONFIG: {
        p += snprintf(out + p, sizeof(out) - (size_t)p,
            "\033[1;36m CONFIG\033[0m\n\n");

        const char *labels[CFG_COUNT] = {
            "AUDIO", "BUFFER", "MIDI", "RESTART", "MONITOR"
        };
        for (int i = 0; i < CFG_COUNT; i++) {
            int sel = (i == g_cfg_cursor);
            p += snprintf(out + p, sizeof(out) - (size_t)p,
                " %s\033[33m%s\033[0m ",
                sel ? "\033[7m" : "", labels[i]);

            switch (i) {
            case CFG_AUDIO:
                if (g_adev_count > 0)
                    p += snprintf(out + p, sizeof(out) - (size_t)p,
                        "%s", g_adev_names[g_adev_sel]);
                else
                    p += snprintf(out + p, sizeof(out) - (size_t)p, "-");
                break;
            case CFG_BUFFER:
                p += snprintf(out + p, sizeof(out) - (size_t)p,
                    "%d", BUF_SIZES[g_buf_sel]);
                break;
            case CFG_MIDI:
                if (g_mdev_count > 0)
                    p += snprintf(out + p, sizeof(out) - (size_t)p,
                        "%s", g_mdev_names[g_mdev_sel]);
                else
                    p += snprintf(out + p, sizeof(out) - (size_t)p, "-");
                break;
            case CFG_RESTART:
                break;
            case CFG_MIDMON:
                break;
            }

            if (sel)
                p += snprintf(out + p, sizeof(out) - (size_t)p, "\033[0m");
            p += snprintf(out + p, sizeof(out) - (size_t)p, "\n");
        }

        p += snprintf(out + p, sizeof(out) - (size_t)p,
            "\n\033[90m lf/rt:change tab:apply\033[0m\n");
        break;
    }

    case MODE_MIDI_MON:
        p += snprintf(out + p, sizeof(out) - (size_t)p,
            "\033[1;36m MIDI\033[0m\n\n");
        for (int i = 0; i < MIDI_LOG_LINES; i++) {
            int idx = (g_midi_log_pos - MIDI_LOG_LINES + i);
            if (idx < 0) idx += MIDI_LOG_LINES * 100;
            idx = idx % MIDI_LOG_LINES;
            if (g_midi_log[idx][0])
                p += snprintf(out + p, sizeof(out) - (size_t)p,
                    " %s\n", g_midi_log[idx]);
            else
                p += snprintf(out + p, sizeof(out) - (size_t)p, "\n");
        }
        break;

    case MODE_MAIN:
    default:
        /* header */
        p += snprintf(out + p, sizeof(out) - (size_t)p,
            "\033[1;36m POCKETWAVE\033[0m"
            "\033[90m %d%%\033[0m",
            g_cpu);
        if (g_flash_ticks > 0)
            p += snprintf(out + p, sizeof(out) - (size_t)p,
                " \033[1;32m%s\033[0m", g_flash_msg);
        p += snprintf(out + p, sizeof(out) - (size_t)p, "\n");

        /* selected synth + mode flags */
        p += snprintf(out + p, sizeof(out) - (size_t)p,
            " \033[1;37m%s\033[0m",
            SYNTH_LABELS[g_type_idx]);
        if (g_legato)
            p += snprintf(out + p, sizeof(out) - (size_t)p, " \033[33mLEG\033[0m");
        else if (g_mono)
            p += snprintf(out + p, sizeof(out) - (size_t)p, " \033[33mMON\033[0m");
        p += snprintf(out + p, sizeof(out) - (size_t)p, "\n\n");

        /* preset */
        p += snprintf(out + p, sizeof(out) - (size_t)p,
            " \033[33mP\033[0m %d\n",
            g_preset);

        /* knob labels — two rows of 4 */
        const char **kn = KNOB_LABELS[g_type_idx];
        p += snprintf(out + p, sizeof(out) - (size_t)p,
            "\033[90m");
        for (int r = 0; r < 2; r++) {
            p += snprintf(out + p, sizeof(out) - (size_t)p, " ");
            for (int k = 0; k < 4; k++) {
                int ki = r * 4 + k;
                p += snprintf(out + p, sizeof(out) - (size_t)p,
                    "%-5s", kn[ki]);
            }
            p += snprintf(out + p, sizeof(out) - (size_t)p, "\n");
        }
        p += snprintf(out + p, sizeof(out) - (size_t)p, "\033[0m");
        break;
    }

    write(STDOUT_FILENO, out, (size_t)p);
}

/* ── Startup state detection ──────────────────────────────────────── */

static void detect_state(void) {
    FILE *f = fopen("/tmp/miniwave.log", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *sp = strstr(line, "slot 0 = ");
        if (sp) {
            sp += 9;
            char *nl = strchr(sp, '\n');
            if (nl) *nl = '\0';
            const char *display_names[] = {
                "FM Synth (yama-bruh)", "YM2413 OPLL",
                "Subtractive Synth", "FM Drums",
                "Additive Synth", "Phase Distortion",
                "Bird"
            };
            for (int i = 0; i < NUM_TYPES; i++) {
                if (strcmp(sp, display_names[i]) == 0) {
                    g_type_idx = i;
                    break;
                }
            }
        }
        /* detect audio device */
        char *hw = strstr(line, "ALSA hw:");
        if (hw) {
            int card = atoi(hw + 8);
            for (int i = 0; i < g_adev_count; i++) {
                int ac = atoi(g_adev_ids[i] + 3);
                if (ac == card) { g_adev_sel = i; break; }
            }
        }
        /* detect period */
        char *per = strstr(line, "period=");
        if (per) {
            int ps = atoi(per + 7);
            for (int i = 0; i < NUM_BUFS; i++) {
                if (BUF_SIZES[i] == ps) { g_buf_sel = i; break; }
            }
        }
    }
    fclose(f);
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    /* --launch: re-exec self inside fullscreen stterm */
    if (argc > 1 && strcmp(argv[1], "--launch") == 0) {
        char self[256];
        ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (len > 0) {
            self[len] = '\0';
            execlp("stterm", "stterm",
                   "-f", "monospace:size=24",
                   "-e", self, NULL);
        }
        return 1;
    }

    if (osc_init(9000) != 0) {
        fprintf(stderr, "can't create OSC socket\n");
        return 1;
    }

    scan_audio_devices();
    scan_midi_devices();
    detect_state();

    get_cpu_pct();
    update_battery();
    term_raw();
    render();

    int tick = 0;
    while (!g_quit) {
        char seq[8];
        int n = (int)read(STDIN_FILENO, seq, sizeof(seq));

        int dirty = 0;

        /* poll MIDI monitor if active */
        if (g_mode == MODE_MIDI_MON) {
            if (midi_mon_poll()) dirty = 1;
        }

        if (n > 0) {
            /* Ctrl+C always quits */
            if (n == 1 && seq[0] == 3)
                break;

            /* MIDI monitor: log keystrokes (except ESC) */
            if (g_mode == MODE_MIDI_MON && !(n == 1 && seq[0] == '\033')) {
                char msg[32];
                int mp = snprintf(msg, sizeof(msg), "KEY");
                for (int i = 0; i < n && i < 6; i++)
                    mp += snprintf(msg + mp, sizeof(msg) - (size_t)mp,
                        " %02x", (unsigned char)seq[i]);
                midi_log_push(msg);
                dirty = 1;
            }

            /* ESC (bare, n==1) — go back */
            if (n == 1 && seq[0] == '\033') {
                switch (g_mode) {
                case MODE_HELP:
                case MODE_CONFIG:
                    g_mode = MODE_MAIN; dirty = 1; break;
                case MODE_MIDI_MON:
                    midi_mon_close();
                    g_mode = MODE_CONFIG; dirty = 1; break;
                case MODE_QUIT:
                    g_mode = MODE_MAIN; dirty = 1; break;
                case MODE_MAIN:
                    g_mode = MODE_QUIT; dirty = 1; break;
                }
                continue;
            }

            /* quit confirm */
            if (g_mode == MODE_QUIT) {
                if (n == 1 && (seq[0] == 'y' || seq[0] == 'Y'))
                    break;
                g_mode = MODE_MAIN; dirty = 1;
                continue;
            }

            /* ? — help toggle */
            if (n == 1 && seq[0] == '?') {
                g_mode = (g_mode == MODE_HELP) ? MODE_MAIN : MODE_HELP;
                dirty = 1;
                continue;
            }

            /* s — save */
            if (n == 1 && seq[0] == 's' && g_mode == MODE_MAIN) {
                /* trigger save by sending current master volume (no-op change) */
                uint8_t obuf[64];
                int olen = osc_build(obuf, (int)sizeof(obuf), "/rack/save", "");
                osc_send(obuf, olen);
                snprintf(g_flash_msg, sizeof(g_flash_msg), "SAVED");
                g_flash_ticks = 15;
                dirty = 1;
                continue;
            }

            /* m — toggle mono */
            if (n == 1 && seq[0] == 'm' && g_mode == MODE_MAIN) {
                g_mono = !g_mono;
                uint8_t obuf[64];
                int olen = osc_build(obuf, (int)sizeof(obuf), "/rack/slot/mono", "ii",
                                     (int32_t)g_channel, (int32_t)g_mono);
                osc_send(obuf, olen);
                snprintf(g_flash_msg, sizeof(g_flash_msg), "MONO %s", g_mono ? "ON" : "OFF");
                g_flash_ticks = 15;
                dirty = 1;
                continue;
            }

            /* l — toggle legato */
            if (n == 1 && seq[0] == 'l' && g_mode == MODE_MAIN) {
                g_legato = !g_legato;
                if (g_legato) g_mono = 1; /* legato implies mono */
                uint8_t obuf[64];
                int olen = osc_build(obuf, (int)sizeof(obuf), "/rack/slot/legato", "ii",
                                     (int32_t)g_channel, (int32_t)g_legato);
                osc_send(obuf, olen);
                olen = osc_build(obuf, (int)sizeof(obuf), "/rack/slot/mono", "ii",
                                 (int32_t)g_channel, (int32_t)g_mono);
                osc_send(obuf, olen);
                snprintf(g_flash_msg, sizeof(g_flash_msg), "LEGATO %s", g_legato ? "ON" : "OFF");
                g_flash_ticks = 15;
                dirty = 1;
                continue;
            }

            /* Enter / Return */
            if (n == 1 && (seq[0] == '\t' || seq[0] == '\r' || seq[0] == '\n')) {
                if (g_mode == MODE_MAIN) {
                    scan_audio_devices();
                    scan_midi_devices();
                    g_mode = MODE_CONFIG;
                    dirty = 1;
                } else if (g_mode == MODE_CONFIG) {
                    if (g_cfg_cursor == CFG_MIDMON) {
                        midi_mon_open();
                        g_mode = MODE_MIDI_MON;
                        dirty = 1;
                    } else if (g_cfg_cursor == CFG_MIDI) {
                        action_apply_midi();
                        g_mode = MODE_MAIN;
                        dirty = 1;
                    } else if (g_cfg_cursor == CFG_RESTART) {
                        /* rescan devices then restart miniwave */
                        scan_audio_devices();
                        scan_midi_devices();
                        action_apply_audio();
                        g_mode = MODE_MAIN;
                        snprintf(g_flash_msg, sizeof(g_flash_msg), "RESTARTED");
                        g_flash_ticks = 15;
                        dirty = 1;
                    } else {
                        /* audio/buffer — apply together, restart miniwave */
                        action_apply_audio();
                        g_mode = MODE_MAIN;
                        dirty = 1;
                    }
                }
                continue;
            }

            /* arrow keys */
            if (n == 3 && seq[0] == '\033' && seq[1] == '[') {
                char arrow = seq[2];

                if (g_mode == MODE_MAIN) {
                    switch (arrow) {
                    case 'A': action_set_type(g_type_idx - 1); dirty = 1; break;
                    case 'B': action_set_type(g_type_idx + 1); dirty = 1; break;
                    case 'C': action_set_preset(g_preset + 1); dirty = 1; break;
                    case 'D': action_set_preset(g_preset - 1); dirty = 1; break;
                    }
                } else if (g_mode == MODE_CONFIG) {
                    switch (arrow) {
                    case 'A': /* up */
                        g_cfg_cursor--;
                        if (g_cfg_cursor < 0) g_cfg_cursor = CFG_COUNT - 1;
                        dirty = 1; break;
                    case 'B': /* down */
                        g_cfg_cursor++;
                        if (g_cfg_cursor >= CFG_COUNT) g_cfg_cursor = 0;
                        dirty = 1; break;
                    case 'C': /* right — next option */
                        if (g_cfg_cursor == CFG_AUDIO && g_adev_count > 0) {
                            g_adev_sel = (g_adev_sel + 1) % g_adev_count;
                            dirty = 1;
                        } else if (g_cfg_cursor == CFG_BUFFER) {
                            g_buf_sel = (g_buf_sel + 1) % NUM_BUFS;
                            dirty = 1;
                        } else if (g_cfg_cursor == CFG_MIDI && g_mdev_count > 0) {
                            g_mdev_sel = (g_mdev_sel + 1) % g_mdev_count;
                            dirty = 1;
                        }
                        break;
                    case 'D': /* left — prev option */
                        if (g_cfg_cursor == CFG_AUDIO && g_adev_count > 0) {
                            g_adev_sel = (g_adev_sel - 1 + g_adev_count) % g_adev_count;
                            dirty = 1;
                        } else if (g_cfg_cursor == CFG_BUFFER) {
                            g_buf_sel = (g_buf_sel - 1 + NUM_BUFS) % NUM_BUFS;
                            dirty = 1;
                        } else if (g_cfg_cursor == CFG_MIDI && g_mdev_count > 0) {
                            g_mdev_sel = (g_mdev_sel - 1 + g_mdev_count) % g_mdev_count;
                            dirty = 1;
                        }
                        break;
                    }
                }
            }

            /* +/- volume (main mode only) */
            if (g_mode == MODE_MAIN) {
                if (n == 1 && (seq[0] == '+' || seq[0] == '=')) {
                    set_master_volume(g_volume + 5); dirty = 1;
                }
                if (n == 1 && seq[0] == '-') {
                    set_master_volume(g_volume - 5); dirty = 1;
                }
            }

            /* Ctrl+Up/Down volume */
            if (g_mode == MODE_MAIN && n == 6 && seq[0] == '\033' && seq[1] == '['
                && seq[2] == '1' && seq[3] == ';' && seq[4] == '5') {
                if (seq[5] == 'A') { set_master_volume(g_volume + 5); dirty = 1; }
                if (seq[5] == 'B') { set_master_volume(g_volume - 5); dirty = 1; }
            }
        }

        /* flash message countdown */
        if (g_flash_ticks > 0 && --g_flash_ticks == 0 && g_mode == MODE_MAIN)
            dirty = 1;

        /* CPU update every ~2s */
        if (++tick >= 20) {
            int cpu = get_cpu_pct();
            if (cpu >= 0 && abs(cpu - g_cpu) > 2) {
                g_cpu = cpu;
                if (g_mode == MODE_MAIN) dirty = 1;
            }
            int old_batt = g_batt_pct;
            update_battery();
            if (g_batt_pct != old_batt && g_mode == MODE_MAIN) dirty = 1;
            tick = 0;
        }

        if (dirty) render();
    }

    midi_mon_close();
    term_restore();
    if (g_osc_fd >= 0) close(g_osc_fd);
    return 0;
}
