# Nmp.cpp - 空走裁剪 / Null Move Pruning

## 中文

### 概述
`Nmp.cpp` 实现了 MagnusChess 引擎的**空走裁剪（Null Move Pruning, NMP）**系统。NMP 是一种高效的搜索优化技术，通过让对手"连走两步"来检测当前局面是否好得不需要仔细搜索。

### 算法原理

空走裁剪基于一个简单的假设：如果让对手走两步（己方跳过一步），对方仍无法获得优势，则己方当前局面已足够好，可直接剪枝。

### 核心 API

**`decide_null_move(const NmpNodeContext& node)`** → 返回 `NmpDecision`

决策通过两个结构体分离输入和输出：

- **`NmpNodeContext`** — 输入上下文，由 `pvs()` 填充
- **`NmpDecision`** — 决策输出

**`nmp_disabled_for_ply(int ply, int nmp_min_ply)`** — 检查当前 ply 是否被禁用空步（当之前的 NMP 验证失败后，在一定 ply 范围内禁用）

### 前置条件检查

空走裁剪在以下情况**不**执行：

1. `allow_null == false` — 可能被奇异延展暂时禁用
2. `pv_node == true` — PV 节点需要保留主变异
3. `checked == true` — 被将军时跳过一步是非法操作
4. `exclusion_search == true` — 排除搜索不应空步
5. `depth < 3` — 深度太浅
6. `material_ok == false` — 无非兵材料（防止 zugzwang 误剪）
7. `nmp_disabled_for_ply(...)` — 当前 ply 被禁用
8. `static_eval < eval_gate` — 静态评估未达门槛

额外的 TT 保护：
- 如果 TT 命中且为上界（UPPER）且 TT 分数 < beta → 不执行（TT 已表明无法截断）

### 评估门槛

```
eval_gate = beta - 8 × depth + 160 - (improving ? 64 : 0)
```

### 空走深度计算

```
reduction = 2 + depth/4 + clamp(eval_margin / 96, 0, 3)
          + cut_node
          + !improving
          + !tt_move_present
          - (TT hit LOWER && tt_score >= beta)

reduction = clamp(reduction, 2, max(2, depth - 2))

null_depth = max(0, depth - 1 - reduction)
```

其中 `eval_margin = static_eval - beta`。

### 验证搜索

当 NMP 触发后（`eligible == true`）：

**不需要验证**（`depth < 16` 或 `nmp_min_ply != 0`）：
直接返回截断分数。

**需要验证**（`depth >= 16` 且 `nmp_min_ply == 0`）：
执行验证搜索，参数：
- `verify_depth = null_depth` — 使用与空步相同的深度
- `verify_min_ply = ply + max(2, 3 × max(1, null_depth) / 4)` — 验证后在此 ply 范围内禁用空步

### 关键结构体

#### NmpNodeContext（输入）

| 字段 | 类型 | 描述 |
|------|------|------|
| `depth` | int | 当前搜索深度 |
| `ply` | int | 从根节点算起的半步数 |
| `alpha` | int | 当前 alpha 边界 |
| `beta` | int | 当前 beta 边界 |
| `static_eval` | int | 静态评估值 |
| `tt_score` | int | TT 存储的分数 |
| `nmp_min_ply` | int | 禁用空步的最小 ply |
| `allow_null` | bool | 是否允许空步 |
| `pv_node` | bool | 是否 PV 节点 |
| `cut_node` | bool | 是否 cut-node |
| `checked` | bool | 是否被将军 |
| `improving` | bool | 静态评估是否改善中 |
| `exclusion_search` | bool | 是否排除搜索 |
| `tt_hit` | bool | TT 是否命中 |
| `tt_move_present` | bool | TT 是否有走法 |
| `material_ok` | bool | 是否有足够非兵材料 |
| `tt_bound` | Bound | TT 边界类型 |

#### NmpDecision（输出）

| 字段 | 类型 | 描述 |
|------|------|------|
| `eligible` | bool | 是否触发空步 |
| `requires_verification` | bool | 是否需要验证搜索 |
| `eval_gate` | int | 评估门槛 |
| `eval_margin` | int | 评估余量 (static_eval - beta) |
| `reduction` | int | 空步减免 ply 数 |
| `null_depth` | int | 空步搜索深度 |
| `verify_depth` | int | 验证搜索深度 |
| `verify_min_ply` | int | 验证后禁用空步的最小 ply |

### 关键常量

| 常量 | 值 | 描述 |
|------|-----|------|
| `NMP_STATIC_BASE` | 160 | 基础评估门槛 |
| `NMP_STATIC_DEPTH_SLOPE` | 8 | 深度斜率 |
| `NMP_IMPROVING_MARGIN` | 64 | 改善局面放宽量 |
| `NMP_EVAL_BUCKET` | 96 | 评估余量分桶大小 |
| `NMP_MIN_REDUCTION` | 2 | 最小减免 |
| `NMP_VERIFICATION_MIN_DEPTH` | 16 | 验证搜索最小深度 |
| `NMP_VERIFICATION_MIN_SPAN` | 2 | 验证后最小禁用跨度 |

### 设计要点
- NMP 节省大量搜索时间，特别是在中局阶段
- 子力阈值检查避免残局 zugzwang 风险
- 动态减免公式使深层节点获得更激进的裁剪
- 验证搜索防止深层 zugzwang 或战术密集局面的误剪
- TT 上界保护避免重复无效空步

---

## English

### Overview
`Nmp.cpp` implements the **Null Move Pruning (NMP)** system for the MagnusChess engine. NMP is an efficient search optimization technique that detects whether the current position is good enough to skip detailed search by letting the opponent "move twice."

### Algorithm Principle

Null move pruning is based on a simple assumption: if letting the opponent move twice (skipping one's own turn) still fails to give them an advantage, then the current position is already good enough for a cutoff.

### Core API

**`decide_null_move(const NmpNodeContext& node)`** → returns `NmpDecision`

**`nmp_disabled_for_ply(int ply, int nmp_min_ply)`** → checks if NMP is disabled at this ply

### Precondition Checks

Null move pruning is **not** executed when:

1. `allow_null == false` — May be temporarily disabled by singular extension
2. `pv_node == true` — PV nodes must preserve the principal variation
3. `checked == true` — Skipping a move while in check is illegal
4. `exclusion_search == true` — Exclusion search should not null-move
5. `depth < 3` — Too shallow
6. `material_ok == false` — No non-pawn material (zugzwang risk)
7. `nmp_disabled_for_ply(...)` — Disabled at this ply
8. `static_eval < eval_gate` — Static eval below threshold

TT guard: if TT hit with UPPER bound and tt_score < beta → skip (TT already indicates no cutoff).

### Eval Gate

```
eval_gate = beta - 8 × depth + 160 - (improving ? 64 : 0)
```

### Null Move Depth Calculation

```
reduction = 2 + depth/4 + clamp(eval_margin / 96, 0, 3)
          + cut_node
          + !improving
          + !tt_move_present
          - (TT hit LOWER && tt_score >= beta)

reduction = clamp(reduction, 2, max(2, depth - 2))

null_depth = max(0, depth - 1 - reduction)
```

Where `eval_margin = static_eval - beta`.

### Verification Search

**No verification needed** (`depth < 16` or `nmp_min_ply != 0`): direct cutoff.

**Verification needed** (`depth >= 16` and `nmp_min_ply == 0`):
- `verify_depth = null_depth`
- `verify_min_ply = ply + max(2, 3 × max(1, null_depth) / 4)`

### Key Structs

#### NmpNodeContext (Input)

| Field | Type | Description |
|-------|------|-------------|
| `depth` | int | Current search depth |
| `ply` | int | Half-moves from root |
| `alpha` | int | Current alpha bound |
| `beta` | int | Current beta bound |
| `static_eval` | int | Static evaluation |
| `tt_score` | int | TT stored score |
| `nmp_min_ply` | int | Min ply where NMP is disabled |
| `allow_null` | bool | Whether null move is allowed |
| `pv_node` | bool | Whether PV node |
| `cut_node` | bool | Whether cut-node |
| `checked` | bool | Whether in check |
| `improving` | bool | Whether static eval is improving |
| `exclusion_search` | bool | Whether exclusion search |
| `tt_hit` | bool | Whether TT hit |
| `tt_move_present` | bool | Whether TT has a move |
| `material_ok` | bool | Whether enough non-pawn material |
| `tt_bound` | Bound | TT bound type |

#### NmpDecision (Output)

| Field | Type | Description |
|-------|------|-------------|
| `eligible` | bool | Whether NMP triggers |
| `requires_verification` | bool | Whether verification is needed |
| `eval_gate` | int | Eval threshold |
| `eval_margin` | int | Eval margin (static_eval - beta) |
| `reduction` | int | Reduction in plies |
| `null_depth` | int | Null move search depth |
| `verify_depth` | int | Verification search depth |
| `verify_min_ply` | int | Min ply to disable NMP after verification |

### Key Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `NMP_STATIC_BASE` | 160 | Base eval gate |
| `NMP_STATIC_DEPTH_SLOPE` | 8 | Depth slope |
| `NMP_IMPROVING_MARGIN` | 64 | Relaxation for improving |
| `NMP_EVAL_BUCKET` | 96 | Eval margin bucket size |
| `NMP_MIN_REDUCTION` | 2 | Minimum reduction |
| `NMP_VERIFICATION_MIN_DEPTH` | 16 | Min depth for verification |
| `NMP_VERIFICATION_MIN_SPAN` | 2 | Min span after verification |

### Design Notes
- NMP saves significant search time, especially in middlegame positions
- Material threshold avoids endgame zugzwang risks
- Dynamic reduction formula enables more aggressive pruning at deeper nodes
- Verification search prevents false cutoffs in deep zugzwang or tactical positions
- TT upper-bound guard avoids repeated ineffective null moves
