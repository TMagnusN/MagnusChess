# History.cpp - 历史启发式表 / History Heuristic Tables

## 中文

### 概述
`History.cpp` 实现了 MagnusChess 搜索引擎中所有历史启发式表格的辅助函数，包括深度分类、SEE 分类、历史奖励/惩罚计算以及 `HistoryTables` 的清理。

### 命名空间
所有函数和类型位于 `namespace magnus::search`。

### HistoryTables 结构体

`HistoryTables` 是历史启发式系统的核心，聚合了所有子表：

| 子表 | 类型 | 索引维度 | 描述 |
|------|------|---------|------|
| `killers` | `KillerTable` | `[MAX_PLY][2]` | 杀手走法表 |
| `quiet` | `QuietHistoryTable` | `[COLOR_NB][SQ_NB][SQ_NB]` | 安静走法历史（from→to） |
| `capture` | `CaptureHistoryTable` | `[COLOR_NB][PIECE_TYPE_NB][SQ_NB][PIECE_TYPE_NB]` | 吃子历史（mover, to, captured） |
| `countermove` | `CounterMoveTable` | `[COLOR_NB][SQ_NB][SQ_NB]` | 反走法表（对方上一步→回复走法） |
| `continuation` | `ContinuationHistoryTable` | `[COLOR_NB][PIECE_TYPE_NB][SQ_NB][PIECE_TYPE_NB][SQ_NB]` | 延续历史（(prev_piece, prev_to)→(cur_piece, cur_to)） |
| `see_bias` | `SeeBiasTable` | `[DepthClass::Count][SeeClass::Count]` | SEE 偏置表（按深度×SEE类别） |
| `pawn_history` | `PawnHistoryTable` | `[PAWN_HISTORY_SIZE][PIECE_TYPE_NB][SQ_NB]` | 兵形历史（兵哈希×棋子类型×目标格） |

### 深度分类（DepthClass）

| 类别 | 条件 |
|------|------|
| `Shallow` | d ≤ 3 |
| `MediumLow` | 3 < d ≤ 6 |
| `MediumHigh` | 6 < d ≤ 10 |
| `Deep` | d > 10 |

### SEE 分类（SeeClass）

| SeeClass | 条件 |
|----------|------|
| `LossBig` | SEE ≤ -100 |
| `LossSmall` | -100 < SEE < 0 |
| `Equal` | SEE == 0 |
| `GainSmall` | 0 < SEE < 200 |
| `GainBig` | SEE ≥ 200 |
| `Promo` | 升变吃子 |
| `Check` | 带将吃子 |

**`classify_see_bias()`** — 简化四类（用于 SEE 偏置表）：

| 类别 | 条件 |
|------|------|
| `LossSmall` | SEE < -50 |
| `Equal` | -50 ≤ SEE ≤ 50 |
| `GainSmall` | 50 < SEE ≤ 200 |
| `GainBig` | SEE > 200 |

### 核心内联方法（HistoryTables）

#### 查询方法

| 方法 | 描述 |
|------|------|
| `killer_fast(ply, slot)` | 查询杀手走法 |
| `quiet_value_fast(pos, move)` | 查询安静走法历史分 |
| `capture_value_fast(pos, move)` | 查询吃子历史分 |
| `countermove_fast(pos, prev_move)` | 查询反走法 |
| `countermove_bonus_fast(pos, move, prev_move)` | 反走法匹配奖励（匹配=`MAGNUS_COUNTERMOVE_BONUS=4096`） |
| `continuation_value_fast(pos, move, previous, slot)` | 按已保存的前序棋子类型与目标格查询延续历史分 |
| `see_bias_value_fast(depth, see_value)` | 查询 SEE 偏置项（`clamp(raw/4, -96, 96)`） |
| `pawn_history_value_fast(pos, move)` | 查询兵形历史分 |

#### 排序评分

| 方法 | 描述 |
|------|------|
| `quiet_ordering_score_fast(pos, move, prev_move, prev2, prev4, prev8)` | 安静走法综合排序分（6 项加权和） |
| `capture_ordering_score_fast(pos, move, depth, see)` | 吃子综合排序分（3 项加权和） |

`ContinuationHistoryContext` 在走法执行前保存棋子类型与目标格。查询和更新延续历史时直接使用该快照，不从数个 ply 后的当前局面反推旧走法的棋子类型；因此即使该棋子后来移动、被吃或升变，索引仍保持稳定。

#### 奖励/惩罚

| 方法 | 描述 |
|------|------|
| `bonus_fast(pos, move, depth)` | 奖励安静走法（截断触发） |
| `penalty_fast(pos, move, depth)` | 惩罚安静走法（未截断） |
| `bonus_capture_fast(pos, move, depth)` | 奖励吃子 |
| `penalty_capture_fast(pos, move, depth)` | 惩罚吃子 |
| `bonus_pawn_history_fast(pos, move, depth)` | 奖励兵形历史 |
| `penalty_pawn_history_fast(pos, move, depth)` | 惩罚兵形历史 |

所有更新使用有界重力公式：

`next = current + bonus - current × abs(bonus) / limit`

其中奖励传入正 `bonus`，惩罚传入负 `bonus`。越接近同方向上限，更新量越小；方向相反的新证据则会更快把历史值拉回。一般历史表的 `limit=32767`，SEE 偏置表为 `2048`。

### 兵形历史（PawnHistory）

- `PAWN_HISTORY_SIZE = 512`
- 索引：`pawn_key = pieces(WHITE, PAWN) ^ rotl(pieces(BLACK, PAWN), 7)` → `pawn_key % 512`
- 仅在安静走法中生效

### 历史奖励/惩罚公式

| 函数 | 公式 |
|------|------|
| `history_bonus(depth)` | `max(1, depth × depth)` |
| `history_penalty(depth)` | `bonus × 4` |

### SEE 即时项

| 预设 | 公式 | 范围 |
|------|------|------|
| Weak | `clamp(see/4, -75, 75)` | [-75, 75] |
| Medium（默认） | `clamp(see/2, -150, 150)` | [-150, 150] |
| Strong | `clamp(see, -300, 300)` | [-300, 300] |

### 配置宏（部分）

| 宏 | 默认值 | 描述 |
|----|--------|------|
| `MAGNUS_COUNTERMOVE_BONUS` | 4096 | 反走法匹配奖励 |
| `MAGNUS_QUIET_HISTORY_WEIGHT` | 1 | 安静走法历史权重 |
| `MAGNUS_CONT1_WEIGHT_NUM/DEN` | 1/1 | 延续历史 1 权重 |
| `MAGNUS_CONT2_WEIGHT_NUM/DEN` | 1/4 | 延续历史 2 权重 |
| `MAGNUS_CAPTURE_HISTORY_WEIGHT` | 1 | 吃子历史权重 |
| `MAGNUS_CAPTURE_IMM_SEE_WEIGHT` | 1 | SEE 即时项权重 |
| `MAGNUS_CAPTURE_SEE_BIAS_WEIGHT` | 1 | SEE 偏置项权重 |
| `MAGNUS_SEE_BIAS_BAD_THRESHOLD` | -50 | classify_see_bias 坏阈值 |
| `MAGNUS_SEE_BIAS_EQ_THRESHOLD` | 50 | classify_see_bias 平阈值 |
| `MAGNUS_SEE_BIAS_GOOD_BIG_THRESHOLD` | 200 | classify_see_bias 大好阈值 |

### 辅助函数

| 函数 | 描述 |
|------|------|
| `depth_class(depth)` | 深度 → DepthClass |
| `classify_see(see, gives_check, is_promo)` | SEE → SeeClass（7 类） |
| `classify_see_bias(see)` | SEE → 简化的 4 类 |
| `history_bonus(depth)` | 计算奖励值 |
| `history_penalty(depth)` | 计算惩罚值 |
| `see_immediate_term(see, preset)` | SEE → 缩放后的即时排序项 |

---

## English

### Overview
`History.cpp` implements auxiliary functions for all history heuristic tables in the MagnusChess search engine, including depth classification, SEE classification, bonus/penalty computation, and `HistoryTables` cleanup.

### Namespace
All functions and types reside in `namespace magnus::search`.

### HistoryTables Struct

The core of the history heuristic system, aggregating all sub-tables.

### Depth Classification (DepthClass)

| Class | Condition |
|-------|-----------|
| `Shallow` | d ≤ 3 |
| `MediumLow` | 3 < d ≤ 6 |
| `MediumHigh` | 6 < d ≤ 10 |
| `Deep` | d > 10 |

### SEE Classification (SeeClass)

7 categories from `LossBig` to `Check`, plus simplified 4-category `classify_see_bias()` for the SEE bias table.

### Core Inline Methods (HistoryTables)

**Query**: `killer_fast`, `quiet_value_fast`, `capture_value_fast`, `countermove_fast`, `countermove_bonus_fast`, `continuation_value_fast`, `see_bias_value_fast`, `pawn_history_value_fast`

**Ordering**: `quiet_ordering_score_fast` (6-term weighted sum), `capture_ordering_score_fast` (3-term weighted sum)

`ContinuationHistoryContext` snapshots the moved piece type and destination before the move is made. Continuation lookup and updates use that snapshot rather than reconstructing an old move from the current board.

**Update**: `bonus_fast`/`penalty_fast` (quiets), `bonus_capture_fast`/`penalty_capture_fast` (captures), `bonus_pawn_history_fast`/`penalty_pawn_history_fast` (pawn history)

All stored values use bounded gravity updates:

`next = current + bonus - current * abs(bonus) / limit`

Rewards pass a positive bonus and penalties pass a negative bonus. Same-direction updates shrink near the bound, while contradictory evidence moves the value back faster. The general history limit is 32767; SEE bias uses 2048.

### Pawn History

`PAWN_HISTORY_SIZE = 512`, indexed by pawn structure hash modulo 512.

### Bonus/Penalty Formulas

| Function | Formula |
|----------|---------|
| `history_bonus(depth)` | `max(1, depth²)` |
| `history_penalty(depth)` | `bonus × 4` |

### SEE Immediate Term

| Preset | Formula | Range |
|--------|---------|-------|
| Weak | `clamp(see/4, -75, 75)` | [-75, 75] |
| Medium (default) | `clamp(see/2, -150, 150)` | [-150, 150] |
| Strong | `clamp(see, -300, 300)` | [-300, 300] |

### Config Macros (selected)

`MAGNUS_COUNTERMOVE_BONUS`=4096, `MAGNUS_QUIET_HISTORY_WEIGHT`=1, `MAGNUS_CONT1_WEIGHT`=1/1, `MAGNUS_CONT2_WEIGHT`=1/4, `MAGNUS_CAPTURE_HISTORY_WEIGHT`=1, `MAGNUS_CAPTURE_IMM_SEE_WEIGHT`=1, `MAGNUS_CAPTURE_SEE_BIAS_WEIGHT`=1.

### Helper Functions

`depth_class`, `classify_see`, `classify_see_bias`, `history_bonus`, `history_penalty`, `see_immediate_term`.
