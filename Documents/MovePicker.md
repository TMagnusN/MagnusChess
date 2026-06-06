# MovePicker.cpp - 走法选择与排序 / Move Picking and Ordering

## 中文

### 概述
`MovePicker.cpp` 实现了 MagnusChess 的分阶段走法选择器。`MovePicker` 类负责从走法列表中按优先级顺序逐一产生走法，采用惰性构建策略——每个阶段在首次使用时才生成和排序，早期截断无需支付后续阶段的开销。

### 分阶段架构

走法按以下**九个阶段**依次产生：

```
阶段 0: TT_MOVE       — 置换表提示走法（验证合法性后优先尝试）
阶段 1: GEN_CAPTURES  — 惰性生成：产生所有吃子并分类为好/坏
阶段 2: GOOD_CAPTURES — MVV-LVA + SEE 筛选的正值吃子
阶段 3: GEN_QUIETS    — 惰性生成：产生所有安静走法并排序
阶段 4: KILLER_1      — 槽位 0 的杀手走法
阶段 5: KILLER_2      — 槽位 1 的杀手走法
阶段 6: QUIETS        — 余下的安静走法（按历史排序）
阶段 7: BAD_CAPTURES  — SEE 负值的吃子（延后尝试）
阶段 8: DONE          — 所有走法已穷尽
```

GEN_CAPTURES 和 GEN_QUIETS 是惰性构建阶段：`next()` 第一次遇到时才实际调用 `build_capture_stage()` 或 `build_quiet_stage()`。

### 核心 API

#### 构造函数
```cpp
MovePicker(pos, mem, info, history, tt_move, ply, prev_move, prev2, prev4, prev8, depth, quiet_control)
```

#### 主要方法

| 方法 | 描述 |
|------|------|
| `next()` | 从当前阶段获取下一个走法，返回 `Move(0)` 表示结束 |
| `last_score()` | 上一个走法的排序分数 |
| `last_was_capture()` | 上一个走法是否为吃子 |
| `last_was_bad_capture()` | 上一个走法是否为坏吃子（SEE < 0） |
| `last_see_value()` | 上一个走法的 SEE 值 |
| `last_quiet_in_skip_band()` | 上一个安静走法是否在跳过带内 |
| `last_quiet_suppressed()` | 上一个安静走法是否被抑制 |
| `quiet_generated()` | 生成的安静走法总数 |
| `quiet_scored()` | 完整评分的安静走法数 |
| `quiet_suppressed()` | 被抑制（仅历史排序）的安静走法数 |
| `good_captures()` | 好捕获列表（供观测/工具使用） |
| `bad_captures()` | 坏捕获列表 |
| `good_capture_count()` / `bad_capture_count()` | 好/坏捕获数量 |

### QuietControl 结构体

控制安静走法的晚期抑制策略：

| 字段 | 类型 | 描述 |
|------|------|------|
| `skip_quiets` | bool | 是否启用安静走法跳过模式 |
| `quiet_limit` | int | 允许通过的晚期安静走法数量上限 |
| `history_floor` | int | 历史分数下限（低于此值且不在跳过带外的走法被抑制） |
| `keep_top_history` | int | 保留的高分安静走法数量（0-8，不计入 limit） |

当 `skip_quiets == true` 时，安静走法的处理逻辑：
1. 反走法（countermove）和 top-K 高分走法不受 limit 限制
2. 超出 `quiet_limit` 的走法进入"跳过带"（`quiet_in_skip_band = true`）
3. 跳过带内且历史分 ≤ `history_floor` 且不将军的走法被抑制（仅按裸历史分排序，不计算完整评分）
4. 杀手走法自动从安静列表中提升到 KILLER 阶段

### 吃子排序（`score_capture()`）

```cpp
score = mvv_lva_term + capture_ordering_score_fast(pos, move, depth, see_value)
```

其中 `mvv_lva_term = victim_value × 32 - attacker_value`，capture_ordering_score_fast 包含：
- 吃子历史（权重 1）
- SEE 即时项（权重 1，Medium 预设）
- SEE 偏置项（权重 1）

吃子按 SEE 分为两类：`see >= 0` → good_caps，`see < 0` → bad_caps。

### 安静走法排序（`score_quiet()`）

```cpp
score = quiet_ordering_score_fast(pos, move, prev_move, prev2, prev4, prev8)
```

包含六项（详见 History.md）：
1. 安静走法历史
2. 反走法奖励
3. 2-ply 延续历史
4. 4-ply 延续历史
5. 8-ply 延续历史
6. 兵形历史

`prev2`、`prev4`、`prev8` 是在对应走法执行前保存的 `ContinuationHistoryContext`，不会从当前棋盘反推旧棋子的类型。

### 惰性选择（`pick_best_entry()`）

使用 O(n²) 部分选择排序：每次调用在剩余走法中选出得分最高的，与当前位置交换后返回。这比完整排序更高效，因为搜索通常在前几个走法就触发截断。

### 内部方法

| 方法 | 描述 |
|------|------|
| `prepare_tt_move()` | 验证 TT 走法合法性，计算评分和 SEE |
| `build_capture_stage()` | 生成伪合法吃子 → 合法性过滤 → 计算 SEE → 分入好/坏列表 |
| `build_quiet_stage()` | 生成伪合法安静走法 → 合法性过滤 → 按 QuietControl 策略排序/抑制 |
| `add_capture(move)` | 将吃子加入好/坏列表（规避 TT 走法重复） |
| `add_quiet(move, history_score, in_skip_band, suppressed)` | 将安静走法加入列表或提升为杀手 |
| `score_capture(move, see_value)` | 计算吃子排序分 |
| `score_quiet(move)` | 计算安静走法排序分 |

### 内部状态（ScoredEntry）

```cpp
struct ScoredEntry {
    Move move;
    int score;           // 排序分数
    int see_value;       // SEE 值（仅吃子有效）
    bool quiet_in_skip_band;  // 是否在跳过带内
    bool quiet_suppressed;    // 是否被抑制
};
```

### 设计要点
- 惰性构建：GEN_CAPTURES/GEN_QUIETS 阶段在首次访问时才执行生成和排序
- 杀手走法自动从安静列表提升，避免在 QUIETS 阶段重复出现
- QuietControl 支持节点驱动的晚期安静走法抑制，在不利局面下跳过低分安静走法
- O(n²) 部分选择排序在搜索早期截断的场景中比完整 O(n log n) 排序更优

---

## English

### Overview
`MovePicker.cpp` implements MagnusChess's staged move selector. The `MovePicker` class produces moves one by one in priority order using lazy construction — each stage is built and sorted only on first access, so early cutoffs never pay for later stage work.

### Staged Architecture

Moves are produced sequentially across **nine stages**:

```
Stage 0: TT_MOVE       — Transposition table hint move (validated, tried first)
Stage 1: GEN_CAPTURES  — Lazy: generate all captures, split into good/bad
Stage 2: GOOD_CAPTURES — Positive-SEE captures sorted by MVV-LVA + history
Stage 3: GEN_QUIETS    — Lazy: generate all quiets, sort/suppress per QuietControl
Stage 4: KILLER_1      — Killer from slot 0
Stage 5: KILLER_2      — Killer from slot 1
Stage 6: QUIETS        — Remaining quiet moves (history-sorted)
Stage 7: BAD_CAPTURES  — Negative-SEE captures (tried last)
Stage 8: DONE          — All moves exhausted
```

GEN_CAPTURES and GEN_QUIETS are lazy construction stages: `next()` calls `build_capture_stage()` / `build_quiet_stage()` only on first encounter.

### Core API

#### Constructor
```cpp
MovePicker(pos, mem, info, history, tt_move, ply, prev_move, prev2, prev4, prev8, depth, quiet_control)
```

#### Key Methods

| Method | Description |
|--------|-------------|
| `next()` | Get next move from current stage, returns `Move(0)` when done |
| `last_score()` | Ordering score of the last move |
| `last_was_capture()` | Whether last move was a capture |
| `last_was_bad_capture()` | Whether last move was a bad capture (SEE < 0) |
| `last_see_value()` | SEE value of the last move |
| `last_quiet_in_skip_band()` | Whether last quiet was in the skip band |
| `last_quiet_suppressed()` | Whether last quiet was suppressed |
| `quiet_generated()` / `quiet_scored()` / `quiet_suppressed()` | Quiet move statistics |
| `good_captures()` / `bad_captures()` | Capture lists for observation/tooling |

### QuietControl Struct

Controls late quiet move suppression:

| Field | Type | Description |
|-------|------|-------------|
| `skip_quiets` | bool | Enable quiet skipping mode |
| `quiet_limit` | int | Max late quiets to pass through |
| `history_floor` | int | History score floor (quiets below this in skip band are suppressed) |
| `keep_top_history` | int | Top-K high-history quiets to keep (0-8, exempt from limit) |

When `skip_quiets == true`:
1. Countermoves and top-K high-history moves are exempt from the limit
2. Quiets beyond `quiet_limit` enter the "skip band" (`quiet_in_skip_band = true`)
3. Skip-band quiets with history ≤ `history_floor` and not giving check are suppressed (history-only ordering)
4. Killer moves are auto-promoted from quiet list to KILLER stages

### Capture Ordering (`score_capture()`)

```cpp
score = mvv_lva_term + capture_ordering_score_fast(pos, move, depth, see_value)
```

Where `mvv_lva_term = victim_value × 32 - attacker_value`. Captures split by SEE: `see >= 0` → good_caps, `see < 0` → bad_caps.

### Quiet Ordering (`score_quiet()`)

```cpp
score = quiet_ordering_score_fast(pos, move, prev_move, prev2, prev4, prev8)
```

Six terms (see History.md): quiet history, countermove bonus, 2-ply continuation, 4-ply continuation, 8-ply continuation, and pawn history. The continuation arguments are snapshots captured before their moves were made.

### Lazy Selection (`pick_best_entry()`)

O(n²) partial selection sort: each call picks the highest-scoring remaining move, swaps it to the current position, and returns it. More efficient than full sorting since search typically cuts off on early moves.

### Internal Methods

| Method | Description |
|--------|-------------|
| `prepare_tt_move()` | Validate TT move legality, compute score and SEE |
| `build_capture_stage()` | Generate pseudo-legal captures → legality filter → SEE → split good/bad |
| `build_quiet_stage()` | Generate pseudo-legal quiets → legality filter → sort/suppress per QuietControl |
| `add_capture(move)` | Add capture to good/bad list (avoid TT move duplicate) |
| `add_quiet(move, history_score, in_skip_band, suppressed)` | Add quiet to list or promote to killer |
| `score_capture(move, see_value)` | Compute capture ordering score |
| `score_quiet(move)` | Compute quiet ordering score |

### Internal State (ScoredEntry)

```cpp
struct ScoredEntry {
    Move move;
    int score;
    int see_value;           // captures only
    bool quiet_in_skip_band;
    bool quiet_suppressed;
};
```

### Design Notes
- Lazy construction: GEN_CAPTURES/GEN_QUIETS stages only execute on first access
- Killer moves auto-promoted from quiet list, avoiding duplicates in QUIETS stage
- QuietControl enables node-driven late quiet suppression, skipping low-score quiets in unfavorable positions
- O(n²) partial selection beats full O(n log n) sort when search cuts off early
