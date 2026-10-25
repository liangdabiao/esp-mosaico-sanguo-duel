// SPDX-License-Identifier: Apache-2.0
// sd_session.h —— 应用会话层：页面导航、进度、对局编排（AI 驱动）、消息队列。
// game.c 只做渲染与触摸转发；所有状态变更走这里。
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sd/sd_core.h"
#include "sd/sd_progress.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SD_PAGE_MENU = 0,
    SD_PAGE_LEVELS,
    SD_PAGE_DUEL,
    SD_PAGE_DECK,
    SD_PAGE_GALLERY,
    SD_PAGE_SETTINGS,
} sd_page_t;

#define SD_MSG_LINES 3
#define SD_MSG_LEN 64

void sd_app_init(void);                  /* 读档 + 音效 */
void sd_app_tick(void);                  /* AI 决策驱动（每帧调用） */

sd_page_t sd_page(void);
void sd_page_go(sd_page_t p);
sd_progress_t *sd_prog(void);
void sd_prog_save(void);

/* ---- 对局 ---- */
bool sd_duel_active(void);
bool sd_start_level(uint8_t level);      /* level 0..2 */
void sd_concede(void);
sd_duel_t *sd_duel(void);
uint8_t sd_human_side(void);             /* 人类玩家方（0/1） */
/* 对局结果：0 进行中 / 1 胜 / 2 负 */
int sd_duel_result(void);
/* 结束帧的结果已入进度（胜利解锁下一关） */

/* ---- 消息队列（核心日志 → 对局界面滚动条） ---- */
const char *sd_msg(int back);            /* back=0 最新，1 次新… */

/* ---- 组卡器（DECK 页临时卡组，保存时写进度） ---- */
uint8_t *sd_deck_buf(void);              /* 20 项临时卡组 */
int sd_deck_count(int id);               /* 临时卡组里某卡张数 */
bool sd_deck_add(int id);                /* 同名≤2、总数<20 */
bool sd_deck_remove_at(int idx);         /* 按槽位移除 */
void sd_deck_reload(void);               /* 从进度重载临时卡组 */
bool sd_deck_commit(void);               /* 校验 20 张后写进度 */

#ifdef __cplusplus
}
#endif
