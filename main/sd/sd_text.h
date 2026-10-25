// SPDX-License-Identifier: Apache-2.0
// sd_text.h —— 卡牌效果描述文本（UI 详情页）；字库生成工具扫描本文件收字。
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 按卡 id 返回效果描述（含普通武将的势力注记）；越界返回空串。 */
const char *sd_card_desc(uint8_t id);

#ifdef __cplusplus
}
#endif
