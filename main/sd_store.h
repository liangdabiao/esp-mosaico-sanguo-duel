// SPDX-License-Identifier: Apache-2.0
// sd_store.h —— 进度的 NVS 落盘（真机实现）与宿主内存桩，语义两端一致。
#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "sd/sd_progress.h"

esp_err_t sd_store_init(void);
esp_err_t sd_store_load(sd_progress_t *progress);
esp_err_t sd_store_save(const sd_progress_t *progress);
esp_err_t sd_store_erase(void);
