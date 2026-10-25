// SPDX-License-Identifier: Apache-2.0
// sd_ai.h —— 启发式 AI（设计文档 §7）：场面评估 + 动作优先级，非搜索。
// AI 与人类走同一指令接口（sd_submit_command 的决策者）。
#pragma once
#include <stdint.h>
#include "sd_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在当前决策点（主指令/响应窗口/选目标）为 player 生成一条指令。
 * 返回 false 表示该决策点不属于 player 或无合理动作（此时应提交放弃/过阶段）。 */
bool sd_ai_decide(sd_duel_t *d, uint8_t player, sd_cmd_t *out);

/* 设定 player 的难度（0 教学随机 / 1 标准 / 2 老练），默认 1。 */
void sd_ai_set_difficulty(uint8_t player, int diff);

#ifdef __cplusplus
}
#endif
