// SPDX-License-Identifier: Apache-2.0
// sd_ai.c —— 启发式 AI：目标选择 / 响应窗口 / 主阶段动作优先级。
// 难度：0=教学（带随机失误），1=标准，2=老练（更紧的伏兵时机与攻击选择）。
#include <string.h>
#include "sd_ai.h"

#define NONE_UID 255u

static uint32_t ai_rng = 0xC0FFEEu;
static uint8_t opp(uint8_t p) { return (uint8_t)(p ^ 1); }

static int my_mon_count(const sd_duel_t *d, uint8_t p)
{
    int n = 0;
    for (int s = 0; s < SD_SLOTS; s++)
        if (d->pl[p].mon[s] != NONE_UID) n++;
    return n;
}
static int strongest_mon(const sd_duel_t *d, uint8_t p, bool face_up_only)
{
    int best = -1, bv = -1;
    for (int s = 0; s < SD_SLOTS; s++) {
        uint8_t u = d->pl[p].mon[s];
        if (u == NONE_UID) continue;
        if (face_up_only && d->insts[u].face_down) continue;
        int v = d->insts[u].face_down ? sd_eff_def(d, u) : sd_eff_atk(d, u);
        if (v > bv) { bv = v; best = u; }
    }
    return best;
}
static int highest_uid_in(const uint8_t *cand, int n, const sd_duel_t *d, bool by_atk)
{
    int best = -1;
    long bv = -1;
    for (int i = 0; i < n; i++) {
        long v = by_atk ? sd_eff_atk(d, cand[i]) : 0;
        if (v > bv) { bv = v; best = cand[i]; }
    }
    return best;
}
static bool chance(int pct)
{
    return (int)(ai_rng % 100u) < pct;
}

/* ---- 目标选择 ---- */
static bool ai_pick_target(sd_duel_t *d, uint8_t player, sd_cmd_t *out)
{
    const sd_target_req_t *t = &d->target;
    if (t->chooser != player) return false;
    if (t->sel == SD_SEL_INTERNAL_DISCARD) { /* 弃最不值钱的：优先低星武将，再普通计略 */
        int best = -1;
        long bv = 1L << 30;
        for (int i = 0; i < t->cand_n; i++) {
            uint8_t u = t->cand[i];
            const sd_card_def_t *c = &SD_CARDS[d->insts[u].def_id];
            long v;
            if (c->type == SD_CARD_GENERAL)
                v = 1000L * c->tier + c->atk;
            else if (c->type == SD_CARD_AMBUSH)
                v = 500; /* 伏兵留着有价值 */
            else
                v = 300;
            if (v < bv) { bv = v; best = u; }
        }
        if (best < 0) return false;
        out->kind = SD_CMD_TARGET;
        out->p0 = (uint8_t)best;
        return true;
    }
    bool by_atk = true;
    int pick = -1;
    switch (t->sel) {
    case SD_SEL_CHOICE_ENEMY_SET:
        pick = highest_uid_in(t->cand, t->cand_n, d, false); /* 布阵武将按防御无所谓，选一个 */
        break;
    case SD_SEL_CHOICE_ENEMY_MON:
        pick = highest_uid_in(t->cand, t->cand_n, d, true);
        break;
    case SD_SEL_CHOICE_MY_MON:
    case SD_SEL_EQUIP_TARGET:
    default:
        pick = highest_uid_in(t->cand, t->cand_n, d, true);
        break;
    }
    (void)by_atk;
    if (pick < 0) return false;
    out->kind = SD_CMD_TARGET;
    out->p0 = (uint8_t)pick;
    return true;
}

/* ---- 响应窗口 ---- */
static bool ai_respond(sd_duel_t *d, uint8_t player, sd_cmd_t *out)
{
    const sd_window_t *w = &d->window;
    if (w->responder != player || w->cand_n == 0) return false;
    int diff = 1; /* 由调用方设定卡组难度；这里按候选卡主人的 AI 等级 */
    uint8_t use = w->cand[0];
    const sd_card_def_t *c = &SD_CARDS[d->insts[use].def_id];
    bool fire = true;
    switch (w->kind) {
    case SD_WIN_ATTACK: { /* 埋伏：攻击者够强才值得烧 */
        uint8_t att = w->src_uid;
        int atk = sd_uid_valid(d, att) ? sd_eff_atk(d, att) : 0;
        fire = atk >= (diff >= 1 ? 1200 : 1800);
        if (diff == 0) fire = fire && chance(70);
        break;
    }
    case SD_WIN_SUMMON: /* 火箭：刚上阵武将够强才减攻 */
        fire = sd_uid_valid(d, w->src_uid) && sd_eff_atk(d, w->src_uid) >= 2000;
        if (diff == 0) fire = fire && chance(60);
        break;
    case SD_WIN_SPELL: /* 八门金锁阵：高难度看计略价值 */
        fire = diff >= 1 ? true : chance(50);
        break;
    case SD_WIN_DESTROY: /* 铁蒺藜：怪照样没，换 1000 伤害 */
        fire = diff >= 1 ? true : chance(50);
        break;
    default:
        fire = false;
        break;
    }
    (void)c;
    if (!fire) {
        out->kind = SD_CMD_RESPONSE;
        out->p0 = NONE_UID;
        return true;
    }
    out->kind = SD_CMD_RESPONSE;
    out->p0 = use;
    return true;
}

/* ---- 找手牌/场上一张可用卡 ---- */
static int find_hand_card(const sd_duel_t *d, uint8_t player, uint8_t def_id)
{
    const sd_player_t *p = &d->pl[player];
    for (uint8_t i = 0; i < p->hand_n; i++)
        if (d->insts[p->hand[i]].def_id == def_id) return i;
    return -1;
}
static int find_zone_card(const sd_duel_t *d, uint8_t player, uint8_t def_id)
{
    const sd_player_t *p = &d->pl[player];
    for (uint8_t s = 0; s < SD_SLOTS; s++) {
        uint8_t u = p->spell[s];
        if (u != NONE_UID && d->insts[u].def_id == def_id && d->insts[u].face_down) return s;
    }
    return -1;
}
static bool try_activate(sd_duel_t *d, uint8_t player, uint8_t def_id, sd_cmd_t *out, bool *did)
{
    int h = find_hand_card(d, player, def_id);
    if (h >= 0 && sd_can_activate(d, player, (uint8_t)h, false, NULL)) {
        out->kind = SD_CMD_ACTIVATE;
        out->p0 = (uint8_t)h;
        out->p1 = 0;
        *did = true;
        return true;
    }
    int z = find_zone_card(d, player, def_id);
    if (z >= 0 && sd_can_activate(d, player, (uint8_t)z, true, NULL)) {
        out->kind = SD_CMD_ACTIVATE;
        out->p0 = 0;
        out->p1 = 1;
        out->p2 = (uint8_t)z;
        *did = true;
        return true;
    }
    return false;
}

/* ---- 主阶段：上阵 ---- */
static bool ai_try_summon(sd_duel_t *d, uint8_t player, int diff, sd_cmd_t *out)
{
    const sd_player_t *p = &d->pl[player];
    if (p->summoned) return false;
    int best = -1, bv = -1;
    for (uint8_t i = 0; i < p->hand_n; i++) {
        const sd_card_def_t *c = &SD_CARDS[d->insts[p->hand[i]].def_id];
        if (!sd_is_monster(c)) continue;
        if (sd_tribute_count(d, player, c) > 0) continue;
        int e;
        if (!sd_can_summon(d, player, i, false, &e)) continue;
        int v = c->atk;
        if (diff == 0) v += (int)(ai_rng % 400); /* 教学关随机化 */
        if (v > bv) { bv = v; best = i; }
    }
    if (best < 0) return false;
    const sd_card_def_t *c = &SD_CARDS[d->insts[p->hand[best]].def_id];
    int enemy_best = strongest_mon(d, opp(player), false);
    bool down = enemy_best >= 0 && c->atk < sd_eff_atk(d, (uint8_t)enemy_best) - 300;
    out->kind = down ? SD_CMD_SET_MONSTER : SD_CMD_SUMMON;
    out->p0 = (uint8_t)best;
    return true;
}
static bool ai_try_tribute_summon(sd_duel_t *d, uint8_t player, int diff, sd_cmd_t *out)
{
    const sd_player_t *p = &d->pl[player];
    if (p->summoned) return false;
    if (diff == 0) return false;
    int best = -1, bv = -1;
    for (uint8_t i = 0; i < p->hand_n; i++) {
        const sd_card_def_t *c = &SD_CARDS[d->insts[p->hand[i]].def_id];
        if (!sd_is_monster(c)) continue;
        int need = sd_tribute_count(d, player, c);
        if (need == 0) continue;
        if (c->tier == 5 && d->global_turn < 5 && diff < 2) continue; /* ★5 保守 */
        if (!sd_can_summon(d, player, i, false, NULL)) continue;
        if (c->atk > bv) { bv = c->atk; best = i; }
    }
    if (best < 0) return false;
    /* 点最弱的场上的怪 */
    int weak = -1;
    long wv = 1L << 30;
    for (int s = 0; s < SD_SLOTS; s++) {
        uint8_t u = p->mon[s];
        if (u == NONE_UID) continue;
        long v = sd_eff_atk(d, u);
        if (v < wv) { wv = v; weak = s; }
    }
    if (weak < 0) return false;
    const sd_card_def_t *c = &SD_CARDS[d->insts[p->hand[best]].def_id];
    out->kind = SD_CMD_SUMMON;
    out->p0 = (uint8_t)best;
    out->p2 = (uint8_t)weak;
    out->p3 = 255;
    if (sd_tribute_count(d, player, c) == 2) {
        int weak2 = -1;
        long wv2 = 1L << 30;
        for (int s = 0; s < SD_SLOTS; s++) {
            if (s == weak) continue;
            uint8_t u = p->mon[s];
            if (u == NONE_UID) continue;
            long v = sd_eff_atk(d, u);
            if (v < wv2) { wv2 = v; weak2 = s; }
        }
        if (weak2 < 0) return false;
        out->p3 = (uint8_t)weak2;
    }
    return true;
}

/* ---- 战斗阶段 ---- */
static bool ai_try_attack(sd_duel_t *d, uint8_t player, int diff, sd_cmd_t *out)
{
    const sd_player_t *p = &d->pl[player];
    const sd_player_t *en = &d->pl[opp(player)];
    /* 直击优先（含致命判定） */
    if (sd_can_direct_attack(d, player)) {
        for (int s = 0; s < SD_SLOTS; s++) {
            if (!sd_can_attack(d, player, (uint8_t)s, NULL)) continue;
            uint8_t u = p->mon[s];
            if (sd_eff_atk(d, u) >= (int)en->lp) return false; /* 让下面的循环处理也一样，这里直接打 */
            out->kind = SD_CMD_ATTACK;
            out->p0 = (uint8_t)s;
            out->p1 = 255;
            return true;
        }
    }
    /* 逐将评估目标 */
    for (int s = 0; s < SD_SLOTS; s++) {
        if (!sd_can_attack(d, player, (uint8_t)s, NULL)) continue;
        uint8_t u = p->mon[s];
        int atk = sd_eff_atk(d, u);
        int best_t = -1, best_dmg = -1;
        bool safe = false;
        for (int t = 0; t < SD_SLOTS; t++) {
            uint8_t v = en->mon[t];
            if (v == NONE_UID) continue;
            sd_battle_preview_t pv;
            sd_preview_battle(d, u, v, false, &pv);
            if (pv.attacker_destroyed) continue; /* 不送头 */
            int dmg = pv.dmg_defender_side;
            int score = dmg * 2 + (pv.defender_destroyed ? 10 : 0);
            if (score > best_dmg) { best_dmg = score; best_t = t; safe = true; }
        }
        if (best_t < 0 && diff >= 1) { /* 高难度会打盖放怪（赌统率） */
            for (int t = 0; t < SD_SLOTS; t++) {
                uint8_t v = en->mon[t];
                if (v == NONE_UID || !d->insts[v].face_down) continue;
                if (atk >= 2300) { best_t = t; safe = true; break; }
            }
        }
        if (best_t >= 0 && safe) {
            out->kind = SD_CMD_ATTACK;
            out->p0 = (uint8_t)s;
            out->p1 = (uint8_t)best_t;
            return true;
        }
    }
    return false;
}

/* ---- 主阶段决策 ---- */
static bool ai_main(sd_duel_t *d, uint8_t player, int diff, sd_cmd_t *out)
{
    const sd_player_t *p = &d->pl[player];
    bool did = false;

    /* 1) 即时收益计略 */
    if (try_activate(d, player, 27, out, &did)) return true;              /* 草船借箭 */
    if (try_activate(d, player, 35, out, &did)) return true;              /* 屯田令 */
    if (diff >= 1 && try_activate(d, player, 36, out, &did)) return true; /* 挟天子 */
    /* 诈降：敌方有怪就烧最强的 */
    {
        int h = find_hand_card(d, player, 40);
        int z = find_zone_card(d, player, 40);
        if ((h >= 0 || z >= 0) && strongest_mon(d, opp(player), true) >= 0) {
            if (try_activate(d, player, 40, out, &did)) return true;
        }
    }
    /* 火计：敌方有布阵怪且我方要抢攻 */
    if (diff >= 1 && my_mon_count(d, player) > 0) {
        bool has_set = false;
        for (int s = 0; s < SD_SLOTS; s++) {
            uint8_t u = d->pl[opp(player)].mon[s];
            if (u != NONE_UID && d->insts[u].face_down) has_set = true;
        }
        if (has_set && try_activate(d, player, 28, out, &did)) return true;
    }
    /* 火烧连营：能烧到才用 */
    {
        int h = find_hand_card(d, player, 37);
        int z = find_zone_card(d, player, 37);
        if (h >= 0 || z >= 0) {
            for (int s = 0; s < SD_SLOTS; s++) {
                uint8_t u = d->pl[opp(player)].mon[s];
                if (u != NONE_UID && !d->insts[u].face_down && sd_eff_atk(d, u) <= 2000) {
                    if (try_activate(d, player, 37, out, &did)) return true;
                    break;
                }
            }
        }
    }
    /* 水淹七军：敌方场面更强时 */
    {
        int h = find_hand_card(d, player, 30);
        if (h >= 0) {
            int my = 0, en = 0;
            for (int s = 0; s < SD_SLOTS; s++) {
                uint8_t u = d->pl[player].mon[s];
                uint8_t v = d->pl[opp(player)].mon[s];
                if (u != NONE_UID && !d->insts[u].face_down) my += sd_eff_atk(d, u);
                if (v != NONE_UID && !d->insts[v].face_down) en += sd_eff_atk(d, v);
            }
            if (en > my + 500 && try_activate(d, player, 30, out, &did)) return true;
        }
    }
    /* 桃园结义 / 连环计：正面铺收益 */
    if (try_activate(d, player, 33, out, &did)) return true;
    if (diff >= 1 && try_activate(d, player, 34, out, &did)) return true;
    /* 装备：有正面武将就装 */
    if (strongest_mon(d, player, true) >= 0) {
        if (try_activate(d, player, 31, out, &did)) return true;
        if (try_activate(d, player, 32, out, &did)) return true;
    }

    /* 2) 上阵 */
    if (ai_try_summon(d, player, diff, out)) return true;
    if (ai_try_tribute_summon(d, player, diff, out)) return true;

    /* 3) 盖放伏兵（每回合最多 1 张，保持手牌节奏） */
    {
        int set_count = 0;
        for (int s = 0; s < SD_SLOTS; s++)
            if (p->spell[s] != NONE_UID) set_count++;
        if (set_count < 2) {
            for (uint8_t i = 0; i < p->hand_n; i++) {
                const sd_card_def_t *c = &SD_CARDS[d->insts[p->hand[i]].def_id];
                if (c->type == SD_CARD_AMBUSH || c->strat_kind == SD_SK_QUICK) {
                    if (sd_can_set_spell(d, player, i, NULL)) {
                        out->kind = SD_CMD_SET_SPELL;
                        out->p0 = i;
                        return true;
                    }
                }
            }
        }
    }

    /* 4) 主2：布阵武将转出击 */
    if (d->phase == SD_PHASE_MAIN2) {
        int enemy_best = strongest_mon(d, opp(player), false);
        for (int s = 0; s < SD_SLOTS; s++) {
            uint8_t u = p->mon[s];
            if (u == NONE_UID || !d->insts[u].face_down) continue;
            int e;
            if (!sd_can_change_pos(d, player, (uint8_t)s, &e)) continue;
            if (enemy_best < 0 || sd_eff_def(d, u) < sd_eff_atk(d, (uint8_t)enemy_best)) {
                out->kind = SD_CMD_CHANGE_POS;
                out->p0 = (uint8_t)s;
                return true;
            }
        }
    }
    return false;
}

static int s_diff[2] = { 1, 1 };
void sd_ai_set_difficulty(uint8_t player, int diff)
{
    if (player < 2) s_diff[player] = diff;
}

bool sd_ai_decide(sd_duel_t *d, uint8_t player, sd_cmd_t *out)
{
    if (!d || d->game_over || !out) return false;
    memset(out, 0, sizeof(*out));
    if (d->target.active) return ai_pick_target(d, player, out);
    if (d->window.active) return ai_respond(d, player, out);
    if (!d->awaiting_command || d->turn_player != player) return false;
    int diff = s_diff[player] < 2 ? s_diff[player] : 2;
    if (d->phase == SD_PHASE_BATTLE) {
        if (ai_try_attack(d, player, diff, out)) return true;
        out->kind = SD_CMD_NEXT_PHASE;
        return true;
    }
    if (ai_main(d, player, diff, out)) return true;
    out->kind = SD_CMD_NEXT_PHASE;
    return true;
}
