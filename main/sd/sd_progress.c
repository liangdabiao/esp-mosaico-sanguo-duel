// SPDX-License-Identifier: Apache-2.0
// sd_progress.c —— 进度结构的格式校验与复位（纯逻辑，落盘在 sd_store）。
#include "sd_progress.h"
#include "sd_core.h"

void sd_progress_reset(sd_progress_t *p)
{
    if (!p) return;
    p->magic = SD_PROGRESS_MAGIC;
    p->version = SD_PROGRESS_VERSION;
    p->unlocked = 0;
    p->sound_on = 1;
    p->deck_valid = 0;
    for (int i = 0; i < 20; i++) p->deck[i] = SD_DECKS[SD_DECK_SHU_START].ids[i];
    for (int i = 0; i < 3; i++) p->reserved[i] = 0;
}

bool sd_progress_validate(sd_progress_t *p)
{
    if (!p || p->magic != SD_PROGRESS_MAGIC || p->version != SD_PROGRESS_VERSION) return false;
    if (p->unlocked > SD_LEVEL_COUNT) p->unlocked = SD_LEVEL_COUNT;
    if (p->sound_on > 1) p->sound_on = 1;
    if (p->deck_valid > 1) p->deck_valid = 0;
    if (p->deck_valid) {
        for (int i = 0; i < 20; i++) {
            if (p->deck[i] >= SD_CARD_COUNT) {
                p->deck_valid = 0;
                break;
            }
        }
    }
    if (!p->deck_valid)
        for (int i = 0; i < 20; i++) p->deck[i] = SD_DECKS[SD_DECK_SHU_START].ids[i];
    return true;
}
