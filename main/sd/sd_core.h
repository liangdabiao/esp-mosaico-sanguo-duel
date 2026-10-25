// SPDX-License-Identifier: Apache-2.0
// sd_core.h —— 《三国点将录》对局规则核心：类型定义与对外 API。
//
// 纯 C11，零 UI / 零 ESP 依赖；设备端与宿主测试共用同一份实现。
// 设计基线：sanguo-duel-design.md（工作区根目录）。
//
// 模型速记：
//   - 所有卡在 sd_duel->insts[] 池里以 uid 编址，区域用 uid 数组表达；
//   - 玩家指令经 sd_submit_command() 进动作队列，sd_pump() 逐步执行，
//     遇到"响应窗口 / 选目标 / 等待主指令"暂停；
//   - 响应窗口单张结算（V1 无连锁），资格见 sd_window_t；
//   - AI 与人类走同一指令接口。
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 容量与数值口径（设计文档 §2/§4） ------------------------------- */
#define SD_SLOTS 3          /* 阵位 / 计略区格数 */
#define SD_LP_INIT 4000
#define SD_HAND_LIMIT 6
#define SD_MAX_HAND 8       /* 手牌缓冲上限（含抽牌瞬间的越界检查） */
#define SD_MAX_DECK 20      /* 卡组固定 20 张 */
#define SD_MAX_GRAVE 42
#define SD_MAX_INSTS 128    /* 全局卡实例池（含双方卡组/手牌/场上/墓地） */
#define SD_MAX_QUEUE 32

/* ---- 枚举 ------------------------------------------------------------ */
typedef enum { SD_P0 = 0, SD_P1 = 1 } sd_pid_t;

typedef enum {
    SD_PHASE_DRAW = 0,   /* 抽卡阶段（自动结算） */
    SD_PHASE_STANDBY,    /* 准备阶段（回合开始触发，自动） */
    SD_PHASE_MAIN1,
    SD_PHASE_BATTLE,
    SD_PHASE_MAIN2,
    SD_PHASE_END,        /* 结束阶段（弃牌/回合结束结算，自动） */
} sd_phase_t;

typedef enum { SD_CARD_GENERAL = 0, SD_CARD_STRAT = 1, SD_CARD_AMBUSH = 2 } sd_ctype_t;
typedef enum { SD_F_WEI = 0, SD_F_SHU = 1, SD_F_WU = 2, SD_F_QUN = 3 } sd_faction_t;
typedef enum { SD_U_QI = 0, SD_U_QIANG, SD_U_GONG, SD_U_WEI, SD_U_DAO, SD_U_JUNSHI } sd_unit_t;

typedef enum { SD_SK_NORMAL = 0, SD_SK_QUICK, SD_SK_CONT, SD_SK_EQUIP, SD_SK_FIELD } sd_strat_kind_t;
typedef enum { SD_AK_NORMAL = 0, SD_AK_CONT, SD_AK_COUNTER } sd_ambush_kind_t;

/* 效果原语（设计文档 §3 的实现口径；arg 含义见 sd_effect_action 注释） */
typedef enum {
    SD_OP_NONE = 0,
    SD_OP_DRAW,          /* a=张数 */
    SD_OP_HEAL,          /* a=兵力 */
    SD_OP_DAMAGE,        /* a=兵力（直接伤害，作用于对方玩家） */
    SD_OP_ATK_MOD,       /* a=±武力 b=0永久/1本回合 */
    SD_OP_DEF_MOD,       /* a=±统率 b=0永久/1本回合 */
    SD_OP_DESTROY,       /* 按 sel 选目标破坏 */
    SD_OP_SEAL_ATTACK,   /* a=封攻回合数 */
    SD_OP_PIERCE,        /* 获得贯通（equip/永久，作用于 THIS） */
    SD_OP_MULTI,         /* 获得双重攻击（作用于 THIS） */
    SD_OP_NO_RESPOND,    /* 攻击宣言时对方本回合不能发动伏兵 */
    SD_OP_DISCARD,       /* a=张数（sel 指定随机/自选） */
    SD_OP_TRIBUTE_COST,  /* a=±点兵数修正（对某一方，sel 表达） */
    SD_OP_NEGATE,        /* 无效目标计略（反击伏兵专用） */
    SD_OP_AURA_ATK,      /* a=±武力 光环（条件由 sel/b 表达） */
    SD_OP_AURA_DEF,
    SD_OP_SEARCH,        /* a=卡类型过滤（0不限/1武将/2计略/3伏兵）抽 1 张 */
    SD_OP_REVIVE,        /* 从墓地特殊上阵（V1 仅参数化预留） */
} sd_op_t;

/* 目标选择子（V1 全集） */
typedef enum {
    SD_SEL_NONE = 0,
    SD_SEL_THIS,               /* 效果所属卡自身/装备对象 */
    SD_SEL_ACTOR,              /* 触发武将本身 */
    SD_SEL_CHOICE_MY_MON,      /* 发动者自选己方 1 武将（UI/AI 决策） */
    SD_SEL_CHOICE_ENEMY_MON,   /* 自选敌方 1 武将 */
    SD_SEL_CHOICE_ENEMY_SET,   /* 自选敌方 1 名布阵武将（火计） */
    SD_SEL_ALL_ENEMY,          /* 敌方全部武将 */
    SD_SEL_ALL_FIELD,          /* 全场全部武将 */
    SD_SEL_ENEMY_HIGHEST_ATK,  /* 敌方武力最高者 */
    SD_SEL_ATK_LE,             /* 敌方全部武力<=a 的武将（火烧连营） */
    SD_SEL_ATTACKER,           /* 本次攻击宣言的攻击者（埋伏/铁蒺藜） */
    SD_SEL_BATTLE_OPPONENT,    /* 与触发武将战斗的敌将（典韦） */
    SD_SEL_RANDOM_OPP_HAND,    /* 对方随机弃牌 */
    SD_SEL_EQUIP_TARGET,       /* 装备计略的装备对象（己方正面武将） */
    SD_SEL_OPPONENT_LP,        /* 伤害/回复作用于对方玩家 */
    SD_SEL_OWNER_LP,           /* 作用于发动者玩家 */
    SD_SEL_INTERNAL_DISCARD = 0xFE, /* 内部：结束阶段超限弃牌（target.req 专用） */
} sd_sel_t;

/* 光环/静态条件位（sd_act_t.b 参数，OP_AURA_* / OP_SEAL_ATTACK / 关羽战斗加成用） */
#define COND_SET_DOWN 0x01       /* 仅布阵(背面)武将 */
#define COND_SAME_OWNER 0x04     /* 与来源卡同主 */
#define COND_SAME_FACTION 0x02   /* 与来源卡同势力 */
#define COND_BATTLE_VS_SET 0x08  /* 仅战斗中对布阵武将（关羽） */

/* 效果动作：一条可执行指令 */
typedef struct {
    uint8_t op;   /* sd_op_t */
    uint8_t sel;  /* sd_sel_t */
    int16_t a;    /* 数值参数（张数/兵力/±数值/条件阈值） */
    int16_t b;    /* 次级参数：ATK/DEF_MOD: 0永久/1本回合；AURA 条件位；TRIBUTE 方向 */
} sd_act_t;

/* 触发时机 */
typedef enum {
    SD_TR_NONE = 0,
    SD_TR_ACTIVATED,        /* 计略/伏兵发动时（主体效果） */
    SD_TR_ON_SUMMON,        /* 上阵（出击）时（布阵不触发，V1 规则） */
    SD_TR_ON_DESTROY,       /* 被破坏送墓时 */
    SD_TR_ON_TURN_START,    /* 己方回合开始时 */
    SD_TR_ON_ATTACK_DECL,   /* 该武将攻击宣言时 */
    SD_TR_ON_BATTLE_WIN,    /* 战斗破坏对方武将时 */
    SD_TR_ON_DESTROY_BY_BATTLE, /* 被战斗破坏时（典韦类） */
    SD_TR_ON_ALLY_SUMMON = 8,   /* 同阵营武将上阵时（桃园结义，观察者触发） */
} sd_trigger_t;

typedef struct {
    uint8_t trigger;
    uint8_t n_act;          /* 0..2 */
    sd_act_t act[2];
} sd_fx_t;

/* ---- 卡定义（sd_cards.c 的静态表） ----------------------------------- */
typedef struct {
    uint8_t id;             /* 卡表下标即 id */
    const char *name;
    uint8_t type;           /* sd_ctype_t */
    uint8_t faction;        /* sd_faction_t */
    uint8_t unit;           /* sd_unit_t */
    uint8_t tier;           /* 品级 1..5 */
    uint16_t atk, def;
    uint8_t strat_kind;     /* sd_strat_kind_t（仅计略） */
    uint8_t ambush_kind;    /* sd_ambush_kind_t（仅伏兵） */
    uint8_t window;         /* 响应类伏兵的资格窗口：0=非响应卡；否则 sd_window_kind_t */
    sd_fx_t fx[2];          /* 两个效果槽 */
} sd_card_def_t;

extern const sd_card_def_t SD_CARDS[];
extern const int SD_CARD_COUNT;

/* 卡组定义（预设：3 个 AI 军团 + 玩家初始卡组） */
typedef struct {
    const char *name;
    const char *desc;
    uint8_t ids[SD_MAX_DECK];
    uint8_t difficulty;     /* 0 教学随机 / 1 标准 / 2 老练 */
} sd_deck_def_t;

enum { SD_DECK_QINGZHOU = 0, SD_DECK_JIANGDONG, SD_DECK_HULANG, SD_DECK_SHU_START, SD_DECK_DEF_COUNT };
extern const sd_deck_def_t SD_DECKS[SD_DECK_DEF_COUNT];

/* ---- 运行时卡实例 ---------------------------------------------------- */
typedef struct {
    uint8_t def_id;
    uint8_t owner;          /* sd_pid_t */
    int16_t atk_perm, def_perm;   /* 永久增减（火箭/徐晃/魏延） */
    int16_t atk_temp, def_temp;   /* 本回合增减（结束阶段清零） */
    uint8_t face_down;      /* 武将布阵 / 伏兵·速攻盖放 */
    uint8_t attacked;       /* 本回合已攻击次数 */
    uint8_t seal_turns;     /* 剩余封攻回合 */
    uint8_t set_on_turn;    /* 盖放时的全局回合数（伏兵"熬一回合"判定） */
    uint8_t equip_uid;      /* 装备计略：被装备武将 uid */
    uint8_t pierce, multi, no_respond, protect; /* 标志（equip/永久授予） */
    uint8_t pos_changed;    /* 本回合已改过表示形式 */
    uint8_t alive;          /* 在池中有效 */
} sd_inst_t;

typedef struct {
    uint16_t lp;
    uint8_t deck[SD_MAX_DECK];  uint8_t deck_n;
    uint8_t hand[SD_MAX_HAND];  uint8_t hand_n;
    uint8_t grave[SD_MAX_GRAVE]; uint8_t grave_n;
    uint8_t mon[SD_SLOTS];      /* uid 或 255=空 */
    uint8_t spell[SD_SLOTS];    /* uid 或 255=空 */
    uint8_t field_card;         /* 阵法区 uid 或 255（V1 卡池无阵法卡，结构预留） */
    uint8_t summoned;           /* 本回合已通常上阵 */
    uint8_t discarded_to_limit; /* 结束阶段弃牌待定 */
} sd_player_t;

/* 响应窗口 */
typedef enum { SD_WIN_NONE = 0, SD_WIN_SUMMON = 1, SD_WIN_ATTACK = 2, SD_WIN_SPELL = 3, SD_WIN_DESTROY = 4 } sd_window_kind_t;
#define SD_WIN_MAX_CAND 6
typedef struct {
    uint8_t active;
    uint8_t kind;           /* sd_window_kind_t */
    uint8_t responder;      /* 有资格响应的一方 */
    uint8_t cand[SD_WIN_MAX_CAND]; /* 可发动的 uid */
    uint8_t cand_n;
    uint8_t src_uid;        /* 窗口来源事件（攻击者/召唤者/计略 uid） */
} sd_window_t;

/* 选目标等待 */
typedef struct {
    uint8_t active;
    uint8_t chooser;        /* 由谁选（sd_pid_t） */
    uint8_t sel;            /* sd_sel_t */
    uint8_t uid;            /* 效果来源卡 uid */
    uint8_t fx_slot;        /* 第几个效果槽 */
    uint8_t act_idx;        /* 第几条动作 */
    uint8_t cand[SD_WIN_MAX_CAND];
    uint8_t cand_n;
} sd_target_req_t;

/* 动作队列条目 */
typedef struct {
    uint8_t kind;
    uint8_t p0, p1, p2, p3;
    int16_t a0;
} sd_qitem_t;

enum {
    QK_SUMMON = 1,      /* p0=player p1=hand_idx p2=0出击/1布阵 p3=点兵slot1 a0=点兵slot2 */
    QK_SET_SPELL,       /* p0=player p1=hand_idx */
    QK_ACTIVATE,        /* p0=uid（已就位于计略区） */
    QK_ATTACK,          /* p0=player p1=my_slot p2=enemy_slot 255=直击 */
    QK_CHANGE_POS,      /* p0=player p1=slot */
    QK_RESOLVE_FX,      /* p0=uid p1=fx_slot p2=起始动作序号 */
    QK_RESOLVE_TRIGGERS,/* p0=事件主体uid p1=trigger 类型 */
    QK_WINDOW,          /* p0=kind p1=event侧 p2=src_uid */
    QK_END_PHASE,       /* 推进阶段 */
    QK_DESTROY,         /* p0=uid（非战斗破坏：ON_DESTROY 触发） */
    QK_DESTROY_BATTLE,  /* p0=victim p1=attacker（战斗破坏：W4 + BY_BATTLE 触发） */
    QK_SEND_GRAVE,      /* p0=uid */
    QK_BATTLE,          /* p0=attacker p1=defender p2=direct */
    QK_AFTER_BATTLE_WIN,/* p0=attacker p1=defender（魏延类） */
    QK_END_TURN,        /* 切换回合 */
};

/* 对局状态 */
typedef struct {
    sd_inst_t insts[SD_MAX_INSTS];
    sd_player_t pl[2];
    uint8_t turn_player;     /* sd_pid_t */
    uint8_t phase;           /* sd_phase_t */
    uint16_t global_turn;    /* 全局回合计数（双方合计，从 1 起） */
    uint32_t rng;
    uint8_t winner;          /* sd_pid_t 或 255 */
    uint8_t game_over;
    uint8_t first_player;    /* 先攻方（第一回合不抽卡不攻阵） */
    sd_window_t window;
    sd_target_req_t target;
    sd_qitem_t queue[SD_MAX_QUEUE];
    uint8_t q_head, q_n;
    sd_qitem_t after_window[4]; /* 响应窗口打开时暂存的被打断的后续动作 */
    uint8_t after_n;
    uint8_t ctx_uid;         /* 当前效果上下文主体（事件武将/发动卡） */
    uint8_t battle_ctx_uid;  /* 当前战斗的另一方 uid（典韦/魏延用） */
    uint8_t battle_no_respond; /* 本场战斗禁响应（赵云） */
    uint8_t negate_next;     /* 待结算且已被反击无效的计略 uid */
    uint8_t destroy_by_battle; /* 内部：当前破坏是否战斗破坏 */
    uint8_t awaiting_command;/* 1=等待 turn_player 主指令 */
    uint32_t stat_actions;   /* 统计：已执行动作数（自对弈用） */
} sd_duel_t;

/* ---- 主指令（人类/AI 提交） ------------------------------------------ */
typedef enum {
    SD_CMD_SUMMON = 1,   /* p0=hand_idx p1=0出击/1布阵 p2/p3=点兵slot(0..2, 255空) */
    SD_CMD_ACTIVATE,     /* p0=hand_idx 或 p1=zone:'h'手/'s'计略区 p2=uid(场上) */
    SD_CMD_SET_SPELL,    /* p0=hand_idx（盖放计略/伏兵） */
    SD_CMD_SET_MONSTER,  /* p0=hand_idx p2/p3=点兵slot */
    SD_CMD_ATTACK,       /* p0=my_slot p1=enemy_slot(255=直击) */
    SD_CMD_CHANGE_POS,   /* p0=slot */
    SD_CMD_NEXT_PHASE,   /* 主1→战斗→主2→结束 */
    SD_CMD_RESPONSE,     /* p0=响应uid(255=放弃) */
    SD_CMD_TARGET,       /* p0=目标uid（选目标阶段） */
    SD_CMD_DISCARD_CHOICE, /* p0=hand_idx（结束阶段超限弃牌，V1 简化随机弃） */
} sd_cmd_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t p0, p1, p2, p3;
} sd_cmd_t;

/* ---- API ------------------------------------------------------------- */
/* 创建对局：deck 双方 20 张卡 id；first 0/1；seed 驱动洗牌与随机裁决。
 * 返回 NULL 表示参数非法。创建后对局已开始（先攻方抽卡阶段已结算，处于 MAIN1）。 */
sd_duel_t *sd_duel_create(const uint8_t *deck0, const uint8_t *deck1, uint8_t first, uint32_t seed);
void sd_duel_free(sd_duel_t *d);

/* 可选调试日志（宿主测试/排错用；不设置则静默）。回调不得修改对局状态。 */
typedef void (*sd_log_fn)(void *ud, const char *msg);
void sd_set_log(sd_log_fn fn, void *ud);

/* 查询接口（只读，UI 与 AI 使用） */
bool sd_is_monster(const sd_card_def_t *c);
const sd_card_def_t *sd_card(uint8_t uid);            /* uid -> 定义 */
uint8_t sd_uid_def(uint8_t uid);                      /* uid -> def_id */
bool sd_uid_valid(const sd_duel_t *d, uint8_t uid);
/* 武将有效面板：基础 + 永久 + 本回合 + 光环/装备 */
int sd_eff_atk(const sd_duel_t *d, uint8_t uid);
int sd_eff_def(const sd_duel_t *d, uint8_t uid);
int sd_tribute_count(const sd_duel_t *d, uint8_t player, const sd_card_def_t *c);
bool sd_can_summon(const sd_duel_t *d, uint8_t player, uint8_t hand_idx, bool set_down, int *err);
bool sd_can_activate(const sd_duel_t *d, uint8_t player, uint8_t uid_or_handidx, bool from_zone_spell, int *err);
bool sd_can_set_spell(const sd_duel_t *d, uint8_t player, uint8_t hand_idx, int *err);
bool sd_can_attack(const sd_duel_t *d, uint8_t player, uint8_t slot, int *err);
bool sd_can_change_pos(const sd_duel_t *d, uint8_t player, uint8_t slot, int *err);
bool sd_can_direct_attack(const sd_duel_t *d, uint8_t player);

/* 提交主指令；返回 false 表示非法（不改变状态）。 */
bool sd_submit_command(sd_duel_t *d, const sd_cmd_t *cmd);
/* 泵动作队列直到暂停点（等待指令/窗口/选目标/终局）。 */
void sd_pump(sd_duel_t *d);

/* 便捷断言辅助：当前是否在等待主指令/窗口/选目标。 */
static inline bool sd_await_command(const sd_duel_t *d) { return d && d->awaiting_command && !d->game_over; }
static inline bool sd_window_open(const sd_duel_t *d) { return d && d->window.active; }
static inline bool sd_target_open(const sd_duel_t *d) { return d && d->target.active; }

/* 战斗预览（UI 确认框/直击提示）：返回伤害给双方 LP，破坏标记。 */
typedef struct {
    int dmg_attacker_side;   /* 攻击方玩家将受的兵力伤害 */
    int dmg_defender_side;
    bool attacker_destroyed, defender_destroyed;
} sd_battle_preview_t;
void sd_preview_battle(const sd_duel_t *d, uint8_t attacker_uid, uint8_t defender_uid, bool direct, sd_battle_preview_t *out);

/* 错误码（sd_can_* 的 err 出参，可传 NULL） */
enum {
    SD_OK = 0, SD_ERR_ARG, SD_ERR_NOT_YOUR_TURN, SD_ERR_PHASE, SD_ERR_ZONE_FULL,
    SD_ERR_ALREADY_SUMMONED, SD_ERR_NEED_TRIBUTE, SD_ERR_BAD_TRIBUTE,
    SD_ERR_CANNOT_ACTIVATE, SD_ERR_FACEUP_ONLY, SD_ERR_SET_TOO_SOON,
    SD_ERR_NO_TARGETS, SD_ERR_ALREADY_ATTACKED, SD_ERR_SEALED, SD_ERR_NO_TARGET,
    SD_ERR_EMPTY_FIELD_DIRECT, SD_ERR_WRONG_POS, SD_ERR_ALREADY_CHANGED,
    SD_ERR_WINDOW_BUSY, SD_ERR_TARGET_BUSY, SD_ERR_HAND_FULL, SD_ERR_DECK_EMPTY,
};

#ifdef __cplusplus
}
#endif
