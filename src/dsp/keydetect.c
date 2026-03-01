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
    strcpy(inst->detected_key, "---");

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

static const char *CHAIN_PARAMS =
    "["
        "{\"key\":\"window\",\"name\":\"Window\",\"type\":\"float\","
         "\"min\":1,\"max\":8,\"step\":0.5,\"default\":2,\"unit\":\"s\"}"
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
