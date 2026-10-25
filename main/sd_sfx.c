// SPDX-License-Identifier: Apache-2.0
// sd_sfx.c —— 真机音效：ES8311 + rtttl_player（host 用 sd_sfx_host.c 空桩）。
#include "sd_sfx.h"
#include <stddef.h>
#include <stdbool.h>
#include "bsp/audio.h"
#include "esp_codec_dev.h"
#include "rtttl_player.h"

// RTTTL：战鼓用低八度短音，号角上行，胜负各一段短旋律。
static const char *const TONES[SD_TONE_COUNT] = {
    [SD_TONE_TAP] = "tp:d=8,o=5,b=500:g",
    [SD_TONE_DRAW] = "dw:d=8,o=5,b=400:e,g",
    [SD_TONE_SUMMON] = "sm:d=8,o=4,b=300:c,g,c6",
    [SD_TONE_BATTLE] = "bt:d=16,o=3,b=400:c,c,g",
    [SD_TONE_DESTROY] = "ds:d=8,o=4,b=300:g,e",
    [SD_TONE_SPELL] = "sp:d=8,o=5,b=350:e,g,c6",
    [SD_TONE_BACK] = "bk:d=8,o=5,b=300:g,e",
    [SD_TONE_WIN] = "wn:d=8,o=5,b=200:c,e,g,c6,e6",
    [SD_TONE_LOSE] = "ls:d=8,o=4,b=220:e,d,c",
};

#define SD_SFX_BASE_VOLUME 58
static bool s_enabled = true;
static esp_codec_dev_handle_t s_speaker;

esp_err_t sd_sfx_start(void)
{
    s_speaker = bsp_audio_codec_speaker_init();
    if (!s_speaker) return ESP_FAIL;
    (void)esp_codec_dev_set_out_vol(s_speaker, SD_SFX_BASE_VOLUME);
    return rtttl_player_start();
}

void sd_sfx_stop(void)
{
    rtttl_player_stop();
}

void sd_sfx_set_enabled(bool on)
{
    s_enabled = on;
}

void sd_sfx_play(sd_tone_t tone)
{
    if (!s_enabled || tone >= SD_TONE_COUNT) return;
    (void)rtttl_player_play(TONES[tone]);
}
