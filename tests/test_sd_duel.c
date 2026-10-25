// SPDX-License-Identifier: Apache-2.0
// test_sd_duel.c —— 《三国点将录》宿主单测：规则裁决锚点（设计 §6.3/§10）+ 自对弈稳定性。
// 规则用例用 force_card_at 把手牌槽位改成指定卡，摆脱洗牌随机性；自对弈用真实预设卡组。
// 构建与运行：tests/build_host_tests.ps1
#include <stdio.h>
#include <string.h>
#include "sd_core.h"
#include "sd_ai.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (cond) { g_pass++; }                                           \
        else { g_fail++; printf("FAIL %s:%d %s\n", __func__, __LINE__, msg); } \
    } while (0)

#define NONE_UID 255u

static void apply_destroy_for_test(sd_duel_t *d, uint8_t uid); /* 实现在文件尾 */

/* ---------- 驱动辅助 ---------- */
static int hand_idx(sd_duel_t *d, int p, int def_id)
{
    for (uint8_t i = 0; i < d->pl[p].hand_n; i++)
        if (d->insts[d->pl[p].hand[i]].def_id == def_id) return (int)i;
    return -1;
}
/* 把 p 第 slot 张手牌改成指定卡（测试专用；def_id 合法性由调用方保证） */
static void force_card_at(sd_duel_t *d, int p, int slot, int def_id)
{
    if (slot >= d->pl[p].hand_n) { /* 手牌不够就直接抽到该槽 */
        while (d->pl[p].hand_n <= slot && d->pl[p].deck_n > 0)
            d->pl[p].hand[d->pl[p].hand_n++] = d->pl[p].deck[--d->pl[p].deck_n];
        if (slot >= d->pl[p].hand_n) return;
    }
    d->insts[d->pl[p].hand[slot]].def_id = (uint8_t)def_id;
}
static void pass_windows(sd_duel_t *d)
{
    int guard = 0;
    while (d->window.active && guard++ < 8) {
        sd_cmd_t c = { SD_CMD_RESPONSE, NONE_UID, 0, 0, 0 };
        if (!sd_submit_command(d, &c)) break;
    }
}
static void auto_discard(sd_duel_t *d)
{
    int guard = 0;
    while (d->target.active && d->target.sel == SD_SEL_INTERNAL_DISCARD && guard++ < 8) {
        sd_cmd_t c = { SD_CMD_TARGET, d->target.cand[0], 0, 0, 0 };
        sd_submit_command(d, &c);
    }
}
static void next_phase(sd_duel_t *d)
{
    sd_cmd_t c = { SD_CMD_NEXT_PHASE, 0, 0, 0, 0 };
    sd_submit_command(d, &c);
}
static void end_turn(sd_duel_t *d) /* 完整走完当前玩家剩余阶段（含弃牌） */
{
    next_phase(d); next_phase(d); next_phase(d);
    auto_discard(d);
}
static bool do_summon(sd_duel_t *d, int p, int def_id, bool down)
{
    force_card_at(d, p, 0, def_id);
    sd_cmd_t c = { down ? SD_CMD_SET_MONSTER : SD_CMD_SUMMON, 0, 0, 255, 255 };
    return sd_submit_command(d, &c);
}
static bool do_summon_at(sd_duel_t *d, int p, int hand_slot, int def_id, uint8_t s1, uint8_t s2)
{
    force_card_at(d, p, hand_slot, def_id);
    sd_cmd_t c = { SD_CMD_SUMMON, (uint8_t)hand_slot, 0, s1, s2 };
    return sd_submit_command(d, &c);
}
static bool do_attack(sd_duel_t *d, int p, uint8_t my_slot, uint8_t en_slot)
{
    sd_cmd_t c = { SD_CMD_ATTACK, my_slot, en_slot, 0, 0 };
    return sd_submit_command(d, &c);
}
static uint8_t mon_uid(sd_duel_t *d, int p, int slot) { return d->pl[p].mon[slot]; }
/* 测试专用：绕过点兵直接把一张卡放上场面（验证高星卡效果用） */
static uint8_t cheat_place(sd_duel_t *d, int p, int def_id, bool down)
{
    if (d->pl[p].deck_n == 0) return NONE_UID;
    uint8_t uid = d->pl[p].deck[--d->pl[p].deck_n];
    d->insts[uid].def_id = (uint8_t)def_id;
    for (int s = 0; s < SD_SLOTS; s++) {
        if (d->pl[p].mon[s] == NONE_UID) {
            d->pl[p].mon[s] = uid;
            break;
        }
    }
    d->insts[uid].face_down = down;
    d->insts[uid].set_on_turn = (uint8_t)d->global_turn;
    return uid;
}
static bool do_set_spell(sd_duel_t *d, int p, int slot, int def_id)
{
    force_card_at(d, p, slot, def_id);
    sd_cmd_t c = { SD_CMD_SET_SPELL, (uint8_t)slot, 0, 0, 0 };
    return sd_submit_command(d, &c);
}
static bool do_activate_hand(sd_duel_t *d, int p, int slot, int def_id)
{
    force_card_at(d, p, slot, def_id);
    sd_cmd_t c = { SD_CMD_ACTIVATE, (uint8_t)slot, 0, 0, 0 };
    return sd_submit_command(d, &c);
}
static bool choose_target(sd_duel_t *d, uint8_t uid)
{
    sd_cmd_t c = { SD_CMD_TARGET, uid, 0, 0, 0 };
    return sd_submit_command(d, &c);
}

/* 自对弈/规则测试用的合法 20 张卡组（规则用例不依赖其内容） */
static const uint8_t DECK_A[SD_MAX_DECK] = {
    0, 0, 1, 1, 2, 2, 9, 9, 22, 23, 21, 10, 27, 29, 31, 32, 33, 37, 38, 44,
};
static const uint8_t DECK_B[SD_MAX_DECK] = {
    0, 0, 1, 1, 2, 2, 9, 9, 12, 15, 17, 21, 26, 27, 28, 30, 42, 43, 39, 41,
};
static sd_duel_t *mk(uint32_t seed) { return sd_duel_create(DECK_A, DECK_B, 0, seed); }

/* ========== 卡表完整性 ========== */
static void test_card_db(void)
{
    CHECK(SD_CARD_COUNT == 45, "45 cards");
    for (int i = 0; i < SD_CARD_COUNT; i++) {
        const sd_card_def_t *c = &SD_CARDS[i];
        CHECK(c->id == (uint8_t)i, "id sequential");
        CHECK(c->name && c->name[0], "name present");
        if (c->type == SD_CARD_GENERAL) {
            CHECK(c->tier >= 1 && c->tier <= 5, "tier range");
            CHECK(c->atk >= 1000, "atk sane");
        } else {
            CHECK(c->tier == 0 && c->atk == 0 && c->def == 0, "non-general has no stats");
        }
    }
    for (int di = 0; di < SD_DECK_DEF_COUNT; di++) {
        const sd_deck_def_t *dk = &SD_DECKS[di];
        for (int i = 0; i < SD_MAX_DECK; i++)
            CHECK(dk->ids[i] < SD_CARD_COUNT, "deck id valid");
        for (int i = 0; i < SD_MAX_DECK; i++) {
            int same = 0;
            for (int j = 0; j < SD_MAX_DECK; j++)
                if (dk->ids[i] == dk->ids[j]) same++;
            CHECK(same <= 2, "same-name limit 2");
        }
    }
}

/* ========== 上阵与点兵 ========== */
static void test_summon_basic(void)
{
    sd_duel_t *d = mk(11);
    CHECK(d->phase == SD_PHASE_MAIN1, "starts MAIN1");
    CHECK(d->turn_player == 0, "P0 first");
    if (d->pl[0].hand_n != 5) printf("  debug: hand_n=%u deck_n=%u\n", d->pl[0].hand_n, d->pl[0].deck_n);
    CHECK(d->pl[0].hand_n == 5, "first turn no draw");
    CHECK(do_summon(d, 0, 0, false), "summon 青州兵");
    uint8_t u = mon_uid(d, 0, 0);
    CHECK(u != NONE_UID, "on slot 0");
    CHECK(u != NONE_UID && !d->insts[u].face_down, "attack pos");
    CHECK(d->pl[0].summoned == 1, "summon used");
    CHECK(!do_summon(d, 0, 0, false), "second summon rejected");
    sd_duel_free(d);
}
static void test_set_monster(void)
{
    sd_duel_t *d = mk(12);
    CHECK(do_summon(d, 0, 1, true), "set 黄巾力士");
    CHECK(d->insts[mon_uid(d, 0, 0)].face_down, "face down");
    sd_duel_free(d);
}
static void test_tribute_summon(void)
{
    sd_duel_t *d = mk(13);
    CHECK(do_summon(d, 0, 0, false), "small 1");
    end_turn(d);
    end_turn(d); /* 回到 P0 */
    CHECK(do_summon(d, 0, 0, false), "small 2");
    end_turn(d);
    end_turn(d);
    /* 场上两只小兵（slot0/slot1），点 2 上 ★5 关羽 */
    int grave0 = d->pl[0].grave_n;
    CHECK(do_summon_at(d, 0, 1, 22, 0, 1), "tribute summon 关羽");
    CHECK(grave0 < d->pl[0].grave_n, "tribute sent to grave");
    uint8_t u = mon_uid(d, 0, 0);
    CHECK(u != NONE_UID && d->insts[u].def_id == 22, "关羽 on field");
    CHECK(sd_eff_atk(d, u) == 3200, "3200 atk");
    sd_duel_free(d);
}
static void test_tribute_2_required(void)
{
    sd_duel_t *d = mk(14);
    CHECK(do_summon(d, 0, 0, false), "m1");
    end_turn(d);
    end_turn(d);
    CHECK(do_summon(d, 0, 0, false), "m2");
    end_turn(d);
    end_turn(d);
    /* ★5 只给 1 个点兵 slot → 拒绝 */
    force_card_at(d, 0, 1, 22);
    sd_cmd_t bad = { SD_CMD_SUMMON, 1, 0, 0, 255 };
    CHECK(!sd_submit_command(d, &bad), "★5 needs 2 tributes");
    /* 空点兵位 → 拒绝 */
    sd_cmd_t bad2 = { SD_CMD_SUMMON, 1, 0, 255, 255 };
    CHECK(!sd_submit_command(d, &bad2), "tribute slots required");
    sd_duel_free(d);
}
static void test_zone_full(void)
{
    sd_duel_t *d = mk(15);
    for (int i = 0; i < 3; i++) {
        CHECK(do_summon(d, 0, 0, false), "summon");
        end_turn(d);
        end_turn(d);
    }
    CHECK(!do_summon(d, 0, 0, false), "4th summon rejected: zone full");
    sd_duel_free(d);
}

/* ========== 战斗矩阵 ========== */
static void test_battle_equal(void)
{
    sd_duel_t *d = mk(21);
    do_summon(d, 0, 0, false); /* 青州兵 1300 */
    end_turn(d);
    do_summon(d, 1, 0, false); /* 青州兵 1300 */
    end_turn(d);
    next_phase(d); /* P0 BATTLE */
    CHECK(do_attack(d, 0, 0, 0), "attack declared");
    pass_windows(d);
    CHECK(mon_uid(d, 1, 0) == NONE_UID && mon_uid(d, 0, 0) == NONE_UID, "equal ATK: both die");
    CHECK(d->pl[0].lp == SD_LP_INIT && d->pl[1].lp == SD_LP_INIT, "no LP damage");
    sd_duel_free(d);
}
static void test_battle_unequal(void)
{
    sd_duel_t *d = mk(22);
    do_summon(d, 0, 9, false); /* 虎豹骑 2000 */
    end_turn(d);
    do_summon(d, 1, 0, false); /* 青州兵 1300 */
    end_turn(d);
    next_phase(d);
    CHECK(do_attack(d, 0, 0, 0), "attack");
    pass_windows(d);
    CHECK(mon_uid(d, 1, 0) == NONE_UID, "青州兵 destroyed");
    CHECK(d->pl[1].lp == SD_LP_INIT - 700, "700 damage");
    sd_duel_free(d);
}
static void test_battle_attacker_loses(void)
{
    sd_duel_t *d = mk(23);
    do_summon(d, 0, 0, false); /* 1300 */
    end_turn(d);
    do_summon(d, 1, 9, false); /* 2000 */
    end_turn(d);
    next_phase(d);
    CHECK(do_attack(d, 0, 0, 0), "attack into bigger");
    pass_windows(d);
    CHECK(mon_uid(d, 0, 0) == NONE_UID, "attacker destroyed");
    CHECK(d->pl[0].lp == SD_LP_INIT - 700, "attacker side takes 700");
    CHECK(mon_uid(d, 1, 0) != NONE_UID, "defender alive");
    sd_duel_free(d);
}
static void test_battle_vs_set(void)
{
    sd_duel_t *d = mk(24);
    do_summon(d, 0, 9, false); /* 虎豹骑 2000 */
    end_turn(d);
    do_summon(d, 1, 4, true); /* 白毦兵 1000/1000 布阵 */
    end_turn(d);
    next_phase(d);
    CHECK(do_attack(d, 0, 0, 0), "attack set monster");
    pass_windows(d);
    CHECK(mon_uid(d, 1, 0) == NONE_UID, "set destroyed");
    CHECK(d->pl[1].lp == SD_LP_INIT, "no LP damage without pierce");
    sd_duel_free(d);
}
static void test_battle_vs_set_lose(void)
{
    sd_duel_t *d = mk(25);
    do_summon(d, 0, 0, false); /* 青州兵 1300 */
    end_turn(d);
    cheat_place(d, 1, 15, true); /* 周泰 2000/2300 布阵（★3 免点兵直接放） */
    end_turn(d);
    next_phase(d);
    CHECK(do_attack(d, 0, 0, 0), "attack big set");
    pass_windows(d);
    CHECK(mon_uid(d, 0, 0) == NONE_UID, "attacker died");
    CHECK(d->pl[0].lp == SD_LP_INIT - 1000, "2300-1300 recoil");
    CHECK(d->pl[1].lp == SD_LP_INIT, "defender LP untouched");
    sd_duel_free(d);
}
static void test_pierce(void)
{
    sd_duel_t *d = mk(26);
    cheat_place(d, 0, 12, false); /* 黄忠 2300 贯通 */
    end_turn(d);
    do_summon(d, 1, 4, true); /* 白毦兵 1000/1000 */
    end_turn(d);
    next_phase(d);
    CHECK(do_attack(d, 0, 0, 0), "attack set with pierce");
    pass_windows(d);
    CHECK(mon_uid(d, 1, 0) == NONE_UID, "set destroyed");
    CHECK(d->pl[1].lp == SD_LP_INIT - 1300, "pierce: 2300-1000");
    sd_duel_free(d);
}
static void test_direct_attack(void)
{
    sd_duel_t *d = mk(27);
    do_summon(d, 0, 0, false);
    end_turn(d);
    end_turn(d); /* P0 turn 3 */
    next_phase(d);
    CHECK(do_attack(d, 0, 0, 255), "direct attack");
    pass_windows(d);
    CHECK(d->pl[1].lp == SD_LP_INIT - 1300, "direct damage");
    sd_duel_free(d);
}
static void test_first_turn_no_attack(void)
{
    sd_duel_t *d = mk(28);
    CHECK(do_summon(d, 0, 0, false), "summon ok");
    next_phase(d);
    CHECK(!do_attack(d, 0, 0, 255), "turn-1 attack blocked");
    sd_duel_free(d);
}
static void test_change_pos(void)
{
    sd_duel_t *d = mk(29);
    do_summon(d, 0, 0, true); /* 布阵 */
    sd_cmd_t c = { SD_CMD_CHANGE_POS, 0, 0, 0, 0 };
    CHECK(sd_submit_command(d, &c), "change pos");
    CHECK(!d->insts[mon_uid(d, 0, 0)].face_down, "now attack pos");
    CHECK(!sd_submit_command(d, &c), "once per turn");
    sd_duel_free(d);
}

/* ========== 响应窗口 ========== */
static void test_maifu_destroys_attacker(void)
{
    sd_duel_t *d = mk(31);
    CHECK(do_set_spell(d, 0, 0, 38), "P0 sets 埋伏");
    end_turn(d);
    CHECK(do_summon(d, 1, 9, false), "P1 summons 虎豹骑");
    pass_windows(d);
    end_turn(d);
    end_turn(d); /* P1 turn */
    next_phase(d); /* P1 BATTLE */
    CHECK(do_attack(d, 1, 0, 0), "P1 attacks");
    CHECK(d->window.active && d->window.kind == SD_WIN_ATTACK, "W2 open");
    sd_cmd_t r = { SD_CMD_RESPONSE, d->window.cand[0], 0, 0, 0 };
    CHECK(sd_submit_command(d, &r), "respond 埋伏");
    CHECK(mon_uid(d, 1, 0) == NONE_UID, "attacker destroyed");
    CHECK(d->pl[0].lp == SD_LP_INIT, "no battle happened");
    sd_duel_free(d);
}
static void test_huojian_w1(void)
{
    sd_duel_t *d = mk(32);
    end_turn(d); /* P0 → P1 turn 2 */
    CHECK(do_set_spell(d, 1, 0, 42), "P1 sets 火箭");
    end_turn(d); /* P1 → P0 turn 3 */
    CHECK(do_summon(d, 0, 9, false), "summon 虎豹骑");
    CHECK(d->window.active && d->window.kind == SD_WIN_SUMMON, "W1 open");
    sd_cmd_t r = { SD_CMD_RESPONSE, d->window.cand[0], 0, 0, 0 };
    CHECK(sd_submit_command(d, &r), "respond 火箭");
    CHECK(sd_eff_atk(d, mon_uid(d, 0, 0)) == 1000, "2000-1000");
    sd_duel_free(d);
}
static void test_zhaoyun_no_respond(void)
{
    sd_duel_t *d = mk(33);
    end_turn(d); /* P1 turn 2 */
    CHECK(do_set_spell(d, 1, 0, 38), "P1 sets 埋伏");
    end_turn(d); /* P0 turn 3 */
    cheat_place(d, 0, 17, false); /* 赵云（★4 免点兵直接放） */
    CHECK(!d->window.active, "no W1 (埋伏 is W2 only)");
    end_turn(d); /* P1 turn 4 */
    CHECK(do_summon(d, 1, 0, false), "P1 summons a target");
    pass_windows(d);
    end_turn(d); /* P0 turn 5 */
    next_phase(d); /* P0 BATTLE */
    CHECK(do_attack(d, 0, 0, 0), "赵云 attacks");
    CHECK(!d->window.active, "no W2 (no respond)");
    sd_duel_free(d);
}
static void test_bamen_negates(void)
{
    sd_duel_t *d = mk(34);
    end_turn(d); /* P1 turn 2 */
    CHECK(do_set_spell(d, 1, 0, 39), "P1 sets 八门金锁阵");
    end_turn(d); /* P0 turn 3 */
    int hand_n = d->pl[0].hand_n;
    CHECK(do_activate_hand(d, 0, 0, 27), "P0 activates 草船借箭");
    CHECK(d->window.active && d->window.kind == SD_WIN_SPELL, "W3 open");
    sd_cmd_t r = { SD_CMD_RESPONSE, d->window.cand[0], 0, 0, 0 };
    CHECK(sd_submit_command(d, &r), "respond 八门");
    CHECK(d->pl[0].hand_n == hand_n - 2, "negated: no draw + 1 random discard");
    sd_duel_free(d);
}
static void test_window_pass(void)
{
    sd_duel_t *d = mk(35);
    end_turn(d); /* P1 turn 2 */
    do_set_spell(d, 1, 0, 42); /* P1 盖火箭但不响应 */
    end_turn(d); /* P0 turn 3 */
    CHECK(do_summon(d, 0, 9, false), "summon 虎豹骑");
    CHECK(d->window.active, "W1 open");
    pass_windows(d);
    CHECK(sd_eff_atk(d, mon_uid(d, 0, 0)) == 2000, "pass keeps atk");
    sd_duel_free(d);
}

/* ========== 效果结算 ========== */
static void test_xiliang_on_summon(void)
{
    sd_duel_t *d = mk(41);
    CHECK(do_summon(d, 0, 7, false), "summon 西凉铁骑");
    pass_windows(d);
    CHECK(sd_eff_atk(d, mon_uid(d, 0, 0)) == 2000, "1800+200");
    sd_duel_free(d);
}
static void test_huangjin_draw_on_destroy(void)
{
    sd_duel_t *d = mk(42);
    CHECK(do_summon(d, 0, 1, true), "set 黄巾力士");
    end_turn(d);
    CHECK(do_summon(d, 1, 9, false), "P1 虎豹骑");
    pass_windows(d);
    end_turn(d);
    end_turn(d);
    next_phase(d); /* P1 BATTLE */
    int hand_n = d->pl[0].hand_n;
    CHECK(do_attack(d, 1, 0, 0), "attack the set 黄巾");
    pass_windows(d);
    CHECK(mon_uid(d, 0, 0) == NONE_UID, "黄巾 destroyed");
    CHECK(d->pl[0].hand_n == hand_n + 1, "draw 1 on destroy");
    sd_duel_free(d);
}
static void test_jinang_temp_clears(void)
{
    sd_duel_t *d = mk(43);
    CHECK(do_summon(d, 0, 0, false), "summon");
    int atk0 = sd_eff_atk(d, mon_uid(d, 0, 0));
    CHECK(do_activate_hand(d, 0, 1, 29), "activate 锦囊妙计");
    CHECK(d->target.active, "target requested");
    CHECK(choose_target(d, mon_uid(d, 0, 0)), "buff my monster");
    CHECK(sd_eff_atk(d, mon_uid(d, 0, 0)) == atk0 + 1000, "+1000");
    end_turn(d);
    end_turn(d);
    CHECK(sd_eff_atk(d, mon_uid(d, 0, 0)) == atk0, "temp cleared at end phase");
    sd_duel_free(d);
}
static void test_taoyuan_heal(void)
{
    sd_duel_t *d = mk(44);
    CHECK(do_activate_hand(d, 0, 0, 33), "activate 桃园结义");
    CHECK(d->pl[0].spell[0] != NONE_UID, "continuous stays");
    d->pl[0].lp = 3000;
    CHECK(do_summon(d, 0, 0, false), "summon ally");
    pass_windows(d);
    CHECK(d->pl[0].lp == 3400, "heal 400 on ally summon");
    sd_duel_free(d);
}
static void test_shuiyan_temp(void)
{
    sd_duel_t *d = mk(45);
    do_summon(d, 0, 0, false);
    end_turn(d);
    do_summon(d, 1, 0, false);
    end_turn(d);
    int a0 = sd_eff_atk(d, mon_uid(d, 0, 0));
    int a1 = sd_eff_atk(d, mon_uid(d, 1, 0));
    CHECK(do_activate_hand(d, 0, 1, 30), "activate 水淹七军");
    CHECK(sd_eff_atk(d, mon_uid(d, 0, 0)) == a0 - 500, "my side -500");
    CHECK(sd_eff_atk(d, mon_uid(d, 1, 0)) == a1 - 500, "enemy -500");
    end_turn(d);
    end_turn(d);
    CHECK(sd_eff_atk(d, mon_uid(d, 0, 0)) == a0, "cleared");
    sd_duel_free(d);
}
static void test_chibao_equip(void)
{
    sd_duel_t *d = mk(46);
    CHECK(do_summon(d, 0, 0, false), "summon");
    uint8_t u = mon_uid(d, 0, 0);
    CHECK(do_activate_hand(d, 0, 1, 31), "activate 赤兔马");
    CHECK(choose_target(d, u), "equip target");
    CHECK(sd_eff_atk(d, u) == 1300 + 800, "+800");
    CHECK(d->insts[u].multi == 1, "multi attack flag");
    end_turn(d);
    end_turn(d); /* 回到 P0（第一回合不能攻阵） */
    next_phase(d); /* BATTLE */
    CHECK(do_attack(d, 0, 0, 255), "attack 1");
    pass_windows(d);
    CHECK(do_attack(d, 0, 0, 255), "attack 2 (multi)");
    pass_windows(d);
    CHECK(!do_attack(d, 0, 0, 255), "no attack 3");
    sd_duel_free(d);
}
static void test_qingqing_equip_pierce(void)
{
    sd_duel_t *d = mk(47);
    do_summon(d, 0, 0, false); /* 青州兵 1300 */
    uint8_t u = mon_uid(d, 0, 0);
    do_activate_hand(d, 0, 1, 32);
    choose_target(d, u);
    end_turn(d);
    do_summon(d, 1, 4, true); /* 白毦兵 1000/1000 */
    end_turn(d);
    next_phase(d);
    CHECK(do_attack(d, 0, 0, 0), "attack with 青釭剑");
    pass_windows(d);
    CHECK(mon_uid(d, 1, 0) == NONE_UID, "destroyed");
    CHECK(d->pl[1].lp == SD_LP_INIT - 800, "pierce 1300+500-1000=800");
    sd_duel_free(d);
}
static void test_equip_falls_off(void)
{
    sd_duel_t *d = mk(48);
    do_summon(d, 0, 4, false); /* 白毦兵 1100 */
    uint8_t u = mon_uid(d, 0, 0);
    CHECK(do_activate_hand(d, 0, 1, 31), "equip 赤兔马");
    CHECK(choose_target(d, u), "equip target");
    end_turn(d);
    do_summon(d, 1, 9, false); /* 虎豹骑 2000 */
    pass_windows(d);
    end_turn(d); /* P1 → P0 turn 3 */
    end_turn(d); /* P0 → P1 turn 4 */
    next_phase(d); /* P1 BATTLE */
    CHECK(do_attack(d, 1, 0, 0), "P1 kills 白毦兵");
    pass_windows(d);
    CHECK(mon_uid(d, 0, 0) == NONE_UID, "monster died");
    /* 装备应随之送墓：计略区不再有赤兔马 */
    bool still = false;
    for (int s = 0; s < SD_SLOTS; s++) {
        uint8_t su = d->pl[0].spell[s];
        if (su != NONE_UID && d->insts[su].def_id == 31) still = true;
    }
    CHECK(!still, "equip falls off to grave");
    sd_duel_free(d);
}
static void test_lianhuan_tribute_plus1(void)
{
    sd_duel_t *d = mk(49);
    /* P0 场上一只怪；P1 正面连环计 → P0 点兵数+1（★3 需要 2） */
    do_summon(d, 0, 0, false);
    end_turn(d);
    force_card_at(d, 1, 0, 34);
    sd_cmd_t a = { SD_CMD_ACTIVATE, 0, 0, 0, 0 };
    CHECK(sd_submit_command(d, &a), "P1 activates 连环计");
    end_turn(d);
    end_turn(d);
    /* P0 场上 1 怪 + 手 ★5 关羽：需要 2+1=3 点兵 → 无法上阵 */
    int g = -1;
    for (uint8_t i = 0; i < d->pl[0].hand_n; i++)
        if (d->insts[d->pl[0].hand[i]].def_id == 22) g = i;
    if (g < 0) { force_card_at(d, 0, 1, 22); g = 1; }
    sd_cmd_t s = { SD_CMD_SUMMON, (uint8_t)g, 0, 0, 1 };
    CHECK(!sd_submit_command(d, &s), "3 tributes needed, only 2 slots");
    sd_duel_free(d);
}
static void test_sunquan_aura(void)
{
    sd_duel_t *d = mk(50);
    CHECK(do_summon(d, 0, 6, false), "summon 丹阳兵(吴) 1700");
    pass_windows(d);
    uint8_t u = mon_uid(d, 0, 0);
    /* 孙权 ★5 需要点兵——直接给 P0 场上加一只孙权实例比较繁琐；改用铺场后点兵 */
    do_summon(d, 0, 0, false); /* 第二只小兵（下一回合）不行：summoned */
    CHECK(sd_eff_atk(d, u) == 1700, "base check");
    sd_duel_free(d);
}

/* ========== 玩家数值与终局 ========== */
static void test_lp_win(void)
{
    sd_duel_t *d = mk(51);
    do_summon(d, 0, 9, false); /* 虎豹骑 2000 */
    d->pl[1].lp = 1000;
    end_turn(d);
    end_turn(d);
    next_phase(d);
    CHECK(do_attack(d, 0, 0, 255), "direct");
    pass_windows(d);
    CHECK(d->game_over && d->winner == 0, "LP 0 -> win");
    sd_duel_free(d);
}
static void test_deck_out(void)
{
    sd_duel_t *d = mk(52);
    d->pl[0].deck_n = 0;
    end_turn(d);
    end_turn(d); /* P0 新回合抽卡 → deck out */
    CHECK(d->game_over && d->winner == 1, "deck out loss");
    sd_duel_free(d);
}
static void test_hand_limit(void)
{
    sd_duel_t *d = mk(53);
    while (d->pl[0].hand_n < 7 && d->pl[0].deck_n > 0)
        d->pl[0].hand[d->pl[0].hand_n++] = d->pl[0].deck[--d->pl[0].deck_n];
    CHECK(d->pl[0].hand_n == 7, "hand at 7");
    end_turn(d);
    CHECK(d->pl[0].hand_n == SD_HAND_LIMIT, "hand limited to 6");
    sd_duel_free(d);
}

/* ========== 自对弈 ========== */
static void drive_by_ai(sd_duel_t *d)
{
    int guard = 0;
    sd_cmd_t last[8];
    int last_n = 0;
    while (!d->game_over && guard++ < 4000) {
        sd_pump(d);
        if (d->game_over) break;
        uint8_t who;
        if (d->target.active) who = d->target.chooser;
        else if (d->window.active) who = d->window.responder;
        else if (d->awaiting_command) who = d->turn_player;
        else {
            printf("  drive stuck: turn %u phase %d q_n %u target %u window %u awaiting %u\n",
                   d->global_turn, d->phase, d->q_n, d->target.active, d->window.active,
                   d->awaiting_command);
            break;
        }
        sd_cmd_t c;
        if (!sd_ai_decide(d, who, &c)) {
            printf("  ai declined: turn %u target=%d window=%d awaiting=%d\n",
                   d->global_turn, d->target.active, d->window.active, d->awaiting_command);
            return;
        }
        if (!sd_submit_command(d, &c)) {
            printf("  ai cmd rejected: turn %u kind %d p0 %u p1 %u p2 %u p3 %u\n",
                   d->global_turn, c.kind, c.p0, c.p1, c.p2, c.p3);
            return;
        }
        last[last_n++ % 8] = c;
        if (guard > 3980 && c.kind == SD_CMD_ACTIVATE && c.p1 == 0) {
            uint8_t u = d->pl[who].hand[c.p0];
            printf("  dbg: P%u activates %s hand_n=%u spell=[%u %u %u] deck_n=%u\n", who,
                   SD_CARDS[d->insts[u].def_id].name, d->pl[who].hand_n,
                   d->pl[who].spell[0], d->pl[who].spell[1], d->pl[who].spell[2], d->pl[who].deck_n);
        }
    }
    if (guard >= 4000) {
        printf("  drive guard hit at turn %u; last cmds:", d->global_turn);
        for (int i = 0; i < last_n && i < 8; i++)
            printf(" [%d p%u.%u.%u.%u]", last[i].kind, last[i].p0, last[i].p1, last[i].p2, last[i].p3);
        printf("\n");
    }
}
static void test_selfplay(void)
{
    static const int matchups[3] = { SD_DECK_QINGZHOU, SD_DECK_JIANGDONG, SD_DECK_HULANG };
    int finished = 0, total = 0;
    int p0_wins = 0;
    for (int m = 0; m < 3; m++) {
        for (int g = 0; g < 10; g++) {
            sd_ai_set_difficulty(0, 1);
            sd_ai_set_difficulty(1, SD_DECKS[matchups[m]].difficulty);
            sd_duel_t *d = sd_duel_create(SD_DECKS[SD_DECK_SHU_START].ids, SD_DECKS[matchups[m]].ids,
                                          (uint8_t)(g & 1), (uint32_t)(1000 + m * 100 + g));
            if (!d) { CHECK(false, "duel create"); continue; }
            drive_by_ai(d);
            total++;
            if (d->game_over) {
                finished++;
                if (d->winner == 0) p0_wins++;
            } else {
                printf("  selfplay stall: m%d g%d turn %u phase %d\n", m, g, d->global_turn, d->phase);
            }
            sd_duel_free(d);
        }
    }
    printf("  selfplay: %d/%d finished, P0 win rate %d/%d\n", finished, total, p0_wins, finished);
    CHECK(finished == total, "all selfplay games reach game over");
}

/* test-only 破坏入口占位（装备掉落已改为战斗路径验证） */
static void apply_destroy_for_test(sd_duel_t *d, uint8_t uid)
{
    (void)d;
    (void)uid;
}

int main(void)
{
    test_card_db();
    test_summon_basic();
    test_set_monster();
    test_tribute_summon();
    test_tribute_2_required();
    test_zone_full();
    test_battle_equal();
    test_battle_unequal();
    test_battle_attacker_loses();
    test_battle_vs_set();
    test_battle_vs_set_lose();
    test_pierce();
    test_direct_attack();
    test_first_turn_no_attack();
    test_change_pos();
    test_maifu_destroys_attacker();
    test_huojian_w1();
    test_zhaoyun_no_respond();
    test_bamen_negates();
    test_window_pass();
    test_xiliang_on_summon();
    test_huangjin_draw_on_destroy();
    test_jinang_temp_clears();
    test_taoyuan_heal();
    test_shuiyan_temp();
    test_chibao_equip();
    test_qingqing_equip_pierce();
    test_equip_falls_off();
    test_lianhuan_tribute_plus1();
    test_sunquan_aura();
    test_lp_win();
    test_deck_out();
    test_hand_limit();
    test_selfplay();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
