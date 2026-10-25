// SPDX-License-Identifier: Apache-2.0
// sd_duel.c —— 对局规则引擎：状态机、动作队列、效果解释、战斗与响应窗口。
// 设计基线 sanguo-duel-design.md §6；裁决顺序锚点即宿主测试用例。
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sd_core.h"

/* ============ 日志 ============ */
static sd_log_fn g_log;
static void *g_log_ud;
void sd_set_log(sd_log_fn fn, void *ud) { g_log = fn; g_log_ud = ud; }
#define LOG(...)                                   \
    do {                                           \
        if (g_log) {                               \
            char b_[128];                          \
            snprintf(b_, sizeof(b_), __VA_ARGS__); \
            g_log(g_log_ud, b_);                   \
        }                                          \
    } while (0)

/* ============ 基础 ============ */
#define NONE_UID 255u

static uint32_t rng_next(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static uint32_t rng_below(uint32_t *s, uint32_t n) { return n ? rng_next(s) % n : 0; }

const sd_card_def_t *sd_card(uint8_t uid) { return &SD_CARDS[uid]; }
uint8_t sd_uid_def(uint8_t uid) { return uid; } /* uid=实例号；def_id 才是卡表下标 */
bool sd_is_monster(const sd_card_def_t *c) { return c && c->type == SD_CARD_GENERAL; }
bool sd_uid_valid(const sd_duel_t *d, uint8_t uid)
{
    return d && uid != NONE_UID && uid < SD_MAX_INSTS && d->insts[uid].alive;
}
static sd_inst_t *inst(sd_duel_t *d, uint8_t uid) { return &d->insts[uid]; }
static bool in_main(int phase) { return phase == SD_PHASE_MAIN1 || phase == SD_PHASE_MAIN2; }
static uint8_t opp(uint8_t p) { return (uint8_t)(p ^ 1); }

/* 双端队列 */
static void q_push_back(sd_duel_t *d, sd_qitem_t it)
{
    if (d->q_n >= SD_MAX_QUEUE) { LOG("queue overflow"); return; }
    d->queue[(d->q_head + d->q_n) % SD_MAX_QUEUE] = it;
    d->q_n++;
}
static void q_push_front(sd_duel_t *d, sd_qitem_t it)
{
    if (d->q_n >= SD_MAX_QUEUE) { LOG("queue overflow"); return; }
    d->q_head = (uint8_t)((d->q_head + SD_MAX_QUEUE - 1) % SD_MAX_QUEUE);
    d->queue[d->q_head] = it;
    d->q_n++;
}
static sd_qitem_t q_pop(sd_duel_t *d)
{
    sd_qitem_t it = d->queue[d->q_head];
    d->q_head = (uint8_t)((d->q_head + 1) % SD_MAX_QUEUE);
    d->q_n--;
    return it;
}
#define Q(kind_, p0_, p1_, p2_, p3_, a0_) \
    (sd_qitem_t) { (kind_), (p0_), (p1_), (p2_), (p3_), (a0_) }

/* ============ 区域 ============ */
static uint8_t alloc_uid(sd_duel_t *d, uint8_t def_id, uint8_t owner)
{
    for (int i = 1; i < SD_MAX_INSTS; i++) {
        if (!d->insts[i].alive) {
            sd_inst_t *c = &d->insts[i];
            memset(c, 0, sizeof(*c));
            c->def_id = def_id;
            c->owner = owner;
            c->alive = 1;
            c->equip_uid = NONE_UID;
            return (uint8_t)i;
        }
    }
    LOG("inst pool full");
    return NONE_UID;
}

static int zone_find(const uint8_t *arr, uint8_t n, uint8_t uid)
{
    for (uint8_t i = 0; i < n; i++)
        if (arr[i] == uid) return (int)i;
    return -1;
}
static void zone_remove(uint8_t *arr, uint8_t *n, int idx)
{
    if (idx < 0) return;
    for (int i = idx; i < *n - 1; i++) arr[i] = arr[i + 1];
    (*n)--;
}
static void zone_add(uint8_t *arr, uint8_t *n, uint8_t cap, uint8_t uid)
{
    if (*n < cap) arr[(*n)++] = uid;
}

static int mon_slot_of(const sd_duel_t *d, uint8_t uid)
{
    for (int p = 0; p < 2; p++)
        for (int s = 0; s < SD_SLOTS; s++)
            if (d->pl[p].mon[s] == uid) return s;
    return -1;
}
static bool on_field_mon(const sd_duel_t *d, uint8_t uid) { return mon_slot_of(d, uid) >= 0; }
static bool on_field_spell(const sd_duel_t *d, uint8_t uid)
{
    const sd_player_t *p = &d->pl[d->insts[uid].owner];
    for (int s = 0; s < SD_SLOTS; s++)
        if (p->spell[s] == uid) return true;
    return p->field_card == uid;
}
static bool on_field(const sd_duel_t *d, uint8_t uid)
{
    if (!sd_uid_valid(d, uid)) return false;
    return sd_is_monster(&SD_CARDS[d->insts[uid].def_id]) ? on_field_mon(d, uid) : on_field_spell(d, uid);
}

static void detach_from_zone(sd_duel_t *d, uint8_t uid)
{
    sd_player_t *p = &d->pl[inst(d, uid)->owner];
    for (int s = 0; s < SD_SLOTS; s++) {
        if (p->mon[s] == uid) p->mon[s] = NONE_UID;
        if (p->spell[s] == uid) p->spell[s] = NONE_UID;
    }
    if (p->field_card == uid) p->field_card = NONE_UID;
    int h = zone_find(p->hand, p->hand_n, uid);
    if (h >= 0) zone_remove(p->hand, &p->hand_n, h);
}

static void send_to_grave(sd_duel_t *d, uint8_t uid)
{
    sd_inst_t *c = inst(d, uid);
    if (!c->alive) return;
    if (sd_is_monster(&SD_CARDS[c->def_id])) {
        sd_player_t *p = &d->pl[c->owner];
        for (int s = 0; s < SD_SLOTS; s++) {
            uint8_t su = p->spell[s];
            if (su != NONE_UID && d->insts[su].equip_uid == uid) {
                LOG("equip falls off: %s", SD_CARDS[d->insts[su].def_id].name);
                d->insts[su].equip_uid = NONE_UID;
                q_push_back(d, Q(QK_SEND_GRAVE, su, 0, 0, 0, 0));
            }
        }
    }
    detach_from_zone(d, uid);
    c->face_down = 0;
    zone_add(d->pl[c->owner].grave, &d->pl[c->owner].grave_n, SD_MAX_GRAVE, uid);
    LOG("%s -> grave(P%u)", SD_CARDS[c->def_id].name, c->owner);
}

/* ============ 面板 / 光环 ============ */
static bool aura_cond_ok(const sd_inst_t *src, const sd_inst_t *tgt, int16_t cond)
{
    if (cond & COND_SET_DOWN && !tgt->face_down) return false;
    if (cond & COND_SAME_OWNER && tgt->owner != src->owner) return false;
    if (cond & COND_SAME_FACTION &&
        SD_CARDS[tgt->def_id].faction != SD_CARDS[src->def_id].faction) return false;
    return true;
}

/* 扫描场上正面卡静态效果，累加到 tgt 的 atk/def */
static int aura_scan(const sd_duel_t *d, uint8_t uid, bool atk, int base)
{
    const sd_inst_t *c = &d->insts[uid];
    int v = base;
    for (int p = 0; p < 2; p++) {
        for (int s = 0; s < SD_SLOTS; s++) {
            for (int z = 0; z < 2; z++) {
                uint8_t su = z == 0 ? d->pl[p].mon[s] : d->pl[p].spell[s];
                if (su == NONE_UID) continue;
                const sd_inst_t *src = &d->insts[su];
                if (src->face_down) continue;
                const sd_card_def_t *sd = &SD_CARDS[src->def_id];
                for (int f = 0; f < 2; f++) {
                    const sd_fx_t *fx = &sd->fx[f];
                    if (fx->trigger != SD_TR_NONE) continue;
                    for (int a = 0; a < fx->n_act; a++) {
                        const sd_act_t *act = &fx->act[a];
                        bool want = atk ? act->op == SD_OP_AURA_ATK : act->op == SD_OP_AURA_DEF;
                        if (!want) continue;
                        if (act->b & COND_BATTLE_VS_SET) continue; /* 战斗期专用（关羽） */
                        if (act->sel == SD_SEL_THIS) {
                            if (su != uid) continue;
                        } else if (act->sel == SD_SEL_ALL_FIELD) {
                            if (!on_field_mon(d, uid)) continue;
                        } else {
                            continue;
                        }
                        if (!aura_cond_ok(src, c, act->b)) continue;
                        v += act->a;
                    }
                }
            }
        }
    }
    return v;
}

int sd_eff_atk(const sd_duel_t *d, uint8_t uid)
{
    const sd_inst_t *c = &d->insts[uid];
    int v = SD_CARDS[c->def_id].atk + c->atk_perm + c->atk_temp;
    if (on_field_mon(d, uid)) v = aura_scan(d, uid, true, v);
    for (int s = 0; s < SD_SLOTS; s++) {
        uint8_t su = d->pl[c->owner].spell[s];
        if (su != NONE_UID && d->insts[su].equip_uid == uid && !d->insts[su].face_down)
            v += d->insts[su].atk_perm;
    }
    return v < 0 ? 0 : v;
}

int sd_eff_def(const sd_duel_t *d, uint8_t uid)
{
    const sd_inst_t *c = &d->insts[uid];
    int v = SD_CARDS[c->def_id].def + c->def_perm + c->def_temp;
    if (on_field_mon(d, uid)) v = aura_scan(d, uid, false, v);
    for (int s = 0; s < SD_SLOTS; s++) {
        uint8_t su = d->pl[c->owner].spell[s];
        if (su != NONE_UID && d->insts[su].equip_uid == uid && !d->insts[su].face_down)
            v += d->insts[su].def_perm;
    }
    return v < 0 ? 0 : v;
}

static bool has_static_flag(const sd_duel_t *d, uint8_t uid, sd_op_t op)
{
    const sd_inst_t *c = &d->insts[uid];
    if (!on_field_mon(d, uid)) return false;
    const sd_card_def_t *def = &SD_CARDS[c->def_id];
    for (int f = 0; f < 2; f++) {
        const sd_fx_t *fx = &def->fx[f];
        if (fx->trigger != SD_TR_NONE) continue;
        for (int a = 0; a < fx->n_act; a++)
            if (fx->act[a].op == op && fx->act[a].sel == SD_SEL_THIS) return true;
    }
    return false;
}

int sd_tribute_count(const sd_duel_t *d, uint8_t player, const sd_card_def_t *c)
{
    if (!sd_is_monster(c)) return 0;
    int n = c->tier >= 5 ? 2 : (c->tier >= 3 ? 1 : 0);
    for (int s = 0; s < SD_SLOTS; s++) { /* 连环计(id34)：对方正面连续使我方点兵+1 */
        uint8_t su = d->pl[opp(player)].spell[s];
        if (su != NONE_UID && !d->insts[su].face_down && d->insts[su].def_id == 34) n++;
    }
    return n;
}

/* ============ 玩家数值 ============ */
static void add_lp(sd_duel_t *d, uint8_t player, int delta)
{
    sd_player_t *p = &d->pl[player];
    int v = (int)p->lp + delta;
    if (v < 0) v = 0;
    if (v > 9999) v = 9999;
    p->lp = (uint16_t)v;
    if (p->lp == 0 && !d->game_over) {
        d->game_over = 1;
        d->winner = opp(player);
        LOG("P%u LP=0, P%u wins", player, d->winner);
    }
}

static void draw_cards(sd_duel_t *d, uint8_t player, int n)
{
    sd_player_t *p = &d->pl[player];
    for (int i = 0; i < n; i++) {
        if (p->deck_n == 0) {
            d->game_over = 1;
            d->winner = opp(player);
            LOG("P%u deck out, P%u wins", player, d->winner);
            return;
        }
        if (p->hand_n >= SD_MAX_HAND) { p->deck_n--; continue; } /* 极端场景丢弃 */
        uint8_t uid = p->deck[--p->deck_n];
        zone_add(p->hand, &p->hand_n, SD_MAX_HAND, uid);
    }
}

static void shuffle_deck(sd_duel_t *d, uint8_t player)
{
    sd_player_t *p = &d->pl[player];
    for (int i = p->deck_n - 1; i > 0; i--) {
        int j = (int)rng_below(&d->rng, (uint32_t)(i + 1));
        uint8_t t = p->deck[i]; p->deck[i] = p->deck[j]; p->deck[j] = t;
    }
}

/* ============ 触发收集 ============ */
typedef struct { uint8_t uid, fx_slot; } fx_ref_t;
static int collect_triggers(const sd_duel_t *d, uint8_t trig, uint8_t owner_filter,
                            uint8_t exclude_uid, fx_ref_t *out, int cap)
{
    int n = 0;
    for (int p = 0; p < 2; p++) {
        for (int s = 0; s < SD_SLOTS; s++) {
            for (int z = 0; z < 2; z++) {
                uint8_t su = z == 0 ? d->pl[p].mon[s] : d->pl[p].spell[s];
                if (su == NONE_UID || su == exclude_uid) continue;
                const sd_inst_t *src = &d->insts[su];
                if (src->face_down) continue;
                if (owner_filter != 255 && src->owner != owner_filter) continue;
                const sd_card_def_t *sd = &SD_CARDS[src->def_id];
                for (int f = 0; f < 2; f++)
                    if (sd->fx[f].trigger == trig && n < cap) {
                        out[n].uid = su;
                        out[n].fx_slot = (uint8_t)f;
                        n++;
                    }
            }
        }
    }
    return n;
}

/* ============ 目标候选 ============ */
static int collect_candidates(const sd_duel_t *d, uint8_t chooser, int sel, uint8_t out[SD_WIN_MAX_CAND])
{
    int n = 0;
    const sd_player_t *me = &d->pl[chooser], *en = &d->pl[opp(chooser)];
    switch (sel) {
    case SD_SEL_CHOICE_MY_MON:
    case SD_SEL_EQUIP_TARGET:
        for (int s = 0; s < SD_SLOTS; s++) {
            uint8_t u = me->mon[s];
            if (u != NONE_UID && !d->insts[u].face_down && n < SD_WIN_MAX_CAND) out[n++] = u;
        }
        break;
    case SD_SEL_CHOICE_ENEMY_MON:
        for (int s = 0; s < SD_SLOTS; s++) {
            uint8_t u = en->mon[s];
            if (u != NONE_UID && !d->insts[u].face_down && n < SD_WIN_MAX_CAND) out[n++] = u;
        }
        break;
    case SD_SEL_CHOICE_ENEMY_SET:
        for (int s = 0; s < SD_SLOTS; s++) {
            uint8_t u = en->mon[s];
            if (u != NONE_UID && d->insts[u].face_down && n < SD_WIN_MAX_CAND) out[n++] = u;
        }
        break;
    case SD_SEL_INTERNAL_DISCARD:
        for (uint8_t i = 0; i < me->hand_n && n < SD_WIN_MAX_CAND; i++) out[n++] = me->hand[i];
        break;
    default:
        break;
    }
    return n;
}
static bool sel_needs_choice(int sel)
{
    return sel == SD_SEL_CHOICE_MY_MON || sel == SD_SEL_CHOICE_ENEMY_MON ||
           sel == SD_SEL_CHOICE_ENEMY_SET || sel == SD_SEL_EQUIP_TARGET;
}

/* ============ 效果执行 ============ */
static void apply_destroy(sd_duel_t *d, uint8_t uid)
{
    if (!on_field(d, uid)) return;
    LOG("%s will be destroyed", SD_CARDS[d->insts[uid].def_id].name);
    q_push_back(d, Q(QK_DESTROY, uid, 0, 0, 0, 0));
}

static void exec_one_act(sd_duel_t *d, uint8_t src_uid, const sd_act_t *act, uint8_t chosen)
{
    sd_inst_t *src = inst(d, src_uid);
    uint8_t owner = src->owner;
    switch (act->op) {
    case SD_OP_DRAW:
        draw_cards(d, owner, act->a);
        LOG("P%u draws %d", owner, act->a);
        break;
    case SD_OP_HEAL: {
        uint8_t pl = act->sel == SD_SEL_OPPONENT_LP ? opp(owner) : owner;
        add_lp(d, pl, act->a);
        LOG("P%u LP+%d", pl, act->a);
        break;
    }
    case SD_OP_DAMAGE: {
        uint8_t pl = act->sel == SD_SEL_OWNER_LP ? owner : opp(owner);
        add_lp(d, pl, -act->a);
        LOG("P%u LP-%d", pl, act->a);
        break;
    }
    case SD_OP_ATK_MOD:
    case SD_OP_DEF_MOD: {
        if (act->sel == SD_SEL_ALL_FIELD) { /* 全场（水淹七军） */
            for (int p = 0; p < 2; p++)
                for (int s = 0; s < SD_SLOTS; s++) {
                    uint8_t u = d->pl[p].mon[s];
                    if (u == NONE_UID) continue;
                    sd_inst_t *t = inst(d, u);
                    if (act->op == SD_OP_ATK_MOD) t->atk_temp = (int16_t)(t->atk_temp + act->a);
                    else t->def_temp = (int16_t)(t->def_temp + act->a);
                }
            LOG("all field %s %+d", act->op == SD_OP_ATK_MOD ? "atk" : "def", act->a);
            break;
        }
        uint8_t tgt = NONE_UID;
        if (act->sel == SD_SEL_ACTOR || act->sel == SD_SEL_THIS) tgt = src_uid;
        else if (act->sel == SD_SEL_ATTACKER || act->sel == SD_SEL_BATTLE_OPPONENT) tgt = d->ctx_uid;
        else if (act->sel == SD_SEL_EQUIP_TARGET) tgt = src_uid; /* 增减挂装备卡实例 */
        else tgt = chosen;
        if (act->sel == SD_SEL_BATTLE_OPPONENT) tgt = d->battle_ctx_uid;
        if (tgt == NONE_UID || !on_field_mon(d, tgt)) {
            if (!(act->sel == SD_SEL_EQUIP_TARGET && on_field_spell(d, src_uid))) break;
            tgt = src_uid; /* 装备加成存装备卡上 */
        }
        sd_inst_t *t = inst(d, tgt);
        if (act->op == SD_OP_ATK_MOD) {
            if (act->b) t->atk_temp = (int16_t)(t->atk_temp + act->a);
            else t->atk_perm = (int16_t)(t->atk_perm + act->a);
        } else {
            if (act->b) t->def_temp = (int16_t)(t->def_temp + act->a);
            else t->def_perm = (int16_t)(t->def_perm + act->a);
        }
        LOG("%s atk=%d def=%d", SD_CARDS[tgt == src_uid && !on_field_mon(d, tgt) ? src_uid : tgt].name,
            sd_eff_atk(d, tgt), sd_eff_def(d, tgt));
        break;
    }
    case SD_OP_DESTROY: {
        if (act->sel == SD_SEL_ALL_ENEMY) {
            for (int s = 0; s < SD_SLOTS; s++) {
                uint8_t u = d->pl[opp(owner)].mon[s];
                if (u != NONE_UID) apply_destroy(d, u);
            }
        } else if (act->sel == SD_SEL_ALL_FIELD) {
            for (int p = 0; p < 2; p++)
                for (int s = 0; s < SD_SLOTS; s++) {
                    uint8_t u = d->pl[p].mon[s];
                    if (u != NONE_UID) apply_destroy(d, u);
                }
        } else if (act->sel == SD_SEL_ATK_LE) {
            for (int s = 0; s < SD_SLOTS; s++) {
                uint8_t u = d->pl[opp(owner)].mon[s];
                if (u != NONE_UID && !d->insts[u].face_down && sd_eff_atk(d, u) <= act->a)
                    apply_destroy(d, u);
            }
        } else if (act->sel == SD_SEL_ENEMY_HIGHEST_ATK) {
            uint8_t best = NONE_UID;
            int bv = -1;
            for (int s = 0; s < SD_SLOTS; s++) {
                uint8_t u = d->pl[opp(owner)].mon[s];
                if (u == NONE_UID || d->insts[u].face_down) continue;
                int v = sd_eff_atk(d, u);
                if (v > bv) { bv = v; best = u; }
            }
            if (best != NONE_UID) apply_destroy(d, best);
        } else {
            uint8_t tgt = chosen;
            if (act->sel == SD_SEL_ATTACKER) tgt = d->ctx_uid;
            if (act->sel == SD_SEL_BATTLE_OPPONENT) tgt = d->battle_ctx_uid;
            if (tgt != NONE_UID) apply_destroy(d, tgt);
        }
        break;
    }
    case SD_OP_SEAL_ATTACK: {
        uint8_t tgt = chosen;
        if (tgt != NONE_UID && on_field_mon(d, tgt)) {
            inst(d, tgt)->seal_turns = (uint8_t)act->a;
            LOG("%s sealed for %d turn(s)", SD_CARDS[d->insts[tgt].def_id].name, act->a);
        }
        break;
    }
    case SD_OP_PIERCE:
    case SD_OP_MULTI: {
        uint8_t tgt = act->sel == SD_SEL_EQUIP_TARGET ? chosen : src_uid;
        if (tgt != NONE_UID && on_field_mon(d, tgt)) {
            if (act->op == SD_OP_PIERCE) inst(d, tgt)->pierce = 1;
            else inst(d, tgt)->multi = 1;
        }
        break;
    }
    case SD_OP_NO_RESPOND:
        d->battle_no_respond = 1;
        LOG("no respond allowed this battle");
        break;
    case SD_OP_DISCARD: {
        uint8_t pl = act->sel == SD_SEL_RANDOM_OPP_HAND ? opp(owner) : owner;
        sd_player_t *p = &d->pl[pl];
        for (int i = 0; i < act->a && p->hand_n > 0; i++) {
            int idx = (int)rng_below(&d->rng, p->hand_n);
            uint8_t u = p->hand[idx];
            zone_remove(p->hand, &p->hand_n, idx);
            zone_add(p->grave, &p->grave_n, SD_MAX_GRAVE, u);
            LOG("P%u discards %s", pl, SD_CARDS[d->insts[u].def_id].name);
        }
        break;
    }
    case SD_OP_NEGATE:
        d->negate_next = d->ctx_uid; /* ctx=被反击的计略 uid（响应提交时设置） */
        LOG("spell negated by counter");
        break;
    default:
        break; /* TRIBUTE_COST/AURA_*/ /*/SEARCH 等静态原语不经执行器 */
    }
}

/* 执行效果槽（从 act_idx 起）；需选择时登记 target 请求并暂停 */
static void exec_effect(sd_duel_t *d, uint8_t src_uid, uint8_t fx_slot, uint8_t act_idx)
{
    sd_inst_t *src = inst(d, src_uid);
    const sd_card_def_t *def = &SD_CARDS[src->def_id];
    if (fx_slot >= 2 || act_idx >= def->fx[fx_slot].n_act) return;
    for (int i = act_idx; i < def->fx[fx_slot].n_act; i++) {
        const sd_act_t *act = &def->fx[fx_slot].act[i];
        if (act->sel == SD_SEL_EQUIP_TARGET && src->equip_uid != NONE_UID) {
            /* 装备目标已定：多段装备效果不再重复询问 */
            exec_one_act(d, src_uid, act, src->equip_uid);
            if (d->game_over) return;
            continue;
        }
        if (sel_needs_choice(act->sel)) {
            uint8_t cand[SD_WIN_MAX_CAND];
            int n = collect_candidates(d, src->owner, act->sel, cand);
            if (n == 0) continue; /* 无候选：动作落空 */
            d->target.active = 1;
            d->target.chooser = src->owner;
            d->target.sel = act->sel;
            d->target.uid = src_uid;
            d->target.fx_slot = fx_slot;
            d->target.act_idx = (uint8_t)i;
            memcpy(d->target.cand, cand, (size_t)n);
            d->target.cand_n = (uint8_t)n;
            return;
        }
        exec_one_act(d, src_uid, act, NONE_UID);
        if (d->game_over) return;
    }
}

/* ============ 响应窗口 ============ */
static bool ambush_ready(const sd_duel_t *d, const sd_inst_t *c)
{
    return c->face_down && c->set_on_turn < d->global_turn;
}

static void open_window(sd_duel_t *d, int kind, uint8_t event_side, uint8_t src_uid)
{
    if (d->battle_no_respond) {
        LOG("window skipped (no respond)");
        return;
    }
    uint8_t responder = opp(event_side);
    sd_window_t *w = &d->window;
    w->active = 0;
    w->kind = (uint8_t)kind;
    w->responder = responder;
    w->src_uid = src_uid;
    w->cand_n = 0;
    const sd_player_t *p = &d->pl[responder];
    for (int s = 0; s < SD_SLOTS && w->cand_n < SD_WIN_MAX_CAND; s++) {
        uint8_t u = p->spell[s];
        if (u == NONE_UID) continue;
        const sd_inst_t *c = &d->insts[u];
        if (!ambush_ready(d, c)) continue;
        if (SD_CARDS[c->def_id].window == kind) w->cand[w->cand_n++] = u;
    }
    if (w->cand_n > 0) {
        w->active = 1;
        LOG("window kind=%d open (P%u, %d cand)", kind, responder, w->cand_n);
    }
}

/* ============ 战斗 ============ */
static bool has_pierce(const sd_duel_t *d, uint8_t uid)
{
    return d->insts[uid].pierce || has_static_flag(d, uid, SD_OP_PIERCE);
}
static int battle_vs_set_bonus(const sd_duel_t *d, uint8_t uid)
{
    int v = 0;
    const sd_inst_t *c = &d->insts[uid];
    const sd_card_def_t *def = &SD_CARDS[c->def_id];
    for (int f = 0; f < 2; f++) {
        const sd_fx_t *fx = &def->fx[f];
        if (fx->trigger != SD_TR_NONE) continue;
        for (int a = 0; a < fx->n_act; a++) {
            const sd_act_t *act = &fx->act[a];
            if (act->op == SD_OP_AURA_ATK && act->sel == SD_SEL_THIS && (act->b & COND_BATTLE_VS_SET))
                v += act->a;
        }
    }
    return v;
}

void sd_preview_battle(const sd_duel_t *d, uint8_t au, uint8_t du, bool direct, sd_battle_preview_t *out)
{
    memset(out, 0, sizeof(*out));
    int atk = sd_eff_atk(d, au);
    if (direct) {
        out->dmg_defender_side = atk;
        return;
    }
    const sd_inst_t *b = &d->insts[du];
    if (b->face_down) {
        int def = sd_eff_def(d, du);
        int va = atk + battle_vs_set_bonus(d, au);
        if (va > def) {
            out->defender_destroyed = true;
            if (has_pierce(d, au)) out->dmg_defender_side = va - def;
        } else if (va < def) {
            out->attacker_destroyed = true;
            out->dmg_attacker_side = def - va;
        }
    } else {
        int bd = sd_eff_atk(d, du);
        if (atk > bd) {
            out->defender_destroyed = true;
            out->dmg_defender_side = atk - bd;
        } else if (atk < bd) {
            out->attacker_destroyed = true;
            out->dmg_attacker_side = bd - atk;
        } else {
            out->attacker_destroyed = out->defender_destroyed = true;
        }
    }
}

static void do_battle(sd_duel_t *d, uint8_t au, uint8_t du, bool direct)
{
    if (!on_field_mon(d, au)) { LOG("battle cancelled: attacker gone"); return; }
    if (!direct && !on_field_mon(d, du)) { LOG("battle cancelled: defender gone"); return; }
    uint8_t a_own = d->insts[au].owner;
    int atk = sd_eff_atk(d, au);
    bool pierce = has_pierce(d, au);
    inst(d, au)->attacked++;
    d->battle_ctx_uid = au;

    if (direct) {
        add_lp(d, opp(a_own), -atk);
        LOG("direct attack: %d dmg", atk);
        return;
    }
    sd_inst_t *b = inst(d, du);
    bool b_was_set = b->face_down;
    b->face_down = 0; /* 战斗翻开布阵武将 */
    uint8_t b_own = b->owner;

    if (b_was_set) {
        int def = sd_eff_def(d, du);
        int va = atk + battle_vs_set_bonus(d, au);
        LOG("battle: %s(%d) vs set %s(%d)", SD_CARDS[d->insts[au].def_id].name, va,
            SD_CARDS[d->insts[du].def_id].name, def);
        if (va > def) {
            if (pierce) add_lp(d, b_own, -(va - def));
            q_push_back(d, Q(QK_WINDOW, SD_WIN_DESTROY, opp(b_own), du, 0, 0));
            q_push_back(d, Q(QK_DESTROY_BATTLE, du, au, 0, 0, 0));
            q_push_back(d, Q(QK_AFTER_BATTLE_WIN, au, du, 0, 0, 0));
        } else if (va < def) {
            add_lp(d, a_own, -(def - va));
            q_push_back(d, Q(QK_WINDOW, SD_WIN_DESTROY, opp(a_own), au, 0, 0));
            q_push_back(d, Q(QK_DESTROY_BATTLE, au, du, 0, 0, 0));
        }
    } else {
        int bd = sd_eff_atk(d, du);
        LOG("battle: %s(%d) vs %s(%d)", SD_CARDS[d->insts[au].def_id].name, atk,
            SD_CARDS[d->insts[du].def_id].name, bd);
        if (atk > bd) {
            add_lp(d, b_own, -(atk - bd));
            q_push_back(d, Q(QK_WINDOW, SD_WIN_DESTROY, opp(b_own), du, 0, 0));
            q_push_back(d, Q(QK_DESTROY_BATTLE, du, au, 0, 0, 0));
            q_push_back(d, Q(QK_AFTER_BATTLE_WIN, au, du, 0, 0, 0));
        } else if (atk < bd) {
            add_lp(d, a_own, -(bd - atk));
            q_push_back(d, Q(QK_WINDOW, SD_WIN_DESTROY, opp(a_own), au, 0, 0));
            q_push_back(d, Q(QK_DESTROY_BATTLE, au, du, 0, 0, 0));
        } else {
            q_push_back(d, Q(QK_DESTROY_BATTLE, du, au, 0, 0, 0));
            q_push_back(d, Q(QK_DESTROY_BATTLE, au, du, 0, 0, 0));
        }
    }
}

/* ============ 触发结算 ============ */
static void push_card_triggers(sd_duel_t *d, uint8_t uid, uint8_t trig)
{
    const sd_inst_t *c = &d->insts[uid];
    const sd_card_def_t *def = &SD_CARDS[c->def_id];
    for (int f = 0; f < 2; f++) {
        if (def->fx[f].trigger == trig)
            q_push_back(d, Q(QK_RESOLVE_FX, uid, (uint8_t)f, 0, 0, 0));
    }
}

static void after_summon(sd_duel_t *d, uint8_t uid)
{
    d->ctx_uid = uid;
    open_window(d, SD_WIN_SUMMON, d->insts[uid].owner, uid);
    push_card_triggers(d, uid, SD_TR_ON_SUMMON);
    fx_ref_t refs[SD_WIN_MAX_CAND];
    int n = collect_triggers(d, SD_TR_ON_ALLY_SUMMON, d->insts[uid].owner, uid, refs, SD_WIN_MAX_CAND);
    for (int i = 0; i < n; i++) {
        d->ctx_uid = uid;
        q_push_back(d, Q(QK_RESOLVE_FX, refs[i].uid, refs[i].fx_slot, 0, 0, 0));
    }
}

/* ============ 阶段 ============ */
static void end_phase_cleanup(sd_duel_t *d)
{
    uint8_t tp = d->turn_player;
    for (int p = 0; p < 2; p++) {
        for (int s = 0; s < SD_SLOTS; s++) {
            uint8_t u = d->pl[p].mon[s];
            if (u == NONE_UID) continue;
            sd_inst_t *c = inst(d, u);
            c->atk_temp = 0;
            c->def_temp = 0;
            if (p == tp && c->seal_turns > 0) c->seal_turns--;
        }
    }
    for (int s = 0; s < SD_SLOTS; s++) {
        uint8_t u = d->pl[tp].mon[s];
        if (u != NONE_UID) {
            inst(d, u)->attacked = 0;
            inst(d, u)->pos_changed = 0;
        }
    }
    d->battle_no_respond = 0;
    sd_player_t *p = &d->pl[tp];
    if (p->hand_n > SD_HAND_LIMIT) { /* 超限弃牌：等待回合玩家逐张选择 */
        d->target.active = 1;
        d->target.chooser = tp;
        d->target.sel = SD_SEL_INTERNAL_DISCARD;
        d->target.uid = NONE_UID;
        d->target.fx_slot = 0;
        d->target.act_idx = 0;
        d->target.cand_n = (uint8_t)collect_candidates(d, tp, SD_SEL_INTERNAL_DISCARD, d->target.cand);
        LOG("end phase: P%u must discard", tp);
    }
}

static void start_turn(sd_duel_t *d, uint8_t player)
{
    d->turn_player = player;
    d->global_turn++;
    d->phase = SD_PHASE_DRAW;
    LOG("---- turn %u P%u ----", d->global_turn, player);
    if (!(d->global_turn == 1 && player == d->first_player)) draw_cards(d, player, 1);
    if (d->game_over) return;
    d->phase = SD_PHASE_STANDBY;
    fx_ref_t refs[SD_WIN_MAX_CAND];
    int n = collect_triggers(d, SD_TR_ON_TURN_START, player, 255, refs, SD_WIN_MAX_CAND);
    for (int i = 0; i < n; i++) {
        d->ctx_uid = refs[i].uid;
        q_push_back(d, Q(QK_RESOLVE_FX, refs[i].uid, refs[i].fx_slot, 0, 0, 0));
    }
    d->phase = SD_PHASE_MAIN1;
    d->pl[player].summoned = 0;
    d->awaiting_command = 1;
}

static void advance_phase(sd_duel_t *d)
{
    switch (d->phase) {
    case SD_PHASE_MAIN1:
        d->phase = SD_PHASE_BATTLE;
        d->awaiting_command = 1;
        break;
    case SD_PHASE_BATTLE:
        d->phase = SD_PHASE_MAIN2;
        d->awaiting_command = 1;
        break;
    case SD_PHASE_MAIN2:
        d->phase = SD_PHASE_END;
        end_phase_cleanup(d);
        if (d->target.active || d->game_over) return; /* 弃牌暂停 / 终局 */
        q_push_back(d, Q(QK_END_TURN, 0, 0, 0, 0, 0));
        d->awaiting_command = 0;
        break;
    default:
        break;
    }
}

/* ============ 校验 ============ */
static int count_free_slots(const sd_player_t *p)
{
    int n = 0;
    for (int s = 0; s < SD_SLOTS; s++)
        if (p->mon[s] == NONE_UID) n++;
    return n;
}
static int first_free_slot(const sd_player_t *p)
{
    for (int s = 0; s < SD_SLOTS; s++)
        if (p->mon[s] == NONE_UID) return s;
    return -1;
}
static bool tributes_valid(const sd_duel_t *d, uint8_t player, uint8_t s1, uint8_t s2, int need)
{
    if (need == 0) return true; /* 槽位参数忽略 */
    if (need > 2) return false;
    if (s1 >= SD_SLOTS || d->pl[player].mon[s1] == NONE_UID) return false;
    if (need == 2 && (s2 >= SD_SLOTS || d->pl[player].mon[s2] == NONE_UID || s2 == s1)) return false;
    if (need == 1 && s2 != 255) return false;
    return true;
}

bool sd_can_summon(const sd_duel_t *d, uint8_t player, uint8_t hand_idx, bool set_down, int *err)
{
    (void)set_down;
    int e = SD_OK;
    do {
        if (!d) { e = SD_ERR_ARG; break; }
        if (d->game_over || d->window.active || d->target.active) { e = SD_ERR_WINDOW_BUSY; break; }
        if (player != d->turn_player || !d->awaiting_command) { e = SD_ERR_NOT_YOUR_TURN; break; }
        if (!in_main(d->phase)) { e = SD_ERR_PHASE; break; }
        const sd_player_t *p = &d->pl[player];
        if (hand_idx >= p->hand_n) { e = SD_ERR_ARG; break; }
        const sd_card_def_t *c = &SD_CARDS[d->insts[p->hand[hand_idx]].def_id];
        if (!sd_is_monster(c)) { e = SD_ERR_ARG; break; }
        if (p->summoned) { e = SD_ERR_ALREADY_SUMMONED; break; }
        int need = sd_tribute_count(d, player, c);
        int my_mons = 0;
        for (int s = 0; s < SD_SLOTS; s++)
            if (p->mon[s] != NONE_UID) my_mons++;
        if (need > my_mons) { e = SD_ERR_BAD_TRIBUTE; break; }
        int free_slots = count_free_slots(p);
        if (need == 0 && free_slots == 0) { e = SD_ERR_ZONE_FULL; break; }
        if (need > 0 && free_slots + need < 1) { e = SD_ERR_ZONE_FULL; break; }
    } while (0);
    if (err) *err = e;
    return e == SD_OK;
}

bool sd_can_set_spell(const sd_duel_t *d, uint8_t player, uint8_t hand_idx, int *err)
{
    int e = SD_OK;
    do {
        if (!d) { e = SD_ERR_ARG; break; }
        if (d->game_over || d->window.active || d->target.active) { e = SD_ERR_WINDOW_BUSY; break; }
        if (player != d->turn_player || !d->awaiting_command) { e = SD_ERR_NOT_YOUR_TURN; break; }
        if (!in_main(d->phase)) { e = SD_ERR_PHASE; break; }
        const sd_player_t *p = &d->pl[player];
        if (hand_idx >= p->hand_n) { e = SD_ERR_ARG; break; }
        if (sd_is_monster(&SD_CARDS[d->insts[p->hand[hand_idx]].def_id])) { e = SD_ERR_ARG; break; }
        bool free_slot = false;
        for (int s = 0; s < SD_SLOTS; s++) /* 从手牌发动也要占用计略区 */
            if (p->spell[s] == NONE_UID) { free_slot = true; break; }
        if (!free_slot) { e = SD_ERR_ZONE_FULL; break; }
        for (int s = 0; s < SD_SLOTS; s++)
            if (p->spell[s] == NONE_UID) { free_slot = true; break; }
        if (!free_slot) { e = SD_ERR_ZONE_FULL; break; }
    } while (0);
    if (err) *err = e;
    return e == SD_OK;
}

static bool fx_has_targets(const sd_duel_t *d, uint8_t src_uid, uint8_t chooser)
{
    const sd_inst_t *src = &d->insts[src_uid];
    const sd_card_def_t *def = &SD_CARDS[src->def_id];
    bool has_activated = false;
    for (int f = 0; f < 2; f++) {
        if (def->fx[f].trigger == SD_TR_ACTIVATED) has_activated = true;
        const sd_fx_t *fx = &def->fx[f];
        if (fx->trigger != SD_TR_ACTIVATED) continue;
        for (int a = 0; a < fx->n_act; a++) {
            const sd_act_t *act = &fx->act[a];
            if (!sel_needs_choice(act->sel)) return true;
            uint8_t cand[SD_WIN_MAX_CAND];
            if (collect_candidates(d, chooser, act->sel, cand) > 0) return true;
        }
    }
    return !has_activated; /* 纯被动/永续卡无主体效果，也可发动 */
}

bool sd_can_activate(const sd_duel_t *d, uint8_t player, uint8_t idx, bool from_zone_spell, int *err)
{
    int e = SD_OK;
    do {
        if (!d) { e = SD_ERR_ARG; break; }
        if (d->game_over || d->window.active || d->target.active) { e = SD_ERR_WINDOW_BUSY; break; }
        if (player != d->turn_player || !d->awaiting_command) { e = SD_ERR_NOT_YOUR_TURN; break; }
        if (!in_main(d->phase) && d->phase != SD_PHASE_BATTLE) { e = SD_ERR_PHASE; break; } /* 速攻可在战斗阶段发动 */
        const sd_player_t *p = &d->pl[player];
        uint8_t uid;
        if (from_zone_spell) {
            if (idx >= SD_SLOTS || p->spell[idx] == NONE_UID) { e = SD_ERR_ARG; break; }
            uid = p->spell[idx];
        } else {
            if (idx >= p->hand_n) { e = SD_ERR_ARG; break; }
            uid = p->hand[idx];
        }
        const sd_inst_t *c = &d->insts[uid];
        const sd_card_def_t *def = &SD_CARDS[c->def_id];
        if (sd_is_monster(def)) { e = SD_ERR_ARG; break; }
        if (!from_zone_spell) { /* 手牌发动同样占用计略区 */
            bool free_slot = false;
            for (int s = 0; s < SD_SLOTS; s++)
                if (p->spell[s] == NONE_UID) { free_slot = true; break; }
            if (!free_slot) { e = SD_ERR_ZONE_FULL; break; }
        }
        if (def->window != 0) { e = SD_ERR_FACEUP_ONLY; break; } /* 响应伏兵只走窗口 */
        if (from_zone_spell && c->face_down) {
            if (def->type == SD_CARD_STRAT && def->strat_kind == SD_SK_QUICK) {
                if (c->set_on_turn >= d->global_turn) { e = SD_ERR_SET_TOO_SOON; break; }
            } else if (def->type == SD_CARD_AMBUSH && !ambush_ready(d, c)) {
                e = SD_ERR_SET_TOO_SOON;
                break;
            }
        } else if (from_zone_spell && !c->face_down) {
            e = SD_ERR_FACEUP_ONLY; /* 已正面的永续/装备不能重复发动 */
            break;
        } else if (!from_zone_spell && def->type == SD_CARD_AMBUSH) {
            e = SD_ERR_FACEUP_ONLY; /* 伏兵不能从手牌发动 */
            break;
        }
        if (!fx_has_targets(d, uid, player)) { e = SD_ERR_NO_TARGETS; break; }
    } while (0);
    if (err) *err = e;
    return e == SD_OK;
}

bool sd_can_attack(const sd_duel_t *d, uint8_t player, uint8_t slot, int *err)
{
    int e = SD_OK;
    do {
        if (!d) { e = SD_ERR_ARG; break; }
        if (d->game_over || d->window.active || d->target.active) { e = SD_ERR_WINDOW_BUSY; break; }
        if (player != d->turn_player || !d->awaiting_command) { e = SD_ERR_NOT_YOUR_TURN; break; }
        if (d->phase != SD_PHASE_BATTLE) { e = SD_ERR_PHASE; break; }
        if (d->global_turn == 1 && player == d->first_player) { e = SD_ERR_PHASE; break; }
        const sd_player_t *p = &d->pl[player];
        if (slot >= SD_SLOTS || p->mon[slot] == NONE_UID) { e = SD_ERR_ARG; break; }
        const sd_inst_t *c = &d->insts[p->mon[slot]];
        if (c->face_down) { e = SD_ERR_WRONG_POS; break; }
        if (c->seal_turns > 0) { e = SD_ERR_SEALED; break; }
        int max_atk = c->multi ? 2 : 1;
        if (c->attacked >= max_atk) { e = SD_ERR_ALREADY_ATTACKED; break; }
        /* 空城计(id41)：对方正面空城计且我方无武将 → 不能攻阵 */
        for (int s = 0; s < SD_SLOTS && e == SD_OK; s++) {
            uint8_t u = d->pl[opp(player)].spell[s];
            if (u == NONE_UID || d->insts[u].face_down || d->insts[u].def_id != 41) continue;
            bool my_field_empty = true;
            for (int s2 = 0; s2 < SD_SLOTS; s2++)
                if (p->mon[s2] != NONE_UID) my_field_empty = false;
            if (my_field_empty) e = SD_ERR_SEALED;
        }
    } while (0);
    if (err) *err = e;
    return e == SD_OK;
}

bool sd_can_direct_attack(const sd_duel_t *d, uint8_t player)
{
    for (int s = 0; s < SD_SLOTS; s++)
        if (d->pl[opp(player)].mon[s] != NONE_UID) return false;
    return true;
}

bool sd_can_change_pos(const sd_duel_t *d, uint8_t player, uint8_t slot, int *err)
{
    int e = SD_OK;
    do {
        if (!d) { e = SD_ERR_ARG; break; }
        if (d->game_over || d->window.active || d->target.active) { e = SD_ERR_WINDOW_BUSY; break; }
        if (player != d->turn_player || !d->awaiting_command) { e = SD_ERR_NOT_YOUR_TURN; break; }
        if (!in_main(d->phase)) { e = SD_ERR_PHASE; break; }
        const sd_player_t *p = &d->pl[player];
        if (slot >= SD_SLOTS || p->mon[slot] == NONE_UID) { e = SD_ERR_ARG; break; }
        const sd_inst_t *c = &d->insts[p->mon[slot]];
        if (!c->face_down) { e = SD_ERR_WRONG_POS; break; }
        if (c->pos_changed) { e = SD_ERR_ALREADY_CHANGED; break; }
    } while (0);
    if (err) *err = e;
    return e == SD_OK;
}

/* ============ 激活管线 ============ */
static void begin_activation(sd_duel_t *d, uint8_t uid)
{
    sd_inst_t *c = inst(d, uid);
    const sd_card_def_t *def = &SD_CARDS[c->def_id];
    c->face_down = 0;
    d->ctx_uid = uid;
    if (def->type == SD_CARD_STRAT) { /* 计略发动 → 对方 W3 反击窗口 */
        d->negate_next = uid;
        open_window(d, SD_WIN_SPELL, c->owner, uid);
        if (!d->window.active) d->negate_next = NONE_UID;
    }
    q_push_back(d, Q(QK_RESOLVE_FX, uid, 0, 0, 0, 0));
    bool one_shot = (def->type == SD_CARD_STRAT &&
                     def->strat_kind != SD_SK_CONT && def->strat_kind != SD_SK_EQUIP &&
                     def->strat_kind != SD_SK_FIELD) ||
                    (def->type == SD_CARD_AMBUSH && def->ambush_kind != SD_AK_CONT);
    if (one_shot) q_push_back(d, Q(QK_SEND_GRAVE, uid, 0, 0, 0, 0));
}

/* ============ 队列执行 ============ */
static void exec_item(sd_duel_t *d, const sd_qitem_t *it)
{
    switch (it->kind) {
    case QK_SUMMON: {
        uint8_t pl = it->p0;
        sd_player_t *p = &d->pl[pl];
        if (it->p1 >= p->hand_n) return;
        uint8_t uid = p->hand[it->p1];
        const sd_card_def_t *c = &SD_CARDS[d->insts[uid].def_id];
        int need = sd_tribute_count(d, pl, c);
        if (need >= 1) {
            uint8_t t1 = p->mon[it->p3];
            if (t1 == NONE_UID) return;
            p->mon[it->p3] = NONE_UID;
            d->insts[t1].face_down = 0;
            send_to_grave(d, t1);
            if (need == 2) {
                uint8_t t2 = p->mon[(uint8_t)it->a0];
                if (t2 == NONE_UID) return;
                p->mon[(uint8_t)it->a0] = NONE_UID;
                d->insts[t2].face_down = 0;
                send_to_grave(d, t2);
            }
        }
        int slot = first_free_slot(p);
        if (slot < 0) { LOG("summon: no slot"); return; }
        zone_remove(p->hand, &p->hand_n, zone_find(p->hand, p->hand_n, uid));
        p->mon[slot] = uid;
        sd_inst_t *mi = inst(d, uid);
        mi->face_down = it->p2;
        mi->set_on_turn = (uint8_t)d->global_turn;
        mi->attacked = 0;
        mi->pos_changed = 0;
        p->summoned = 1;
        LOG("P%u summon %s (%s)", pl, c->name, mi->face_down ? "set" : "attack");
        if (!mi->face_down) after_summon(d, uid);
        break;
    }
    case QK_SET_SPELL: {
        uint8_t pl = it->p0;
        sd_player_t *p = &d->pl[pl];
        if (it->p1 >= p->hand_n) return;
        uint8_t uid = p->hand[it->p1];
        int slot = -1;
        for (int s = 0; s < SD_SLOTS; s++)
            if (p->spell[s] == NONE_UID) { slot = s; break; }
        if (slot < 0) return;
        zone_remove(p->hand, &p->hand_n, zone_find(p->hand, p->hand_n, uid));
        p->spell[slot] = uid;
        inst(d, uid)->face_down = 1;
        inst(d, uid)->set_on_turn = (uint8_t)d->global_turn;
        inst(d, uid)->equip_uid = NONE_UID;
        LOG("P%u sets %s", pl, SD_CARDS[d->insts[uid].def_id].name);
        break;
    }
    case QK_ACTIVATE: {
        uint8_t uid = it->p0;
        if (!on_field(d, uid)) return;
        LOG("P%u activates %s", d->insts[uid].owner, SD_CARDS[d->insts[uid].def_id].name);
        begin_activation(d, uid);
        break;
    }
    case QK_ATTACK: {
        uint8_t pl = it->p0;
        uint8_t au = d->pl[pl].mon[it->p1];
        if (au == NONE_UID) return;
        d->ctx_uid = au;
        push_card_triggers(d, au, SD_TR_ON_ATTACK_DECL); /* 甘宁/赵云：先于窗口结算 */
        if (it->p2 == 255 || d->pl[opp(pl)].mon[it->p2] == NONE_UID) {
            q_push_back(d, Q(QK_WINDOW, SD_WIN_ATTACK, pl, au, 0, 0));
            q_push_back(d, Q(QK_BATTLE, au, NONE_UID, 1, 0, 0));
        } else {
            uint8_t du = d->pl[opp(pl)].mon[it->p2];
            q_push_back(d, Q(QK_WINDOW, SD_WIN_ATTACK, pl, au, 0, 0));
            q_push_back(d, Q(QK_BATTLE, au, du, 0, 0, 0));
        }
        break;
    }
    case QK_BATTLE:
        do_battle(d, it->p0, it->p1 == NONE_UID ? 0 : it->p1, it->p2 == 1);
        break;
    case QK_AFTER_BATTLE_WIN: {
        uint8_t au = it->p0;
        if (!on_field_mon(d, au)) return;
        d->ctx_uid = au;
        d->battle_ctx_uid = it->p1;
        push_card_triggers(d, au, SD_TR_ON_BATTLE_WIN); /* 魏延 */
        break;
    }
    case QK_CHANGE_POS: {
        uint8_t u = d->pl[it->p0].mon[it->p1];
        if (u == NONE_UID) return;
        inst(d, u)->face_down = 0;
        inst(d, u)->pos_changed = 1;
        LOG("P%u %s -> attack pos", it->p0, SD_CARDS[d->insts[u].def_id].name);
        break;
    }
    case QK_RESOLVE_FX: {
        uint8_t uid = it->p0;
        if (d->negate_next == uid) {
            LOG("%s negated", SD_CARDS[d->insts[uid].def_id].name);
            d->negate_next = NONE_UID;
            return;
        }
        exec_effect(d, uid, it->p1, it->p2);
        break;
    }
    case QK_RESOLVE_TRIGGERS:
        /* 预留：批量触发（当前各路径直连 push_card_triggers） */
        break;
    case QK_WINDOW: {
        open_window(d, it->p0, it->p1, it->p2);
        if (d->window.active) {
            /* 窗口打开：把被打断的后续动作（战斗/破坏等）暂存，响应结算完再续 */
            d->after_n = 0;
            while (d->q_n > 0 && d->after_n < 4) d->after_window[d->after_n++] = q_pop(d);
        }
        break;
    }
    case QK_DESTROY: {
        uint8_t uid = it->p0;
        if (!on_field(d, uid)) return;
        send_to_grave(d, uid);
        push_card_triggers(d, uid, SD_TR_ON_DESTROY); /* 黄巾力士 */
        break;
    }
    case QK_DESTROY_BATTLE: {
        uint8_t victim = it->p0;
        if (!on_field_mon(d, victim)) return;
        d->battle_ctx_uid = it->p1; /* 典韦反击目标 */
        send_to_grave(d, victim);
        push_card_triggers(d, victim, SD_TR_ON_DESTROY);
        push_card_triggers(d, victim, SD_TR_ON_DESTROY_BY_BATTLE); /* 周泰/典韦 */
        break;
    }
    case QK_SEND_GRAVE:
        send_to_grave(d, it->p0);
        break;
    case QK_END_PHASE:
        advance_phase(d);
        break;
    case QK_END_TURN:
        if (!d->game_over) start_turn(d, opp(d->turn_player));
        break;
    default:
        break;
    }
}

/* 响应结算完毕后续上被打断的动作（战斗等）；窗口可再次打开则继续暂存 */
static void resume_after_window(sd_duel_t *d)
{
    while (!d->game_over && d->after_n > 0 && !d->window.active && !d->target.active) {
        for (int i = 0; i < d->after_n; i++) q_push_back(d, d->after_window[i]);
        d->after_n = 0;
        sd_pump(d);
    }
}

void sd_pump(sd_duel_t *d)
{
    if (!d) return;
    int guard = 0;
    while (!d->game_over && guard++ < 8192) {
        if (d->window.active || d->target.active) return;
        if (d->q_n == 0) {
            if (in_main(d->phase) || d->phase == SD_PHASE_BATTLE) d->awaiting_command = 1;
            return;
        }
        sd_qitem_t it = q_pop(d);
        exec_item(d, &it);
    }
}

/* ============ 创建 ============ */
sd_duel_t *sd_duel_create(const uint8_t *deck0, const uint8_t *deck1, uint8_t first, uint32_t seed)
{
    if (!deck0 || !deck1 || first > 1) return NULL;
    for (int i = 0; i < SD_MAX_DECK; i++)
        if (deck0[i] >= SD_CARD_COUNT || deck1[i] >= SD_CARD_COUNT) return NULL;
    sd_duel_t *d = calloc(1, sizeof(sd_duel_t));
    if (!d) return NULL;
    d->rng = seed ? seed : 1u;
    d->first_player = first;
    d->winner = 255;
    for (int p = 0; p < 2; p++) {
        sd_player_t *pl = &d->pl[p];
        pl->lp = SD_LP_INIT;
        for (int s = 0; s < SD_SLOTS; s++) { pl->mon[s] = NONE_UID; pl->spell[s] = NONE_UID; }
        pl->field_card = NONE_UID;
        const uint8_t *deck = p == 0 ? deck0 : deck1;
        for (int i = 0; i < SD_MAX_DECK; i++) {
            uint8_t uid = alloc_uid(d, deck[i], (uint8_t)p);
            if (uid == NONE_UID) { free(d); return NULL; }
            pl->deck[pl->deck_n++] = uid;
        }
        shuffle_deck(d, (uint8_t)p);
    }
    /* 开局各发 5 张（先攻第一回合不再抽卡） */
    draw_cards(d, 0, 5);
    draw_cards(d, 1, 5);
    d->global_turn = 0;
    d->turn_player = (uint8_t)(first ^ 1);
    start_turn(d, first);
    if (d->game_over) { free(d); return NULL; }
    return d;
}

void sd_duel_free(sd_duel_t *d) { free(d); }

/* ============ 指令提交 ============ */
static bool finish_and_pump(sd_duel_t *d)
{
    d->awaiting_command = 0;
    sd_pump(d);
    return true;
}

bool sd_submit_command(sd_duel_t *d, const sd_cmd_t *cmd)
{
    if (!d || !cmd || d->game_over) return false;

    /* ---- 选目标 ---- */
    if (d->target.active) {
        if (cmd->kind != SD_CMD_TARGET) return false;
        bool ok = false;
        for (int i = 0; i < d->target.cand_n; i++)
            if (d->target.cand[i] == cmd->p0) { ok = true; break; }
        if (!ok) return false;
        uint8_t chosen = cmd->p0;
        uint8_t src = d->target.uid;
        uint8_t fx_slot = d->target.fx_slot;
        uint8_t act_idx = d->target.act_idx;
        int sel = d->target.sel;
        uint8_t chooser = d->target.chooser;
        d->target.active = 0;
        if (sel == SD_SEL_INTERNAL_DISCARD) {
            sd_player_t *p = &d->pl[chooser];
            int idx = zone_find(p->hand, p->hand_n, chosen);
            if (idx >= 0) {
                zone_remove(p->hand, &p->hand_n, idx);
                zone_add(p->grave, &p->grave_n, SD_MAX_GRAVE, chosen);
                LOG("P%u discards %s", chooser, SD_CARDS[d->insts[chosen].def_id].name);
            }
            if (p->hand_n > SD_HAND_LIMIT) {
                d->target.active = 1;
                d->target.chooser = chooser;
                d->target.sel = SD_SEL_INTERNAL_DISCARD;
                d->target.cand_n = (uint8_t)collect_candidates(d, chooser, SD_SEL_INTERNAL_DISCARD, d->target.cand);
            } else if (d->phase == SD_PHASE_END && d->q_n == 0) {
                q_push_back(d, Q(QK_END_TURN, 0, 0, 0, 0, 0));
                sd_pump(d);
            }
            return true;
        }
        if (sel == SD_SEL_EQUIP_TARGET && src != NONE_UID) inst(d, src)->equip_uid = chosen;
        const sd_card_def_t *def = &SD_CARDS[d->insts[src].def_id];
        if (fx_slot < 2 && act_idx < def->fx[fx_slot].n_act)
            exec_one_act(d, src, &def->fx[fx_slot].act[act_idx], chosen);
        if (!d->game_over && act_idx + 1 < def->fx[fx_slot].n_act)
            q_push_front(d, Q(QK_RESOLVE_FX, src, fx_slot, (uint8_t)(act_idx + 1), 0, 0));
        sd_pump(d);
        resume_after_window(d);
        return true;
    }

    /* ---- 响应窗口 ---- */
    if (d->window.active) {
        if (cmd->kind != SD_CMD_RESPONSE) return false;
        uint8_t uid = cmd->p0;
        if (uid != NONE_UID) {
            bool ok = false;
            for (int i = 0; i < d->window.cand_n; i++)
                if (d->window.cand[i] == uid) { ok = true; break; }
            if (!ok) return false;
            sd_inst_t *c = inst(d, uid);
            const sd_card_def_t *def = &SD_CARDS[c->def_id];
            c->face_down = 0;
            LOG("P%u responds: %s", c->owner, def->name);
            d->ctx_uid = d->window.src_uid;
            if (def->ambush_kind != SD_AK_CONT)
                q_push_front(d, Q(QK_SEND_GRAVE, uid, 0, 0, 0, 0));
            q_push_front(d, Q(QK_RESOLVE_FX, uid, 0, 0, 0, 0));
        } else {
            d->negate_next = NONE_UID; /* 放弃反击：解除待无效标记 */
            LOG("P%u passes", d->window.responder);
        }
        d->window.active = 0;
        sd_pump(d);
        resume_after_window(d);
        return true;
    }

    /* ---- 主指令 ---- */
    if (!d->awaiting_command) return false;
    uint8_t tp = d->turn_player;
    switch (cmd->kind) {
    case SD_CMD_SUMMON:
    case SD_CMD_SET_MONSTER: {
        bool down = cmd->kind == SD_CMD_SET_MONSTER || cmd->p1 == 1;
        if (!sd_can_summon(d, tp, cmd->p0, down, NULL)) return false;
        const sd_player_t *p = &d->pl[tp];
        const sd_card_def_t *c = &SD_CARDS[d->insts[p->hand[cmd->p0]].def_id];
        int need = sd_tribute_count(d, tp, c);
        if (!tributes_valid(d, tp, cmd->p2, cmd->p3, need)) return false;
        d->stat_actions++;
        q_push_back(d, Q(QK_SUMMON, tp, cmd->p0, down, cmd->p2, cmd->p3));
        return finish_and_pump(d);
    }
    case SD_CMD_ACTIVATE: {
        bool from_zone = cmd->p1 == 1;
        if (!sd_can_activate(d, tp, from_zone ? cmd->p2 : cmd->p0, from_zone, NULL)) return false;
        uint8_t uid;
        if (from_zone) {
            uid = d->pl[tp].spell[cmd->p2];
        } else {
            uid = d->pl[tp].hand[cmd->p0];
            for (int s = 0; s < SD_SLOTS; s++) { /* 手牌 → 计略区 */
                if (d->pl[tp].spell[s] == NONE_UID) {
                    zone_remove(d->pl[tp].hand, &d->pl[tp].hand_n,
                                zone_find(d->pl[tp].hand, d->pl[tp].hand_n, uid));
                    d->pl[tp].spell[s] = uid;
                    break;
                }
            }
        }
        d->stat_actions++;
        q_push_back(d, Q(QK_ACTIVATE, uid, 0, 0, 0, 0));
        return finish_and_pump(d);
    }
    case SD_CMD_SET_SPELL:
        if (!sd_can_set_spell(d, tp, cmd->p0, NULL)) return false;
        d->stat_actions++;
        q_push_back(d, Q(QK_SET_SPELL, tp, cmd->p0, 0, 0, 0));
        return finish_and_pump(d);
    case SD_CMD_ATTACK:
        if (!sd_can_attack(d, tp, cmd->p0, NULL)) return false;
        d->stat_actions++;
        q_push_back(d, Q(QK_ATTACK, tp, cmd->p0, cmd->p1, 0, 0));
        return finish_and_pump(d);
    case SD_CMD_CHANGE_POS:
        if (!sd_can_change_pos(d, tp, cmd->p0, NULL)) return false;
        d->stat_actions++;
        q_push_back(d, Q(QK_CHANGE_POS, tp, cmd->p0, 0, 0, 0));
        return finish_and_pump(d);
    case SD_CMD_NEXT_PHASE:
        if (d->q_n != 0) return false;
        d->stat_actions++;
        q_push_back(d, Q(QK_END_PHASE, 0, 0, 0, 0, 0));
        return finish_and_pump(d);
    default:
        return false;
    }
}
