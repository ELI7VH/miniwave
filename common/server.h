/* miniwave — HTTP/SSE + OSC server
 *
 * Embedded HTTP server (WaveUI), SSE push, OSC UDP control.
 * All platform-independent — calls platform_midi_* for MIDI operations.
 */

#ifndef MINIWAVE_SERVER_H
#define MINIWAVE_SERVER_H

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>

/* ── HTML content (loaded at startup) ──────────────────────────────── */

static char *g_html_content = NULL;
static size_t g_html_length = 0;

typedef struct {
    int  fd;
    int  is_sse;
    int  detail_channel;
} HttpClient;

static HttpClient g_http_clients[MAX_HTTP_CLIENTS];
static pthread_mutex_t g_http_lock = PTHREAD_MUTEX_INITIALIZER;

static void http_load_html(void) {
    char exe_dir[1024];
    if (platform_exe_dir(exe_dir, sizeof(exe_dir)) < 0) {
        fprintf(stderr, "[http] can't resolve executable directory\n");
        return;
    }

    char html_path[1280];
    snprintf(html_path, sizeof(html_path), "%sweb/index.html", exe_dir);

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

    /* Count active SSE clients */
    int sse_count = 0;
    pthread_mutex_lock(&g_http_lock);
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++)
        if (g_http_clients[i].fd >= 0 && g_http_clients[i].is_sse) sse_count++;
    pthread_mutex_unlock(&g_http_lock);

    pos += snprintf(buf + pos, (size_t)(max - pos),
        "],\"master_volume\":%.4f,\"midi_device\":\"%s\","
        "\"audio_backend\":\"%s\",\"local_mute\":%d,"
        "\"osc_port\":%d,\"mcast_active\":%d,\"mcast_group\":\"%s:%d\","
        "\"bus_active\":%d,\"bus_slot\":%d,"
        "\"sse_clients\":%d}",
        (double)g_rack.master_volume, g_midi_device_name,
        g_use_jack ? "JACK" : platform_audio_fallback_name(),
        g_rack.local_mute,
        DEFAULT_OSC_PORT, g_mcast_active,
        MCAST_GROUP, DEFAULT_MCAST_PORT,
        g_bus_active, g_bus_slot,
        sse_count);

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
    int ndevs = platform_midi_list_devices(devs, devnames, 16);

    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(max - pos), "{\"type\":\"midi_devices\",\"devices\":[");
    for (int i = 0; i < ndevs; i++) {
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

    if (strcmp(itype->name, "sub-synth") == 0) {
        SubSynth *sub = (SubSynth *)slot->state;
        int active_v = 0;
        for (int v = 0; v < SUB_MAX_VOICES; v++)
            if (sub->voices[v].active) active_v++;

        const char *wave_names[] = {"Saw","Square","Pulse","Triangle","Sine","Noise"};
        const char *wname = (sub->params.waveform >= 0 && sub->params.waveform < SUB_WAVE_COUNT)
                            ? wave_names[sub->params.waveform] : "Saw";

        return snprintf(buf, (size_t)max,
            "{\"type\":\"ch_status\",\"channel\":%d,"
            "\"instrument_type\":\"sub-synth\","
            "\"waveform\":%d,\"waveform_name\":\"%s\","
            "\"volume\":%.4f,"
            "\"params\":{"
            "\"filter_cutoff\":%.4f,\"filter_reso\":%.4f,"
            "\"filter_env_depth\":%.4f,\"pulse_width\":%.4f,"
            "\"filt_attack\":%.4f,\"filt_decay\":%.4f,"
            "\"filt_sustain\":%.4f,\"filt_release\":%.4f,"
            "\"amp_attack\":%.4f,\"amp_decay\":%.4f,"
            "\"amp_sustain\":%.4f,\"amp_release\":%.4f},"
            "\"active_voices\":%d}",
            ch, sub->params.waveform, wname,
            (double)sub->volume,
            (double)sub->params.filter_cutoff, (double)sub->params.filter_reso,
            (double)sub->params.filter_env_depth, (double)sub->params.pulse_width,
            (double)sub->params.filt_attack, (double)sub->params.filt_decay,
            (double)sub->params.filt_sustain, (double)sub->params.filt_release,
            (double)sub->params.amp_attack, (double)sub->params.amp_decay,
            (double)sub->params.amp_sustain, (double)sub->params.amp_release,
            active_v);
    }

    /* FM Drums */
    if (strcmp(itype->name, "fm-drums") == 0) {
        FMDrumState *ds = (FMDrumState *)slot->state;
        int active_v = 0;
        for (int v = 0; v < FMD_MAX_VOICES; v++)
            if (ds->voices[v].active) active_v++;

        int en = ds->editing_note;
        if (en < 0 || en >= FMD_NUM_NOTES) en = 36;
        const FMDrumNote *dn = &ds->notes[en];
        const char *pname = (dn->preset >= 0 && dn->preset < FMD_NUM_PRESETS)
                            ? FMD_PRESET_NAMES[dn->preset] : "Custom";
        const FMDrumDef *ed = &dn->def;

        return snprintf(buf, (size_t)max,
            "{\"type\":\"ch_status\",\"channel\":%d,"
            "\"instrument_type\":\"fm-drums\","
            "\"volume\":%.4f,"
            "\"editing_note\":%d,\"preset\":%d,\"preset_name\":\"%s\","
            "\"params\":{"
            "\"carrier_freq\":%.4f,\"mod_freq\":%.4f,\"mod_index\":%.4f,"
            "\"pitch_sweep\":%.4f,\"pitch_decay\":%.5f,"
            "\"decay\":%.4f,\"noise_amt\":%.4f,"
            "\"click_amt\":%.4f,\"feedback\":%.4f},"
            "\"active_voices\":%d}",
            ch, (double)ds->volume,
            en, dn->preset, pname,
            (double)ed->carrier_freq, (double)ed->mod_freq, (double)ed->mod_index,
            (double)ed->pitch_sweep, (double)ed->pitch_decay,
            (double)ed->decay, (double)ed->noise_amt,
            (double)ed->click_amt, (double)ed->feedback,
            active_v);
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
    "{\"name\":\"miniwave\",\"version\":\"2.0\","
    "\"transports\":{\"osc_udp\":9000,\"http_sse\":8080,\"multicast\":{\"group\":\"239.0.0.42\",\"port\":9001}},"
    "\"endpoints\":["
    "{\"path\":\"/rack/slot/set\",\"args\":\"is\",\"dir\":\"bidi\",\"desc\":\"Set slot instrument (ch, type_name)\"},"
    "{\"path\":\"/rack/slot/clear\",\"args\":\"i\",\"dir\":\"bidi\",\"desc\":\"Clear slot (ch)\"},"
    "{\"path\":\"/rack/slot/volume\",\"args\":\"if\",\"dir\":\"bidi\",\"desc\":\"Set slot volume (ch, 0-1)\"},"
    "{\"path\":\"/rack/slot/mute\",\"args\":\"ii\",\"dir\":\"bidi\",\"desc\":\"Mute slot (ch, 0/1)\"},"
    "{\"path\":\"/rack/slot/solo\",\"args\":\"ii\",\"dir\":\"bidi\",\"desc\":\"Solo slot (ch, 0/1)\"},"
    "{\"path\":\"/rack/master\",\"args\":\"f\",\"dir\":\"bidi\",\"desc\":\"Master volume (0-1)\"},"
    "{\"path\":\"/rack/local_mute\",\"args\":\"i\",\"dir\":\"bidi\",\"desc\":\"Bus-only mode (0/1)\"},"
    "{\"path\":\"/rack/status\",\"args\":\"\",\"dir\":\"query\",\"desc\":\"Get full rack state\"},"
    "{\"path\":\"/rack/types\",\"args\":\"\",\"dir\":\"query\",\"desc\":\"List instrument types\"},"
    "{\"path\":\"/note/on\",\"args\":\"iii\",\"dir\":\"out\",\"desc\":\"Note on (ch, note, vel) — multicast only\"},"
    "{\"path\":\"/note/off\",\"args\":\"ii\",\"dir\":\"out\",\"desc\":\"Note off (ch, note) — multicast only\"},"
    "{\"path\":\"/midi/device\",\"args\":\"s\",\"dir\":\"bidi\",\"desc\":\"Connect MIDI device\"},"
    "{\"path\":\"/midi/disconnect\",\"args\":\"\",\"dir\":\"bidi\",\"desc\":\"Disconnect MIDI\"},"
    "{\"path\":\"/midi/devices\",\"args\":\"\",\"dir\":\"query\",\"desc\":\"List MIDI sources\"},"
    "{\"path\":\"/ch/N/status\",\"args\":\"\",\"dir\":\"query\",\"desc\":\"Get channel detail\"},"
    "{\"path\":\"/ch/N/preset\",\"args\":\"i\",\"dir\":\"in\",\"desc\":\"Set preset index\"},"
    "{\"path\":\"/ch/N/param/KEY\",\"args\":\"f\",\"dir\":\"in\",\"desc\":\"Set parameter by name\"},"
    "{\"path\":\"/ch/N/volume\",\"args\":\"f\",\"dir\":\"in\",\"desc\":\"Set channel volume\"}"
    "]}";

/* ── HTTP response helpers ─────────────────────────────────────────── */

static inline ssize_t http_write(int fd, const void *buf, size_t len) {
    ssize_t r = write(fd, buf, len);
    return r;
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

/* ── OSC Multicast Relay (fire-and-forget broadcast) ───────────────── */

#define MCAST_RING_SIZE 64
#define MCAST_MSG_MAX   512

typedef struct {
    uint8_t data[MCAST_MSG_MAX];
    int     len;
} McastMsg;

static struct {
    McastMsg    ring[MCAST_RING_SIZE];
    _Atomic int write_pos;
    _Atomic int read_pos;
    int         sock;
    struct sockaddr_in addr;
    int         active;
} g_mcast = {0};

static void mcast_push_raw(const uint8_t *data, int len) {
    if (!g_mcast.active || len <= 0 || len > MCAST_MSG_MAX) return;
    int wp = atomic_load(&g_mcast.write_pos);
    int next = (wp + 1) % MCAST_RING_SIZE;
    if (next == atomic_load(&g_mcast.read_pos)) return;
    memcpy(g_mcast.ring[wp].data, data, (size_t)len);
    g_mcast.ring[wp].len = len;
    atomic_store(&g_mcast.write_pos, next);
}

static void mcast_push(const char *osc_addr, const char *types,
                        const int32_t *ivals, const float *fvals,
                        const char *sval) {
    uint8_t buf[MCAST_MSG_MAX];
    int pos = 0, w;
    w = osc_write_string(buf, MCAST_MSG_MAX, osc_addr);
    if (w < 0) return; pos += w;
    char ttag[64] = ","; int tp = 1;
    for (const char *t = types; *t; t++) ttag[tp++] = *t;
    ttag[tp] = '\0';
    w = osc_write_string(buf + pos, MCAST_MSG_MAX - pos, ttag);
    if (w < 0) return; pos += w;
    int ii = 0, fi = 0;
    for (const char *t = types; *t && pos + 4 <= MCAST_MSG_MAX; t++) {
        switch (*t) {
        case 'i': osc_write_i32(buf + pos, ivals[ii++]); pos += 4; break;
        case 'f': osc_write_f32(buf + pos, fvals[fi++]); pos += 4; break;
        case 's': if (sval) { w = osc_write_string(buf + pos, MCAST_MSG_MAX - pos, sval); if (w > 0) pos += w; } break;
        }
    }
    mcast_push_raw(buf, pos);
}

static void mcast_slot_set(int ch, const char *type)   { int32_t iv[] = {ch}; mcast_push("/rack/slot/set", "is", iv, NULL, type); }
static void mcast_slot_clear(int ch)                    { int32_t iv[] = {ch}; mcast_push("/rack/slot/clear", "i", iv, NULL, NULL); }
static void mcast_slot_volume(int ch, float vol)        { int32_t iv[] = {ch}; float fv[] = {vol}; mcast_push("/rack/slot/volume", "if", iv, fv, NULL); }
static void mcast_slot_mute(int ch, int val)            { int32_t iv[] = {ch, val}; mcast_push("/rack/slot/mute", "ii", iv, NULL, NULL); }
static void mcast_slot_solo(int ch, int val)            { int32_t iv[] = {ch, val}; mcast_push("/rack/slot/solo", "ii", iv, NULL, NULL); }
static void mcast_master(float vol)                     { float fv[] = {vol}; mcast_push("/rack/master", "f", NULL, fv, NULL); }
static void mcast_local_mute(int val)                   { int32_t iv[] = {val}; mcast_push("/rack/local_mute", "i", iv, NULL, NULL); }
static void mcast_midi_device(const char *name)         { mcast_push("/midi/device", "s", NULL, NULL, name); }
static void mcast_note_on(int ch, int note, int vel)    { int32_t iv[] = {ch, note, vel}; mcast_push("/note/on", "iii", iv, NULL, NULL); }
static void mcast_note_off(int ch, int note)            { int32_t iv[] = {ch, note}; mcast_push("/note/off", "ii", iv, NULL, NULL); }

static void *mcast_thread_fn(void *arg) {
    (void)arg;
    while (!g_quit) {
        int rp = atomic_load(&g_mcast.read_pos);
        int wp = atomic_load(&g_mcast.write_pos);
        if (rp == wp) { usleep(1000); continue; }
        while (rp != wp) {
            McastMsg *m = &g_mcast.ring[rp];
            sendto(g_mcast.sock, m->data, (size_t)m->len, 0,
                   (struct sockaddr *)&g_mcast.addr, sizeof(g_mcast.addr));
            rp = (rp + 1) % MCAST_RING_SIZE;
        }
        atomic_store(&g_mcast.read_pos, rp);
    }
    return NULL;
}

static void mcast_midi_callback(int ch, int note, int vel, int is_on) {
    if (is_on) mcast_note_on(ch, note, vel);
    else       mcast_note_off(ch, note);
}

static int mcast_init(void) {
    g_mcast.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_mcast.sock < 0) return -1;
    memset(&g_mcast.addr, 0, sizeof(g_mcast.addr));
    g_mcast.addr.sin_family = AF_INET;
    g_mcast.addr.sin_addr.s_addr = inet_addr(MCAST_GROUP);
    g_mcast.addr.sin_port = htons(DEFAULT_MCAST_PORT);
    int ttl = 1; setsockopt(g_mcast.sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    int loop = 0; setsockopt(g_mcast.sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    atomic_store(&g_mcast.write_pos, 0);
    atomic_store(&g_mcast.read_pos, 0);
    g_mcast.active = 1;
    g_mcast_active = 1;
    g_midi_broadcast = mcast_midi_callback;
    fprintf(stderr, "[miniwave] OSC multicast → %s:%d\n", MCAST_GROUP, DEFAULT_MCAST_PORT);
    return 0;
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
        mcast_slot_set(ch, instr);
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "slot_clear") == 0) {
        int ch = 0;
        json_get_int(body, "channel", &ch);
        rack_clear_slot(ch);
        mcast_slot_clear(ch);
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
            mcast_slot_volume(ch, val);
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "slot_mute") == 0) {
        int ch = 0, val = 0;
        json_get_int(body, "channel", &ch);
        json_get_int(body, "value", &val);
        if (ch >= 0 && ch < MAX_SLOTS) {
            g_rack.slots[ch].mute = val ? 1 : 0;
            mcast_slot_mute(ch, g_rack.slots[ch].mute);
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "slot_solo") == 0) {
        int ch = 0, val = 0;
        json_get_int(body, "channel", &ch);
        json_get_int(body, "value", &val);
        if (ch >= 0 && ch < MAX_SLOTS) {
            g_rack.slots[ch].solo = val ? 1 : 0;
            mcast_slot_solo(ch, g_rack.slots[ch].solo);
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "master_volume") == 0) {
        float val = 0.75f;
        json_get_float(body, "value", &val);
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;
        g_rack.master_volume = val;
        mcast_master(val);
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "local_mute") == 0) {
        int val = 0;
        json_get_int(body, "value", &val);
        g_rack.local_mute = val ? 1 : 0;
        mcast_local_mute(g_rack.local_mute);
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
                state_mark_dirty();
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
        mcast_note_on(ch, note, vel);
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
        mcast_note_off(ch, note);
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "midi_device") == 0) {
        char dev[64] = "";
        json_get_string(body, "value", dev, sizeof(dev));
        if (dev[0]) {
            int err2 = platform_midi_connect(dev);
            mcast_midi_device(g_midi_device_name);
            rlen = snprintf(resp, sizeof(resp), err2 == 0
                ? "{\"ok\":true}" : "{\"error\":\"can't connect\"}");
        } else {
            platform_midi_disconnect();
            mcast_midi_device("");
            rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
        }
    }
    else if (strcmp(type_str, "seq_dsl") == 0) {
        int ch = 0;
        char dsl[256] = "";
        json_get_int(body, "channel", &ch);
        json_get_string(body, "dsl", dsl, sizeof(dsl));

        if (ch >= 0 && ch < MAX_SLOTS && dsl[0]) {
            RackSlot *slot = &g_rack.slots[ch];
            if (slot->active && slot->state) {
                MiniSeq *mseq = NULL;
                const char *tname = g_type_registry[slot->type_idx]->name;
                if (strcmp(tname, "fm-synth") == 0)
                    mseq = &((FMSynth *)slot->state)->seq;
                else if (strcmp(tname, "sub-synth") == 0)
                    mseq = &((SubSynth *)slot->state)->seq;
                else if (strcmp(tname, "ym2413") == 0)
                    mseq = &((YM2413State *)slot->state)->mini_seq;
                else if (strcmp(tname, "fm-drums") == 0)
                    mseq = &((FMDrumState *)slot->state)->seq;

                if (mseq) {
                    seq_handle_dsl(mseq, dsl);
                    rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
                } else {
                    rlen = snprintf(resp, sizeof(resp), "{\"error\":\"no seq\"}");
                }
            } else {
                rlen = snprintf(resp, sizeof(resp), "{\"error\":\"no instrument\"}");
            }
        } else {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"bad args\"}");
        }
    }
    else if (strcmp(type_str, "seq_stop") == 0) {
        int ch = 0;
        json_get_int(body, "channel", &ch);
        if (ch >= 0 && ch < MAX_SLOTS) {
            RackSlot *slot = &g_rack.slots[ch];
            if (slot->active && slot->state) {
                MiniSeq *mseq = NULL;
                const char *tname = g_type_registry[slot->type_idx]->name;
                if (strcmp(tname, "fm-synth") == 0)
                    mseq = &((FMSynth *)slot->state)->seq;
                else if (strcmp(tname, "sub-synth") == 0)
                    mseq = &((SubSynth *)slot->state)->seq;
                else if (strcmp(tname, "ym2413") == 0)
                    mseq = &((YM2413State *)slot->state)->mini_seq;
                else if (strcmp(tname, "fm-drums") == 0)
                    mseq = &((FMDrumState *)slot->state)->seq;
                if (mseq) seq_stop(mseq);
            }
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "keyseq_dsl") == 0) {
        int ch = 0;
        char dsl[256] = "";
        json_get_int(body, "channel", &ch);
        json_get_string(body, "dsl", dsl, sizeof(dsl));

        if (ch >= 0 && ch < MAX_SLOTS && dsl[0]) {
            RackSlot *slot = &g_rack.slots[ch];
            if (slot->active && slot->state) {
                KeySeq *ks = NULL;
                const char *tname = g_type_registry[slot->type_idx]->name;
                if (strcmp(tname, "fm-synth") == 0)
                    ks = &((FMSynth *)slot->state)->keyseq;
                else if (strcmp(tname, "sub-synth") == 0)
                    ks = &((SubSynth *)slot->state)->keyseq;
                else if (strcmp(tname, "ym2413") == 0)
                    ks = &((YM2413State *)slot->state)->keyseq;
                else if (strcmp(tname, "fm-drums") == 0)
                    ks = &((FMDrumState *)slot->state)->keyseq;

                if (ks) {
                    keyseq_parse(ks, dsl);
                    rlen = snprintf(resp, sizeof(resp),
                        "{\"ok\":true,\"steps\":%d}", ks->num_steps);
                } else {
                    rlen = snprintf(resp, sizeof(resp), "{\"error\":\"no keyseq\"}");
                }
            } else {
                rlen = snprintf(resp, sizeof(resp), "{\"error\":\"no instrument\"}");
            }
        } else {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"bad args\"}");
        }
    }
    else if (strcmp(type_str, "keyseq_stop") == 0) {
        int ch = 0;
        json_get_int(body, "channel", &ch);
        if (ch >= 0 && ch < MAX_SLOTS) {
            RackSlot *slot = &g_rack.slots[ch];
            if (slot->active && slot->state) {
                KeySeq *ks = NULL;
                const char *tname = g_type_registry[slot->type_idx]->name;
                if (strcmp(tname, "fm-synth") == 0)
                    ks = &((FMSynth *)slot->state)->keyseq;
                else if (strcmp(tname, "sub-synth") == 0)
                    ks = &((SubSynth *)slot->state)->keyseq;
                else if (strcmp(tname, "ym2413") == 0)
                    ks = &((YM2413State *)slot->state)->keyseq;
                else if (strcmp(tname, "fm-drums") == 0)
                    ks = &((FMDrumState *)slot->state)->keyseq;
                if (ks) { keyseq_stop(ks); ks->enabled = 0; }
            }
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
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

    pthread_mutex_lock(&g_http_lock);
    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
        g_http_clients[i].fd = -1;
        g_http_clients[i].is_sse = 0;
        g_http_clients[i].detail_channel = -1;
    }
    pthread_mutex_unlock(&g_http_lock);

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

    fcntl(listen_fd, F_SETFL, O_NONBLOCK);

    fprintf(stderr, "[http] WaveUI at http://0.0.0.0:%d\n", ctx->port);

    struct timespec last_rack_poll = {0, 0};
    struct timespec last_detail_poll = {0, 0};

    while (!g_quit) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd >= 0) {
            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            int flag = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            char reqbuf[HTTP_BUF_SIZE];
            memset(reqbuf, 0, sizeof(reqbuf));
            ssize_t nread = recv(client_fd, reqbuf, sizeof(reqbuf) - 1, 0);

            /* If we got headers, check Content-Length and read remaining body */
            if (nread > 0) {
                reqbuf[nread] = '\0';
                char *hdr_end = strstr(reqbuf, "\r\n\r\n");
                if (hdr_end) {
                    int body_got = (int)(nread - (hdr_end + 4 - reqbuf));
                    /* Parse Content-Length */
                    const char *cl = strstr(reqbuf, "Content-Length:");
                    if (!cl) cl = strstr(reqbuf, "content-length:");
                    if (cl) {
                        int content_len = atoi(cl + 15);
                        int remaining = content_len - body_got;
                        while (remaining > 0 && nread < (ssize_t)(sizeof(reqbuf) - 1)) {
                            ssize_t r = recv(client_fd, reqbuf + nread,
                                (size_t)(sizeof(reqbuf) - 1 - (size_t)nread), 0);
                            if (r <= 0) break;
                            nread += r;
                            remaining -= (int)r;
                        }
                        reqbuf[nread] = '\0';
                    }
                }

                char method[8] = "";
                char path[256] = "";
                sscanf(reqbuf, "%7s %255s", method, path);

                /* Strip query string for routing */
                char *qmark = strchr(path, '?');
                if (qmark) *qmark = '\0';

                if (strcmp(method, "OPTIONS") == 0) {
                    http_send_response(client_fd, 204, "text/plain", "", 0);
                    close(client_fd);
                }
                else if (strcmp(method, "GET") == 0 &&
                         (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
                    if (g_html_content) {
                        char hdr[512];
                        int hlen = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/html; charset=utf-8\r\n"
                            "Content-Length: %zu\r\n"
                            "Cache-Control: no-cache\r\n"
                            "Access-Control-Allow-Origin: *\r\n"
                            "Connection: close\r\n"
                            "\r\n", g_html_length);
                        http_write(client_fd, hdr, (size_t)hlen);
                        http_write(client_fd, g_html_content, g_html_length);
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
                else if (strcmp(method, "GET") == 0 && strcmp(path, "/keyseq-test") == 0) {
                    /* Serve keyseq-test.html from web/ dir */
                    char exe_dir[1024];
                    if (platform_exe_dir(exe_dir, sizeof(exe_dir)) == 0) {
                        char fpath[1280];
                        snprintf(fpath, sizeof(fpath), "%sweb/keyseq-test.html", exe_dir);
                        FILE *f = fopen(fpath, "r");
                        if (f) {
                            fseek(f, 0, SEEK_END);
                            long sz = ftell(f);
                            fseek(f, 0, SEEK_SET);
                            char *data = malloc((size_t)sz);
                            if (data) {
                                size_t rd = fread(data, 1, (size_t)sz, f);
                                http_send_response(client_fd, 200, "text/html; charset=utf-8",
                                                   data, (int)rd);
                                free(data);
                            }
                            fclose(f);
                        } else {
                            http_send_response(client_fd, 404, "text/plain", "Not Found", 9);
                        }
                    }
                    close(client_fd);
                }
                else if (strcmp(method, "GET") == 0 &&
                         (strcmp(path, "/osc-spec") == 0 || strcmp(path, "/osc-map") == 0)) {
                    http_send_response(client_fd, 200, "application/json",
                                       OSC_SPEC_JSON, (int)strlen(OSC_SPEC_JSON));
                    close(client_fd);
                }
                else if (strcmp(method, "GET") == 0 && strcmp(path, "/events") == 0) {
                    fcntl(client_fd, F_SETFL, O_NONBLOCK);

                    http_send_sse_headers(client_fd);

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
                        char json[SSE_BUF_SIZE];
                        build_rack_status_json(json, (int)sizeof(json));
                        sse_send_event(client_fd, "rack_status", json);

                        build_rack_types_json(json, (int)sizeof(json));
                        sse_send_event(client_fd, "rack_types", json);

                        build_midi_devices_json(json, (int)sizeof(json));
                        sse_send_event(client_fd, "midi_devices", json);
                    }
                }
                else if (strcmp(method, "POST") == 0 && strcmp(path, "/api") == 0) {
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
                else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/detail") == 0) {
                    const char *body = strstr(reqbuf, "\r\n\r\n");
                    int detail_ch = -1;
                    if (body) {
                        body += 4;
                        json_get_int(body, "channel", &detail_ch);
                    }

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

        long rack_elapsed_ms = (now.tv_sec - last_rack_poll.tv_sec) * 1000 +
                               (now.tv_nsec - last_rack_poll.tv_nsec) / 1000000;
        if (rack_elapsed_ms >= 500) {
            last_rack_poll = now;
            char json[SSE_BUF_SIZE];
            build_rack_status_json(json, (int)sizeof(json));
            sse_broadcast("rack_status", json);
        }

        long detail_elapsed_ms = (now.tv_sec - last_detail_poll.tv_sec) * 1000 +
                                 (now.tv_nsec - last_detail_poll.tv_nsec) / 1000000;
        if (detail_elapsed_ms >= 200) {
            last_detail_poll = now;
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

        /* Auto-save state (debounced: 2 seconds after last change) */
        if (g_state_dirty) {
            static struct timespec dirty_time = {0, 0};
            if (dirty_time.tv_sec == 0) {
                dirty_time = now;
            } else {
                long save_elapsed = (now.tv_sec - dirty_time.tv_sec) * 1000 +
                                    (now.tv_nsec - dirty_time.tv_nsec) / 1000000;
                if (save_elapsed >= 2000) {
                    state_save();
                    g_state_dirty = 0;
                    dirty_time.tv_sec = 0;
                }
            }
        }

        usleep(10000);
    }

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

/* ── OSC Server Thread ──────────────────────────────────────────────── */

typedef struct {
    int port;
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

    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 }; /* 50ms timeout for broadcast cadence */
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

        if (buf[0] != '/') continue;
        int addr_end = 0;
        while (addr_end < n && buf[addr_end] != '\0') addr_end++;
        if (addr_end >= n) continue;
        const char *osc_addr = (const char *)buf;
        int pos = osc_pad4(addr_end + 1);

        const char *types = "";
        int n_args = 0;
        if (pos < n && buf[pos] == ',') {
            types = (const char *)&buf[pos + 1];
            int tag_end = pos;
            while (tag_end < n && buf[tag_end] != '\0') tag_end++;
            n_args = tag_end - pos - 1;
            pos = osc_pad4(tag_end + 1);
        }

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

        if (strcmp(osc_addr, "/rack/slot/set") == 0 && ai >= 1 && asi >= 1) {
            rack_set_slot(arg_i[0], arg_s[0]);
            mcast_slot_set(arg_i[0], arg_s[0]);
        }
        else if (strcmp(osc_addr, "/rack/slot/clear") == 0 && ai >= 1) {
            rack_clear_slot(arg_i[0]);
            mcast_slot_clear(arg_i[0]);
        }
        else if (strcmp(osc_addr, "/rack/slot/volume") == 0 && ai >= 1 && afi >= 1) {
            int ch = arg_i[0];
            if (ch >= 0 && ch < MAX_SLOTS) {
                float vol = arg_f[0];
                if (vol < 0.0f) vol = 0.0f;
                if (vol > 1.0f) vol = 1.0f;
                g_rack.slots[ch].volume = vol;
                mcast_slot_volume(ch, vol);
            }
        }
        else if (strcmp(osc_addr, "/rack/slot/mute") == 0 && ai >= 2) {
            int ch = arg_i[0];
            if (ch >= 0 && ch < MAX_SLOTS) {
                g_rack.slots[ch].mute = arg_i[1] ? 1 : 0;
                mcast_slot_mute(ch, g_rack.slots[ch].mute);
            }
        }
        else if (strcmp(osc_addr, "/rack/slot/solo") == 0 && ai >= 2) {
            int ch = arg_i[0];
            if (ch >= 0 && ch < MAX_SLOTS) {
                g_rack.slots[ch].solo = arg_i[1] ? 1 : 0;
                mcast_slot_solo(ch, g_rack.slots[ch].solo);
            }
        }
        else if (strcmp(osc_addr, "/rack/master") == 0 && afi >= 1) {
            float vol = arg_f[0];
            if (vol < 0.0f) vol = 0.0f;
            if (vol > 1.0f) vol = 1.0f;
            g_rack.master_volume = vol;
            mcast_master(vol);
        }
        else if (strcmp(osc_addr, "/rack/local_mute") == 0 && ai >= 1) {
            g_rack.local_mute = arg_i[0] ? 1 : 0;
            mcast_local_mute(g_rack.local_mute);
            fprintf(stderr, "[miniwave] local mute: %s (bus-only)\n",
                    g_rack.local_mute ? "ON" : "OFF");
        }
        else if (strcmp(osc_addr, "/rack/status") == 0) {
            uint8_t reply[OSC_BUF_SIZE];
            int rpos = 0;
            int w;

            w = osc_write_string(reply, (int)sizeof(reply), "/rack/status");
            if (w < 0) continue;
            rpos += w;

            char ttag[256] = ",";
            int tpos = 1;
            for (int i = 0; i < MAX_SLOTS; i++) {
                ttag[tpos++] = 'i'; ttag[tpos++] = 's'; ttag[tpos++] = 'f';
                ttag[tpos++] = 'i'; ttag[tpos++] = 'i';
            }
            ttag[tpos++] = 'f'; ttag[tpos++] = 's';
            ttag[tpos] = '\0';

            w = osc_write_string(reply + rpos, (int)sizeof(reply) - rpos, ttag);
            if (w < 0) continue;
            rpos += w;

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
        else if (strcmp(osc_addr, "/rack/types") == 0) {
            uint8_t reply[OSC_BUF_SIZE];
            int rpos = 0;
            int w;

            w = osc_write_string(reply, (int)sizeof(reply), "/rack/types");
            if (w < 0) continue;
            rpos += w;

            char ttag[128] = ",";
            int tpos = 1;
            for (int i = 0; i < g_n_types && tpos < 126; i++)
                ttag[tpos++] = 's';
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
        else if (strcmp(osc_addr, "/midi/devices") == 0) {
            char devs[16][64];
            char devnames[16][128];
            int ndevs = platform_midi_list_devices(devs, devnames, 16);

            uint8_t reply[OSC_BUF_SIZE];
            int rpos = 0;
            int w;

            w = osc_write_string(reply, (int)sizeof(reply), "/midi/devices");
            if (w < 0) continue;
            rpos += w;

            char ttag[128] = ",";
            int tpos = 1;
            for (int i = 0; i < ndevs && tpos < 124; i++) {
                ttag[tpos++] = 's'; ttag[tpos++] = 's';
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
        else if (strcmp(osc_addr, "/midi/device") == 0 && asi >= 1) {
            platform_midi_connect(arg_s[0]);
            mcast_midi_device(g_midi_device_name);
        }
        else if (strcmp(osc_addr, "/midi/disconnect") == 0) {
            platform_midi_disconnect();
            mcast_midi_device("");
        }
        else if (strncmp(osc_addr, "/ch/", 4) == 0) {
            const char *p = osc_addr + 4;
            char *end = NULL;
            long ch = strtol(p, &end, 10);
            if (end && end != p && ch >= 0 && ch < MAX_SLOTS) {
                const char *sub_path = end;
                RackSlot *slot = &g_rack.slots[ch];

                if (slot->active && slot->state) {
                    if (strcmp(sub_path, "/status") == 0) {
                        InstrumentType *itype = g_type_registry[slot->type_idx];
                        uint8_t reply[512];
                        int rlen = itype->osc_status(slot->state, reply, (int)sizeof(reply));
                        if (rlen > 0) {
                            sendto(sock, reply, (size_t)rlen, 0,
                                   (struct sockaddr *)&sender, sender_len);
                        }
                    } else {
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

#endif
