// SPDX-License-Identifier: Apache-2.0
// sd_progress.h —— 应用进度（纯逻辑层）：关卡解锁 / 音效开关 / 玩家卡组。
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_PROGRESS_MAGIC 0x53443175u /* "SD1u" */
#define SD_PROGRESS_VERSION 1

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t unlocked;      /* 已解锁到的关卡（0..SD_LEVEL_COUNT） */
    uint8_t sound_on;
    uint8_t deck_valid;    /* 玩家卡组是否有效（首次开机用预设） */
    uint8_t deck[20];      /* 玩家自组卡组（卡 id） */
    uint8_t reserved[3];
} sd_progress_t;

enum { SD_LEVEL_COUNT = 3 };

void sd_progress_reset(sd_progress_t *p);
/* 校验并修复；返回 false 表示传入内容不可用（已被调用方重置）。 */
bool sd_progress_validate(sd_progress_t *p);

#ifdef __cplusplus
}
#endif
