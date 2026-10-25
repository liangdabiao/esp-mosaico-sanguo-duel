// SPDX-License-Identifier: Apache-2.0
// sd_cards.c —— 45 张卡定义与 4 副预设卡组（设计文档 §4/§5 的数据化落地）。
// 全部效果由原语+选择子+参数表达，无脚本。改动数值后必须跑宿主测试与自对弈。
#include "sd_core.h"

/* 效果槽速记宏：T=触发 S=选择子 a/b=参数 */
#define FX0(tr, _op, _sel, _a, _b) { (tr), 1, { { (_op), (_sel), (_a), (_b) } } }
#define FX2(tr, op1, sel1, a1, b1, op2, sel2, a2, b2) \
    { (tr), 2, { { (op1), (sel1), (a1), (b1) }, { (op2), (sel2), (a2), (b2) } } }
#define NO_FX { SD_TR_NONE, 0, { { 0, 0, 0, 0 } } }

/* 光环/静态条件位（b 参数，OP_AURA_* 与 OP_SEAL_ATTACK 用） */
#define COND_SET_DOWN   0x01   /* 仅布阵(背面)武将 */
#define COND_SAME_FACTION 0x02 /* 与来源卡同势力 */
#define COND_SAME_OWNER 0x04   /* 与来源卡同主 */
#define COND_BATTLE_VS_SET 0x08 /* 仅战斗中对布阵武将（关羽） */

const sd_card_def_t SD_CARDS[] = {
    /* ---- 武将 ★1 ---- */
    { 0, "青州兵",   SD_CARD_GENERAL, SD_F_QUN, SD_U_QIANG, 1, 1300, 900, 0, 0, 0, { NO_FX } },
    { 1, "黄巾力士", SD_CARD_GENERAL, SD_F_QUN, SD_U_QIANG, 1, 1000, 500, 0, 0, 0,
      { FX0(SD_TR_ON_DESTROY, SD_OP_DRAW, SD_SEL_OWNER_LP, 1, 0) } },
    { 2, "弓箭手",   SD_CARD_GENERAL, SD_F_QUN, SD_U_GONG, 1, 1200, 600, 0, 0, 0, { NO_FX } },
    { 3, "藤甲兵",   SD_CARD_GENERAL, SD_F_SHU, SD_U_QIANG, 1, 1400, 800, 0, 0, 0,
      { FX0(SD_TR_NONE, SD_OP_AURA_DEF, SD_SEL_THIS, 300, COND_SET_DOWN) } },
    { 4, "白毦兵",   SD_CARD_GENERAL, SD_F_SHU, SD_U_WEI, 1, 1100, 1000, 0, 0, 0, { NO_FX } },
    /* ---- 武将 ★2 ---- */
    { 5, "白马义从", SD_CARD_GENERAL, SD_F_QUN, SD_U_QI, 2, 1900, 1000, 0, 0, 0, { NO_FX } },
    { 6, "丹阳兵",   SD_CARD_GENERAL, SD_F_WU, SD_U_QIANG, 2, 1700, 1600, 0, 0, 0, { NO_FX } },
    { 7, "西凉铁骑", SD_CARD_GENERAL, SD_F_QUN, SD_U_QI, 2, 1800, 1200, 0, 0, 0,
      { FX0(SD_TR_ON_SUMMON, SD_OP_ATK_MOD, SD_SEL_ACTOR, 200, 0) } },
    { 8, "无当飞军", SD_CARD_GENERAL, SD_F_SHU, SD_U_GONG, 2, 1700, 1400, 0, 0, 0, { NO_FX } },
    { 9, "虎豹骑",   SD_CARD_GENERAL, SD_F_WEI, SD_U_QI, 2, 2000, 1500, 0, 0, 0, { NO_FX } },
    /* ---- 武将 ★3 ---- */
    { 10, "张辽", SD_CARD_GENERAL, SD_F_WEI, SD_U_QI, 3, 2400, 1600, 0, 0, 0,
      { FX0(SD_TR_ON_SUMMON, SD_OP_SEAL_ATTACK, SD_SEL_CHOICE_ENEMY_MON, 1, 0) } },
    { 11, "徐晃", SD_CARD_GENERAL, SD_F_WEI, SD_U_QI, 3, 2300, 1600, 0, 0, 0,
      { FX0(SD_TR_ON_SUMMON, SD_OP_ATK_MOD, SD_SEL_ENEMY_HIGHEST_ATK, -500, 0) } },
    { 12, "黄忠", SD_CARD_GENERAL, SD_F_SHU, SD_U_GONG, 3, 2300, 1400, 0, 0, 0,
      { FX0(SD_TR_NONE, SD_OP_PIERCE, SD_SEL_THIS, 0, 0) } },
    { 13, "魏延", SD_CARD_GENERAL, SD_F_SHU, SD_U_QIANG, 3, 2400, 1500, 0, 0, 0,
      { FX0(SD_TR_ON_BATTLE_WIN, SD_OP_ATK_MOD, SD_SEL_ACTOR, 300, 0) } },
    { 14, "甘宁", SD_CARD_GENERAL, SD_F_WU, SD_U_QIANG, 3, 2400, 1200, 0, 0, 0,
      { FX0(SD_TR_ON_ATTACK_DECL, SD_OP_ATK_MOD, SD_SEL_ACTOR, 300, 1) } },
    { 15, "周泰", SD_CARD_GENERAL, SD_F_WU, SD_U_WEI, 3, 2000, 2300, 0, 0, 0,
      { FX0(SD_TR_ON_DESTROY_BY_BATTLE, SD_OP_HEAL, SD_SEL_OWNER_LP, 1000, 0) } },
    { 16, "华雄", SD_CARD_GENERAL, SD_F_QUN, SD_U_DAO, 3, 2200, 1400, 0, 0, 0, { NO_FX } },
    /* ---- 武将 ★4 ---- */
    { 17, "赵云", SD_CARD_GENERAL, SD_F_SHU, SD_U_QI, 4, 2800, 2000, 0, 0, 0,
      { FX0(SD_TR_ON_ATTACK_DECL, SD_OP_NO_RESPOND, SD_SEL_NONE, 1, 0) } },
    { 18, "许褚", SD_CARD_GENERAL, SD_F_WEI, SD_U_WEI, 4, 2700, 1900, 0, 0, 0, { NO_FX } },
    { 19, "马超", SD_CARD_GENERAL, SD_F_SHU, SD_U_QI, 4, 2800, 1700, 0, 0, 0,
      { FX0(SD_TR_NONE, SD_OP_PIERCE, SD_SEL_THIS, 0, 0) } },
    { 20, "太史慈", SD_CARD_GENERAL, SD_F_WU, SD_U_GONG, 4, 2600, 1800, 0, 0, 0, { NO_FX } },
    { 21, "典韦", SD_CARD_GENERAL, SD_F_WEI, SD_U_WEI, 4, 2900, 1500, 0, 0, 0,
      { FX0(SD_TR_ON_DESTROY_BY_BATTLE, SD_OP_DESTROY, SD_SEL_BATTLE_OPPONENT, 0, 0) } },
    /* ---- 武将 ★5 ---- */
    { 22, "关羽", SD_CARD_GENERAL, SD_F_SHU, SD_U_DAO, 5, 3200, 2200, 0, 0, 0,
      { FX0(SD_TR_NONE, SD_OP_PIERCE, SD_SEL_THIS, 0, 0),
        FX0(SD_TR_NONE, SD_OP_AURA_ATK, SD_SEL_THIS, 500, COND_BATTLE_VS_SET) } },
    { 23, "吕布", SD_CARD_GENERAL, SD_F_QUN, SD_U_QI, 5, 3400, 2000, 0, 0, 0,
      { FX0(SD_TR_NONE, SD_OP_MULTI, SD_SEL_THIS, 0, 0) } },
    { 24, "曹操", SD_CARD_GENERAL, SD_F_WEI, SD_U_JUNSHI, 5, 3000, 2400, 0, 0, 0,
      { FX0(SD_TR_ON_SUMMON, SD_OP_DRAW, SD_SEL_OWNER_LP, 2, 0) } },
    { 25, "刘备", SD_CARD_GENERAL, SD_F_SHU, SD_U_JUNSHI, 5, 2800, 2600, 0, 0, 0,
      { FX0(SD_TR_ON_TURN_START, SD_OP_HEAL, SD_SEL_OWNER_LP, 500, 0) } },
    { 26, "孙权", SD_CARD_GENERAL, SD_F_WU, SD_U_JUNSHI, 5, 2900, 2400, 0, 0, 0,
      { FX0(SD_TR_NONE, SD_OP_AURA_ATK, SD_SEL_ALL_FIELD, 300, COND_SAME_FACTION | COND_SAME_OWNER) } },
    /* ---- 计略 ---- */
    { 27, "草船借箭", SD_CARD_STRAT, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, SD_SK_NORMAL, 0, 0,
      { FX0(SD_TR_ACTIVATED, SD_OP_DRAW, SD_SEL_OWNER_LP, 2, 0) } },
    { 28, "火计", SD_CARD_STRAT, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, SD_SK_QUICK, 0, 0,
      { FX0(SD_TR_ACTIVATED, SD_OP_DESTROY, SD_SEL_CHOICE_ENEMY_SET, 0, 0) } },
    { 29, "锦囊妙计", SD_CARD_STRAT, SD_F_SHU, SD_U_JUNSHI, 0, 0, 0, SD_SK_QUICK, 0, 0,
      { FX0(SD_TR_ACTIVATED, SD_OP_ATK_MOD, SD_SEL_CHOICE_MY_MON, 1000, 1) } },
    { 30, "水淹七军", SD_CARD_STRAT, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, SD_SK_NORMAL, 0, 0,
      { FX0(SD_TR_ACTIVATED, SD_OP_ATK_MOD, SD_SEL_ALL_FIELD, -500, 1) } },
    { 31, "赤兔马", SD_CARD_STRAT, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, SD_SK_EQUIP, 0, 0,
      { FX2(SD_TR_ACTIVATED, SD_OP_ATK_MOD, SD_SEL_EQUIP_TARGET, 800, 0,
            SD_OP_MULTI, SD_SEL_EQUIP_TARGET, 0, 0) } },
    { 32, "青釭剑", SD_CARD_STRAT, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, SD_SK_EQUIP, 0, 0,
      { FX2(SD_TR_ACTIVATED, SD_OP_ATK_MOD, SD_SEL_EQUIP_TARGET, 500, 0,
            SD_OP_PIERCE, SD_SEL_EQUIP_TARGET, 0, 0) } },
    { 33, "桃园结义", SD_CARD_STRAT, SD_F_SHU, SD_U_JUNSHI, 0, 0, 0, SD_SK_CONT, 0, 0,
      { FX0(SD_TR_ON_ALLY_SUMMON, SD_OP_HEAL, SD_SEL_OWNER_LP, 400, 0) } },
    { 34, "连环计", SD_CARD_STRAT, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, SD_SK_CONT, 0, 0,
      { FX0(SD_TR_NONE, SD_OP_TRIBUTE_COST, SD_SEL_NONE, 1, 0) } },
    { 35, "屯田令", SD_CARD_STRAT, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, SD_SK_NORMAL, 0, 0,
      { FX2(SD_TR_ACTIVATED, SD_OP_DRAW, SD_SEL_OWNER_LP, 1, 0,
            SD_OP_HEAL, SD_SEL_OWNER_LP, 600, 0) } },
    { 36, "挟天子", SD_CARD_STRAT, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, SD_SK_NORMAL, 0, 0,
      { FX0(SD_TR_ACTIVATED, SD_OP_DISCARD, SD_SEL_RANDOM_OPP_HAND, 1, 0) } },
    { 37, "火烧连营", SD_CARD_STRAT, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, SD_SK_NORMAL, 0, 0,
      { FX0(SD_TR_ACTIVATED, SD_OP_DESTROY, SD_SEL_ATK_LE, 2000, 0) } },
    /* ---- 伏兵 ---- */
    { 38, "埋伏", SD_CARD_AMBUSH, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, 0, SD_AK_NORMAL, SD_WIN_ATTACK,
      { FX0(SD_TR_ACTIVATED, SD_OP_DESTROY, SD_SEL_ATTACKER, 0, 0) } },
    { 39, "八门金锁阵", SD_CARD_AMBUSH, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, 0, SD_AK_COUNTER, SD_WIN_SPELL,
      { FX2(SD_TR_ACTIVATED, SD_OP_NEGATE, SD_SEL_NONE, 0, 0,
            SD_OP_DISCARD, SD_SEL_RANDOM_OPP_HAND, 1, 0) } },
    { 40, "诈降", SD_CARD_AMBUSH, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, 0, SD_AK_NORMAL, 0,
      { FX2(SD_TR_ACTIVATED, SD_OP_DESTROY, SD_SEL_CHOICE_ENEMY_MON, 0, 0,
            SD_OP_HEAL, SD_SEL_OWNER_LP, 800, 0) } },
    { 41, "空城计", SD_CARD_AMBUSH, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, 0, SD_AK_CONT, 0,
      { FX0(SD_TR_NONE, SD_OP_SEAL_ATTACK, SD_SEL_NONE, 1, 1) } },
    { 42, "火箭", SD_CARD_AMBUSH, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, 0, SD_AK_NORMAL, SD_WIN_SUMMON,
      { FX0(SD_TR_ACTIVATED, SD_OP_ATK_MOD, SD_SEL_ATTACKER, -1000, 0) } },
    { 43, "拒马阵", SD_CARD_AMBUSH, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, 0, SD_AK_CONT, 0,
      { FX0(SD_TR_NONE, SD_OP_AURA_DEF, SD_SEL_ALL_FIELD, 500, COND_SET_DOWN | COND_SAME_OWNER) } },
    { 44, "铁蒺藜", SD_CARD_AMBUSH, SD_F_QUN, SD_U_JUNSHI, 0, 0, 0, 0, SD_AK_NORMAL, SD_WIN_DESTROY,
      { FX0(SD_TR_ACTIVATED, SD_OP_DAMAGE, SD_SEL_OPPONENT_LP, 1000, 0) } },
};

const int SD_CARD_COUNT = (int)(sizeof(SD_CARDS) / sizeof(SD_CARDS[0]));

/* ---- 预设卡组（设计 §5；各 20 张，同名≤2 已校验于宿主测试） ---- */
const sd_deck_def_t SD_DECKS[SD_DECK_DEF_COUNT] = {
    [SD_DECK_QINGZHOU] = {
        "青州军", "教学军团：直白铺场互撞", {
            0, 0, 1, 2, 2, 5, 5, 7, 16, 11, 3,
            27, 27, 35, 28, 38, 44, 41, 29, 30 },
        0,
    },
    [SD_DECK_JIANGDONG] = {
        "江东猛虎", "速攻压制：抢攻与火计点杀", {
            26, 14, 15, 20, 6, 6, 28, 28, 29, 42, 27,
            35, 31, 38, 39, 37, 2, 8, 32, 30 },
        1,
    },
    [SD_DECK_HULANG] = {
        "虎狼之师", "控场反击：封攻与伏兵反制", {
            24, 10, 11, 21, 18, 9, 9, 0, 1, 2, 39,
            38, 37, 43, 27, 35, 28, 29, 32, 36 },
        2,
    },
    [SD_DECK_SHU_START] = {
        "蜀汉义军", "玩家初始卡组", {
            25, 22, 17, 19, 12, 13, 4, 4, 8, 3, 33,
            35, 40, 41, 27, 27, 32, 28, 38, 44 },
        1,
    },
};
