/*
 * keyfinder_wrapper.h - Plain C interface to libkeyfinder
 *
 * Wraps the C++11 libkeyfinder library in a simple C API suitable for
 * use from the Move Anything audio FX plugin (which is pure C).
 */

#ifndef KEYFINDER_WRAPPER_H
#define KEYFINDER_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create a key detection context.
 * sample_rate: audio sample rate (e.g. 44100)
 * Returns opaque context pointer, or NULL on failure. */
void* kd_create(int sample_rate);

/* Destroy a key detection context and free all resources. */
void kd_destroy(void *ctx);

/* Feed stereo interleaved int16 audio for analysis.
 * The audio is downmixed to mono internally.
 * When enough audio has been accumulated (>= window size),
 * analysis runs automatically. */
void kd_feed(void *ctx, const int16_t *stereo_audio, int frames);

/* Get the currently detected key as a human-readable string.
 * Returns bytes written (excluding NUL), or 0 if no key detected yet.
 * Example output: "Eb min", "A maj", "no key" */
int kd_get_key(void *ctx, char *buf, int buf_len);

/* Set the analysis window size in seconds (1.0 - 8.0).
 * Larger windows are more accurate but slower to update. */
void kd_set_window(void *ctx, float seconds);

/* Get the current window size in seconds. */
float kd_get_window(void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* KEYFINDER_WRAPPER_H */
