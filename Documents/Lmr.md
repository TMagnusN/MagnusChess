# Lmr.cpp - 迟走法裁剪 / Late Move Reduction

## 中文

### 概述
`Lmr.cpp` 实现了 MagnusChess 引擎的**迟走法裁剪（Late Move Reduction, LMR）**系统。LMR 是提高搜索效率的核心技术：对排序靠后的走法使用降低的搜索深度，根据走法质量和搜索上下文动态调整减少量。

### 核心架构

#### 统一 API

LMR 通过单一入口 `decide_lmr()` 处理安静走法和吃子走法的裁剪决策，不再区分 `decide_lmr_quiet` 和 `decide_lmr_capture`。决策过程使用三个结构体分离关注点：

- **`LmrNodeContext`** — 节点层上下文（深度、PV/非PV、改善状态、TT 状态等），由 `pvs()` 填充
- **`LmrMoveContext`** — 走法层上下文（类型、历史得分、SEE 值、排序分数等），由 `MovePicker` 填充
- **`LmrDecision`** — 决策输出（是否裁剪、固定点裁剪量、re-search 深度）

#### 固定点精度（Fixed-Point）

LMR 使用**分数精度 ply**进行计算，`FP_ONE_PLY = 1024` 代表一个标准搜索层：
```
base_depth       = 原始深度（以 ply 为单位）
reduction_fp     = 计算出的减少量（以 FP 为单位）
search_depth     = base_depth - (reduction_fp / FP_ONE_PLY)
```

#### 走法分类

两类走法采用不同的基础裁剪公式：

1. **安静走法裁剪** — 当 `move_index >= 2`（非 PV）或 `move_index >= 4`（PV，`MAGNUS_ENABLE_PV_LMR=1`）且 `depth >= 3` 时触发
2. **吃子裁剪** — 仅对简单吃子（非升变），当 `reduction_index >= 2` 且 `depth >= 4` 时触发（`MAGNUS_ENABLE_CAPTURE_LMR=1`）

以下走法**永远不会被裁剪**：
- TT 走法
- 将军走法
- 反吃走法（recapture）
- 升变走法
- 排除搜索（exclusion search）
- 将杀窗口（mate window）
- 被将军状态（checked）
- 有走法延展（move_extension != 0）

### 基础裁剪公式

**安静走法**：
```
base_reduction_fp = LnTable[depth] × LnTable[move_index + 1] + FP_ONE_PLY/2
```

**吃子走法**：
```
base_reduction_fp = max(0, (LnTable[depth] × LnTable[reduction_index + 1]) × 3/4 - FP_ONE_PLY/4)
```

其中 `LnTable[i] = max(1, (2747/128) × ln(i))` 是预计算的对数表（i ∈ [1, 64]）。

### 调整因子

基础裁剪量经过以下调整（正值增加裁剪，负值减少裁剪）：

| 条件 | 调整（FP 单位） | 原因 |
|------|-----------------|------|
| `improving == true` | `-FP_ONE_PLY/8` | 改善局面更可靠 |
| `improving == false` | `+FP_ONE_PLY/4` | 恶化局面更激进裁剪 |
| PV 节点 | `-FP_ONE_PLY/2` | PV 走法需谨慎 |
| 无 TT 走法 | `+FP_ONE_PLY/2` | 无 TT 指导 |
| cut-node | `+FP_ONE_PLY/2` | 预期快速截断 |
| all-node | 按比例减少 | `fp -= fp / (depth + 1)` |
| TT 走法是吃子（当前是安静走法） | `+FP_ONE_PLY/8` | TT 提示可能是战术局面 |
| TT 深度 ≥ 当前深度 | `-FP_ONE_PLY/4` | TT 有信心 |
| TT 边界为 EXACT | `-FP_ONE_PLY/4` | 精确 TT 值 |
| TT 边界为 LOWER | `-FP_ONE_PLY/8` | 下界 TT |
| 下一层截断 > 1 | `+min(2, n-1) × FP_ONE_PLY/4` | 连续截断增加裁剪 |
| 父节点裁剪 > 1 ply | `+FP_ONE_PLY/8` | 连续裁剪惩罚 |

#### 吃子 SEE 调整

| SEE 值 | 调整 |
|--------|------|
| `see >= 200` | `-FP_ONE_PLY` |
| `see >= 0` | `-FP_ONE_PLY/2` |
| bad capture | `+FP_ONE_PLY/2` |
| `see < 0`（非 bad） | `+FP_ONE_PLY/4` |

#### 历史得分减免

综合统计分数 `stat_score` 由以下组成：

**安静走法**：
```
stat_score = clamp(2×quiet_history + weighted_continuation + countermove_bonus/2 + ordering_score/8, -16384, 16384)
```
其中 `weighted_continuation = continuation_score × depth_factor / 256`，depth_factor 随深度线性增长（depth=4→192, depth=12→320）。

**吃子走法**：
```
stat_score = clamp(capture_history + see_bias_term×16 + clamp(see, -512, 512) - (bad_capture ? 256 : 0) + (gives_check ? 96 : 0), -8192, 8192)
```

历史减免：`history_bonus_fp = clamp(stat_score / divisor, ...)`，安静走法 divisor=12，吃子 divisor=16。

### 裁剪上限

| 走法类型 | 最大裁剪 |
|---------|---------|
| PV 安静走法 | 1 ply |
| 非 PV 安静走法 | min(depth-1, 5) ply |
| 吃子 (see >= 200) | 1 ply |
| 吃子 (see >= 0) | min(depth-1, 2) ply |
| 吃子 (see < 0) | min(depth-1, 3) ply |

### LMR Re-search（`lmr_research_depth()`）

当裁剪搜索的分数意外超过 alpha 时，触发 re-search 确认：

- 如果 `score > alpha + 96` 且裁剪 ≥ 2 ply → re-search 深度 +1
- 如果 `score < alpha + 8` → re-search 深度 -1
- 如果 `score > best_score + 192` 且裁剪 ≥ 3 ply → re-search 深度再 +1

re-search 深度限制在 `[1, full_depth + 1]` 范围内。

### 关键函数

| 函数 | 描述 |
|------|------|
| `decide_lmr(node, move)` | 统一 LMR 决策，返回 `LmrDecision` |
| `lmr_research_depth(decision, full_depth, score, alpha, best_score)` | 计算 re-search 深度 |

### 关键结构体

| 结构体 | 描述 |
|--------|------|
| `LmrNodeContext` | 节点层参数：depth, alpha, beta, ply, pv_node, cut_node, all_node, checked, improving, exclusion_search, mate_window, tt_move_present, tt_move_is_capture, static_eval, move_extension, next_ply_cutoff_count, parent_reduction_fp, tt_depth, tt_bound |
| `LmrMoveContext` | 走法层参数：move, move_index, reduction_index, is_tt_move, quiet, capture, simple_capture, bad_capture, gives_check, recapture, promotion, ordering_score, quiet_history_score, continuation_score, countermove_bonus, capture_history_score, see_value, see_bias_term |
| `LmrDecision` | 决策输出：stat_score, base_reduction_fp, final_reduction_fp, final_reduction, eligible |

### 编译宏

| 宏 | 默认值 | 描述 |
|----|--------|------|
| `MAGNUS_ENABLE_PV_LMR` | 1 | 启用 PV 节点的 LMR |
| `MAGNUS_ENABLE_CAPTURE_LMR` | 1 | 启用吃子走法的 LMR |

---

## English

### Overview
`Lmr.cpp` implements the **Late Move Reduction (LMR)** system for the MagnusChess engine. LMR is a core technique for improving search efficiency: later-sorted moves are searched with reduced depth, with the reduction dynamically adjusted based on move quality and search context.

### Core Architecture

#### Unified API

LMR uses a single entry point `decide_lmr()` for both quiet and capture move reduction decisions. Three structs separate concerns:

- **`LmrNodeContext`** — Node-level context (depth, PV/non-PV, improving state, TT state, etc.), populated by `pvs()`
- **`LmrMoveContext`** — Move-level context (type, history scores, SEE value, ordering score, etc.), populated by `MovePicker`
- **`LmrDecision`** — Decision output (eligible, fixed-point reduction, re-search depth)

#### Fixed-Point Arithmetic

LMR uses **fractional precision ply** where `FP_ONE_PLY = 1024` represents one standard search ply.

#### Move Classification

Two move categories with different base reduction formulas:

1. **Quiet reduction** — Triggered when `move_index >= 2` (non-PV) or `move_index >= 4` (PV, `MAGNUS_ENABLE_PV_LMR=1`) and `depth >= 3`
2. **Capture reduction** — For simple captures only (non-promotion), triggered when `reduction_index >= 2` and `depth >= 4` (`MAGNUS_ENABLE_CAPTURE_LMR=1`)

The following moves are **never reduced**: TT moves, checks, recaptures, promotions, exclusion searches, mate windows, in-check positions, moves with extensions.

### Base Reduction Formula

**Quiet moves**:
```
base_reduction_fp = LnTable[depth] × LnTable[move_index + 1] + FP_ONE_PLY/2
```

**Capture moves**:
```
base_reduction_fp = max(0, (LnTable[depth] × LnTable[reduction_index + 1]) × 3/4 - FP_ONE_PLY/4)
```

Where `LnTable[i] = max(1, (2747/128) × ln(i))` is a precomputed log table (i ∈ [1, 64]).

### Adjustment Factors

The base reduction is modified by the following factors (positive = more reduction, negative = less reduction):

| Condition | Adjustment (FP units) | Reason |
|-----------|----------------------|--------|
| `improving == true` | `-FP_ONE_PLY/8` | Improving positions are more reliable |
| `improving == false` | `+FP_ONE_PLY/4` | Declining positions get more aggressive reduction |
| PV node | `-FP_ONE_PLY/2` | PV moves deserve caution |
| No TT move | `+FP_ONE_PLY/2` | No TT guidance |
| cut-node | `+FP_ONE_PLY/2` | Expected fast cutoff |
| all-node | proportional reduction | `fp -= fp / (depth + 1)` |
| TT move is capture (current is quiet) | `+FP_ONE_PLY/8` | TT suggests tactical position |
| TT depth ≥ current depth | `-FP_ONE_PLY/4` | TT confidence |
| TT bound == EXACT | `-FP_ONE_PLY/4` | Exact TT value |
| TT bound == LOWER | `-FP_ONE_PLY/8` | Lower bound TT |
| Next-ply cutoffs > 1 | `+min(2, n-1) × FP_ONE_PLY/4` | Consecutive cutoffs increase reduction |
| Parent reduction > 1 ply | `+FP_ONE_PLY/8` | Consecutive reduction penalty |

#### Capture SEE Adjustments

| SEE value | Adjustment |
|-----------|-----------|
| `see >= 200` | `-FP_ONE_PLY` |
| `see >= 0` | `-FP_ONE_PLY/2` |
| bad capture | `+FP_ONE_PLY/2` |
| `see < 0` (non-bad) | `+FP_ONE_PLY/4` |

#### History Score Bonus

Composite stat score:

**Quiet moves**:
```
stat_score = clamp(2×quiet_history + weighted_continuation + countermove_bonus/2 + ordering_score/8, -16384, 16384)
```

**Capture moves**:
```
stat_score = clamp(capture_history + see_bias_term×16 + clamp(see, -512, 512) - (bad_capture ? 256 : 0) + (gives_check ? 96 : 0), -8192, 8192)
```

History bonus: `history_bonus_fp = clamp(stat_score / divisor, ...)`, divisor=12 for quiets, 16 for captures.

### Reduction Caps

| Move type | Max reduction |
|-----------|--------------|
| PV quiet | 1 ply |
| Non-PV quiet | min(depth-1, 5) ply |
| Capture (see >= 200) | 1 ply |
| Capture (see >= 0) | min(depth-1, 2) ply |
| Capture (see < 0) | min(depth-1, 3) ply |

### LMR Re-search (`lmr_research_depth()`)

When a reduced search unexpectedly exceeds alpha, a re-search confirms:

- If `score > alpha + 96` and reduction ≥ 2 ply → re-search depth +1
- If `score < alpha + 8` → re-search depth -1
- If `score > best_score + 192` and reduction ≥ 3 ply → re-search depth another +1

Re-search depth is clamped to `[1, full_depth + 1]`.

### Key Functions

| Function | Description |
|----------|-------------|
| `decide_lmr(node, move)` | Unified LMR decision, returns `LmrDecision` |
| `lmr_research_depth(decision, full_depth, score, alpha, best_score)` | Compute re-search depth |

### Key Structs

| Struct | Description |
|--------|-------------|
| `LmrNodeContext` | Node-level parameters: depth, alpha, beta, ply, pv_node, cut_node, all_node, checked, improving, exclusion_search, mate_window, tt_move_present, tt_move_is_capture, static_eval, move_extension, next_ply_cutoff_count, parent_reduction_fp, tt_depth, tt_bound |
| `LmrMoveContext` | Move-level parameters: move, move_index, reduction_index, is_tt_move, quiet, capture, simple_capture, bad_capture, gives_check, recapture, promotion, ordering_score, quiet_history_score, continuation_score, countermove_bonus, capture_history_score, see_value, see_bias_term |
| `LmrDecision` | Decision output: stat_score, base_reduction_fp, final_reduction_fp, final_reduction, eligible |

### Compile-Time Macros

| Macro | Default | Description |
|-------|---------|-------------|
| `MAGNUS_ENABLE_PV_LMR` | 1 | Enable LMR in PV nodes |
| `MAGNUS_ENABLE_CAPTURE_LMR` | 1 | Enable LMR for capture moves |
