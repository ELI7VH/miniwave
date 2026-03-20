/* miniwave — platform abstraction interface
 *
 * Each platform (linux/miniwave.c, macos/miniwave.c) implements these
 * static functions. They are forward-declared here so that common code
 * (server.h, rack.h) can call them before the definitions appear in the
 * translation unit.
 */

#ifndef MINIWAVE_PLATFORM_H
#define MINIWAVE_PLATFORM_H

/* MIDI subsystem */
static int  platform_midi_init(void);
static int  platform_midi_connect(const char *addr);
static void platform_midi_disconnect(void);
static int  platform_midi_list_devices(char devices[][64], char names[][128], int max);
static void *platform_midi_thread(void *arg);
static void platform_midi_cleanup(void);

/* Resolve directory containing the executable (trailing slash).
 * Returns 0 on success, -1 on failure. */
static int platform_exe_dir(char *buf, int max);

/* Human-readable name for the native audio fallback ("ALSA", "Core Audio") */
static const char *platform_audio_fallback_name(void);

#endif
