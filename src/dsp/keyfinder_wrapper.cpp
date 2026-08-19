/*
 * keyfinder_wrapper.cpp - C++ wrapper around libkeyfinder
 *
 * Audio thread is completely lock-free with zero-copy handoff (ping-pong).
 * Audio is downsampled 8x before analysis to reduce CPU load.
 * Analysis thread forces itself to SCHED_OTHER on cores 0-2 — it inherits
 * Move's realtime audio priority otherwise, see analysis_thread_fn.
 */

#include "keyfinder_wrapper.h"

#include <keyfinder/keyfinder.h>
#include <keyfinder/audiodata.h>
#include <keyfinder/constants.h>

#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <sched.h>

/* Key enum to display string mapping */
static const char* key_names[] = {
    "A maj",  "A min",  "Bb maj", "Bb min",
    "B maj",  "B min",  "C maj",  "C min",
    "Db maj", "Db min", "D maj",  "D min",
    "Eb maj", "Eb min", "E maj",  "E min",
    "F maj",  "F min",  "Gb maj", "Gb min",
    "G maj",  "G min",  "Ab maj", "Ab min",
    /* Not "---": the Shadow UI's enum square strips '-' and '_' as word
     * separators before splitting, so a run of dashes renders as an EMPTY
     * box. Two short words also split cleanly across its two lines. */
    "no key"  /* SILENCE = 24 */
};

/*
 * Downsample factor: feed libkeyfinder at ~5512 Hz instead of 44100 Hz.
 * Key detection only needs pitch info up to ~2 kHz (covers all musical keys).
 * Reduces FFT/analysis CPU by ~8x vs raw rate.
 */
#define DOWNSAMPLE 8
#define EFFECTIVE_RATE (44100 / DOWNSAMPLE)  /* 5512 Hz */

/* Max buffer: 8 seconds at downsampled rate, plus headroom for one block */
#define MAX_BUF_SAMPLES (8 * EFFECTIVE_RATE + 128)
#define NUM_KEYS 25      /* 24 keys + SILENCE */
#define VOTE_DECAY 0.6f  /* old votes multiplied by this each new analysis */

struct kd_context {
    /* Ping-pong buffers: audio thread writes to one, analysis reads the other.
     * No copy needed — just swap which index is active. */
    double bufs[2][MAX_BUF_SAMPLES];
    std::atomic<int> active_buf;       /* 0 or 1: which buf audio thread writes to */
    int write_pos;                      /* position in active buf (only audio thread touches) */
    int downsample_counter;             /* counts input frames for decimation */

    /* Handoff flag: set by audio thread, cleared by analysis thread */
    std::atomic<int> ready_buf;         /* -1 = none ready, 0 or 1 = buf index to analyze */
    std::atomic<int> ready_len;         /* number of samples in ready buf */

    /* Analysis thread */
    std::thread worker;
    std::atomic<bool> shutdown;

    /* Result: decaying vote across analyzed windows.
     * Each new analysis decays old votes by VOTE_DECAY, keeping recent
     * windows dominant so track changes are picked up quickly. */
    float votes[NUM_KEYS];              /* vote tally per key (only analysis thread writes) */
    char detected_key[16];              /* winning key string (updated by analysis thread) */

    /* Config */
    int sample_rate;
    float window_seconds;
    int window_samples;                 /* at downsampled rate */
    int skip_windows;                   /* windows to skip after each analysis (CPU throttle) */
};

static void analysis_thread_fn(kd_context *ctx) {
    /* Drop to SCHED_OTHER explicitly, and do it FIRST.
     *
     * `nice(10)` alone was a no-op and this thread was running REALTIME.
     * kd_create() is reached from the module-load path, which Schwung's shim
     * services inside the SPI callback — i.e. on Move's SCHED_FIFO 70 audio
     * thread. std::thread inherits the creator's scheduling policy, so this
     * "low priority" thread came up at SCHED_FIFO 70, and nice() does not
     * affect SCHED_FIFO threads at all.
     *
     * Consequence, measured on hardware 2026-08-19: keyOfAudio() below is a
     * ~200 ms FFT over the analysis window, and it ran at priority 70 —
     * ABOVE Move's own "Link Main" thread, which is SCHED_FIFO 35. Every
     * analysis pass starved Move's Link Audio publisher for ~200 ms, so Move
     * stopped emitting per-track audio and then delivered ~70 packets in one
     * burst. Audible as periodic dropouts, on a 4.0 s grid (a 2.0 s window
     * with skip_windows == 1 analyses every second window). Loading keydetect
     * caused it; unloading it stopped it.
     *
     * SCHED_OTHER + nice(10) is what the original comment intended.
     */
    {
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = 0;
        sched_setscheduler(0, SCHED_OTHER, &sp);
    }
    if (nice(10) == -1) { /* best-effort; SCHED_OTHER above is what matters */ }

    /* Keep off core 3, which is reserved for the SPI callback. Matches the
     * pinning Schwung applies to its own compute-heavy processes. */
    {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(0, &mask);
        CPU_SET(1, &mask);
        CPU_SET(2, &mask);
        sched_setaffinity(0, sizeof(mask), &mask);
    }

    KeyFinder::KeyFinder keyfinder;

    while (!ctx->shutdown.load(std::memory_order_relaxed)) {
        int buf_idx = ctx->ready_buf.load(std::memory_order_acquire);
        if (buf_idx < 0) {
            usleep(50000);  /* 50ms poll */
            continue;
        }

        int len = ctx->ready_len.load(std::memory_order_relaxed);

        /* Mark consumed immediately so audio thread can queue next */
        ctx->ready_buf.store(-1, std::memory_order_release);

        if (len <= 0) continue;

        /* Build AudioData */
        KeyFinder::AudioData audio;
        audio.setChannels(1);
        audio.setFrameRate(EFFECTIVE_RATE);
        audio.addToSampleCount(len);

        for (int i = 0; i < len; i++) {
            audio.setSample(i, ctx->bufs[buf_idx][i]);
        }

        /* Run analysis */
        KeyFinder::key_t key = keyfinder.keyOfAudio(audio);

        if (key >= 0 && key < KeyFinder::SILENCE) {
            /* Decay old votes so recent windows dominate */
            for (int k = 0; k < NUM_KEYS; k++) {
                ctx->votes[k] *= VOTE_DECAY;
            }

            /* Cast new vote */
            ctx->votes[key] += 1.0f;

            /* Find the key with the most votes */
            int best_key = key;
            float best_count = 0.0f;
            for (int k = 0; k < NUM_KEYS - 1; k++) {  /* exclude SILENCE */
                if (ctx->votes[k] > best_count) {
                    best_count = ctx->votes[k];
                    best_key = k;
                }
            }

            /* Update displayed key to majority winner */
            char tmp[16];
            std::strncpy(tmp, key_names[best_key], sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            std::memcpy(ctx->detected_key, tmp, 16);
        }
    }
}

extern "C" {

void* kd_create(int sample_rate) {
    kd_context *ctx = new (std::nothrow) kd_context();
    if (!ctx) return NULL;

    ctx->sample_rate = sample_rate;
    ctx->window_seconds = 2.0f;
    ctx->window_samples = (int)(ctx->window_seconds * EFFECTIVE_RATE);
    ctx->active_buf.store(0, std::memory_order_relaxed);
    ctx->write_pos = 0;
    ctx->downsample_counter = 0;
    ctx->ready_buf.store(-1, std::memory_order_relaxed);
    ctx->ready_len.store(0, std::memory_order_relaxed);
    ctx->shutdown.store(false, std::memory_order_relaxed);
    ctx->skip_windows = 0;
    std::memset(ctx->votes, 0, sizeof(ctx->votes));
    std::strcpy(ctx->detected_key, "no key");

    ctx->worker = std::thread(analysis_thread_fn, ctx);

    return ctx;
}

void kd_destroy(void *ptr) {
    kd_context *ctx = (kd_context*)ptr;
    if (!ctx) return;

    ctx->shutdown.store(true, std::memory_order_relaxed);
    if (ctx->worker.joinable()) {
        ctx->worker.join();
    }

    delete ctx;
}

void kd_feed(void *ptr, const int16_t *stereo_audio, int frames) {
    kd_context *ctx = (kd_context*)ptr;
    if (!ctx || !stereo_audio || frames <= 0) return;

    int buf_idx = ctx->active_buf.load(std::memory_order_relaxed);
    double *buf = ctx->bufs[buf_idx];
    int pos = ctx->write_pos;

    for (int i = 0; i < frames; i++) {
        /* Simple decimation: take every Nth sample */
        if (ctx->downsample_counter == 0) {
            double left  = stereo_audio[i * 2]     / 32768.0;
            double right = stereo_audio[i * 2 + 1] / 32768.0;
            buf[pos] = (left + right) * 0.5;
            pos++;

            /* Check if we've filled a window */
            if (pos >= ctx->window_samples) {
                if (ctx->skip_windows > 0) {
                    /* Cooldown: skip this window to reduce CPU load */
                    ctx->skip_windows--;
                } else if (ctx->ready_buf.load(std::memory_order_relaxed) < 0) {
                    /* Analysis idle — hand off this buffer */
                    ctx->ready_len.store(pos, std::memory_order_relaxed);
                    ctx->ready_buf.store(buf_idx, std::memory_order_release);
                    /* Swap to other buffer — zero copy */
                    buf_idx = 1 - buf_idx;
                    ctx->active_buf.store(buf_idx, std::memory_order_relaxed);
                    buf = ctx->bufs[buf_idx];
                    /* Skip next window to give CPU a break between analyses */
                    ctx->skip_windows = 1;
                }
                /* else: analysis busy — discard and refill */
                pos = 0;
            }
        }
        ctx->downsample_counter = (ctx->downsample_counter + 1) % DOWNSAMPLE;
    }

    ctx->write_pos = pos;
}

int kd_get_key(void *ptr, char *buf, int buf_len) {
    kd_context *ctx = (kd_context*)ptr;
    if (!ctx || !buf || buf_len <= 0) return 0;

    int len = std::strlen(ctx->detected_key);
    if (len >= buf_len) len = buf_len - 1;
    std::memcpy(buf, ctx->detected_key, len);
    buf[len] = '\0';
    return len;
}

void kd_set_window(void *ptr, float seconds) {
    kd_context *ctx = (kd_context*)ptr;
    if (!ctx) return;

    if (seconds < 1.0f) seconds = 1.0f;
    if (seconds > 8.0f) seconds = 8.0f;

    ctx->window_seconds = seconds;
    ctx->window_samples = (int)(seconds * EFFECTIVE_RATE);
    ctx->write_pos = 0;
    ctx->downsample_counter = 0;
    ctx->skip_windows = 0;
    ctx->ready_buf.store(-1, std::memory_order_relaxed);
    std::memset(ctx->votes, 0, sizeof(ctx->votes));
    std::strcpy(ctx->detected_key, "no key");
}

float kd_get_window(void *ptr) {
    kd_context *ctx = (kd_context*)ptr;
    if (!ctx) return 2.0f;
    return ctx->window_seconds;
}

} /* extern "C" */
