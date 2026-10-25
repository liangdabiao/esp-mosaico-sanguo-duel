// SPDX-License-Identifier: Apache-2.0
// sd_sfx.h —— 对局音效：RTTTL 合成（战鼓/号角风格短旋律）。
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SD_TONE_TAP = 0,    // 点选
    SD_TONE_DRAW,       // 抽卡
    SD_TONE_SUMMON,     // 上阵（点将）
    SD_TONE_BATTLE,     // 单挑（战鼓）
    SD_TONE_DESTROY,    // 破坏
    SD_TONE_SPELL,      // 计略发动
    SD_TONE_BACK,       // 返回
    SD_TONE_WIN,        // 胜利
    SD_TONE_LOSE,       // 败北
    SD_TONE_COUNT,
} sd_tone_t;

#define SD_TONE_ENTER SD_TONE_TAP /* 语义别名：保存/确认 */

esp_err_t sd_sfx_start(void);
void sd_sfx_stop(void);
void sd_sfx_play(sd_tone_t tone);
void sd_sfx_set_enabled(bool on);

#ifdef __cplusplus
}
#endif
