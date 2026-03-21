/* miniwave — macOS platform (CoreMIDI + Core Audio)
 *
 * All portable code lives in common/ headers.
 * This file implements platform_* functions for macOS and main().
 */

#include <errno.h>
#include <math.h>
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
#include <mach-o/dyld.h>

/* CoreMIDI + Core Audio */
#include <CoreMIDI/CoreMIDI.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>

/* ── Common code ───────────────────────────────────────────────────── */

#include "../common/rack.h"
#include "../common/server.h"

/* No JACK on macOS by default — Core Audio is the native path */

/* ══════════════════════════════════════════════════════════════════════
 *  Platform: CoreMIDI
 * ══════════════════════════════════════════════════════════════════════ */

static MIDIClientRef   g_midi_client = 0;
static MIDIPortRef     g_midi_port = 0;
static MIDIEndpointRef g_midi_source = 0;
static int             g_midi_connected = 0;

/* CoreMIDI read callback — called on a CoreMIDI thread */
static void midi_read_proc(const MIDIPacketList *pktlist,
                           void *readProcRefCon __attribute__((unused)),
                           void *srcConnRefCon __attribute__((unused))) {
    const MIDIPacket *pkt = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; i++) {
        if (pkt->length >= 1) {
            midi_dispatch_raw(pkt->data, (int)pkt->length);
        }
        pkt = MIDIPacketNext(pkt);
    }
}

/* Get display name for a MIDI endpoint */
static void midi_endpoint_name(MIDIEndpointRef ep, char *buf, int max) {
    CFStringRef name = NULL;
    MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name);
    if (name) {
        CFStringGetCString(name, buf, max, kCFStringEncodingUTF8);
        CFRelease(name);
    } else {
        snprintf(buf, (size_t)max, "Unknown");
    }
}

static int platform_midi_init(void) {
    OSStatus st = MIDIClientCreate(CFSTR("miniwave"), NULL, NULL, &g_midi_client);
    if (st != noErr) {
        fprintf(stderr, "[miniwave] can't create CoreMIDI client: %d\n", (int)st);
        return -1;
    }

    st = MIDIInputPortCreate(g_midi_client, CFSTR("MIDI In"),
                             midi_read_proc, NULL, &g_midi_port);
    if (st != noErr) {
        fprintf(stderr, "[miniwave] can't create CoreMIDI input port: %d\n", (int)st);
        MIDIClientDispose(g_midi_client);
        g_midi_client = 0;
        return -1;
    }

    fprintf(stderr, "[miniwave] CoreMIDI ready\n");
    return 0;
}

static int platform_midi_connect(const char *addr_str) {
    if (!g_midi_client) return -1;

    /* Disconnect old */
    if (g_midi_connected && g_midi_source) {
        MIDIPortDisconnectSource(g_midi_port, g_midi_source);
        g_midi_connected = 0;
        g_midi_device_name[0] = '\0';
    }

    /* addr_str is the source index as a string (e.g. "0", "1") */
    int idx = atoi(addr_str);
    ItemCount nsrc = MIDIGetNumberOfSources();
    if (idx < 0 || (ItemCount)idx >= nsrc) {
        fprintf(stderr, "[miniwave] MIDI source %d out of range (have %lu)\n",
                idx, (unsigned long)nsrc);
        return -1;
    }

    MIDIEndpointRef ep = MIDIGetSource((ItemCount)idx);
    OSStatus st = MIDIPortConnectSource(g_midi_port, ep, NULL);
    if (st != noErr) {
        fprintf(stderr, "[miniwave] can't connect to MIDI source %d: %d\n",
                idx, (int)st);
        return -1;
    }

    g_midi_source = ep;
    g_midi_connected = 1;

    midi_endpoint_name(ep, g_midi_device_name, sizeof(g_midi_device_name));
    fprintf(stderr, "[miniwave] MIDI connected: %s\n", g_midi_device_name);
    return 0;
}

static void platform_midi_disconnect(void) {
    if (!g_midi_connected || !g_midi_source) return;
    MIDIPortDisconnectSource(g_midi_port, g_midi_source);
    g_midi_connected = 0;
    g_midi_source = 0;
    g_midi_device_name[0] = '\0';
    fprintf(stderr, "[miniwave] MIDI disconnected\n");
}

static int platform_midi_list_devices(char devices[][64], char names[][128], int max_devices) {
    ItemCount nsrc = MIDIGetNumberOfSources();
    int count = 0;
    for (ItemCount i = 0; i < nsrc && count < max_devices; i++) {
        MIDIEndpointRef ep = MIDIGetSource(i);
        snprintf(devices[count], 64, "%lu", (unsigned long)i);
        midi_endpoint_name(ep, names[count], 128);
        count++;
    }
    return count;
}

/* CoreMIDI drives callbacks on its own thread — no poll thread needed,
 * but we provide one that just sleeps to satisfy the platform interface. */
static void *platform_midi_thread(void *arg) {
    (void)arg;
    while (!g_quit) {
        usleep(100000); /* 100ms — CoreMIDI callbacks do the real work */
    }
    return NULL;
}

static void platform_midi_cleanup(void) {
    if (g_midi_connected) platform_midi_disconnect();
    if (g_midi_port) { MIDIPortDispose(g_midi_port); g_midi_port = 0; }
    if (g_midi_client) { MIDIClientDispose(g_midi_client); g_midi_client = 0; }
}

/* ══════════════════════════════════════════════════════════════════════
 *  Platform: Core Audio (AudioUnit output)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    AudioComponentInstance  au;
    float                  *mix_buf;
    float                  *slot_buf;
    int                     buf_frames;
    int                     sample_rate;
    WaveosBus              *bus;
    int                     bus_slot;
} CoreAudioCtx;

static CoreAudioCtx g_ca = {0};

/* AudioUnit render callback — realtime thread */
static OSStatus ca_render_cb(void *inRefCon,
                             AudioUnitRenderActionFlags *ioActionFlags __attribute__((unused)),
                             const AudioTimeStamp *inTimeStamp __attribute__((unused)),
                             UInt32 inBusNumber __attribute__((unused)),
                             UInt32 inNumberFrames,
                             AudioBufferList *ioData) {
    CoreAudioCtx *ctx = (CoreAudioCtx *)inRefCon;

    int frames = (int)inNumberFrames;
    if (frames > ctx->buf_frames) frames = ctx->buf_frames;

    render_mix(ctx->mix_buf, ctx->slot_buf, frames, ctx->sample_rate);

    /* Core Audio: interleaved float32 stereo */
    float *out = (float *)ioData->mBuffers[0].mData;

    if (g_rack.local_mute) {
        memset(out, 0, sizeof(float) * (size_t)(frames * CHANNELS));
    } else {
        memcpy(out, ctx->mix_buf, sizeof(float) * (size_t)(frames * CHANNELS));
    }

    /* Write to bus if available */
    if (ctx->bus && ctx->bus_slot >= 0) {
        bus_write(ctx->bus, ctx->bus_slot, ctx->mix_buf, frames);
    }

    return noErr;
}

static int ca_init(int period_size) {
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags = 0,
        .componentFlagsMask = 0
    };

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        fprintf(stderr, "[miniwave] can't find default audio output\n");
        return -1;
    }

    OSStatus st = AudioComponentInstanceNew(comp, &g_ca.au);
    if (st != noErr) {
        fprintf(stderr, "[miniwave] can't create AudioUnit: %d\n", (int)st);
        return -1;
    }

    /* Query the device sample rate */
    AudioStreamBasicDescription asbd;
    UInt32 size = sizeof(asbd);
    st = AudioUnitGetProperty(g_ca.au, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Output, 0, &asbd, &size);
    if (st == noErr && asbd.mSampleRate > 0) {
        g_ca.sample_rate = (int)asbd.mSampleRate;
    } else {
        g_ca.sample_rate = SAMPLE_RATE;
    }

    /* Set input format: interleaved float32 stereo */
    AudioStreamBasicDescription fmt = {0};
    fmt.mSampleRate       = (Float64)g_ca.sample_rate;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mBytesPerPacket   = sizeof(float) * CHANNELS;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerFrame    = sizeof(float) * CHANNELS;
    fmt.mChannelsPerFrame = CHANNELS;
    fmt.mBitsPerChannel   = 32;

    st = AudioUnitSetProperty(g_ca.au, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
    if (st != noErr) {
        fprintf(stderr, "[miniwave] can't set audio format: %d\n", (int)st);
        AudioComponentInstanceDispose(g_ca.au);
        g_ca.au = NULL;
        return -1;
    }

    /* Set render callback */
    AURenderCallbackStruct cb = {
        .inputProc = ca_render_cb,
        .inputProcRefCon = &g_ca
    };
    st = AudioUnitSetProperty(g_ca.au, kAudioUnitProperty_SetRenderCallback,
                              kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (st != noErr) {
        fprintf(stderr, "[miniwave] can't set render callback: %d\n", (int)st);
        AudioComponentInstanceDispose(g_ca.au);
        g_ca.au = NULL;
        return -1;
    }

    /* Allocate render buffers */
    g_ca.buf_frames = period_size > 0 ? period_size * 16 : 4096;
    g_ca.mix_buf  = calloc((size_t)(g_ca.buf_frames * CHANNELS), sizeof(float));
    g_ca.slot_buf = calloc((size_t)(g_ca.buf_frames * CHANNELS), sizeof(float));
    if (!g_ca.mix_buf || !g_ca.slot_buf) {
        fprintf(stderr, "[miniwave] can't alloc Core Audio buffers\n");
        AudioComponentInstanceDispose(g_ca.au);
        g_ca.au = NULL;
        return -1;
    }

    st = AudioUnitInitialize(g_ca.au);
    if (st != noErr) {
        fprintf(stderr, "[miniwave] can't initialize AudioUnit: %d\n", (int)st);
        AudioComponentInstanceDispose(g_ca.au);
        g_ca.au = NULL;
        return -1;
    }

    fprintf(stderr, "[miniwave] Core Audio @ %dHz buf=%d\n",
            g_ca.sample_rate, g_ca.buf_frames);
    return 0;
}

static int ca_start(void) {
    if (!g_ca.au) return -1;
    OSStatus st = AudioOutputUnitStart(g_ca.au);
    if (st != noErr) {
        fprintf(stderr, "[miniwave] can't start AudioUnit: %d\n", (int)st);
        return -1;
    }
    return 0;
}

static void ca_cleanup(void) {
    if (g_ca.au) {
        AudioOutputUnitStop(g_ca.au);
        AudioUnitUninitialize(g_ca.au);
        AudioComponentInstanceDispose(g_ca.au);
        g_ca.au = NULL;
    }
    free(g_ca.mix_buf);
    free(g_ca.slot_buf);
    g_ca.mix_buf = NULL;
    g_ca.slot_buf = NULL;
}

/* ── Platform helpers ──────────────────────────────────────────────── */

static int platform_exe_dir(char *buf, int max) {
    uint32_t size = (uint32_t)max;
    if (_NSGetExecutablePath(buf, &size) != 0) return -1;
    char *slash = strrchr(buf, '/');
    if (slash) *(slash + 1) = '\0';
    return 0;
}

static const char *platform_audio_fallback_name(void) {
    return "Core Audio";
}

/* ── Usage ──────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "miniwave — modular rack host (macOS)\n"
        "Usage: %s [options]\n"
        "  -m IDX    MIDI source index (e.g. 0, 1)\n"
        "  -c N      Pre-configure channels 1-N with FM synth (default: 0)\n"
        "  -O PORT   OSC port (default: 9000)\n"
        "  -W PORT   HTTP/SSE port for WaveUI (default: 8080, 0 to disable)\n"
        "  -P SIZE   Audio period hint (default: 64)\n"
        "  -h        Help\n", prog);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGPIPE, SIG_IGN);  /* ignore broken pipe from dead SSE clients */

    char midi_dev[64] = "";
    int pre_config = 0;
    int osc_port = DEFAULT_OSC_PORT;
    int http_port = DEFAULT_HTTP_PORT;
    int period_size = DEFAULT_PERIOD;

    int opt;
    while ((opt = getopt(argc, argv, "m:c:O:W:P:h")) != -1) {
        switch (opt) {
        case 'm': strncpy(midi_dev, optarg, sizeof(midi_dev) - 1); break;
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
    patches_load();
    keyseq_wire_graph_broadcast();

    for (int i = 0; i < pre_config; i++) {
        if (!g_rack.slots[i].active)
            rack_set_slot(i, "fm-synth");
    }

    /* ── CoreMIDI ──────────────────────────────────────────────── */

    if (platform_midi_init() == 0) {
        if (midi_dev[0] != '\0') {
            platform_midi_connect(midi_dev);
        } else {
            fprintf(stderr, "[miniwave] MIDI: ready (use -m IDX or OSC /midi/device)\n");
        }
    }

    /* ── Core Audio ────────────────────────────────────────────── */

    if (ca_init(period_size) != 0) {
        fprintf(stderr, "[miniwave] ERROR: can't init Core Audio\n");
        platform_midi_cleanup();
        return 1;
    }

    /* ── Shared memory bus (optional, for waveOS compat) ─────── */

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
            fprintf(stderr, "[miniwave] bus not available (start WaveLoop first)\n");
        }
    }

    g_ca.bus = bus;
    g_ca.bus_slot = bus_slot;
    g_bus_active = (bus && bus_slot >= 0) ? 1 : 0;
    g_bus_slot = bus_slot;

    fprintf(stderr, "[miniwave] running [Core Audio]%s%s — %d types registered\n",
            (bus && bus_slot >= 0) ? " [BUS]" : "",
            (http_port > 0) ? " [HTTP]" : "",
            g_n_types);

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

    /* ── Start Core Audio and wait ─────────────────────────────── */

    if (ca_start() != 0) {
        fprintf(stderr, "[miniwave] ERROR: can't start Core Audio\n");
        goto cleanup;
    }

    while (!g_quit) {
        usleep(50000); /* 50ms idle — Core Audio callback does the work */
    }

    /* ── Shutdown ───────────────────────────────────────────────── */

    fprintf(stderr, "[miniwave] shutting down...\n");
    state_save();

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

    if (bus && bus_slot >= 0) {
        atomic_store(&bus->slots[bus_slot].active, 0);
        memset(bus->slots[bus_slot].name, 0, 32);
        munmap(bus, sizeof(WaveosBus));
    }

    platform_midi_cleanup();
    ca_cleanup();
    free(g_html_content);

    fprintf(stderr, "[miniwave] done\n");
    return 0;
}
