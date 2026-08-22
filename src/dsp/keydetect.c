/*
 * keydetect.c - Key Detection Audio FX Plugin (v2 API)
 *
 * A transparent audio FX that detects the musical key of audio passing
 * through it using libkeyfinder. Audio is passed through unmodified.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_fx_api_v2.h"
#include "keyfinder_wrapper.h"

static const host_api_v1_t *g_host = NULL;
static audio_fx_api_v2_t g_fx_api_v2;

/* ------------------------------------------------------------------ */
/* Instance                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    void *kd;                   /* keyfinder wrapper context */
    char detected_key[16];      /* cached key string */
    float window;               /* analysis window in seconds */
    char module_dir[512];
} keydetect_instance_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void* v2_create_instance(const char *module_dir, const char *config_json) {
    keydetect_instance_t *inst = (keydetect_instance_t*)calloc(1, sizeof(keydetect_instance_t));
    if (!inst) return NULL;

    if (module_dir) {
        strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    }

    inst->window = 2.0f;
    strcpy(inst->detected_key, "no key");   /* matches key_names[SILENCE] */

    inst->kd = kd_create(MOVE_SAMPLE_RATE);
    if (!inst->kd) {
        free(inst);
        return NULL;
    }

    kd_set_window(inst->kd, inst->window);

    if (g_host && g_host->log) {
        g_host->log("[keydetect] instance created");
    }

    return inst;
}

static void v2_destroy_instance(void *instance) {
    keydetect_instance_t *inst = (keydetect_instance_t*)instance;
    if (!inst) return;

    if (inst->kd) {
        kd_destroy(inst->kd);
    }
    free(inst);
}

/* ------------------------------------------------------------------ */
/* Audio processing                                                    */
/* ------------------------------------------------------------------ */

static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    keydetect_instance_t *inst = (keydetect_instance_t*)instance;
    if (!inst || !inst->kd || !audio_inout || frames <= 0) return;

    /* Feed audio to keyfinder for analysis.
     * We do NOT modify audio_inout — this is a transparent tap. */
    kd_feed(inst->kd, audio_inout, frames);

    /* Cache the detected key */
    kd_get_key(inst->kd, inst->detected_key, sizeof(inst->detected_key));
}

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

static void v2_set_param(void *instance, const char *key, const char *val) {
    keydetect_instance_t *inst = (keydetect_instance_t*)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "window") == 0) {
        float w = (float)atof(val);
        if (w < 1.0f) w = 1.0f;
        if (w > 8.0f) w = 8.0f;
        inst->window = w;
        kd_set_window(inst->kd, w);
    } else if (strcmp(key, "state") == 0) {
        /* Restore from patch — parse window value from JSON.
         * Simple parsing: look for "window": <number> */
        const char *wp = strstr(val, "\"window\":");
        if (wp) {
            wp += 9; /* skip "window": */
            while (*wp == ' ') wp++;
            float w = (float)atof(wp);
            if (w >= 1.0f && w <= 8.0f) {
                inst->window = w;
                kd_set_window(inst->kd, w);
            }
        }
    }
}

static const char *UI_HIERARCHY =
    "{"
        "\"modes\":null,"
        "\"levels\":{"
            "\"root\":{"
                "\"label\":\"KeyDetect\","
                "\"children\":null,"
                "\"knobs\":[\"window\"],"
                "\"params\":["
                    "{\"key\":\"detected_key\",\"label\":\"Key\"},"
                    "{\"key\":\"window\",\"label\":\"Window (s)\"}"
                "]"
            "}"
        "}"
    "}";

/*
 * detected_key is declared as an ENUM even though get_param returns the key
 * NAME rather than an index.
 *
 * It has to be declared at all: without a chain_params entry the Shadow UI
 * cannot know the type and falls back to guessing a 0..1 float, so the
 * detected key was drawn as a knob pointing at nothing.
 *
 * Enum rather than string because of the widget each one selects. A string
 * gets the one-line opaque box (13px of text, about three characters — "A maj"
 * truncates); an enum gets the two-line square, which splits on the space and
 * renders "A" over "MAJ". Both the renderer and the screen reader fall back to
 * the raw value when it is not a valid index, so returning the name keeps
 * working — and the options below stay in key_names order so an index would
 * resolve correctly too.
 *
 * DECLARED read-only. `set_param` ignores it, and "read-only in practice" was
 * fine while an enum could only be nudged one detent at a time and snapped
 * back on the next read. Schwung 1.0 made every enum with options DIVABLE, so
 * the option picker opened on this one and silently discarded whatever was
 * chosen — 25 keys to pick from, none of which does anything.
 *
 * `access: "read"` says it in the contract instead: not turnable, no picker,
 * still refreshed on screen. Older hosts ignore the field entirely (verified
 * against the v0.12.1 parser, both C and JS), so this is safe to ship without
 * a min_host_version bump — it just does nothing until the host understands
 * it.
 */
static const char *CHAIN_PARAMS =
    "["
        "{\"key\":\"window\",\"name\":\"Window\",\"type\":\"float\","
         "\"min\":1,\"max\":8,\"step\":0.5,\"default\":2,\"unit\":\"s\"},"
        "{\"key\":\"detected_key\",\"name\":\"Key\",\"type\":\"enum\","
         "\"access\":\"read\","
         "\"options\":["
            "\"A maj\",\"A min\",\"Bb maj\",\"Bb min\","
            "\"B maj\",\"B min\",\"C maj\",\"C min\","
            "\"Db maj\",\"Db min\",\"D maj\",\"D min\","
            "\"Eb maj\",\"Eb min\",\"E maj\",\"E min\","
            "\"F maj\",\"F min\",\"Gb maj\",\"Gb min\","
            "\"G maj\",\"G min\",\"Ab maj\",\"Ab min\","
            "\"no key\""
         "]}"
    "]";

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    keydetect_instance_t *inst = (keydetect_instance_t*)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;

    if (strcmp(key, "detected_key") == 0) {
        return snprintf(buf, buf_len, "%s", inst->detected_key);
    } else if (strcmp(key, "window") == 0) {
        return snprintf(buf, buf_len, "%.1f", inst->window);
    } else if (strcmp(key, "display_name") == 0) {
        return snprintf(buf, buf_len, "KeyDetect: %s", inst->detected_key);
    } else if (strcmp(key, "ui_hierarchy") == 0) {
        int len = (int)strlen(UI_HIERARCHY);
        if (len < buf_len) {
            strcpy(buf, UI_HIERARCHY);
            return len;
        }
        return -1;
    } else if (strcmp(key, "chain_params") == 0) {
        int len = (int)strlen(CHAIN_PARAMS);
        if (len < buf_len) {
            strcpy(buf, CHAIN_PARAMS);
            return len;
        }
        return -1;
    } else if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len, "{\"window\":%.1f}", inst->window);
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version     = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block   = v2_process_block;
    g_fx_api_v2.set_param       = v2_set_param;
    g_fx_api_v2.get_param       = v2_get_param;
    g_fx_api_v2.on_midi         = NULL;

    return &g_fx_api_v2;
}
