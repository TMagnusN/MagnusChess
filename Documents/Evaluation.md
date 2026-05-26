# Evaluation.cpp - 手写评估函数 / Handcrafted Evaluation

## 中文

### 概述
`Evaluation.cpp` 实现了 MagnusChess 引擎的**手写评估函数（Handcrafted Evaluation, HCE）**。虽然引擎也包含 NNUE/MNUE 评估，HCE 仍被用于增量评估的初始化、NNUE 回退和快速粗略评估场景。

### 命名空间
所有评估函数位于 `namespace magnus::eval`。

### 核心架构

#### 增量评估

`Position` 结构体维护按颜色的增量评估分数：
- `eval_mg[COLOR_NB]` — 中局 PSQT（Piece-Square Table）分数
- `eval_eg[COLOR_NB]` — 残局 PSQT 分数
- `eval_phase` — 局面阶段值（用于中局/残局插值）

增量钩子函数在 `make_move()`/`unmake_move()` 中自动调用：

| 函数 | 描述 |
|------|------|
| `on_piece_added(pos, color, pt, sq)` | 添加棋子时更新 PSQT |
| `on_piece_removed(pos, color, pt, sq)` | 移除棋子时更新 PSQT |
| `on_piece_moved(pos, color, pt, from, to)` | 移动棋子时更新 PSQT |
| `evaluate(pos)` | 完整 HCE 评估 |

#### 棋子价值与阶段

| 棋子 | MG 价值 | EG 价值 | 阶段增量 |
|------|---------|---------|----------|
| 兵 | 82 | 101 | 0 |
| 马 | 337 | 416 | 1 |
| 象 | 365 | 443 | 1 |
| 车 | 477 | 621 | 2 |
| 后 | 1025 | 1204 | 4 |
| 王 | 0 | 0 | 0 |

#### PSQT 表

每种棋子有两张 8×8 的 PSQT 表（中局和残局），从白方视角定义。黑方通过翻转得分表使用。

- 兵 PSQT：鼓励中心化和通路兵推进
- 马/象 PSQT：中心化奖励
- 车 PSQT：奖励开放线和第七横排
- 后 PSQT：中心化
- 王 PSQT：中局奖励王翼安全，残局奖励中心化

#### 阶段插值

```
tapered_score = sum of (mg[color] × phase + eg[color] × (24 - phase)) / 24
```

24 是初始阶段值（所有棋子都在时的 phase 值），范围 0-24。

#### 完整评估（`evaluate()`）

除增量 PSQT 外，完整评估还计算：
- **兵形** — 使用兵形表缓存
- **材料** — 通过材料表查找修正

### 设计要点
- PSQT 值直接嵌入增量评估中，`make_move` 和 `unmake_move` 自动维护
- 按颜色分别存储 `eval_mg`/`eval_eg` 数组（而非单值 psqt_score[MG/EG]）
- 手写评估与 NNUE/MNUE 评估互补，在神经网络不可用时提供合理估值
- 阶段插值平滑过渡中局和残局特征

---

## English

### Overview
`Evaluation.cpp` implements the **Handcrafted Evaluation (HCE)** for the MagnusChess engine. Although the engine also includes NNUE/MNUE evaluation, HCE is still used for incremental evaluation initialization, neural network fallback, and fast coarse evaluation.

### Namespace
All evaluation functions reside in `namespace magnus::eval`.

### Core Architecture

#### Incremental Evaluation

The `Position` struct maintains per-color incremental evaluation scores:
- `eval_mg[COLOR_NB]` — Midgame PSQT scores
- `eval_eg[COLOR_NB]` — Endgame PSQT scores
- `eval_phase` — Position phase value (for MG/EG interpolation)

Incremental hooks are automatically called in `make_move()`/`unmake_move()`:

| Function | Description |
|----------|-------------|
| `on_piece_added(pos, color, pt, sq)` | Update PSQT when piece added |
| `on_piece_removed(pos, color, pt, sq)` | Update PSQT when piece removed |
| `on_piece_moved(pos, color, pt, from, to)` | Update PSQT when piece moved |
| `evaluate(pos)` | Full HCE evaluation |

#### Piece Values and Phase

| Piece | MG Value | EG Value | Phase Increment |
|-------|----------|----------|-----------------|
| Pawn | 82 | 101 | 0 |
| Knight | 337 | 416 | 1 |
| Bishop | 365 | 443 | 1 |
| Rook | 477 | 621 | 2 |
| Queen | 1025 | 1204 | 4 |
| King | 0 | 0 | 0 |

#### Phase Interpolation

```
tapered_score = sum of (mg[color] × phase + eg[color] × (24 - phase)) / 24
```

24 is the initial phase value (phase when all pieces are present), range 0-24.

#### Full Evaluation (`evaluate()`)

Beyond incremental PSQT, full evaluation also computes pawn structure and material corrections.

### Design Notes
- PSQT values embedded in incremental evaluation, auto-maintained by `make_move`/`unmake_move`
- Per-color `eval_mg`/`eval_eg` arrays (not single `psqt_score[MG/EG]` values)
- HCE complements NNUE/MNUE evaluation, providing reasonable estimates when neural networks are unavailable
- Phase interpolation smoothly transitions between midgame and endgame characteristics
