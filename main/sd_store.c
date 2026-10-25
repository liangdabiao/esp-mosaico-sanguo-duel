// SPDX-License-Identifier: Apache-2.0
// sd_store.c —— 真机 NVS 落盘。只做字节进出 NVS，校验在 sd_progress。
#include "sd_store.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"

#define SD_NS "sanguo"
#define SD_KEY_PROGRESS "progress"

esp_err_t sd_store_init(void)
{
    return nvs_flash_init_partition("nvs");
}

esp_err_t sd_store_load(sd_progress_t *progress)
{
    if (!progress) return ESP_FAIL;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SD_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        sd_progress_reset(progress); /* 首次开机不算失败 */
        return ESP_OK;
    }
    size_t len = sizeof(*progress);
    err = nvs_get_blob(h, SD_KEY_PROGRESS, progress, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*progress) || !sd_progress_validate(progress)) {
        sd_progress_reset(progress);
        return err == ESP_OK ? ESP_OK : err;
    }
    return ESP_OK;
}

esp_err_t sd_store_save(const sd_progress_t *progress)
{
    if (!progress) return ESP_FAIL;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SD_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, SD_KEY_PROGRESS, progress, sizeof(*progress));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t sd_store_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SD_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, SD_KEY_PROGRESS);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
