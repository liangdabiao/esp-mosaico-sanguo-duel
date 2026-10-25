// SPDX-License-Identifier: Apache-2.0
// sd_session.c —— 会话层实现：进度持有、对局编排（AI 只在决策点行动）、消息队列、组卡器。
#include "sd_session.h"
#include <string.h>
#include "sd_ai.h"
#include "sd_store.h"
#include "sd_text.h"
#include "sd_sfx.h"

static sd_progress_t s_prog;
static sd_page_t s_page = SD_PAGE_MENU;
static sd_duel_t *s_duel;
static uint8_t s_human = 0;      /* 人类玩家方 */
static uint8_t s_level = 0;
static int s_result = 0;

static char s_msgs[SD_MSG_LINES][SD_MSG_LEN];
static int s_msg_head = 0;       /* 下一条写入位置 */
static bool s_msg_full = false;

static uint8_t s_deck[20];
static bool s_deck_dirty = false;

/* ---- 核心日志 → 消息队列 ---- */
static void on_core_log(void *ud, const char *msg)
{
    (void)ud;
    if (!msg || !msg[0]) return;
    strncpy(s_msgs[s_msg_head], msg, SD_MSG_LEN - 1);
    s_msgs[s_msg_head][SD_MSG_LEN - 1] = '\0';
    s_msg_head = (s_msg_head + 1) % SD_MSG_LINES;
    if (s_msg_head == 0) s_msg_full = true;
}

void sd_app_init(void)
{
    sd_store_init();
    if (sd_store_load(&s_prog) != ESP_OK) sd_progress_reset(&s_prog);
    sd_progress_validate(&s_prog);
    sd_set_log(on_core_log, NULL);
    (void)sd_sfx_start();
    sd_sfx_set_enabled(s_prog.sound_on != 0);
}

void sd_app_tick(void)
{
    if (!s_duel || s_duel->game_over) return;
    /* AI 决策点：窗口响应者 / 目标选择者 / 等待主指令的回合玩家，只要不是人类就驱动 */
    for (int guard = 0; guard < 16; guard++) {
        uint8_t who;
        if (s_duel->target.active) who = s_duel->target.chooser;
        else if (s_duel->window.active) who = s_duel->window.responder;
        else if (s_duel->awaiting_command) who = s_duel->turn_player;
        else break;
        if (who == s_human) break; /* 等人类输入 */
        sd_cmd_t c;
        if (!sd_ai_decide(s_duel, who, &c)) break;
        if (!sd_submit_command(s_duel, &c)) {
            /* AI 指令被拒：放弃窗口/推进阶段兜底，避免卡死 */
            sd_cmd_t fallback = {0};
            if (s_duel->window.active) {
                fallback.kind = SD_CMD_RESPONSE;
                fallback.p0 = 255;
            } else if (s_duel->target.active && s_duel->target.cand_n > 0) {
                fallback.kind = SD_CMD_TARGET;
                fallback.p0 = s_duel->target.cand[0];
            } else {
                fallback.kind = SD_CMD_NEXT_PHASE;
            }
            (void)sd_submit_command(s_duel, &fallback);
            break;
        }
    }
}

sd_page_t sd_page(void) { return s_page; }
void sd_page_go(sd_page_t p) { s_page = p; }
sd_progress_t *sd_prog(void) { return &s_prog; }
void sd_prog_save(void) { (void)sd_store_save(&s_prog); }

/* ---- 对局 ---- */
bool sd_duel_active(void) { return s_duel && !s_duel->game_over; }

bool sd_start_level(uint8_t level)
{
    if (level >= SD_LEVEL_COUNT) return false;
    static const int AI_DECKS[SD_LEVEL_COUNT] = { SD_DECK_QINGZHOU, SD_DECK_JIANGDONG, SD_DECK_HULANG };
    s_level = level;
    s_human = 0;
    s_result = 0;
    s_msg_head = 0;
    s_msg_full = false;
    memset(s_msgs, 0, sizeof(s_msgs));
    sd_ai_set_difficulty(0, 1); /* 人类侧难度不影响（人类自己决策），兜底用 */
    sd_ai_set_difficulty(1, SD_DECKS[AI_DECKS[level]].difficulty);
    sd_duel_free(s_duel);
    const uint8_t *player_deck = s_prog.deck_valid ? s_prog.deck : SD_DECKS[SD_DECK_SHU_START].ids;
    s_duel = sd_duel_create(player_deck, SD_DECKS[AI_DECKS[level]].ids, 0,
                            (uint32_t)(0x51A0u + (uint32_t)level * 97u + s_prog.unlocked * 13u));
    if (!s_duel) return false;
    s_page = SD_PAGE_DUEL;
    return true;
}

void sd_concede(void)
{
    if (!s_duel) return;
    s_duel->pl[s_human].lp = 0;
    /* 触发终局判定：借一次直接伤害路径不必要，直接置终局 */
    s_duel->game_over = 1;
    s_duel->winner = (uint8_t)(s_human ^ 1);
}

sd_duel_t *sd_duel(void) { return s_duel; }
uint8_t sd_human_side(void) { return s_human; }

int sd_duel_result(void)
{
    if (!s_duel || !s_duel->game_over) return 0;
    int r = s_duel->winner == s_human ? 1 : 2;
    if (s_result == 0 && r != 0) {
        s_result = r;
        if (r == 1 && s_level + 1 > s_prog.unlocked && s_level + 1 <= SD_LEVEL_COUNT)
            s_prog.unlocked = (uint8_t)(s_level + 1);
        sd_prog_save();
    }
    return s_result;
}

/* ---- 消息 ---- */
const char *sd_msg(int back)
{
    if (!s_msg_full && back >= s_msg_head) return "";
    int idx = (s_msg_head - 1 - back + SD_MSG_LINES * 4) % SD_MSG_LINES;
    return s_msgs[idx];
}

/* ---- 组卡器 ---- */
#define SD_DECK_EMPTY 0xFFu
uint8_t *sd_deck_buf(void)
{
    if (!s_deck_dirty) {
        memcpy(s_deck, s_prog.deck, sizeof(s_deck));
        s_deck_dirty = true;
    }
    return s_deck;
}
void sd_deck_reload(void)
{
    memcpy(s_deck, s_prog.deck, sizeof(s_deck));
    s_deck_dirty = true;
}
int sd_deck_count(int id)
{
    uint8_t *d = sd_deck_buf();
    int n = 0;
    for (int i = 0; i < 20; i++)
        if (d[i] == id) n++;
    return n;
}
bool sd_deck_add(int id)
{
    if (id < 0 || id >= SD_CARD_COUNT) return false;
    uint8_t *d = sd_deck_buf();
    int free_slot = -1;
    int n = 0, same = 0;
    for (int i = 0; i < 20; i++) {
        if (d[i] == SD_DECK_EMPTY) { if (free_slot < 0) free_slot = i; }
        else n++;
        if (d[i] == id) same++;
    }
    if (n >= 20 || same >= 2 || free_slot < 0) return false;
    d[free_slot] = (uint8_t)id;
    return true;
}
bool sd_deck_remove_at(int idx)
{
    uint8_t *d = sd_deck_buf();
    if (idx < 0 || idx >= 20 || d[idx] == SD_DECK_EMPTY) return false;
    d[idx] = SD_DECK_EMPTY;
    /* 压实 */
    int w = 0;
    for (int i = 0; i < 20; i++)
        if (d[i] != SD_DECK_EMPTY) d[w++] = d[i];
    for (int i = w; i < 20; i++) d[i] = SD_DECK_EMPTY;
    return true;
}
bool sd_deck_commit(void)
{
    uint8_t *d = sd_deck_buf();
    for (int i = 0; i < 20; i++)
        if (d[i] == SD_DECK_EMPTY) return false; /* 必须满 20 张 */
    memcpy(s_prog.deck, d, sizeof(s_prog.deck));
    s_prog.deck_valid = 1;
    sd_prog_save();
    return true;
}
