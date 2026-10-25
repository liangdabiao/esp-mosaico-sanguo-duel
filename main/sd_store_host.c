// SPDX-License-Identifier: Apache-2.0
// sd_store_host.c —— 宿主模拟器存档桩：进程静态内存当"NVS"（只进 game.sim.json）。
#include "sd_store.h"
#include <string.h>

static sd_progress_t s_memory;
static bool s_ready = false;

esp_err_t sd_store_init(void)
{
    if (!s_ready) {
        sd_progress_reset(&s_memory);
        s_ready = true;
    }
    return ESP_OK;
}

esp_err_t sd_store_load(sd_progress_t *progress)
{
    if (!progress) return ESP_FAIL;
    if (!s_ready) {
        sd_progress_reset(&s_memory);
        s_ready = true;
    }
    *progress = s_memory;
    (void)sd_progress_validate(progress);
    return ESP_OK;
}

esp_err_t sd_store_save(const sd_progress_t *progress)
{
    if (!progress) return ESP_FAIL;
    s_memory = *progress;
    s_ready = true;
    return ESP_OK;
}

esp_err_t sd_store_erase(void)
{
    sd_progress_reset(&s_memory);
    s_ready = true;
    return ESP_OK;
}
