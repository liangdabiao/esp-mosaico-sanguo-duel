// SPDX-License-Identifier: Apache-2.0
// sd_sfx_host.c —— 宿主模拟器音效桩（只进 game.sim.json）。
#include "sd_sfx.h"

esp_err_t sd_sfx_start(void) { return ESP_OK; }
void sd_sfx_stop(void) {}
void sd_sfx_play(sd_tone_t tone) { (void)tone; }
void sd_sfx_set_enabled(bool on) { (void)on; }
