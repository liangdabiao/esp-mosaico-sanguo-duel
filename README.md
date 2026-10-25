# sanguo_duel —— 《三国点将录》

三国题材、类游戏王玩法的卡牌对战小游戏（ESP-Mosaico，480×480 触摸屏）。
设计基线：工作区根目录 `sanguo-duel-design.md`。

## 玩法速览

- 与 3 个 AI 军团闯关对战（青州军→江东猛虎→虎狼之师，难度递增）。
- 规则骨架：品级召唤/点兵（★1-2 免费、★3-4 点 1、★5 点 2）、出击/布阵、
  计略/伏兵（盖放次回合起可发动、反击可无效计略）、LP 归零或卡组抽尽判负。
- 45 张原创卡：武将 27（魏蜀吴群四势力）+ 计略 11 + 伏兵 7；效果全部由
  23 种类型化原语表达（无脚本引擎）。
- 通关解锁自由组卡（同名≤2、20 张），NVS 持久化；RTTTL 合成战鼓/号角音效。

## 结构

```
main/
├─ game.c / main.c      UI 层（页面渲染 + 触摸；BOOT 键=返回）
├─ sd_cjk_font.c        中文字模 blit（字表由 tools/sanguo/gen_cjk_font.py 生成）
├─ sd/                  纯 C 逻辑层（宿主与设备共用，零 UI/ESP 依赖）
│  ├─ sd_core.h sd_duel.c     规则引擎：状态机/动作队列/效果解释/响应窗口
│  ├─ sd_cards.c              45 张卡数据 + 4 副预设卡组
│  ├─ sd_ai.c                 启发式 AI（3 档难度）
│  ├─ sd_text.c               卡牌效果描述（UI/字库扫描源）
│  └─ sd_progress.c sd_session.c  进度校验 / 会话编排
├─ sd_store.c(+_host)   NVS 落盘（host 用内存桩）
└─ sd_sfx.c(+_host)     RTTTL 音效（host 空桩）
tests/                  宿主单测（zig cc；446 断言 + 30 局自对弈）
tools/sanguo/           字模生成 + 模拟器回放脚本
```

## 工作流（从工作区根目录）

```sh
# 宿主单测（必须全绿再动 UI/固件）
powershell -ExecutionPolicy Bypass -File projects/sanguo_duel/tests/build_host_tests.ps1

# 字模再生成（改过任何中文文案后）
python projects/sanguo_duel/tools/sanguo/gen_cjk_font.py

# 模拟器（headless 回放 / 交互；路径相对工作区根目录）
python mosaico.py game sim --project projects/sanguo_duel --headless --frames 30
python mosaico.py game sim --project projects/sanguo_duel --scenario projects/sanguo_duel/tools/sanguo/replay-duel-summon.json

# 固件（全量 10-27 分钟；.ninja_deps /D:/ 自检见脚本；失败重跑加 -KeepDeps）
powershell -ExecutionPolicy Bypass -File projects/sanguo_duel/build_sanguo.ps1

# 安装（首次/换布局用 system-update）
python mosaico.py iris system-update --project projects/sanguo_duel
```

Host 状态暴露 `phase`、`tick`、`pointer_*` 与 `state_hash` 供自动化；回放脚本
在 `tools/sanguo/replay-*.json`。Touch/pointer 输入不画光标。

## V1 已知简化（与设计文档的差异）

- 阵法区结构已预留，V1 卡池无阵法卡。
- 效果伤害向下取整；"按武力选择"的破坏效果不翻看布阵武将（跳过）。
- 换皮预留：规则核心与题材解耦（换卡表/文本即可换主题）。
