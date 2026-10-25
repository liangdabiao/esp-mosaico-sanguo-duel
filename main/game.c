// SPDX-License-Identifier: Apache-2.0
// game.c —— 《三国点将录》UI 层：页面渲染与触摸交互。
// 状态与规则全部在 sd/（会话层 + 规则核心）；这里只做两件事：
//   update：手势 → sd_submit_command；输入关键状态一律在 update 计算；
//   render：只读绘制（大卡/目标高亮等可见状态在 update 里已备好）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "game_config.h"
#include "mosaico_raylib_fast.h"
#include "sd_cjk_font.h"
#include "sd/sd_session.h"
#include "sd/sd_core.h"
#include "sd/sd_text.h"
#include "sd_sfx.h"

#define NONE_UID 255u

/* ---- 调色 ---- */
static const Color COL_BG = { 24, 20, 16, 255 };
static const Color COL_PANEL = { 245, 222, 179, 255 };
static const Color COL_PANEL_DARK = { 92, 78, 58, 255 };
static const Color COL_TEXT_DARK = { 40, 30, 15, 255 };
static const Color COL_TEXT_LIGHT = { 235, 225, 205, 255 };
static const Color COL_TEXT_DIM = { 160, 150, 130, 255 };
static const Color COL_BUTTON = { 120, 60, 40, 255 };
static const Color COL_BUTTON_HI = { 180, 90, 50, 255 };
static const Color COL_HIGHLIGHT = { 255, 200, 60, 255 };
static const Color COL_LP = { 150, 60, 50, 255 };
static const Color COL_FACTION[4] = {
    { 70, 100, 190, 255 }, /* 魏 蓝 */
    { 70, 160, 80, 255 },  /* 蜀 绿 */
    { 200, 70, 55, 255 },  /* 吴 红 */
    { 120, 120, 125, 255 },/* 群 灰 */
};
static const Color COL_GOLD = { 205, 170, 90, 255 };
static const Color COL_STRAT = { 60, 140, 150, 255 };   /* 计略 青 */
static const Color COL_AMBUSH = { 130, 70, 140, 255 };  /* 伏兵 紫 */

static Color card_color(const sd_card_def_t *c)
{
    if (c->type == SD_CARD_STRAT) return COL_STRAT;
    if (c->type == SD_CARD_AMBUSH) return COL_AMBUSH;
    return COL_FACTION[c->faction];
}

/* ---- 中英混排（CJK 16px 字模 + ASCII 5x7） ---- */
static int utf8_next_cp(const char *s, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0) {
        *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (uint32_t)(u[1] & 0x3F);
        return 2;
    }
    if ((u[0] & 0xF0) == 0xE0) {
        *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) |
              (uint32_t)(u[2] & 0x3F);
        return 3;
    }
    *cp = u[0];
    return 1;
}
static int draw_mixed(const char *line, int x, int y, Color color, int scale)
{
    if (scale < 1) scale = 1;
    const int ascii_px = 16 * scale;
    const int ascii_dy = 3 * scale;
    const char *s = line;
    int pen = x;
    char ascii[32];
    int an = 0;
    while (*s) {
        uint32_t cp = 0;
        const int step = utf8_next_cp(s, &cp);
        if (cp < 0x80) {
            if (an < (int)sizeof(ascii) - 1) ascii[an++] = (char)cp;
            s += step;
            continue;
        }
        if (an > 0) {
            ascii[an] = '\0';
            DrawText(ascii, pen, y + ascii_dy, ascii_px, color);
            pen += MeasureText(ascii, ascii_px);
            an = 0;
        }
        char one[4] = { 0 };
        memcpy(one, s, (size_t)step);
        pen = sd_cjk_draw_scaled(one, pen, y, color, scale);
        s += step;
    }
    if (an > 0) {
        ascii[an] = '\0';
        DrawText(ascii, pen, y + ascii_dy, ascii_px, color);
        pen += MeasureText(ascii, ascii_px);
    }
    return pen;
}
static int width_mixed(const char *line, int scale)
{
    if (!line) return 0;
    if (scale < 1) scale = 1;
    int w = 0;
    const char *s = line;
    char ascii[32];
    int an = 0;
    while (*s) {
        uint32_t cp = 0;
        const int step = utf8_next_cp(s, &cp);
        if (cp < 0x80) {
            if (an < (int)sizeof(ascii) - 1) ascii[an++] = (char)cp;
            s += step;
            continue;
        }
        if (an > 0) {
            ascii[an] = '\0';
            w += MeasureText(ascii, 16 * scale);
            an = 0;
        }
        w += sd_cjk_width_scaled("武", scale);
        s += step;
    }
    if (an > 0) w += MeasureText(ascii, 16 * scale);
    return w;
}

/* 混排自动换行（16px）：把 text 按可用宽度拆成最多 max_lines 行。 */
static void wrap_mixed(const char *text, int max_w, char lines[][128], int max_lines, int *out_n)
{
    int n = 0;
    const char *s = text;
    while (*s && n < max_lines) {
        char *dst = lines[n];
        int dn = 0;
        int w = 0;
        while (*s) {
            uint32_t cp = 0;
            const int step = utf8_next_cp(s, &cp);
            int cw = cp < 0x80 ? MeasureText("X", 16) : sd_cjk_width_scaled("武", 1);
            if (w + cw > max_w && dn > 0) break;
            if (dn + step >= 127) break;
            memcpy(dst + dn, s, (size_t)step);
            dn += step;
            dst[dn] = '\0';
            w += cw;
            s += step;
        }
        dst[dn] = '\0';
        n++;
    }
    *out_n = n;
}

/* ---- 对局页布局（480x480） ---- */
#define ROW_SPELL_H 44
#define ROW_MON_H 106
#define SLOT_W 150
#define SLOT_X(i) (10 + (i) * 158)
#define ENEMY_SPELL_Y 24
#define ENEMY_MON_Y 70
#define MID_Y 178
#define MID_H 74
#define MY_MON_Y 254
#define MY_SPELL_Y 362
#define HAND_Y 410
#define HAND_H 66
#define HAND_STEP 46
#define HAND_W 74

typedef struct { int x, y, w, h; } rct_t;
static rct_t mon_rect(int side_is_me, int slot)
{
    return (rct_t){ SLOT_X(slot), side_is_me ? MY_MON_Y : ENEMY_MON_Y, SLOT_W, ROW_MON_H };
}
static rct_t spell_rect(int side_is_me, int slot)
{
    return (rct_t){ SLOT_X(slot), side_is_me ? MY_SPELL_Y : ENEMY_SPELL_Y, SLOT_W, ROW_SPELL_H };
}
static bool hit(rct_t r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* ---- UI 状态 ---- */
typedef enum { OV_NONE = 0, OV_CARD, OV_GRAVE, OV_CONFIRM, OV_SETTINGS_RESET } overlay_t;

typedef struct {
    game_config_t config;
    game_snapshot_t state;
    bool inited;
    /* 触摸（update 消费） */
    bool prev_down;
    int down_x, down_y;
    bool tap_valid;
    int tap_x, tap_y;
    bool back_requested;
    /* 覆盖层 */
    overlay_t ov;
    int ov_card_id;        /* OV_CARD：卡 id */
    bool ov_from_hand;     /* 卡来源：手牌（带动作）否则只读 */
    int ov_hand_slot;      /* 来源手牌槽 */
    int ov_zone_slot;      /* 来源计略区槽（-1=手牌） */
    /* OV_CONFIRM：单挑确认 */
    int cf_src_slot, cf_dst_slot; /* dst 255=直击 */
    /* 对局交互 */
    int attack_src;        /* 已选攻击者槽，-1 无 */
    int tribute_hand;      /* 正在点兵上阵的手牌槽，-1 无 */
    int tribute_need, tribute_cnt, tribute_slots[2];
    /* 组卡页反馈 */
    char flash[64];
    int flash_tick;
    bool reset_confirm;
    /* 结束回合按钮可用性（update 计算，render 只读） */
    bool end_turn_enabled;
} ui_t;

static inline ui_t *ui(game_handle_t g) { return (ui_t *)g; }

static void ui_reset_interaction(ui_t *u)
{
    u->attack_src = -1;
    u->tribute_hand = -1;
    u->tribute_cnt = 0;
    u->ov = OV_NONE;
}

static void set_flash(ui_t *u, const char *msg)
{
    snprintf(u->flash, sizeof(u->flash), "%s", msg);
    u->flash_tick = u->state.tick;
}

/* ============ 生命周期 ============ */
bool game_create(const game_config_t *config, game_handle_t *ret_handle)
{
    if (!ret_handle) return false;
    *ret_handle = NULL;
    if (!config || !config->width || !config->height) return false;
    game_handle_t g = calloc(1, sizeof(ui_t));
    if (!g) return false;
    ui_t *u = ui(g);
    u->config = *config;
    u->state.paused = false;
    u->attack_src = -1;
    u->tribute_hand = -1;
    u->ov_zone_slot = -1;
    u->inited = false;
    *ret_handle = g;
    return true;
}

void game_delete(game_handle_t g)
{
    free(g);
}

void game_reset(game_handle_t g)
{
    if (!g) return;
    ui_t *u = ui(g);
    u->state = (game_snapshot_t){0};
    ui_reset_interaction(u);
    u->ov = OV_NONE;
}

void game_set_paused(game_handle_t g, bool paused)
{
    if (g) ui(g)->state.paused = paused;
}

void game_request_back(game_handle_t g)
{
    if (g) ui(g)->back_requested = true;
}

void game_set_pointer(game_handle_t g, int32_t x, int32_t y, bool pressed)
{
    if (!g) return;
    ui_t *u = ui(g);
    x = x < 0 ? 0 : (x >= u->config.width ? u->config.width - 1 : x);
    y = y < 0 ? 0 : (y >= u->config.height ? u->config.height - 1 : y);
    u->state.pointer_x = x;
    u->state.pointer_y = y;
    if (pressed && !u->state.pointer_down) {
        u->down_x = x;
        u->down_y = y;
    }
    u->state.pointer_down = pressed;
    /* 抬指视为一次 tap（要求位移很小，避免拖动误触） */
    if (!pressed && u->prev_down) {
        int dx = x - u->down_x, dy = y - u->down_y;
        if (dx * dx + dy * dy <= 24 * 24) {
            u->tap_x = u->down_x;
            u->tap_y = u->down_y;
            u->tap_valid = true;
        }
    }
    u->prev_down = pressed;
}

/* ============ 对局辅助查询（update 用） ============ */
static bool human_can_act(void)
{
    sd_duel_t *d = sd_duel();
    return d && !d->game_over && !d->window.active && !d->target.active &&
           d->awaiting_command && d->turn_player == sd_human_side();
}

static const char *phase_name(int ph)
{
    switch (ph) {
    case SD_PHASE_DRAW: return "抽卡";
    case SD_PHASE_STANDBY: return "准备";
    case SD_PHASE_MAIN1: return "主1";
    case SD_PHASE_BATTLE: return "战斗";
    case SD_PHASE_MAIN2: return "主2";
    case SD_PHASE_END: return "结束";
    default: return "?";
    }
}

/* ============ 触摸处理：对局页 ============ */
static void tap_big_card_action(ui_t *u, int btn); /* 前置 */

static void open_card_overlay(ui_t *u, int id, bool from_hand, int hand_slot, int zone_slot)
{
    u->ov = OV_CARD;
    u->ov_card_id = id;
    u->ov_from_hand = from_hand;
    u->ov_hand_slot = hand_slot;
    u->ov_zone_slot = zone_slot;
}

static bool submit_summon(ui_t *u, int hand_slot, bool down)
{
    sd_cmd_t c = {0};
    c.kind = down ? SD_CMD_SET_MONSTER : SD_CMD_SUMMON;
    c.p0 = (uint8_t)hand_slot;
    c.p2 = u->tribute_need >= 1 ? (uint8_t)u->tribute_slots[0] : 255;
    c.p3 = u->tribute_need >= 2 ? (uint8_t)u->tribute_slots[1] : 255;
    bool ok = sd_submit_command(sd_duel(), &c);
    if (ok) sd_sfx_play(SD_TONE_SUMMON);
    u->tribute_hand = -1;
    u->tribute_cnt = 0;
    return ok;
}

static void duel_tap(ui_t *u)
{
    sd_duel_t *d = sd_duel();
    if (!d) return;
    const uint8_t me = sd_human_side();
    const int x = u->tap_x, y = u->tap_y;

    /* 终局覆盖层 */
    if (d->game_over) {
        rct_t again = { 60, 300, 160, 50 }, back = { 260, 300, 160, 50 };
        if (hit(again, x, y)) {
            sd_sfx_play(SD_TONE_TAP);
            sd_start_level((uint8_t)(sd_prog()->unlocked >= 1 ? sd_prog()->unlocked - 1 : 0));
            ui_reset_interaction(u);
        } else if (hit(back, x, y)) {
            sd_sfx_play(SD_TONE_BACK);
            sd_page_go(SD_PAGE_LEVELS);
            ui_reset_interaction(u);
        }
        return;
    }

    /* 响应窗口（人类响应） */
    if (d->window.active && d->window.responder == me) {
        rct_t use = { 316, MID_Y + 4, 74, 30 }, pass = { 396, MID_Y + 4, 74, 30 };
        if (hit(use, x, y)) {
            sd_cmd_t c = {0};
            c.kind = SD_CMD_RESPONSE;
            c.p0 = d->window.cand[0];
            if (sd_submit_command(d, &c)) sd_sfx_play(SD_TONE_SPELL);
        } else if (hit(pass, x, y)) {
            sd_cmd_t c = {0};
            c.kind = SD_CMD_RESPONSE;
            c.p0 = 255;
            sd_submit_command(d, &c);
        }
        return;
    }

    /* 选目标（人类选择） */
    if (d->target.active && d->target.chooser == me) {
        if (d->target.sel == SD_SEL_INTERNAL_DISCARD) { /* 弃牌：点手牌 */
            for (int i = 0; i < (int)d->pl[me].hand_n; i++) {
                rct_t r = { 4 + i * HAND_STEP, HAND_Y, HAND_W, HAND_H };
                if (hit(r, x, y)) {
                    sd_cmd_t c = {0};
                    c.kind = SD_CMD_TARGET;
                    c.p0 = d->pl[me].hand[i];
                    sd_submit_command(d, &c);
                    return;
                }
            }
            return;
        }
        /* 场上目标：候选高亮，点卡即提交 */
        for (int p = 0; p < 2; p++) {
            for (int s = 0; s < SD_SLOTS; s++) {
                uint8_t uid = d->pl[p].mon[s];
                if (uid == NONE_UID) continue;
                bool cand = false;
                for (int i = 0; i < d->target.cand_n; i++)
                    if (d->target.cand[i] == uid) cand = true;
                if (!cand) continue;
                rct_t r = mon_rect(p == me, s);
                if (hit(r, x, y)) {
                    sd_cmd_t c = {0};
                    c.kind = SD_CMD_TARGET;
                    c.p0 = uid;
                    if (sd_submit_command(d, &c)) sd_sfx_play(SD_TONE_SPELL);
                    return;
                }
            }
        }
        return; /* 目标模式下其他点击不响应 */
    }

    /* 确认单挑对话框 */
    if (u->ov == OV_CONFIRM) {
        rct_t yes = { 60, 330, 160, 50 }, no = { 260, 330, 160, 50 };
        if (hit(yes, x, y)) {
            sd_cmd_t c = {0};
            c.kind = SD_CMD_ATTACK;
            c.p0 = (uint8_t)u->cf_src_slot;
            c.p1 = (uint8_t)u->cf_dst_slot;
            if (sd_submit_command(d, &c)) sd_sfx_play(SD_TONE_BATTLE);
            u->attack_src = -1;
            u->ov = OV_NONE;
        } else if (hit(no, x, y)) {
            u->ov = OV_NONE;
            u->attack_src = -1;
        }
        return;
    }

    /* 大卡覆盖层 */
    if (u->ov == OV_CARD) {
        tap_big_card_action(u, -1); /* 由按钮坐标处理 */
        return;
    }

    /* 墓地查看 */
    if (u->ov == OV_GRAVE) {
        u->ov = OV_NONE;
        return;
    }

    /* 点兵选择模式 */
    if (u->tribute_hand >= 0) {
        for (int s = 0; s < SD_SLOTS; s++) {
            if (d->pl[me].mon[s] == NONE_UID) continue;
            rct_t r = mon_rect(true, s);
            if (hit(r, x, y)) {
                /* 不可重复选同一只 */
                if (u->tribute_cnt >= 1 && u->tribute_slots[0] == s) continue;
                u->tribute_slots[u->tribute_cnt++] = s;
                if (u->tribute_cnt >= u->tribute_need) submit_summon(u, u->tribute_hand, false);
                return;
            }
        }
        /* 点区域外取消 */
        u->tribute_hand = -1;
        u->tribute_cnt = 0;
        return;
    }

    /* 攻击目标选择模式 */
    if (u->attack_src >= 0) {
        /* 直击按钮（敌空场时中带显示） */
        if (sd_can_direct_attack(d, me)) {
            rct_t db = { 316, MID_Y + 4, 154, 30 };
            if (hit(db, x, y)) {
                u->cf_src_slot = u->attack_src;
                u->cf_dst_slot = 255;
                u->ov = OV_CONFIRM;
                sd_sfx_play(SD_TONE_TAP);
                return;
            }
        }
        for (int s = 0; s < SD_SLOTS; s++) {
            if (d->pl[me ^ 1].mon[s] == NONE_UID) continue;
            rct_t r = mon_rect(false, s);
            if (hit(r, x, y)) {
                u->cf_src_slot = u->attack_src;
                u->cf_dst_slot = s;
                u->ov = OV_CONFIRM;
                return;
            }
        }
        /* 点自己其他出击武将 → 换攻击者 */
        for (int s = 0; s < SD_SLOTS; s++) {
            uint8_t uid = d->pl[me].mon[s];
            if (uid == NONE_UID || d->insts[uid].face_down) continue;
            rct_t r = mon_rect(true, s);
            if (hit(r, x, y) && sd_can_attack(d, me, (uint8_t)s, NULL)) {
                u->attack_src = s;
                return;
            }
        }
        u->attack_src = -1; /* 取消 */
        return;
    }

    /* 顶部：查看墓地 */
    {
        rct_t g0 = { 380, 0, 100, 20 };
        if (hit(g0, x, y)) {
            u->ov = OV_GRAVE;
            return;
        }
    }

    /* 结束回合按钮 */
    {
        rct_t b = { 366, MID_Y + 8, 104, 30 };
        if (hit(b, x, y) && u->end_turn_enabled) {
            sd_cmd_t c = {0};
            c.kind = SD_CMD_NEXT_PHASE;
            sd_submit_command(d, &c);
            sd_sfx_play(SD_TONE_TAP);
            return;
        }
    }

    /* 我方手牌 */
    if (y >= HAND_Y) {
        /* 从右往左命中（重叠时上层优先） */
        for (int i = (int)d->pl[me].hand_n - 1; i >= 0; i--) {
            rct_t r = { 4 + i * HAND_STEP, HAND_Y, HAND_W, HAND_H };
            if (hit(r, x, y)) {
                sd_sfx_play(SD_TONE_TAP);
                open_card_overlay(u, d->insts[d->pl[me].hand[i]].def_id, true, i, -1);
                return;
            }
        }
        return;
    }

    /* 我方计略区 */
    for (int s = 0; s < SD_SLOTS; s++) {
        uint8_t uid = d->pl[me].spell[s];
        if (uid == NONE_UID) continue;
        if (hit(spell_rect(true, s), x, y)) {
            open_card_overlay(u, d->insts[uid].def_id, false, -1, s);
            return;
        }
    }

    /* 我方武将：主阶段点布阵武将 → 菜单（变出击）；战斗阶段 → 攻击选择 */
    for (int s = 0; s < SD_SLOTS; s++) {
        uint8_t uid = d->pl[me].mon[s];
        if (uid == NONE_UID) continue;
        if (!hit(mon_rect(true, s), x, y)) continue;
        if (d->insts[uid].face_down && human_can_act() &&
            sd_can_change_pos(d, me, (uint8_t)s, NULL)) {
            sd_cmd_t c = {0};
            c.kind = SD_CMD_CHANGE_POS;
            c.p0 = (uint8_t)s;
            if (sd_submit_command(d, &c)) sd_sfx_play(SD_TONE_TAP);
            return;
        }
        if (d->phase == SD_PHASE_BATTLE && human_can_act() && !d->insts[uid].face_down &&
            sd_can_attack(d, me, (uint8_t)s, NULL)) {
            u->attack_src = s;
            sd_sfx_play(SD_TONE_TAP);
            return;
        }
        /* 查看大卡 */
        open_card_overlay(u, d->insts[uid].def_id, false, -1, -1);
        return;
    }

    /* 敌方武将（查看大卡：布阵的不给看） */
    for (int s = 0; s < SD_SLOTS; s++) {
        uint8_t uid = d->pl[me ^ 1].mon[s];
        if (uid == NONE_UID) continue;
        if (hit(mon_rect(false, s), x, y)) {
            if (!d->insts[uid].face_down) open_card_overlay(u, d->insts[uid].def_id, false, -1, -1);
            return;
        }
    }
}

/* 大卡覆盖层按钮（坐标在 render 与这里一致） */
static void tap_big_card_action(ui_t *u, int btn)
{
    (void)btn;
    sd_duel_t *d = sd_duel();
    const int x = u->tap_x, y = u->tap_y;
    const uint8_t me = sd_human_side();
    rct_t cancel = { 340, 420, 120, 46 };
    if (hit(cancel, x, y)) {
        u->ov = OV_NONE;
        return;
    }
    if (!u->ov_from_hand || !human_can_act()) return;
    const sd_card_def_t *c = &SD_CARDS[u->ov_card_id];
    int hand_slot = u->ov_hand_slot;

    if (sd_is_monster(c)) {
        rct_t b1 = { 20, 420, 150, 46 }, b2 = { 180, 420, 150, 46 };
        if (hit(b1, x, y)) { /* 上阵（出击） */
            int e;
            if (!sd_can_summon(d, me, (uint8_t)hand_slot, false, &e)) {
                set_flash(u, "现在不能上阵");
                return;
            }
            int need = sd_tribute_count(d, me, c);
            if (need == 0) {
                u->tribute_need = 0;
                submit_summon(u, hand_slot, false);
                u->ov = OV_NONE;
            } else {
                u->tribute_need = need;
                u->tribute_cnt = 0;
                u->tribute_hand = hand_slot;
                u->ov = OV_NONE; /* 进入场上点兵模式 */
            }
        } else if (hit(b2, x, y)) { /* 布阵 */
            int e;
            if (!sd_can_summon(d, me, (uint8_t)hand_slot, true, &e)) {
                set_flash(u, "现在不能布阵");
                return;
            }
            int need = sd_tribute_count(d, me, c);
            if (need == 0) {
                u->tribute_need = 0;
                submit_summon(u, hand_slot, true);
                u->ov = OV_NONE;
            } else {
                u->tribute_need = need;
                u->tribute_cnt = 0;
                u->tribute_hand = hand_slot;
                u->ov = OV_NONE;
            }
        }
        return;
    }

    /* 计略/伏兵 */
    rct_t b1 = { 20, 420, 150, 46 }, b2 = { 180, 420, 150, 46 };
    if (hit(b1, x, y)) { /* 发动 */
        if (!sd_can_activate(d, me, (uint8_t)hand_slot, false, NULL)) {
            set_flash(u, c->type == SD_CARD_AMBUSH ? "伏兵需先盖放" : "现在不能发动");
            return;
        }
        sd_cmd_t cmd = {0};
        cmd.kind = SD_CMD_ACTIVATE;
        cmd.p0 = (uint8_t)hand_slot;
        if (sd_submit_command(d, &cmd)) sd_sfx_play(SD_TONE_SPELL);
        u->ov = OV_NONE;
    } else if (hit(b2, x, y)) { /* 盖放 */
        if (!sd_can_set_spell(d, me, (uint8_t)hand_slot, NULL)) {
            set_flash(u, "计略区已满");
            return;
        }
        sd_cmd_t cmd = {0};
        cmd.kind = SD_CMD_SET_SPELL;
        cmd.p0 = (uint8_t)hand_slot;
        if (sd_submit_command(d, &cmd)) sd_sfx_play(SD_TONE_TAP);
        u->ov = OV_NONE;
    }
}

/* ============ 触摸处理：其他页面 ============ */
static void menu_tap(ui_t *u)
{
    int x = u->tap_x, y = u->tap_y;
    rct_t bs[4] = {
        { 120, 150, 240, 54 }, /* 出征 */
        { 120, 216, 240, 54 }, /* 点将 */
        { 120, 282, 240, 54 }, /* 图鉴 */
        { 120, 348, 240, 54 }, /* 设置 */
    };
    sd_page_t pages[4] = { SD_PAGE_LEVELS, SD_PAGE_DECK, SD_PAGE_GALLERY, SD_PAGE_SETTINGS };
    for (int i = 0; i < 4; i++) {
        if (hit(bs[i], x, y)) {
            sd_sfx_play(SD_TONE_TAP);
            if (pages[i] == SD_PAGE_DECK) sd_deck_reload();
            sd_page_go(pages[i]);
            return;
        }
    }
}

static void levels_tap(ui_t *u)
{
    int x = u->tap_x, y = u->tap_y;
    rct_t back = { 8, 8, 88, 36 };
    if (hit(back, x, y)) {
        sd_sfx_play(SD_TONE_BACK);
        sd_page_go(SD_PAGE_MENU);
        return;
    }
    for (int i = 0; i < SD_LEVEL_COUNT; i++) {
        rct_t r = { 20, 70 + i * 128, 440, 116 };
        if (hit(r, x, y)) {
            if (i > sd_prog()->unlocked) {
                set_flash(u, "先通关前一军团");
                return;
            }
            sd_sfx_play(SD_TONE_ENTER);
            sd_start_level((uint8_t)i);
            ui_reset_interaction(u);
            return;
        }
    }
}

static void deck_tap(ui_t *u)
{
    int x = u->tap_x, y = u->tap_y;
    rct_t back = { 8, 8, 88, 36 };
    if (hit(back, x, y)) {
        sd_sfx_play(SD_TONE_BACK);
        sd_page_go(SD_PAGE_MENU);
        return;
    }
    /* 卡池网格 5 行 × 9 列 */
    for (int i = 0; i < SD_CARD_COUNT; i++) {
        int row = i / 9, col = i % 9;
        rct_t r = { 8 + col * 52, 56 + row * 50, 48, 46 };
        if (hit(r, x, y)) {
            if (!sd_deck_add(i)) set_flash(u, sd_deck_count(i) >= 2 ? "同名最多2张" : "卡组已满");
            else sd_sfx_play(SD_TONE_TAP);
            return;
        }
    }
    /* 卡组槽 2 行 × 10：点击移除 */
    for (int i = 0; i < 20; i++) {
        int row = i / 10, col = i % 10;
        rct_t r = { 8 + col * 47, 316 + row * 28, 44, 26 };
        if (hit(r, x, y)) {
            if (sd_deck_remove_at(i)) sd_sfx_play(SD_TONE_BACK);
            return;
        }
    }
    rct_t save = { 8, 384, 140, 44 }, reset = { 168, 384, 140, 44 };
    if (hit(save, x, y)) {
        if (sd_deck_commit()) { set_flash(u, "卡组已保存"); sd_sfx_play(SD_TONE_ENTER); }
        else set_flash(u, "卡组必须满20张");
        return;
    }
    if (hit(reset, x, y)) {
        sd_deck_reload();
        set_flash(u, "已还原");
        return;
    }
}

static void gallery_tap(ui_t *u)
{
    int x = u->tap_x, y = u->tap_y;
    rct_t back = { 8, 8, 88, 36 };
    if (hit(back, x, y)) {
        sd_sfx_play(SD_TONE_BACK);
        sd_page_go(SD_PAGE_MENU);
        return;
    }
    for (int i = 0; i < SD_CARD_COUNT; i++) {
        int row = i / 9, col = i % 9;
        rct_t r = { 8 + col * 52, 56 + row * 62, 48, 58 };
        if (hit(r, x, y)) {
            open_card_overlay(u, i, false, -1, -1);
            u->ov_from_hand = false;
            sd_sfx_play(SD_TONE_TAP);
            return;
        }
    }
}

static void settings_tap(ui_t *u)
{
    int x = u->tap_x, y = u->tap_y;
    rct_t back = { 8, 8, 88, 36 };
    rct_t sound = { 100, 120, 280, 50 };
    rct_t reset = { 100, 190, 280, 50 };
    if (hit(back, x, y)) {
        sd_sfx_play(SD_TONE_BACK);
        sd_page_go(SD_PAGE_MENU);
        return;
    }
    if (hit(sound, x, y)) {
        sd_prog()->sound_on = (uint8_t)(sd_prog()->sound_on ? 0 : 1);
        sd_sfx_set_enabled(sd_prog()->sound_on != 0);
        sd_prog_save();
        sd_sfx_play(SD_TONE_TAP);
        return;
    }
    if (hit(reset, x, y)) {
        if (!u->reset_confirm) {
            u->reset_confirm = true;
            return;
        }
        sd_progress_reset(sd_prog());
        sd_prog_save();
        u->reset_confirm = false;
        sd_sfx_play(SD_TONE_BACK);
        return;
    }
    u->reset_confirm = false;
}

/* ============ update ============ */
static void handle_back(ui_t *u)
{
    if (!u->back_requested) return;
    u->back_requested = false;
    sd_page_t p = sd_page();
    if (p == SD_PAGE_DUEL) {
        if (u->ov != OV_NONE) u->ov = OV_NONE;
        else if (u->attack_src >= 0) u->attack_src = -1;
        else if (u->tribute_hand >= 0) { u->tribute_hand = -1; u->tribute_cnt = 0; }
        else sd_page_go(SD_PAGE_LEVELS);
    } else if (p != SD_PAGE_MENU) {
        sd_page_go(SD_PAGE_MENU);
    }
}

void game_update(game_handle_t g)
{
    if (!g) return;
    ui_t *u = ui(g);
    if (u->state.paused) return;
    ++u->state.tick;
    if (!u->inited) {
        sd_app_init();
        u->inited = true;
    }
    handle_back(u);
    sd_app_tick(); /* AI 驱动 */

    /* 结束回合按钮可用性（update 计算） */
    sd_duel_t *d = sd_duel();
    u->end_turn_enabled = d && human_can_act() &&
                          (d->phase == SD_PHASE_MAIN1 || d->phase == SD_PHASE_BATTLE ||
                           d->phase == SD_PHASE_MAIN2) &&
                          u->ov == OV_NONE;

    /* tap 消费 */
    if (u->tap_valid) {
        u->tap_valid = false;
        switch (sd_page()) {
        case SD_PAGE_MENU: menu_tap(u); break;
        case SD_PAGE_LEVELS: levels_tap(u); break;
        case SD_PAGE_DUEL: duel_tap(u); break;
        case SD_PAGE_DECK: deck_tap(u); break;
        case SD_PAGE_GALLERY: gallery_tap(u); break;
        case SD_PAGE_SETTINGS: settings_tap(u); break;
        }
    }
}

game_snapshot_t game_read(game_handle_t g)
{
    return g ? ui(g)->state : (game_snapshot_t){0};
}

/* ============ 渲染 ============ */
static void draw_button(rct_t r, const char *label, bool enabled, bool danger)
{
    Color bg = enabled ? (danger ? (Color){ 170, 60, 40, 255 } : COL_BUTTON) : (Color){ 70, 62, 52, 255 };
    DrawRectangle(r.x, r.y, r.w, r.h, bg);
    DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h }, 2,
                         enabled ? COL_GOLD : COL_TEXT_DIM);
    int tw = width_mixed(label, 1);
    draw_mixed(label, r.x + (r.w - tw) / 2, r.y + (r.h - 18) / 2, COL_TEXT_LIGHT, 1);
}

/* 场上一张卡（武将或计略） */
static void draw_field_card(rct_t r, uint8_t uid, bool mine, bool highlight)
{
    (void)mine;
    sd_duel_t *d = sd_duel();
    sd_inst_t *inst = &d->insts[uid];
    const sd_card_def_t *c = &SD_CARDS[inst->def_id];
    Color cc = card_color(c);
    Color face = inst->face_down ? (Color){ c->type == SD_CARD_STRAT ? 40 : 52,
                                         c->type == SD_CARD_STRAT ? 70 : 44,
                                         c->type == SD_CARD_AMBUSH ? 70 : 38, 255 }
                                 : COL_PANEL;
    DrawRectangle(r.x, r.y, r.w, r.h, face);
    DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h },
                         highlight ? 3 : 1, highlight ? COL_HIGHLIGHT : cc);
    if (inst->face_down) {
        const char *tag = sd_is_monster(c) ? "布阵" : (c->type == SD_CARD_AMBUSH ? "伏兵" : "计略");
        draw_mixed(tag, r.x + r.w / 2 - sd_cjk_width_scaled(tag, 1) / 2,
                   r.y + r.h / 2 - 10, COL_TEXT_DIM, 1);
        return;
    }
    /* 卡名（最多 4 字） */
    char name[5 * 4 + 1];
    snprintf(name, sizeof(name), "%.12s", c->name); /* 4 个 UTF-8 汉字=12 字节 */
    draw_mixed(name, r.x + 4, r.y + 3, COL_TEXT_DARK, 1);
    if (sd_is_monster(c)) {
        char stars[8];
        snprintf(stars, sizeof(stars), "★%d", c->tier);
        draw_mixed(stars, r.x + 4, r.y + 22, COL_GOLD, 1);
        char st[32];
        snprintf(st, sizeof(st), "W%04d D%04d", sd_eff_atk(d, uid), sd_eff_def(d, uid));
        DrawText(st, r.x + 4, r.y + r.h - 22, 16, COL_TEXT_DARK);
        if (inst->seal_turns > 0) draw_mixed("封", r.x + r.w - 22, r.y + 3, (Color){200,40,40,255}, 1);
        if (inst->multi) draw_mixed("连", r.x + r.w - 22, r.y + 22, COL_STRAT, 1);
    } else {
        const char *kind = "";
        if (c->type == SD_CARD_STRAT)
            kind = c->strat_kind == SD_SK_QUICK ? "速攻" : c->strat_kind == SD_SK_EQUIP ? "装备" : c->strat_kind == SD_SK_CONT ? "永续" : "通常";
        else
            kind = c->ambush_kind == SD_AK_COUNTER ? "反击" : c->ambush_kind == SD_AK_CONT ? "永续" : "伏兵";
        draw_mixed(kind, r.x + 4, r.y + 22, COL_TEXT_DARK, 1);
    }
}

static void draw_hand_card(rct_t r, uint8_t uid)
{
    sd_duel_t *d = sd_duel();
    const sd_card_def_t *c = &SD_CARDS[d->insts[uid].def_id];
    Color cc = card_color(c);
    DrawRectangle(r.x, r.y, r.w, r.h, COL_PANEL);
    DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h }, 2, cc);
    DrawRectangle(r.x + 2, r.y + 20, r.w - 4, 14, cc);
    char stars[8];
    if (sd_is_monster(c)) snprintf(stars, sizeof(stars), "★%d", c->tier);
    else snprintf(stars, sizeof(stars), "%s", c->type == SD_CARD_STRAT ? "计" : "伏");
    draw_mixed(stars, r.x + 4, r.y + 2, sd_is_monster(c) ? COL_GOLD : COL_STRAT, 1);
    char name[5 * 4 + 1];
    snprintf(name, sizeof(name), "%.12s", c->name);
    draw_mixed(name, r.x + 3, r.y + r.h - 28, COL_TEXT_DARK, 1);
}

/* 大卡覆盖层 */
static void render_card_overlay(ui_t *u)
{
    sd_duel_t *d = sd_duel();
    const sd_card_def_t *c = &SD_CARDS[u->ov_card_id];
    Color cc = card_color(c);
    DrawRectangle(0, 0, GAME_WIDTH, GAME_HEIGHT, (Color){ 0, 0, 0, 190 });
    rct_t p = { 70, 20, 340, 388 };
    DrawRectangle(p.x, p.y, p.w, p.h, COL_PANEL);
    DrawRectangleLinesEx((Rectangle){ (float)p.x, (float)p.y, (float)p.w, (float)p.h }, 4, cc);
    draw_mixed(c->name, p.x + 16, p.y + 12, COL_TEXT_DARK, 2);
    char line[64];
    if (sd_is_monster(c)) {
        snprintf(line, sizeof(line), "★%d ", c->tier);
        draw_mixed(line, p.x + 16, p.y + 56, COL_GOLD, 1);
        static const char *FA = "魏蜀吴群";
        char f[2] = { FA[c->faction], 0 };
        draw_mixed(f, p.x + 90, p.y + 56, COL_FACTION[c->faction], 1);
        static const char *UN = "骑枪弓卫刀师";
        char un[2] = { UN[c->unit], 0 };
        draw_mixed(un, p.x + 130, p.y + 56, COL_TEXT_DARK, 1);
        snprintf(line, sizeof(line), "W%04d D%04d", c->atk, c->def);
        DrawText(line, p.x + 180, p.y + 58, 18, COL_TEXT_DARK);
    } else {
        const char *kind = c->type == SD_CARD_STRAT
                               ? (c->strat_kind == SD_SK_QUICK ? "速攻计略"
                                  : c->strat_kind == SD_SK_EQUIP ? "装备计略"
                                  : c->strat_kind == SD_SK_CONT ? "永续计略" : "通常计略")
                               : (c->ambush_kind == SD_AK_COUNTER ? "反击伏兵"
                                  : c->ambush_kind == SD_AK_CONT ? "永续伏兵" : "通常伏兵");
        draw_mixed(kind, p.x + 16, p.y + 56, cc, 1);
    }
    /* 效果描述（自动换行） */
    char lines[6][128];
    int n;
    wrap_mixed(sd_card_desc(u->ov_card_id), p.w - 32, lines, 6, &n);
    for (int i = 0; i < n; i++)
        draw_mixed(lines[i], p.x + 16, p.y + 90 + i * 22, COL_TEXT_DARK, 1);
    /* 动作按钮（手牌且轮到人类） */
    if (u->ov_from_hand) {
        if (sd_is_monster(c)) {
            bool can = human_can_act();
            draw_button((rct_t){ 20, 420, 150, 46 }, "上阵", can, false);
            draw_button((rct_t){ 180, 420, 150, 46 }, "布阵", can, false);
        } else {
            bool can = human_can_act();
            draw_button((rct_t){ 20, 420, 150, 46 }, "发动", can, false);
            draw_button((rct_t){ 180, 420, 150, 46 }, "盖放", can, false);
        }
    }
    draw_button((rct_t){ 340, 420, 120, 46 }, "取消", true, false);
    (void)d;
}

static bool is_target_candidate(sd_duel_t *d, uint8_t uid)
{
    if (!d->target.active) return false;
    for (int i = 0; i < d->target.cand_n; i++)
        if (d->target.cand[i] == uid) return true;
    return false;
}

static void render_duel(ui_t *u)
{
    sd_duel_t *d = sd_duel();
    if (!d) { draw_mixed("无对局", 200, 200, COL_TEXT_LIGHT, 2); return; }
    const uint8_t me = sd_human_side(), en = (uint8_t)(me ^ 1);
    bool targeting = d->target.active && d->target.chooser == me && d->target.sel != SD_SEL_INTERNAL_DISCARD;
    bool discarding = d->target.active && d->target.chooser == me && d->target.sel == SD_SEL_INTERNAL_DISCARD;

    /* 敌方状态条 */
    char buf[48];
    snprintf(buf, sizeof(buf), "敌方 LP%04d deck %d grave %d hand %d",
             d->pl[en].lp, d->pl[en].deck_n, d->pl[en].grave_n, d->pl[en].hand_n);
    DrawRectangle(0, 0, GAME_WIDTH, 20, COL_PANEL_DARK);
    DrawText(buf, 8, 3, 16, COL_TEXT_LIGHT);
    /* 墓地热区提示 */
    DrawRectangleLinesEx((Rectangle){ 380, 0, 100, 20 }, 1, COL_TEXT_DIM);

    /* 敌方计略/武将 */
    for (int s = 0; s < SD_SLOTS; s++) {
        uint8_t su = d->pl[en].spell[s];
        rct_t r = spell_rect(false, s);
        DrawRectangle(r.x, r.y, r.w, r.h, (Color){ 30, 26, 22, 255 });
        DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h }, 1, COL_PANEL_DARK);
        if (su != NONE_UID) draw_field_card(r, su, false, false);
        uint8_t mu = d->pl[en].mon[s];
        rct_t rm = mon_rect(false, s);
        bool hi = u->attack_src >= 0 || (mu != NONE_UID && is_target_candidate(d, mu));
        DrawRectangle(rm.x, rm.y, rm.w, rm.h, (Color){ 30, 26, 22, 255 });
        if (mu != NONE_UID) draw_field_card(rm, mu, false, hi);
        else DrawRectangleLinesEx((Rectangle){ (float)rm.x, (float)rm.y, (float)rm.w, (float)rm.h }, 1, COL_PANEL_DARK);
    }

    /* 中带：消息 + 窗口 + 结束回合 + 我方 LP */
    DrawRectangle(0, MID_Y, GAME_WIDTH, MID_H, COL_PANEL_DARK);
    if (d->window.active && d->window.responder == me) {
        char msg[80];
        snprintf(msg, sizeof(msg), "是否发动【%s】？", SD_CARDS[d->insts[d->window.cand[0]].def_id].name);
        draw_mixed(msg, 8, MID_Y + 6, COL_HIGHLIGHT, 1);
        draw_button((rct_t){ 316, MID_Y + 4, 74, 30 }, "发动", true, false);
        draw_button((rct_t){ 396, MID_Y + 4, 74, 30 }, "放弃", true, false);
    } else if (targeting) {
        draw_mixed("请点选高亮目标", 8, MID_Y + 6, COL_HIGHLIGHT, 1);
    } else if (discarding) {
        draw_mixed("超限弃牌：点选手牌", 8, MID_Y + 6, COL_HIGHLIGHT, 1);
    } else if (u->tribute_hand >= 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "点兵：选%d名武将 (%d/%d)", u->tribute_need, u->tribute_cnt, u->tribute_need);
        draw_mixed(msg, 8, MID_Y + 6, COL_HIGHLIGHT, 1);
    } else if (u->attack_src >= 0) {
        draw_mixed("选择攻击目标（点空处取消）", 8, MID_Y + 6, COL_HIGHLIGHT, 1);
        if (sd_can_direct_attack(d, me)) {
            draw_mixed("敌空场：点敌阵区=直击", 8, MID_Y + 26, COL_TEXT_LIGHT, 1);
            /* 直击按钮 */
            rct_t db = { 316, MID_Y + 4, 154, 30 };
            draw_button(db, "直击!", true, true);
            int x = u->tap_x, y = u->tap_y;
            (void)x; (void)y;
        }
    } else {
        for (int i = 0; i < 2; i++) { /* 只放两行，给 LP 条让位 */
            const char *m = sd_msg(1 - i);
            if (m && m[0]) draw_mixed(m, 8, MID_Y + 4 + i * 20, COL_TEXT_DIM, 1);
        }
    }
    /* 我方 LP + 阶段 */
    snprintf(buf, sizeof(buf), "我方 LP%04d", d->pl[me].lp);
    DrawText(buf, 8, MID_Y + MID_H - 24, 16, COL_TEXT_LIGHT);
    DrawRectangle(120, MID_Y + MID_H - 21, 220, 14, (Color){ 40, 34, 28, 255 });
    int lpw = (int)(220.0f * (float)d->pl[me].lp / (float)SD_LP_INIT);
    if (lpw > 220) lpw = 220;
    DrawRectangle(120, MID_Y + MID_H - 21, lpw, 14, COL_LP);
    char ph[40];
    snprintf(ph, sizeof(ph), "%s 回合%d", phase_name(d->phase), (d->global_turn + 1) / 2);
    draw_mixed(ph, 236, MID_Y + MID_H - 24, COL_TEXT_LIGHT, 1);
    draw_button((rct_t){ 366, MID_Y + 8, 104, 30 }, "结束回合", u->end_turn_enabled, false);

    /* 我方场地 */
    for (int s = 0; s < SD_SLOTS; s++) {
        uint8_t su = d->pl[me].spell[s];
        rct_t r = spell_rect(true, s);
        DrawRectangle(r.x, r.y, r.w, r.h, (Color){ 30, 26, 22, 255 });
        DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h }, 1, COL_PANEL_DARK);
        if (su != NONE_UID) draw_field_card(r, su, true, false);
        uint8_t mu = d->pl[me].mon[s];
        rct_t rm = mon_rect(true, s);
        bool hi = u->tribute_hand >= 0 || (mu != NONE_UID && is_target_candidate(d, mu));
        DrawRectangle(rm.x, rm.y, rm.w, rm.h, (Color){ 30, 26, 22, 255 });
        if (mu != NONE_UID) draw_field_card(rm, mu, true, hi);
        else DrawRectangleLinesEx((Rectangle){ (float)rm.x, (float)rm.y, (float)rm.w, (float)rm.h }, 1, COL_PANEL_DARK);
    }

    /* 手牌（交错重叠，右侧在上） */
    for (int i = 0; i < (int)d->pl[me].hand_n; i++) {
        rct_t r = { 4 + i * HAND_STEP, HAND_Y, HAND_W, HAND_H };
        draw_hand_card(r, d->pl[me].hand[i]);
        if (discarding) {
            bool cand = false;
            for (int k = 0; k < d->target.cand_n; k++)
                if (d->target.cand[k] == d->pl[me].hand[i]) cand = true;
            if (cand) DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h }, 3, COL_HIGHLIGHT);
        }
    }

    /* 覆盖层 */
    if (u->ov == OV_CARD) render_card_overlay(u);
    else if (u->ov == OV_GRAVE) {
        DrawRectangle(0, 0, GAME_WIDTH, GAME_HEIGHT, (Color){ 0, 0, 0, 200 });
        draw_mixed("墓 地（点任意处关闭）", 60, 24, COL_TEXT_LIGHT, 1);
        uint8_t gside = me; /* 我方墓地 */
        int n = d->pl[gside].grave_n;
        for (int i = 0; i < n && i < 42; i++) {
            int col = i % 6, row = i / 6;
            uint8_t uid = d->pl[gside].grave[i];
            rct_t r = { 8 + col * 78, 60 + row * 50, 74, 44 };
            DrawRectangle(r.x, r.y, r.w, r.h, COL_PANEL);
            const sd_card_def_t *c = &SD_CARDS[d->insts[uid].def_id];
            DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h }, 1, card_color(c));
            char nm[13];
            snprintf(nm, sizeof(nm), "%.9s", c->name);
            draw_mixed(nm, r.x + 3, r.y + 3, COL_TEXT_DARK, 1);
        }
        (void)gside;
    } else if (u->ov == OV_CONFIRM) {
        uint8_t au = d->pl[me].mon[u->cf_src_slot];
        sd_battle_preview_t pv;
        bool direct = u->cf_dst_slot == 255 || d->pl[en].mon[u->cf_dst_slot] == NONE_UID;
        sd_preview_battle(d, au, direct ? au : d->pl[en].mon[u->cf_dst_slot], direct, &pv);
        DrawRectangle(0, 0, GAME_WIDTH, GAME_HEIGHT, (Color){ 0, 0, 0, 190 });
        DrawRectangle(40, 140, 400, 160, COL_PANEL);
        DrawRectangleLinesEx((Rectangle){ 40, 140, 400, 160 }, 3, COL_GOLD);
        char l1[64], l2[64];
        if (direct) {
            snprintf(l1, sizeof(l1), "直击！预期伤害 %d", pv.dmg_defender_side);
            draw_mixed(l1, 60, 160, COL_TEXT_DARK, 1);
        } else {
            uint8_t du = d->pl[en].mon[u->cf_dst_slot];
            snprintf(l1, sizeof(l1), "W%d 对 %s", sd_eff_atk(d, au), SD_CARDS[d->insts[du].def_id].name);
            draw_mixed(l1, 60, 160, COL_TEXT_DARK, 1);
            snprintf(l2, sizeof(l2), "敌损%d 我损%d 敌破%d 我破%d",
                     pv.dmg_defender_side, pv.dmg_attacker_side,
                     pv.defender_destroyed ? 1 : 0, pv.attacker_destroyed ? 1 : 0);
            draw_mixed(l2, 60, 190, COL_TEXT_DARK, 1);
        }
        draw_mixed("伏兵可能反制，仍要单挑吗？", 60, 230, COL_TEXT_DARK, 1);
        draw_button((rct_t){ 60, 330, 160, 50 }, "单挑!", true, true);
        draw_button((rct_t){ 260, 330, 160, 50 }, "取消", true, false);
    }

    /* 终局 */
    if (d->game_over) {
        DrawRectangle(0, 0, GAME_WIDTH, GAME_HEIGHT, (Color){ 0, 0, 0, 210 });
        int res = sd_duel_result();
        draw_mixed(res == 1 ? "胜 利" : "败 北", 160, 180, res == 1 ? COL_GOLD : (Color){ 200, 80, 70, 255 }, 4);
        draw_button((rct_t){ 60, 300, 160, 50 }, "再战", true, false);
        draw_button((rct_t){ 260, 300, 160, 50 }, "返回", true, false);
    }

    /* flash 消息 */
    if (u->flash[0] && u->state.tick - u->flash_tick < 90) {
        DrawRectangle(60, 452, 360, 24, (Color){ 0, 0, 0, 200 });
        draw_mixed(u->flash, 76, 456, COL_HIGHLIGHT, 1);
    }
}

static void render_menu(void)
{
    DrawRectangle(0, 0, GAME_WIDTH, GAME_HEIGHT, COL_BG);
    draw_mixed("三国点将录", 120, 48, COL_GOLD, 3);
    draw_mixed("类游戏王玩法 · 三国题材卡牌对战", 92, 110, COL_TEXT_DIM, 1);
    rct_t bs[4] = {
        { 120, 150, 240, 54 }, { 120, 216, 240, 54 },
        { 120, 282, 240, 54 }, { 120, 348, 240, 54 },
    };
    const char *labels[4] = { "出征", "点将", "图鉴", "设置" };
    for (int i = 0; i < 4; i++) draw_button(bs[i], labels[i], true, false);
}

static void render_levels(void)
{
    DrawRectangle(0, 0, GAME_WIDTH, GAME_HEIGHT, COL_BG);
    draw_button((rct_t){ 8, 8, 88, 36 }, "返回", true, false);
    draw_mixed("选 择 军 团", 160, 14, COL_GOLD, 2);
    static const char *NAMES[SD_LEVEL_COUNT] = { "青州军", "江东猛虎", "虎狼之师" };
    static const char *DESCS[SD_LEVEL_COUNT] = { "教学军团：直白铺场互撞", "速攻压制：抢攻与火计点杀", "控场反击：封攻与伏兵反制" };
    for (int i = 0; i < SD_LEVEL_COUNT; i++) {
        rct_t r = { 20, 70 + i * 128, 440, 116 };
        bool locked = i > sd_prog()->unlocked;
        bool cleared = i < sd_prog()->unlocked;
        DrawRectangle(r.x, r.y, r.w, r.h, locked ? (Color){ 40, 36, 30, 255 } : COL_PANEL);
        DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h }, 2,
                             cleared ? COL_GOLD : COL_BUTTON);
        draw_mixed(NAMES[i], r.x + 16, r.y + 10, COL_TEXT_DARK, 2);
        draw_mixed(DESCS[i], r.x + 16, r.y + 52, COL_TEXT_DARK, 1);
        char st[16];
        snprintf(st, sizeof(st), "lv.%d", SD_DECKS[SD_DECK_QINGZHOU + i].difficulty + 1);
        DrawText(st, r.x + 360, r.y + 16, 18, COL_TEXT_DARK);
        const char *tag = locked ? "锁定" : cleared ? "已通" : "可挑战";
        draw_mixed(tag, r.x + 16, r.y + 84, locked ? COL_TEXT_DIM : COL_BUTTON_HI, 1);
    }
}

static void render_deck(ui_t *u)
{
    DrawRectangle(0, 0, GAME_WIDTH, GAME_HEIGHT, COL_BG);
    draw_button((rct_t){ 8, 8, 88, 36 }, "返回", true, false);
    draw_mixed("点将（组卡）", 120, 14, COL_GOLD, 2);
    /* 卡池 5×9 */
    for (int i = 0; i < SD_CARD_COUNT; i++) {
        int row = i / 9, col = i % 9;
        rct_t r = { 8 + col * 52, 56 + row * 50, 48, 46 };
        const sd_card_def_t *c = &SD_CARDS[i];
        DrawRectangle(r.x, r.y, r.w, r.h, COL_PANEL);
        DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h }, 1, card_color(c));
        char nm[10];
        snprintf(nm, sizeof(nm), "%.6s", c->name); /* 2 字 */
        draw_mixed(nm, r.x + 3, r.y + 2, COL_TEXT_DARK, 1);
        char cnt[8];
        snprintf(cnt, sizeof(cnt), "x%d", sd_deck_count(i));
        DrawText(cnt, r.x + 4, r.y + r.h - 18, 14, sd_deck_count(i) ? COL_BUTTON_HI : COL_TEXT_DIM);
    }
    /* 当前卡组 2×10 */
    draw_mixed("卡组", 8, 294, COL_TEXT_DIM, 1);
    uint8_t *deck = sd_deck_buf();
    int n = 0;
    for (int i = 0; i < 20; i++) {
        int row = i / 10, col = i % 10;
        rct_t r = { 8 + col * 47, 316 + row * 28, 44, 26 };
        if (deck[i] != 0xFF) {
            n++;
            const sd_card_def_t *c = &SD_CARDS[deck[i]];
            DrawRectangle(r.x, r.y, r.w, r.h, COL_PANEL);
            DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h }, 1, card_color(c));
            char nm[10];
            snprintf(nm, sizeof(nm), "%.6s", c->name);
            draw_mixed(nm, r.x + 2, r.y + 4, COL_TEXT_DARK, 1);
        } else {
            DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h }, 1, (Color){ 70, 62, 52, 255 });
        }
    }
    char cnt[24];
    snprintf(cnt, sizeof(cnt), "%d/20", n);
    DrawText(cnt, 100, 292, 18, n == 20 ? COL_GOLD : COL_TEXT_DIM);
    draw_button((rct_t){ 8, 384, 140, 44 }, "保存", true, false);
    draw_button((rct_t){ 168, 384, 140, 44 }, "还原", true, false);
    draw_mixed("点卡池添加", 330, 390, COL_TEXT_DIM, 1);
    draw_mixed("点卡组移除", 330, 412, COL_TEXT_DIM, 1);
    if (u->ov == OV_CARD) render_card_overlay(u);
    if (u->flash[0] && u->state.tick - u->flash_tick < 90) {
        DrawRectangle(60, 444, 360, 24, (Color){ 0, 0, 0, 200 });
        draw_mixed(u->flash, 76, 448, COL_HIGHLIGHT, 1);
    }
}

static void render_gallery(ui_t *u)
{
    DrawRectangle(0, 0, GAME_WIDTH, GAME_HEIGHT, COL_BG);
    draw_button((rct_t){ 8, 8, 88, 36 }, "返回", true, false);
    draw_mixed("图 鉴", 180, 12, COL_GOLD, 2);
    for (int i = 0; i < SD_CARD_COUNT; i++) {
        int row = i / 9, col = i % 9;
        rct_t r = { 8 + col * 52, 56 + row * 62, 48, 58 };
        const sd_card_def_t *c = &SD_CARDS[i];
        DrawRectangle(r.x, r.y, r.w, r.h, COL_PANEL);
        DrawRectangleLinesEx((Rectangle){ (float)r.x, (float)r.y, (float)r.w, (float)r.h }, 1, card_color(c));
        char nm[14];
        snprintf(nm, sizeof(nm), "%.9s", c->name); /* 3 字 */
        draw_mixed(nm, r.x + 3, r.y + 2, COL_TEXT_DARK, 1);
        if (sd_is_monster(c)) {
            char st[24];
            snprintf(st, sizeof(st), "W%d", c->atk);
            DrawText(st, r.x + 3, r.y + r.h - 20, 14, COL_TEXT_DARK);
        }
    }
    if (u->ov == OV_CARD) render_card_overlay(u);
}

static void render_settings(ui_t *u)
{
    DrawRectangle(0, 0, GAME_WIDTH, GAME_HEIGHT, COL_BG);
    draw_button((rct_t){ 8, 8, 88, 36 }, "返回", true, false);
    draw_mixed("设 置", 190, 40, COL_GOLD, 2);
    draw_button((rct_t){ 100, 120, 280, 50 }, sd_prog()->sound_on ? "音效：开" : "音效：关", true, false);
    draw_button((rct_t){ 100, 190, 280, 50 }, u->reset_confirm ? "再点一次确认重置！" : "重置进度", true, true);
}

bool game_render(game_handle_t g)
{
    if (!g) return false;
    ui_t *u = ui(g);
    BeginDrawing();
    if (!MosaicoFastFrameAvailable()) {
        EndDrawing();
        return false;
    }
    ClearBackground(COL_BG);
    switch (sd_page()) {
    case SD_PAGE_MENU: render_menu(); break;
    case SD_PAGE_LEVELS: render_levels(); break;
    case SD_PAGE_DUEL: render_duel(u); break;
    case SD_PAGE_DECK: render_deck(u); break;
    case SD_PAGE_GALLERY: render_gallery(u); break;
    case SD_PAGE_SETTINGS: render_settings(u); break;
    }
    EndDrawing();
    return true;
}
