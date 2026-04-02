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

/* json_escape defined in rack.h (included before server.h) */

/* ── HTML content (loaded at startup) ──────────────────────────────── */

static char *g_html_content = NULL;
static size_t g_html_length = 0;

typedef struct {
    int  fd;
    int  is_sse;
    int  detail_channel;
    int  client_id;      /* unique per SSE connection */
} HttpClient;

static int g_next_client_id = 1;

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

    char esc_midi[256];
    json_escape(esc_midi, sizeof(esc_midi), g_midi_device_name);
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "],\"master_volume\":%.4f,\"midi_device\":\"%s\","
        "\"audio_backend\":\"%s\",\"local_mute\":%d,"
        "\"osc_port\":%d,\"mcast_active\":%d,\"mcast_group\":\"%s:%d\","
        "\"bus_active\":%d,\"bus_slot\":%d,"
        "\"sse_clients\":%d,\"bpm\":%.1f}",
        (double)g_rack.master_volume, esc_midi,
        g_use_jack ? "JACK" : platform_audio_fallback_name(),
        g_rack.local_mute,
        DEFAULT_OSC_PORT, g_mcast_active,
        MCAST_GROUP, DEFAULT_MCAST_PORT,
        g_bus_active, g_bus_slot,
        sse_count, (double)g_bpm);

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
        char esc_id[128], esc_name[256];
        json_escape(esc_id, sizeof(esc_id), devs[i]);
        json_escape(esc_name, sizeof(esc_name), devnames[i]);
        pos += snprintf(buf + pos, (size_t)(max - pos),
            "%s{\"id\":\"%s\",\"name\":\"%s\"}", i ? "," : "",
            esc_id, esc_name);
    }
    pos += snprintf(buf + pos, (size_t)(max - pos), "]}");
    return pos;
}

static int append_keyseq_to_ch_status(int ch, char *buf, int pos, int max) {
    RackSlot *slot = &g_rack.slots[ch];
    if (!slot->active || !slot->state) return pos;
    KeySeq *ks = slot->keyseq;
    if (ks) {
        char esc[256];
        json_escape(esc, sizeof(esc), ks->source);
        pos += snprintf(buf + pos, (size_t)(max - pos),
            ",\"keyseq_enabled\":%d,\"keyseq_dsl\":\"%s\"",
            ks->enabled && (ks->num_steps > 0 || ks->algo_mode), esc);
    }
    return pos;
}

static int build_ch_status_json_inner(int ch, char *buf, int max) {
    RackSlot *slot = &g_rack.slots[ch];
    if (!slot->active || !slot->state || slot->type_idx < 0)
        return snprintf(buf, (size_t)max,
            "{\"type\":\"ch_status\",\"channel\":%d}", ch);

    InstrumentType *itype = g_type_registry[slot->type_idx];

    /* Use vtable json_status when available */
    if (itype->json_status) {
        int pos = snprintf(buf, (size_t)max,
            "{\"type\":\"ch_status\",\"channel\":%d,", ch);
        pos += itype->json_status(slot->state, buf + pos, max - pos);
        if (pos < max - 1) buf[pos++] = '}';
        buf[pos] = '\0';
        return pos;
    }

    /* Fallback — should not be reached once all instruments have json_status */
    return snprintf(buf, (size_t)max,
        "{\"type\":\"ch_status\",\"channel\":%d,\"instrument_type\":\"%s\"}",
        ch, itype->name);
}

static int build_ch_status_json(int ch, char *buf, int max) {
    int len = build_ch_status_json_inner(ch, buf, max);
    /* Strip trailing }, append keyseq state, re-close */
    if (len > 1 && buf[len - 1] == '}') {
        len--;
        len = append_keyseq_to_ch_status(ch, buf, len, max);
        if (len < max - 1) buf[len++] = '}';
        buf[len] = '\0';
    }
    return len;
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
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = write(fd, (const char *)buf + sent, len - sent);
        if (r <= 0) return r;
        sent += (size_t)r;
    }
    return (ssize_t)sent;
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

/* ── KeySeq expression spec (live, always in sync with binary) ──────── */

static const char *KEYSEQ_SPEC_JSON =
    "{"
    "\"variables\":["
    "{\"name\":\"n\",\"desc\":\"Current MIDI note\",\"min\":0,\"max\":127,\"default\":60,\"type\":\"float\"},"
    "{\"name\":\"v\",\"desc\":\"Current velocity\",\"min\":0,\"max\":1,\"default\":0.787,\"type\":\"float\"},"
    "{\"name\":\"t\",\"desc\":\"Step time (beats)\",\"min\":0.001,\"max\":16,\"default\":0.125,\"type\":\"float\"},"
    "{\"name\":\"g\",\"desc\":\"Gate ratio of step\",\"min\":0,\"max\":4,\"default\":1,\"type\":\"float\"},"
    "{\"name\":\"i\",\"desc\":\"Step index\",\"min\":0,\"max\":512,\"default\":0,\"type\":\"int\"},"
    "{\"name\":\"root\",\"desc\":\"Trigger note\",\"min\":0,\"max\":127,\"default\":60,\"type\":\"int\"},"
    "{\"name\":\"rv\",\"desc\":\"Trigger velocity\",\"min\":0,\"max\":1,\"default\":0.787,\"type\":\"float\"},"
    "{\"name\":\"time\",\"desc\":\"Seconds elapsed\",\"min\":0,\"max\":300,\"default\":0,\"type\":\"float\"},"
    "{\"name\":\"bu\",\"desc\":\"Beat position in step\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"float\"},"
    "{\"name\":\"gate\",\"desc\":\"Position in gate\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"float\"},"
    "{\"name\":\"held\",\"desc\":\"Key held\",\"min\":0,\"max\":1,\"default\":1,\"type\":\"bool\"},"
    "{\"name\":\"dt\",\"desc\":\"Sample period\",\"min\":0,\"max\":0.001,\"default\":0.0000208,\"type\":\"float\"}"
    "],"
    "\"constants\":[\"pi\",\"tau\"],"
    "\"operators\":[\"+\",\"-\",\"*\",\"/\",\"%\",\">\",\"<\",\">=\",\"<=\"],"
    "\"functions\":["
    "{\"name\":\"noise\",\"args\":\"1-3\",\"range\":\"0-1\",\"desc\":\"Perlin noise\"},"
    "{\"name\":\"noiseb\",\"args\":\"1-3\",\"range\":\"-1 to 1\",\"desc\":\"Perlin noise bipolar\"},"
    "{\"name\":\"sin\",\"args\":1,\"range\":\"-1 to 1\"},"
    "{\"name\":\"cos\",\"args\":1,\"range\":\"-1 to 1\"},"
    "{\"name\":\"abs\",\"args\":1,\"range\":\"0+\"},"
    "{\"name\":\"rand\",\"args\":0,\"range\":\"0-1\",\"desc\":\"Deterministic PRNG\"},"
    "{\"name\":\"if\",\"args\":3,\"desc\":\"if(cond,then,else)\"},"
    "{\"name\":\"floor\",\"args\":1},{\"name\":\"ceil\",\"args\":1},"
    "{\"name\":\"min\",\"args\":2},{\"name\":\"max\",\"args\":2},"
    "{\"name\":\"clamp\",\"args\":3,\"desc\":\"clamp(x,lo,hi)\"},"
    "{\"name\":\"step\",\"args\":2,\"desc\":\"step(edge,x)\"},"
    "{\"name\":\"smoothstep\",\"args\":3,\"desc\":\"smoothstep(e0,e1,x)\"}"
    "],"
    "\"dsl_tokens\":["
    "{\"token\":\"t<beats>\",\"desc\":\"Step time (default 0.125)\"},"
    "{\"token\":\"g<ratio>\",\"desc\":\"Gate ratio (default 1.0)\"},"
    "{\"token\":\"gated\",\"desc\":\"Stop on key release\"},"
    "{\"token\":\"loop\",\"desc\":\"Loop offsets\"},"
    "{\"token\":\"algo\",\"desc\":\"Algorithm mode\"},"
    "{\"token\":\"n:<expr>\",\"desc\":\"Note expression\"},"
    "{\"token\":\"v:<expr>\",\"desc\":\"Velocity expression\"},"
    "{\"token\":\"t:<expr>\",\"desc\":\"Step time expression\"},"
    "{\"token\":\"g:<expr>\",\"desc\":\"Gate expression\"},"
    "{\"token\":\"end:<expr>\",\"desc\":\"End condition\"},"
    "{\"token\":\"seed:<expr>\",\"desc\":\"Seed expression\"},"
    "{\"token\":\"frame:<expr>\",\"desc\":\"Per-sample cents mod\"},"
    "{\"token\":\"<name>:<expr>\",\"desc\":\"Param bus\"}"
    "],"
    "\"api\":["
    "{\"type\":\"keyseq_dsl\",\"fields\":[\"channel\",\"dsl\"],\"desc\":\"Load keyseq\"},"
    "{\"type\":\"keyseq_preview\",\"fields\":[\"channel\",\"note\",\"velocity\"],\"desc\":\"Preview from channel state\"},"
    "{\"type\":\"keyseq_stop\",\"fields\":[\"channel\"],\"desc\":\"Stop keyseq\"},"
    "{\"type\":\"note_on\",\"fields\":[\"channel\",\"note\",\"velocity\"],\"desc\":\"Trigger note\"},"
    "{\"type\":\"note_off\",\"fields\":[\"channel\",\"note\"],\"desc\":\"Release note\"}"
    "],"
    "\"sse_events\":[\"hello\",\"rack_status\",\"rack_types\",\"midi_devices\",\"ch_status\",\"keyseq_trigger\"]"
    "}";

/* ── KeySeq graph broadcast (SSE + OSC multicast) ──────────────────── */

static void keyseq_graph_broadcast(const char *dsl_source, int len) {
    (void)len;
    /* Build a compact preview and broadcast as SSE event.
     * We can't do the full simulation here (called from MIDI thread),
     * so just broadcast the DSL source — the frontend uses the
     * /api/keyseq_preview endpoint for the full computed graph. */
    char json[1024];
    char esc_dsl[512];
    json_escape(esc_dsl, sizeof(esc_dsl), dsl_source);
    int jlen = snprintf(json, sizeof(json),
        "{\"type\":\"keyseq_trigger\",\"dsl\":\"%s\"}", esc_dsl);
    if (jlen > 0) {
        sse_broadcast("keyseq_trigger", json);
        /* Also push over OSC multicast as /keyseq/trigger with string arg */
        if (g_mcast.active) {
            mcast_push("/keyseq/trigger", "s", NULL, NULL, dsl_source);
        }
    }
}

/* Wire graph broadcast into all slot keyseqs — call after rack_init + state_load */
static void keyseq_wire_graph_broadcast(void) {
    g_keyseq_graph_fn = keyseq_graph_broadcast;
    for (int i = 0; i < MAX_SLOTS; i++) {
        RackSlot *slot = &g_rack.slots[i];
        if (!slot->active || !slot->state) continue;
        KeySeq *ks = slot->keyseq;
        if (ks) ks->graph_fn = keyseq_graph_broadcast;
    }
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
        int err = rack_set_slot(ch, instr);
        if (err == 0) {
            mcast_slot_set(ch, instr);
            rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
        } else {
            rlen = snprintf(resp, sizeof(resp),
                "{\"error\":\"slot_set failed\",\"channel\":%d,\"instrument\":\"%s\"}", ch, instr);
        }
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

                /* Intercept seq paths — route to slot-level seq */
                if (strncmp(path, "/seq/", 5) == 0 || strcmp(path, "/seq") == 0) {
                    if (slot->seq) seq_osc_handle(slot->seq, path, iargs, ni, fargs, nf);
                } else {
                    itype->osc_handle(slot->state, path, iargs, ni, fargs, nf);
                }
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
        if (ch < 0 || ch >= MAX_SLOTS || note < 0 || note > 127) {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"invalid channel or note\"}");
        } else {
            uint8_t msg[3] = { (uint8_t)(0x90 | ch), (uint8_t)note, (uint8_t)vel };
            midi_dispatch_raw(msg, 3);
            mcast_note_on(ch, note, vel);
            rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
        }
    }
    else if (strcmp(type_str, "note_off") == 0) {
        int ch = 0, note = 60;
        json_get_int(body, "channel", &ch);
        json_get_int(body, "note", &note);
        if (ch < 0 || ch >= MAX_SLOTS || note < 0 || note > 127) {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"invalid channel or note\"}");
        } else {
            uint8_t msg[3] = { (uint8_t)(0x80 | ch), (uint8_t)note, 0 };
            midi_dispatch_raw(msg, 3);
            mcast_note_off(ch, note);
            rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
        }
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
            if (slot->active && slot->state && slot->seq) {
                seq_handle_dsl(slot->seq, dsl);
                rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
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
            if (slot->active && slot->seq) {
                seq_stop(slot->seq);
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
            if (slot->active && slot->keyseq) {
                keyseq_parse(slot->keyseq, dsl);
                rlen = snprintf(resp, sizeof(resp),
                    "{\"ok\":true,\"steps\":%d}", slot->keyseq->num_steps);
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
            if (slot->active && slot->keyseq) {
                keyseq_stop(slot->keyseq);
                slot->keyseq->enabled = 0;
            }
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "keyseq_enable") == 0) {
        int ch = 0, en = 1;
        json_get_int(body, "channel", &ch);
        json_get_int(body, "enabled", &en);
        if (ch >= 0 && ch < MAX_SLOTS) {
            RackSlot *slot = &g_rack.slots[ch];
            if (slot->keyseq) {
                slot->keyseq->enabled = en ? 1 : 0;
                if (!en) keyseq_stop(slot->keyseq);
                sse_mark_dirty();
            }
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "keyseq_preset_list") == 0) {
        int pos = 0;
        pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos), "{\"presets\":[");
        for (int i = 0; i < g_num_ks_presets; i++) {
            char en[128], ed[1024];
            json_escape(en, sizeof(en), g_ks_presets[i].name);
            json_escape(ed, sizeof(ed), g_ks_presets[i].dsl);
            pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos),
                "%s{\"name\":\"%s\",\"dsl\":\"%s\"}", i?",":"", en, ed);
        }
        pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos), "]}");
        rlen = pos;
    }
    else if (strcmp(type_str, "keyseq_preset_save") == 0) {
        char name[PATCH_NAME_MAX] = "", dsl[512] = "";
        json_get_string(body, "name", name, sizeof(name));
        json_get_string(body, "dsl", dsl, sizeof(dsl));
        if (!name[0]) { rlen = snprintf(resp, sizeof(resp), "{\"error\":\"no name\"}"); }
        else {
            int idx = ks_preset_find(name);
            if (idx < 0) {
                if (g_num_ks_presets >= MAX_KEYSEQ_PRESETS)
                    { rlen = snprintf(resp, sizeof(resp), "{\"error\":\"limit\"}"); goto ks_preset_done; }
                idx = g_num_ks_presets++;
            }
            strncpy(g_ks_presets[idx].name, name, PATCH_NAME_MAX - 1);
            strncpy(g_ks_presets[idx].dsl, dsl, 511);
            ks_presets_save();
            rlen = snprintf(resp, sizeof(resp), "{\"ok\":true,\"index\":%d}", idx);
        }
        ks_preset_done:;
    }
    else if (strcmp(type_str, "keyseq_preset_delete") == 0) {
        char name[PATCH_NAME_MAX] = "";
        json_get_string(body, "name", name, sizeof(name));
        int idx = ks_preset_find(name);
        if (idx < 0) { rlen = snprintf(resp, sizeof(resp), "{\"error\":\"not found\"}"); }
        else {
            for (int i = idx; i < g_num_ks_presets - 1; i++) g_ks_presets[i] = g_ks_presets[i+1];
            g_num_ks_presets--;
            ks_presets_save();
            rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
        }
    }
    else if (strcmp(type_str, "keyseq_preset_rename") == 0) {
        char name[PATCH_NAME_MAX] = "", new_name[PATCH_NAME_MAX] = "";
        json_get_string(body, "name", name, sizeof(name));
        json_get_string(body, "new_name", new_name, sizeof(new_name));
        int idx = ks_preset_find(name);
        if (idx < 0) { rlen = snprintf(resp, sizeof(resp), "{\"error\":\"not found\"}"); }
        else if (!new_name[0]) { rlen = snprintf(resp, sizeof(resp), "{\"error\":\"empty name\"}"); }
        else {
            strncpy(g_ks_presets[idx].name, new_name, PATCH_NAME_MAX - 1);
            ks_presets_save();
            rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
        }
    }
    else if (strcmp(type_str, "bpm") == 0) {
        float bpm = 120.0f;
        if (json_get_float(body, "value", &bpm) == 0 && bpm > 0 && bpm <= 999) {
            g_bpm = bpm;
            /* Update BPM on all active keyseqs */
            for (int i = 0; i < MAX_SLOTS; i++) {
                if (g_rack.slots[i].keyseq) g_rack.slots[i].keyseq->bpm = bpm;
                if (g_rack.slots[i].seq) g_rack.slots[i].seq->bpm = bpm;
            }
            sse_mark_dirty();
        }
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true,\"bpm\":%.1f}", (double)g_bpm);
    }
    else if (strcmp(type_str, "keyseq_spec") == 0) {
        int ch = -1;
        json_get_int(body, "channel", &ch);

        /* Spec response can be large — use heap buffer */
        #define SPEC_BUF_SIZE 65536
        char *sbuf = (char *)malloc(SPEC_BUF_SIZE);
        if (!sbuf) { rlen = snprintf(resp, sizeof(resp), "{\"error\":\"oom\"}"); goto spec_done; }

        int pos = 0;
        int speclen = (int)strlen(KEYSEQ_SPEC_JSON);
        memcpy(sbuf, KEYSEQ_SPEC_JSON, (size_t)(speclen - 1));
        pos = speclen - 1;

        /* All instrument type param schemas */
        pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos), ",\"instruments\":{");

        /* ── fm-synth schema + presets ── */
        pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
            "\"fm-synth\":{\"params\":["
            "{\"name\":\"carrier_ratio\",\"min\":0.1,\"max\":4,\"default\":1,\"type\":\"float\"},"
            "{\"name\":\"mod_ratio\",\"min\":0.1,\"max\":15,\"default\":2,\"type\":\"float\"},"
            "{\"name\":\"mod_index\",\"min\":0,\"max\":15,\"default\":2.5,\"type\":\"float\"},"
            "{\"name\":\"attack\",\"min\":0.001,\"max\":1,\"default\":0.001,\"type\":\"float\"},"
            "{\"name\":\"decay\",\"min\":0.01,\"max\":3,\"default\":0.3,\"type\":\"float\"},"
            "{\"name\":\"sustain\",\"min\":0,\"max\":1,\"default\":0.5,\"type\":\"float\"},"
            "{\"name\":\"release\",\"min\":0.01,\"max\":4,\"default\":0.3,\"type\":\"float\"},"
            "{\"name\":\"feedback\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"float\"}"
            "],\"envelopes\":["
            "{\"name\":\"envelope\",\"attack\":\"attack\",\"decay\":\"decay\",\"sustain\":\"sustain\",\"release\":\"release\"}"
            "],\"presets\":[");
        for (int p = 0; p < NUM_PRESETS; p++) {
            pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
                "%s\"%s\"", p ? "," : "", PRESET_NAMES[p]);
        }
        pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
            "],\"families\":["
            "{\"name\":\"Piano\",\"start\":0,\"count\":10},"
            "{\"name\":\"Organ\",\"start\":10,\"count\":10},"
            "{\"name\":\"Brass\",\"start\":20,\"count\":10},"
            "{\"name\":\"Strings\",\"start\":30,\"count\":10},"
            "{\"name\":\"Bass\",\"start\":40,\"count\":10},"
            "{\"name\":\"Lead\",\"start\":50,\"count\":10},"
            "{\"name\":\"Bell\",\"start\":60,\"count\":10},"
            "{\"name\":\"Reed\",\"start\":70,\"count\":10},"
            "{\"name\":\"SFX\",\"start\":80,\"count\":10},"
            "{\"name\":\"Retro\",\"start\":90,\"count\":9}"
            "]},");

        /* ── sub-synth schema ── */
        pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
            "\"sub-synth\":{\"params\":["
            "{\"name\":\"waveform\",\"min\":0,\"max\":5,\"default\":0,\"type\":\"int\",\"labels\":[\"Saw\",\"Square\",\"Pulse\",\"Tri\",\"Sine\",\"Noise\"]},"
            "{\"name\":\"pulse_width\",\"min\":0.05,\"max\":0.95,\"default\":0.5,\"type\":\"float\"},"
            "{\"name\":\"filter_cutoff\",\"min\":20,\"max\":20000,\"default\":2000,\"type\":\"float\",\"scale\":\"log\"},"
            "{\"name\":\"filter_reso\",\"min\":0,\"max\":1,\"default\":0.2,\"type\":\"float\"},"
            "{\"name\":\"filter_env_depth\",\"min\":-1,\"max\":1,\"default\":0.8,\"type\":\"float\"},"
            "{\"name\":\"filt_attack\",\"min\":0.001,\"max\":5,\"default\":0.001,\"type\":\"float\"},"
            "{\"name\":\"filt_decay\",\"min\":0.001,\"max\":5,\"default\":0.15,\"type\":\"float\"},"
            "{\"name\":\"filt_sustain\",\"min\":0,\"max\":1,\"default\":0.2,\"type\":\"float\"},"
            "{\"name\":\"filt_release\",\"min\":0.001,\"max\":5,\"default\":0.2,\"type\":\"float\"},"
            "{\"name\":\"amp_attack\",\"min\":0.001,\"max\":5,\"default\":0.001,\"type\":\"float\"},"
            "{\"name\":\"amp_decay\",\"min\":0.001,\"max\":5,\"default\":0.1,\"type\":\"float\"},"
            "{\"name\":\"amp_sustain\",\"min\":0,\"max\":1,\"default\":0.8,\"type\":\"float\"},"
            "{\"name\":\"amp_release\",\"min\":0.001,\"max\":5,\"default\":0.15,\"type\":\"float\"}"
            "],\"envelopes\":["
            "{\"name\":\"amp\",\"attack\":\"amp_attack\",\"decay\":\"amp_decay\",\"sustain\":\"amp_sustain\",\"release\":\"amp_release\"},"
            "{\"name\":\"filter\",\"attack\":\"filt_attack\",\"decay\":\"filt_decay\",\"sustain\":\"filt_sustain\",\"release\":\"filt_release\"}"
            "]},");

        /* ── ym2413 schema + patches ── */
        pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
            "\"ym2413\":{\"params\":["
            "{\"name\":\"instrument\",\"min\":0,\"max\":15,\"default\":1,\"type\":\"int\"},"
            "{\"name\":\"volume\",\"min\":0,\"max\":1,\"default\":1,\"type\":\"float\"},"
            "{\"name\":\"rhythm\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"bool\"},"
            "{\"name\":\"feedback\",\"min\":0,\"max\":7,\"default\":0,\"type\":\"int\"},"
            /* Modulator */
            "{\"name\":\"mod_mult\",\"min\":0,\"max\":15,\"default\":1,\"type\":\"int\",\"group\":\"mod\"},"
            "{\"name\":\"mod_tl\",\"min\":0,\"max\":63,\"default\":32,\"type\":\"int\",\"group\":\"mod\"},"
            "{\"name\":\"mod_attack\",\"min\":0,\"max\":15,\"default\":15,\"type\":\"int\",\"group\":\"mod\"},"
            "{\"name\":\"mod_decay\",\"min\":0,\"max\":15,\"default\":4,\"type\":\"int\",\"group\":\"mod\"},"
            "{\"name\":\"mod_sustain\",\"min\":0,\"max\":15,\"default\":2,\"type\":\"int\",\"group\":\"mod\"},"
            "{\"name\":\"mod_release\",\"min\":0,\"max\":15,\"default\":4,\"type\":\"int\",\"group\":\"mod\"},"
            "{\"name\":\"mod_ksl\",\"min\":0,\"max\":3,\"default\":0,\"type\":\"int\",\"group\":\"mod\"},"
            "{\"name\":\"mod_wave\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"int\",\"group\":\"mod\",\"labels\":[\"Sine\",\"Half\"]},"
            "{\"name\":\"mod_vibrato\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"bool\",\"group\":\"mod\"},"
            "{\"name\":\"mod_am\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"bool\",\"group\":\"mod\"},"
            "{\"name\":\"mod_eg\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"bool\",\"group\":\"mod\"},"
            "{\"name\":\"mod_ksr\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"bool\",\"group\":\"mod\"},"
            /* Carrier */
            "{\"name\":\"car_mult\",\"min\":0,\"max\":15,\"default\":1,\"type\":\"int\",\"group\":\"car\"},"
            "{\"name\":\"car_attack\",\"min\":0,\"max\":15,\"default\":15,\"type\":\"int\",\"group\":\"car\"},"
            "{\"name\":\"car_decay\",\"min\":0,\"max\":15,\"default\":4,\"type\":\"int\",\"group\":\"car\"},"
            "{\"name\":\"car_sustain\",\"min\":0,\"max\":15,\"default\":2,\"type\":\"int\",\"group\":\"car\"},"
            "{\"name\":\"car_release\",\"min\":0,\"max\":15,\"default\":4,\"type\":\"int\",\"group\":\"car\"},"
            "{\"name\":\"car_wave\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"int\",\"group\":\"car\",\"labels\":[\"Sine\",\"Half\"]},"
            "{\"name\":\"car_vibrato\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"bool\",\"group\":\"car\"},"
            "{\"name\":\"car_am\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"bool\",\"group\":\"car\"},"
            "{\"name\":\"car_eg\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"bool\",\"group\":\"car\"},"
            "{\"name\":\"car_ksr\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"bool\",\"group\":\"car\"}"
            "],\"envelopes\":["
            "{\"name\":\"mod\",\"attack\":\"mod_attack\",\"decay\":\"mod_decay\",\"sustain\":\"mod_sustain\",\"release\":\"mod_release\"},"
            "{\"name\":\"car\",\"attack\":\"car_attack\",\"decay\":\"car_decay\",\"sustain\":\"car_sustain\",\"release\":\"car_release\"}"
            "],\"patches\":["
            "\"Custom\",\"Violin\",\"Guitar\",\"Piano\",\"Flute\","
            "\"Clarinet\",\"Oboe\",\"Trumpet\",\"Organ\",\"Horn\","
            "\"Synthesizer\",\"Harpsichord\",\"Vibraphone\",\"Synth Bass\","
            "\"Acoustic Bass\",\"Electric Guitar\""
            "]},");

        /* ── fm-drums schema + presets + drum map ── */
        pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
            "\"fm-drums\":{\"params\":["
            "{\"name\":\"carrier_freq\",\"min\":20,\"max\":2000,\"default\":200,\"type\":\"float\"},"
            "{\"name\":\"mod_freq\",\"min\":20,\"max\":2000,\"default\":300,\"type\":\"float\"},"
            "{\"name\":\"mod_index\",\"min\":0,\"max\":20,\"default\":3,\"type\":\"float\"},"
            "{\"name\":\"pitch_sweep\",\"min\":0,\"max\":5000,\"default\":500,\"type\":\"float\"},"
            "{\"name\":\"pitch_decay\",\"min\":0.001,\"max\":1,\"default\":0.05,\"type\":\"float\"},"
            "{\"name\":\"decay\",\"min\":0.01,\"max\":2,\"default\":0.2,\"type\":\"float\"},"
            "{\"name\":\"noise_amt\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"float\"},"
            "{\"name\":\"click_amt\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"float\"},"
            "{\"name\":\"feedback\",\"min\":0,\"max\":1,\"default\":0,\"type\":\"float\"}"
            "],\"preset_names\":[");
        for (int p = 0; p < FMD_NUM_PRESETS; p++) {
            pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
                "%s\"%s\"", p ? "," : "", FMD_PRESET_NAMES[p]);
        }
        pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
            "],\"drum_map\":["
            "{\"note\":35,\"label\":\"Kick\"},{\"note\":36,\"label\":\"Kick\"},"
            "{\"note\":38,\"label\":\"Snare\"},{\"note\":39,\"label\":\"Clap\"},"
            "{\"note\":40,\"label\":\"Snare\"},{\"note\":41,\"label\":\"Tom\"},"
            "{\"note\":42,\"label\":\"CH\"},{\"note\":43,\"label\":\"Tom\"},"
            "{\"note\":44,\"label\":\"CH\"},{\"note\":45,\"label\":\"Tom\"},"
            "{\"note\":46,\"label\":\"OH\"},{\"note\":47,\"label\":\"Tom\"},"
            "{\"note\":48,\"label\":\"Tom\"},{\"note\":49,\"label\":\"Cymbal\"},"
            "{\"note\":50,\"label\":\"Tom\"},{\"note\":51,\"label\":\"Cymbal\"}"
            "]},");

        /* ── additive schema ── */
        pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
            "\"additive\":{\"params\":["
            "{\"name\":\"mode\",\"min\":0,\"max\":5,\"default\":0,\"type\":\"int\",\"labels\":[\"Harmonic\",\"Cluster\",\"Formant\",\"Metallic\",\"Noise\",\"Expression\"]},"
            "{\"name\":\"harmonics\",\"min\":1,\"max\":64,\"default\":16,\"type\":\"int\"},"
            "{\"name\":\"volume\",\"min\":0,\"max\":1,\"default\":1,\"type\":\"float\"},"
            "{\"name\":\"attack\",\"min\":0.001,\"max\":2,\"default\":0.01,\"type\":\"float\"},"
            "{\"name\":\"decay\",\"min\":0.001,\"max\":3,\"default\":0.1,\"type\":\"float\"},"
            "{\"name\":\"sustain\",\"min\":0,\"max\":1,\"default\":0.7,\"type\":\"float\"},"
            "{\"name\":\"release\",\"min\":0.001,\"max\":4,\"default\":0.3,\"type\":\"float\"},"
            "{\"name\":\"ratio\",\"min\":0.5,\"max\":4,\"default\":1.0,\"type\":\"float\",\"group\":\"shape\"},"
            "{\"name\":\"spread\",\"min\":0,\"max\":2,\"default\":0,\"type\":\"float\",\"group\":\"shape\"},"
            "{\"name\":\"rolloff\",\"min\":0,\"max\":1,\"default\":0.7,\"type\":\"float\",\"group\":\"shape\"},"
            "{\"name\":\"formant_center\",\"min\":100,\"max\":5000,\"default\":800,\"type\":\"float\",\"group\":\"formant\",\"scale\":\"log\"},"
            "{\"name\":\"formant_width\",\"min\":10,\"max\":2000,\"default\":200,\"type\":\"float\",\"group\":\"formant\"},"
            "{\"name\":\"inharmonicity\",\"min\":0,\"max\":1,\"default\":0.5,\"type\":\"float\",\"group\":\"metallic\"}"
            "],\"envelopes\":["
            "{\"name\":\"envelope\",\"attack\":\"attack\",\"decay\":\"decay\",\"sustain\":\"sustain\",\"release\":\"release\"}"
            "]},"

            /* ── phase-dist schema ── */
            "\"phase-dist\":{\"params\":["
            "{\"name\":\"mode\",\"min\":0,\"max\":5,\"default\":0,\"type\":\"int\",\"labels\":[\"Resonant\",\"Saw\",\"Pulse\",\"Cosine\",\"Sync\",\"Wavefold\"]},"
            "{\"name\":\"distortion\",\"min\":0,\"max\":1,\"default\":0.5,\"type\":\"float\"},"
            "{\"name\":\"timbre\",\"min\":0,\"max\":1,\"default\":0.5,\"type\":\"float\"},"
            "{\"name\":\"color\",\"min\":0,\"max\":1,\"default\":0.5,\"type\":\"float\"},"
            "{\"name\":\"volume\",\"min\":0,\"max\":1,\"default\":1,\"type\":\"float\"},"
            "{\"name\":\"attack\",\"min\":0.001,\"max\":2,\"default\":0.01,\"type\":\"float\"},"
            "{\"name\":\"decay\",\"min\":0.001,\"max\":3,\"default\":0.2,\"type\":\"float\"},"
            "{\"name\":\"sustain\",\"min\":0,\"max\":1,\"default\":0.6,\"type\":\"float\"},"
            "{\"name\":\"release\",\"min\":0.001,\"max\":4,\"default\":0.3,\"type\":\"float\"}"
            "],\"envelopes\":["
            "{\"name\":\"envelope\",\"attack\":\"attack\",\"decay\":\"decay\",\"sustain\":\"sustain\",\"release\":\"release\"}"
            "]}"
            "}");

        /* ── Active channel state ── */
        if (ch >= 0 && ch < MAX_SLOTS) {
            RackSlot *slot = &g_rack.slots[ch];
            if (slot->active && slot->state) {
                const char *tname = g_type_registry[slot->type_idx]->name;
                pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
                    ",\"channel\":%d,\"active_instrument\":\"%s\"", ch, tname);

                /* Current param values — use json_save (instrument-agnostic) */
                InstrumentType *itype = g_type_registry[slot->type_idx];
                if (itype->json_save) {
                    char vbuf[4096];
                    int vn = itype->json_save(slot->state, vbuf, (int)sizeof(vbuf));
                    if (vn > 0) pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos), ",%s", vbuf);
                }

                /* KeySeq state */
                KeySeq *ks = slot->keyseq;
                if (ks) {
                    char esc_dsl[512];
                    json_escape(esc_dsl, sizeof(esc_dsl), ks->source);
                    pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
                        ",\"keyseq\":{\"enabled\":%d,\"dsl\":\"%s\",\"algo\":%d,\"gated\":%d,"
                        "\"loop\":%d,\"step_beats\":%.4f,\"gate_beats\":%.4f}",
                        ks->enabled, esc_dsl, ks->algo_mode, ks->gated,
                        ks->loop, (double)ks->step_beats, (double)ks->gate_beats);
                }
            } else {
                pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos),
                    ",\"channel\":%d,\"active_instrument\":null", ch);
            }
        }

        pos += snprintf(sbuf + pos, (size_t)(SPEC_BUF_SIZE - pos), "}");

        /* Send from heap buffer */
        http_send_response(fd, 200, "application/json", sbuf, pos);
        free(sbuf);
        return; /* skip the normal resp send below */
        spec_done:;
    }
    else if (strcmp(type_str, "keyseq_status") == 0) {
        int ch = 0;
        json_get_int(body, "channel", &ch);
        if (ch >= 0 && ch < MAX_SLOTS) {
            RackSlot *slot = &g_rack.slots[ch];
            KeySeq *ks = (slot->active) ? slot->keyseq : NULL;
            if (ks) {
                char esc[512];
                json_escape(esc, sizeof(esc), ks->source);
                int active_voices = 0;
                for (int vi = 0; vi < KEYSEQ_MAX_VOICES; vi++)
                    if (ks->voices[vi].active) active_voices++;
                rlen = snprintf(resp, sizeof(resp),
                    "{\"channel\":%d,\"enabled\":%d,\"dsl\":\"%s\","
                    "\"algo\":%d,\"gated\":%d,\"loop\":%d,"
                    "\"step_beats\":%.4f,\"gate_beats\":%.4f,"
                    "\"active_voices\":%d}",
                    ch, ks->enabled, esc, ks->algo_mode, ks->gated, ks->loop,
                    (double)ks->step_beats, (double)ks->gate_beats, active_voices);
            } else {
                rlen = snprintf(resp, sizeof(resp),
                    "{\"channel\":%d,\"enabled\":0,\"dsl\":\"\"}", ch);
            }
        } else {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"bad channel\"}");
        }
    }
    else if (strcmp(type_str, "keyseq_preview") == 0) {
        int ch = 0;
        int note = 60, velocity = 100;
        json_get_int(body, "channel", &ch);
        json_get_int(body, "note", &note);
        json_get_int(body, "velocity", &velocity);

        if (ch < 0 || ch >= MAX_SLOTS) {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"bad channel\"}");
        } else {
            RackSlot *slot = &g_rack.slots[ch];
            KeySeq *ks = (slot->active) ? slot->keyseq : NULL;

            if (!ks || !ks->enabled || (!ks->num_steps && !ks->algo_mode)) {
                rlen = snprintf(resp, sizeof(resp),
                    "{\"error\":\"no keyseq on channel %d\",\"dsl\":\"\",\"steps\":[],\"end_step\":-1}", ch);
            } else {
                float bpm = ks->bpm > 0 ? ks->bpm : 120.0f;

                /* Compute runtime seed (same logic as keyseq_note_on) */
                uint32_t seed;
                if (ks->expr_seed.valid) {
                    KeySeqCtx sc = {
                        .n = (float)note, .v = (float)velocity,
                        .rv = (float)velocity / 127.0f, .root = (float)note,
                        .i = 0, .time = 0, .gate = 0, .held = 1.0f
                    };
                    float sv = ke_eval(&ks->expr_seed, &sc);
                    float hf = fmodf(fabsf(sv) * 2654435.761f, 4294967000.0f);
                    seed = (uint32_t)hf;
                    if (!seed) seed = 1;
                } else {
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    seed = (uint32_t)(ts.tv_nsec ^ (ts.tv_sec * 1000003)) | 1;
                }
                /* Local RNG/noise state — does not touch globals */
                uint32_t local_rand = seed;
                fnl_state local_fnl = fnlCreateState();
                local_fnl.noise_type = FNL_NOISE_PERLIN;
                local_fnl.frequency = 1.0f;
                local_fnl.seed = (int)seed;

                float root_vel = (float)velocity / 127.0f;
                float algo_n = (float)note, algo_v = root_vel;
                float algo_t = ks->step_beats, algo_g = ks->gate_beats;
                float total_beats = 0;
                int end_step = -1;
                const int max_steps = 512;

                int pos = 0;
                char esc_dsl[512];
                json_escape(esc_dsl, sizeof(esc_dsl), ks->source);
                pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos),
                    "{\"channel\":%d,\"dsl\":\"%s\",\"seed\":%u,\"bpm\":%.1f,"
                    "\"algo\":%d,\"gated\":%d,\"loop\":%d,"
                    "\"step_beats\":%.3f,\"gate_beats\":%.3f,\"steps\":[",
                    ch, esc_dsl, seed, (double)bpm,
                    ks->algo_mode, ks->gated, ks->loop,
                    (double)ks->step_beats, (double)ks->gate_beats);

                for (int i = 0; i < max_steps && pos < SSE_BUF_SIZE - 256; i++) {
                    float cur_n, cur_v;

                    if (i == 0 && ks->algo_mode) {
                        cur_n = (float)note;
                        cur_v = root_vel;
                    } else if (ks->algo_mode) {
                        float time_sec = total_beats * 60.0f / bpm;
                        KeySeqCtx ctx = {
                            .n = algo_n, .v = algo_v, .t = algo_t, .g = algo_g,
                            .i = (float)i, .root = (float)note, .rv = root_vel,
                            .time = time_sec, .bu = 0, .gate = 0, .held = 1,
                            .seed = (float)seed,
                            .local_rand = &local_rand, .local_fnl = &local_fnl
                        };
                        float new_n = ks->expr_n.valid ? ke_eval(&ks->expr_n, &ctx) : ctx.n;
                        float new_v = ks->expr_v.valid ? ke_eval(&ks->expr_v, &ctx) : ctx.v;
                        float new_t = ks->expr_t.valid ? ke_eval(&ks->expr_t, &ctx) : ctx.t;
                        float new_g = ks->expr_g.valid ? ke_eval(&ks->expr_g, &ctx) : ctx.g;
                        algo_n = new_n; algo_v = new_v;
                        algo_t = new_t > 0.001f ? new_t : 0.001f;
                        algo_g = new_g > 0.001f ? new_g : 0.001f;
                        cur_n = algo_n; cur_v = algo_v;
                    } else {
                        if (i >= ks->num_steps) {
                            if (ks->loop) {
                                int idx = i % ks->num_steps;
                                cur_n = (float)(note + ks->offsets[idx]);
                                cur_v = root_vel * ks->levels[idx];
                            } else { end_step = i; break; }
                        } else {
                            cur_n = (float)(note + ks->offsets[i]);
                            cur_v = root_vel * ks->levels[i];
                        }
                    }

                    int midi = (int)roundf(cur_n);
                    if (midi < 0) midi = 0; if (midi > 127) midi = 127;
                    float cents = (cur_n - (float)midi) * 100.0f;
                    float beat_pos = total_beats;

                    /* Evaluate frame expression at mid-gate for this step */
                    float frame_cents = 0;
                    if (ks->expr_frame.valid) {
                        float time_sec = beat_pos * 60.0f / bpm;
                        KeySeqCtx fctx = {
                            .n = cur_n, .v = cur_v, .t = algo_t, .g = algo_g,
                            .i = (float)i, .root = (float)note, .rv = root_vel,
                            .time = time_sec, .bu = 0.5f, .gate = 0.5f, .held = 1.0f,
                            .seed = (float)seed,
                            .local_rand = &local_rand, .local_fnl = &local_fnl
                        };
                        frame_cents = ke_eval(&ks->expr_frame, &fctx);
                    }

                    pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos),
                        "%s{\"i\":%d,\"n\":%.2f,\"midi\":%d,\"v\":%.4f,"
                        "\"t\":%.4f,\"g\":%.4f,\"cents\":%.1f,\"beat\":%.3f,\"frame_cents\":%.1f}",
                        i ? "," : "", i, (double)cur_n, midi, (double)cur_v,
                        (double)algo_t, (double)algo_g, (double)cents, (double)beat_pos, (double)frame_cents);

                    total_beats += algo_t;

                    if (ks->algo_mode && ks->expr_end.valid && i > 0) {
                        KeySeqCtx ectx = {
                            .n = algo_n, .v = algo_v, .t = algo_t, .g = algo_g,
                            .i = (float)i, .root = (float)note, .rv = root_vel,
                            .time = total_beats * 60.0f / bpm, .seed = (float)seed,
                            .local_rand = &local_rand, .local_fnl = &local_fnl
                        };
                        if (ke_eval(&ks->expr_end, &ectx) != 0.0f) { end_step = i; break; }
                    }
                }

                char end_reason[64] = "none";
                if (end_step >= 0 && ks->expr_end.valid) {
                    const char *ep = strstr(ks->source, "end:");
                    if (ep) { ep += 4; const char *ee = ep; while (*ee && *ee != ';') ee++;
                        int el = (int)(ee - ep); if (el > 0 && el < 63) { memcpy(end_reason, ep, (size_t)el); end_reason[el] = '\0'; }
                    }
                } else if (end_step >= 0) { snprintf(end_reason, sizeof(end_reason), "max"); }

                pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos),
                    "],\"end_step\":%d,\"end_reason\":\"%s\",\"total_beats\":%.3f,\"total_steps\":%d}",
                    end_step, end_reason, (double)total_beats,
                    end_step >= 0 ? end_step + 1 : (int)(total_beats / algo_t));
                rlen = pos;
            }
        }
    }
    else if (strcmp(type_str, "patch_list") == 0) {
        /* List user patches, optionally filtered by instrument type */
        char filter[32] = "";
        json_get_string(body, "instrument", filter, sizeof(filter));

        int pos = 0;
        pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos), "{\"patches\":[");
        int first = 1;
        for (int i = 0; i < g_num_patches; i++) {
            if (filter[0] && strcmp(g_patches[i].type, filter) != 0) continue;
            char esc_name[128];
            json_escape(esc_name, sizeof(esc_name), g_patches[i].name);
            pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos),
                "%s{\"name\":\"%s\",\"type\":\"%s\",\"has_keyseq\":%d,\"has_seq\":%d}",
                first ? "" : ",", esc_name, g_patches[i].type,
                g_patches[i].keyseq_dsl[0] ? 1 : 0,
                g_patches[i].seq_dsl[0] ? 1 : 0);
            first = 0;
        }
        pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos), "]}");
        rlen = pos;
    }
    else if (strcmp(type_str, "patch_save") == 0) {
        /* Save current channel state as a named patch */
        int ch = 0;
        char name[PATCH_NAME_MAX] = "";
        json_get_int(body, "channel", &ch);
        json_get_string(body, "name", name, sizeof(name));

        if (!name[0] || ch < 0 || ch >= MAX_SLOTS) {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"bad args\"}");
        } else {
            RackSlot *slot = &g_rack.slots[ch];
            if (!slot->active || !slot->state) {
                rlen = snprintf(resp, sizeof(resp), "{\"error\":\"no instrument\"}");
            } else {
                InstrumentType *itype = g_type_registry[slot->type_idx];
                /* Find or create patch slot */
                int idx = patch_find(name);
                if (idx < 0) {
                    if (g_num_patches >= MAX_USER_PATCHES) {
                        rlen = snprintf(resp, sizeof(resp), "{\"error\":\"patch limit\"}");
                        goto patch_done;
                    }
                    idx = g_num_patches++;
                }
                UserPatch *up = &g_patches[idx];
                strncpy(up->name, name, PATCH_NAME_MAX - 1);
                strncpy(up->type, itype->name, sizeof(up->type) - 1);
                if (itype->json_save) {
                    itype->json_save(slot->state, up->data, PATCH_DATA_MAX);
                } else {
                    up->data[0] = '\0';
                }
                /* Include keyseq + seq DSL */
                up->keyseq_dsl[0] = '\0';
                up->seq_dsl[0] = '\0';
                if (slot->keyseq && slot->keyseq->enabled && slot->keyseq->source[0])
                    strncpy(up->keyseq_dsl, slot->keyseq->source, sizeof(up->keyseq_dsl) - 1);
                if (slot->seq && slot->seq->source[0])
                    strncpy(up->seq_dsl, slot->seq->source, sizeof(up->seq_dsl) - 1);
                patches_save();
                rlen = snprintf(resp, sizeof(resp), "{\"ok\":true,\"index\":%d}", idx);
            }
        }
        patch_done:;
    }
    else if (strcmp(type_str, "patch_load") == 0) {
        /* Load a named patch onto a channel */
        int ch = 0;
        char name[PATCH_NAME_MAX] = "";
        json_get_int(body, "channel", &ch);
        json_get_string(body, "name", name, sizeof(name));

        int idx = patch_find(name);
        if (idx < 0) {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"patch not found\"}");
        } else if (ch < 0 || ch >= MAX_SLOTS) {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"bad channel\"}");
        } else {
            UserPatch *up = &g_patches[idx];
            RackSlot *slot = &g_rack.slots[ch];
            /* Ensure correct instrument type is loaded */
            if (!slot->active || !slot->state || strcmp(g_type_registry[slot->type_idx]->name, up->type) != 0) {
                rack_set_slot(ch, up->type);
                slot = &g_rack.slots[ch];
            }
            if (slot->active && slot->state) {
                InstrumentType *itype = g_type_registry[slot->type_idx];
                if (itype->json_load) itype->json_load(slot->state, up->data);
                /* Restore keyseq + seq */
                if (slot->keyseq && up->keyseq_dsl[0])
                    keyseq_parse(slot->keyseq, up->keyseq_dsl);
                if (slot->seq && up->seq_dsl[0])
                    seq_parse(slot->seq, up->seq_dsl);
                state_mark_dirty();

                /* Return full channel state so frontend can update immediately */
                int pos = 0;
                pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos),
                    "{\"ok\":true,\"channel\":%d,\"instrument\":\"%s\"", ch, itype->name);

                /* Instrument params */
                if (itype->json_status) {
                    pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos), ",");
                    pos += itype->json_status(slot->state, resp + pos, SSE_BUF_SIZE - pos);
                }

                /* Current values */
                if (itype->json_save) {
                    char pbuf[2048];
                    int pn = itype->json_save(slot->state, pbuf, (int)sizeof(pbuf));
                    if (pn > 0) pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos), ",%s", pbuf);
                }

                /* Keyseq state */
                if (slot->keyseq && slot->keyseq->enabled && slot->keyseq->source[0]) {
                    char esc[512];
                    json_escape(esc, sizeof(esc), slot->keyseq->source);
                    pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos),
                        ",\"keyseq_dsl\":\"%s\",\"keyseq_enabled\":1", esc);
                } else {
                    pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos),
                        ",\"keyseq_dsl\":\"\",\"keyseq_enabled\":0");
                }

                pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos), "}");
                rlen = pos;

                /* Also push SSE so other connected clients update */
                char sse_json[SSE_BUF_SIZE];
                build_ch_status_json(ch, sse_json, (int)sizeof(sse_json));
                sse_broadcast("ch_status", sse_json);
            } else {
                rlen = snprintf(resp, sizeof(resp), "{\"error\":\"load failed\"}");
            }
        }
    }
    else if (strcmp(type_str, "patch_delete") == 0) {
        char name[PATCH_NAME_MAX] = "";
        json_get_string(body, "name", name, sizeof(name));
        int idx = patch_find(name);
        if (idx < 0) {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"not found\"}");
        } else {
            /* Shift remaining patches down */
            for (int i = idx; i < g_num_patches - 1; i++)
                g_patches[i] = g_patches[i + 1];
            g_num_patches--;
            patches_save();
            rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
        }
    }
    else if (strcmp(type_str, "patch_rename") == 0) {
        char name[PATCH_NAME_MAX] = "", new_name[PATCH_NAME_MAX] = "";
        json_get_string(body, "name", name, sizeof(name));
        json_get_string(body, "new_name", new_name, sizeof(new_name));
        int idx = patch_find(name);
        if (idx < 0) {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"not found\"}");
        } else if (!new_name[0]) {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"empty name\"}");
        } else {
            strncpy(g_patches[idx].name, new_name, PATCH_NAME_MAX - 1);
            patches_save();
            rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
        }
    }
    else if (strcmp(type_str, "additive_expr") == 0) {
        /* Set expression strings on additive synth */
        int ch = 0;
        char amp_src[256] = "", freq_src[256] = "", phase_src[256] = "";
        json_get_int(body, "channel", &ch);
        json_get_string(body, "amp", amp_src, sizeof(amp_src));
        json_get_string(body, "freq", freq_src, sizeof(freq_src));
        json_get_string(body, "phase", phase_src, sizeof(phase_src));

        if (ch >= 0 && ch < MAX_SLOTS) {
            RackSlot *slot = &g_rack.slots[ch];
            if (slot->active && slot->state &&
                strcmp(g_type_registry[slot->type_idx]->name, "additive") == 0) {
                AdditiveState *as = (AdditiveState *)slot->state;
                if (amp_src[0]) {
                    strncpy(as->amp_expr_src, amp_src, 255);
                    ke_compile(&as->amp_expr, amp_src);
                    fprintf(stderr, "[additive] amp_expr: %s (valid=%d)\n", amp_src, as->amp_expr.valid);
                }
                if (freq_src[0]) {
                    strncpy(as->freq_expr_src, freq_src, 255);
                    ke_compile(&as->freq_expr, freq_src);
                    fprintf(stderr, "[additive] freq_expr: %s (valid=%d)\n", freq_src, as->freq_expr.valid);
                }
                if (phase_src[0]) {
                    strncpy(as->phase_expr_src, phase_src, 255);
                    ke_compile(&as->phase_expr, phase_src);
                }
                as->table_dirty = 1;
                state_mark_dirty();
                rlen = snprintf(resp, sizeof(resp),
                    "{\"ok\":true,\"amp_valid\":%d,\"freq_valid\":%d,\"phase_valid\":%d}",
                    as->amp_expr.valid, as->freq_expr.valid, as->phase_expr.valid);
            } else {
                rlen = snprintf(resp, sizeof(resp), "{\"error\":\"not additive\"}");
            }
        } else {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"bad channel\"}");
        }
    }
    else if (strcmp(type_str, "waveform") == 0) {
        /* Return wavetable data for oscillator view */
        int ch = 0;
        json_get_int(body, "channel", &ch);
        if (ch >= 0 && ch < MAX_SLOTS) {
            RackSlot *slot = &g_rack.slots[ch];
            if (slot->active && slot->state &&
                strcmp(g_type_registry[slot->type_idx]->name, "additive") == 0) {
                AdditiveState *as = (AdditiveState *)slot->state;
                if (as->table_dirty) additive_build_table(as);
                /* Downsample to 256 points for the view */
                int pos = 0;
                pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos), "{\"samples\":[");
                int step = ADD_TABLE_SIZE / 256;
                for (int i = 0; i < 256; i++) {
                    pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos),
                        "%s%.4f", i ? "," : "", (double)as->table[i * step]);
                }
                pos += snprintf(resp + pos, (size_t)(SSE_BUF_SIZE - pos),
                    "],\"length\":256,\"mode\":%d,\"harmonics\":%d}",
                    as->mode, as->num_harmonics);
                rlen = pos;
            } else {
                rlen = snprintf(resp, sizeof(resp), "{\"error\":\"not additive\"}");
            }
        } else {
            rlen = snprintf(resp, sizeof(resp), "{\"error\":\"bad channel\"}");
        }
    }
    else if (strcmp(type_str, "panic") == 0) {
        /* All notes off on all channels, stop all keyseqs */
        for (int ch = 0; ch < MAX_SLOTS; ch++) {
            RackSlot *slot = &g_rack.slots[ch];
            if (!atomic_load(&slot->active) || !slot->state) continue;
            InstrumentType *itype = g_type_registry[slot->type_idx];

            /* Send CC 123 (All Notes Off) */
            uint8_t status = (uint8_t)(0xB0 | ch);
            itype->midi(slot->state, status, 123, 0);

            /* Stop keyseq */
            if (slot->keyseq) keyseq_stop(slot->keyseq);
        }
        fprintf(stderr, "[miniwave] PANIC — all notes off, all keyseqs stopped\n");
        rlen = snprintf(resp, sizeof(resp), "{\"ok\":true}");
    }
    else if (strcmp(type_str, "debug_lifetime") == 0) {
        rlen = snprintf(resp, sizeof(resp),
            "{\"slot_readers\":%d,"
            "\"read_enters\":%llu,\"read_exits\":%llu,"
            "\"deferred_queued\":%d,"
            "\"deferred_delayed\":%llu,\"deferred_drained\":%llu}",
            atomic_load(&g_slot_readers),
            (unsigned long long)atomic_load(&g_slot_read_enters),
            (unsigned long long)atomic_load(&g_slot_read_exits),
            atomic_load(&g_deferred_free_count),
            (unsigned long long)atomic_load(&g_deferred_free_delayed),
            (unsigned long long)atomic_load(&g_deferred_free_drained));
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
                else if (strcmp(method, "GET") == 0 && strcmp(path, "/effex") == 0) {
                    char exe_dir[1024];
                    if (platform_exe_dir(exe_dir, sizeof(exe_dir)) == 0) {
                        char fpath[1280];
                        snprintf(fpath, sizeof(fpath), "%sweb/effex.html", exe_dir);
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
                else if (strcmp(method, "GET") == 0 && strcmp(path, "/api-help") == 0) {
                    char *hbuf = (char *)malloc(8192);
                    if (hbuf) {
                        int hp = 0;
                        hp += snprintf(hbuf + hp, (size_t)(8192 - hp),
                            "miniwave API — %d instrument types, %d slots\n"
                            "==================================================\n"
                            "All POST /api {\"type\":\"<type>\", ...}\n\n", g_n_types, MAX_SLOTS);

                        /* Dynamic: registered instrument types */
                        hp += snprintf(hbuf + hp, (size_t)(8192 - hp), "Instruments: ");
                        for (int i = 0; i < g_n_types; i++)
                            hp += snprintf(hbuf + hp, (size_t)(8192 - hp), "%s%s", i?", ":"", g_type_registry[i]->name);

                        /* Dynamic: active slots */
                        hp += snprintf(hbuf + hp, (size_t)(8192 - hp), "\nActive slots: ");
                        for (int i = 0; i < MAX_SLOTS; i++) {
                            RackSlot *sl = &g_rack.slots[i];
                            if (sl->active && sl->state)
                                hp += snprintf(hbuf + hp, (size_t)(8192 - hp), "ch%d=%s ", i, g_type_registry[sl->type_idx]->name);
                        }

                        hp += snprintf(hbuf + hp, (size_t)(8192 - hp),
                            "\n\n"
                            "── Rack ──\n"
                            "rack_status          → slots, volumes, midi, audio, bus, osc, mcast, sse\n"
                            "rack_types           → registered instrument type names\n"
                            "slot_set             channel, instrument\n"
                            "slot_clear           channel\n"
                            "slot_volume          channel, value(0-1)\n"
                            "slot_mute            channel, value(0/1)\n"
                            "slot_solo            channel, value(0/1)\n"
                            "master_volume        value(0-1)\n"
                            "local_mute           value(0/1) — bus-only mode\n"
                            "\n"
                            "── Notes ──\n"
                            "note_on              channel, note(0-127), velocity(0-127)\n"
                            "note_off             channel, note(0-127)\n"
                            "panic                → all notes off, all keyseqs stopped\n"
                            "\n"
                            "── Instrument Params ──\n"
                            "ch                   channel, path, fargs/iargs — generic param/preset/osc\n"
                            "keyseq_spec          channel(opt) → full spec: expression lang + instrument schemas + values\n"
                            "                     schemas include: params[], envelopes[], presets[], patches[], drum_map[]\n"
                            "\n"
                            "── Key Sequence ──\n"
                            "keyseq_dsl           channel, dsl — load algo (semicolon-delimited)\n"
                            "keyseq_enable        channel, enabled(0/1)\n"
                            "keyseq_stop          channel\n"
                            "keyseq_status        channel → enabled, dsl, active_voices\n"
                            "keyseq_preview       channel, note, velocity → computed steps from server state\n"
                            "\n"
                            "── Step Sequencer ──\n"
                            "seq_dsl              channel, dsl(\"120L C4q E4e G4e\")\n"
                            "seq_stop             channel\n"
                            "\n"
                            "── Patches (server-persisted) ──\n"
                            "patch_save           channel, name → saves instrument + keyseq + seq\n"
                            "patch_load           channel, name → returns full state for UI update\n"
                            "patch_list           instrument(opt) → [{name, type, has_keyseq, has_seq}]\n"
                            "patch_rename         name, new_name\n"
                            "patch_delete         name\n"
                            "\n"
                            "── MIDI ──\n"
                            "midi_device          value(device_id)\n"
                            "midi_devices         → list available\n"
                            "\n"
                            "── Specialized ──\n"
                            "waveform             channel → {samples:[256], mode, harmonics} (additive only)\n"
                            "debug_lifetime       → slot reader/deferred-free counters\n"
                            "\n"
                            "── SSE (GET /events) ──\n"
                            "hello, rack_status, ch_status, rack_types, midi_devices, keyseq_trigger\n"
                            "\n"
                            "── GET ──\n"
                            "/              web UI\n"
                            "/events        SSE stream\n"
                            "/osc-spec      OSC address map\n"
                            "/keyseq-test   keyseq expression tester\n"
                            "/api-help      this page\n");

                        /* Dynamic: user patches */
                        if (g_num_patches > 0) {
                            hp += snprintf(hbuf + hp, (size_t)(8192 - hp), "\nUser patches (%d): ", g_num_patches);
                            for (int i = 0; i < g_num_patches && i < 20; i++)
                                hp += snprintf(hbuf + hp, (size_t)(8192 - hp), "%s\"%s\"(%s)", i?", ":"", g_patches[i].name, g_patches[i].type);
                            hp += snprintf(hbuf + hp, (size_t)(8192 - hp), "\n");
                        }

                        http_send_response(client_fd, 200, "text/plain; charset=utf-8", hbuf, hp);
                        free(hbuf);
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
                    int assigned_cid = 0;
                    pthread_mutex_lock(&g_http_lock);
                    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
                        if (g_http_clients[i].fd < 0) {
                            g_http_clients[i].fd = client_fd;
                            g_http_clients[i].is_sse = 1;
                            g_http_clients[i].detail_channel = -1;
                            assigned_cid = g_next_client_id++;
                            g_http_clients[i].client_id = assigned_cid;
                            registered = 1;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&g_http_lock);

                    if (!registered) {
                        close(client_fd);
                    } else {
                        char json[SSE_BUF_SIZE];

                        /* Send client_id so /api/detail can target this client */
                        int cid = assigned_cid;
                        snprintf(json, sizeof(json), "{\"client_id\":%d}", cid);
                        sse_send_event(client_fd, "hello", json);

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
                        slot_read_begin();
                        http_handle_api(client_fd, body);
                        slot_read_end();
                    } else {
                        http_send_response(client_fd, 400, "application/json",
                                           "{\"error\":\"no body\"}", 18);
                    }
                    close(client_fd);
                }
                else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/detail") == 0) {
                    const char *body = strstr(reqbuf, "\r\n\r\n");
                    int detail_ch = -1;
                    int cid = -1;
                    if (body) {
                        body += 4;
                        json_get_int(body, "channel", &detail_ch);
                        json_get_int(body, "client_id", &cid);
                    }

                    pthread_mutex_lock(&g_http_lock);
                    for (int i = 0; i < MAX_HTTP_CLIENTS; i++) {
                        if (g_http_clients[i].fd >= 0 && g_http_clients[i].is_sse) {
                            /* If client_id provided, only update that client.
                             * Otherwise fall back to updating all (legacy). */
                            if (cid < 0 || g_http_clients[i].client_id == cid) {
                                g_http_clients[i].detail_channel = detail_ch;
                            }
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
        /* Rack status: push on change (throttled 100ms), heartbeat 5s */
        {
            int rack_dirty = atomic_exchange(&g_sse_rack_dirty, 0);
            if ((rack_elapsed_ms >= 100 && rack_dirty) || rack_elapsed_ms >= 5000) {
                last_rack_poll = now;
                slot_read_begin();
                char json[SSE_BUF_SIZE];
                build_rack_status_json(json, (int)sizeof(json));
                slot_read_end();
                sse_broadcast("rack_status", json);
            } else if (rack_dirty) {
                atomic_store(&g_sse_rack_dirty, 1); /* re-mark, throttled */
            }
        }

        /* Channel detail: push on change (throttled 50ms), no heartbeat */
        {
            long detail_elapsed_ms = (now.tv_sec - last_detail_poll.tv_sec) * 1000 +
                                     (now.tv_nsec - last_detail_poll.tv_nsec) / 1000000;
            int detail_dirty = atomic_exchange(&g_sse_detail_dirty, 0);
            if (detail_elapsed_ms >= 50 && detail_dirty) {
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
                    slot_read_begin();
                    char json[SSE_BUF_SIZE];
                    build_ch_status_json(detail_ch, json, (int)sizeof(json));
                    slot_read_end();
                    sse_broadcast("ch_status", json);
                }
            } else if (detail_dirty) {
                atomic_store(&g_sse_detail_dirty, 1);
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
        else if (strcmp(osc_addr, "/rack/save") == 0) {
            state_save();
            fprintf(stderr, "[miniwave] state saved (OSC)\n");
        }
        else if (strcmp(osc_addr, "/rack/slot/mono") == 0 && ai >= 2) {
            int ch = arg_i[0];
            if (ch >= 0 && ch < MAX_SLOTS) {
                g_rack.slots[ch].mono = arg_i[1] ? 1 : 0;
                g_rack.slots[ch].last_note = -1;
                fprintf(stderr, "[miniwave] slot %d mono: %s\n", ch,
                        g_rack.slots[ch].mono ? "ON" : "OFF");
            }
        }
        else if (strcmp(osc_addr, "/rack/slot/legato") == 0 && ai >= 2) {
            int ch = arg_i[0];
            if (ch >= 0 && ch < MAX_SLOTS) {
                g_rack.slots[ch].legato = arg_i[1] ? 1 : 0;
                g_rack.slots[ch].glide_rate = 5.0f; /* ~200ms glide */
                g_rack.slots[ch].glide_pos = 1.0f;  /* no glide until next note */
                if (g_rack.slots[ch].legato) g_rack.slots[ch].mono = 1;
                fprintf(stderr, "[miniwave] slot %d legato: %s\n", ch,
                        g_rack.slots[ch].legato ? "ON" : "OFF");
            }
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

                slot_read_begin();
                if (slot->active && slot->state) {
                    if (strcmp(sub_path, "/status") == 0) {
                        InstrumentType *itype = g_type_registry[slot->type_idx];
                        uint8_t reply[512];
                        int rlen = itype->osc_status(slot->state, reply, (int)sizeof(reply));
                        if (rlen > 0) {
                            sendto(sock, reply, (size_t)rlen, 0,
                                   (struct sockaddr *)&sender, sender_len);
                        }
                    } else if (strncmp(sub_path, "/seq/", 5) == 0 || strcmp(sub_path, "/seq") == 0) {
                        if (slot->seq) seq_osc_handle(slot->seq, sub_path, arg_i, ai, arg_f, afi);
                    } else {
                        InstrumentType *itype = g_type_registry[slot->type_idx];
                        itype->osc_handle(slot->state, sub_path,
                                          arg_i, ai, arg_f, afi);
                    }
                }
                slot_read_end();
            }
        }
    }

    close(sock);
    return NULL;
}

#endif
