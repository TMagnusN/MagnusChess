# Nnue.cpp / Mnue.cpp - 神经网络评估 / NNUE Evaluation

## 中文

### 概述
MagnusChess 包含**两套**神经网络评估系统：

1. **NNUE (Chess768)** — 传统 NNUE 架构，768→128→1，`src/Nnue.cpp`
2. **MNUE-P2/P4** — 新一代双网络架构，P2 快速过滤 + P4 精确精炼，`src/Mnue.cpp`

两套系统共享 `Position` 中的增量累加器，通过钩子函数在 `make_move`/`unmake_move` 时自动更新。

---

## NNUE (Chess768) — `namespace magnus::nnue`

### 网络架构

```
输入层:   768 neurons (双视角棋盘编码)
隐藏层:   128 neurons (全连接 + 量化激活)
输出层:   1 scalar (原始评估值，非 cp)
```

- `kInputSize = 768` = 2 视角 × 6 棋子类型 × 64 格
- `kHiddenSize = 128`（必须为 16 的倍数以支持 AVX2）
- `kActivationClip = 255`（ScreLU 截断阈值）

#### 激活函数：ScreLU

```
ScreLU(x) = clamp(x, 0, 255)²
```

### 增量更新

`Position` 结构体为双方各维护一个累加器（`nnue_acc[COLOR_NB][kHiddenSize]`），通过以下钩子在走法执行/撤销时增量更新：

- `on_piece_added(pos, color, pt, sq)` — 添加棋子
- `on_piece_removed(pos, color, pt, sq)` — 移除棋子
- `on_piece_moved(pos, color, pt, from, to)` — 移动棋子

### 分数转换链

| 函数 | 输入 | 输出 | 描述 |
|------|------|------|------|
| `eval(pos)` | Position | int | 原始 NNUE 输出 |
| `search_score(v, pos)` | 原始值 | int | 转为搜索内部分数 |
| `search_score_to_cp(score, pos)` | 搜索分 | int | 转为 UCI 厘兵显示 |
| `search_score_to_wdl(score, pos)` | 搜索分 | WdlTriplet | 转为胜率和三元组 |
| `search_score_to_winrate(score, pos)` | 搜索分 | int | 转为胜率 (0-1000) |
| `win_rate_params(pos)` | Position | WinRateParams | Sigmoid 参数 (a=中心, b=斜率) |
| `win_rate_model(v, pos)` | 原始值 | int | 直接转胜率 |

### 网络生命周期

| 函数 | 描述 |
|------|------|
| `load(path)` | 从文件加载网络（支持 .nnue 和 .bin 格式） |
| `unload()` | 卸载当前网络 |
| `loaded()` | 网络是否已加载 |
| `path()` | 当前加载的文件路径 |
| `description()` | 网络架构描述字符串 |
| `last_error()` | 最后一次错误信息 |

### 网络文件格式

- **原生 .nnue** — Magnus 自有格式，含标头
- **Bullet .bin** — Rust simple.rs 量化格式，无标头

### AVX2 SIMD 加速

关键计算路径使用 AVX2 指令集：
- 256 位向量化权重累加（每次处理 16 个隐藏神经元，128/16 = 8 轮）
- 累加器对齐到 64 字节以支持 SIMD 加载

---

## MNUE-P2/P4 — `namespace magnus::mnue`

### 架构概述

MNUE 采用**双网络级联**架构：

| 网络 | 结构 | Hidden | 用途 |
|------|------|--------|------|
| **P2** (Filter) | 10240 → 1024 → 1 | 1024 | 快速基础评估，用于大部分搜索节点 |
| **P4** (Refine) | 20480 → 5120 → 1 | 5120 | 精确精炼评估，仅用于选定搜索节点 |

P4 是有意惰性的：在第一阶段集成中不写入 TT 的原始 eval。

### P2 网络详细规格

```
输入: 10240 特征 = 16 input_buckets × 2 colors × 5 non-king piece types × 64 squares
隐藏: 1024 neurons (i16)
架构 ID: 2
```

P2 使用**持久增量累加器**：`Position::mnue_p2_acc[COLOR_NB][1024]`，在 `make_move`/`unmake_move` 时自动更新。

### P4 网络详细规格

```
输入: 20480 特征 = 32 input_buckets × 2 colors × 5 non-king piece types × 64 squares
隐藏: 5120 neurons (i16)
架构 ID: 4
```

P4 使用**惰性重建**：不维护增量累加器，每次评估时从零重建。

### P2/P4 生命周期

| 函数 | 描述 |
|------|------|
| `load_p2(path)` / `load_p4(path)` | 加载 P2/P4 网络 |
| `unload_p2()` / `unload_p4()` / `unload_all()` | 卸载网络 |
| `p2_loaded()` / `p4_loaded()` | 检查加载状态 |
| `p2_path()` / `p4_path()` | 当前文件路径 |
| `last_error()` | 最后一次错误信息 |

### P2/P4 评估

| 函数 | 描述 |
|------|------|
| `eval_p2(pos)` | P2 基础评估（热路径，使用增量累加器） |
| `eval_p4_lazy(pos)` | P4 惰性评估（冷路径，每次重建） |
| `debug_eval_p2_reference(pos)` | P2 参考评估（完整重建，用于验证增量正确性） |
| `debug_check_p2_incremental(pos, out)` | 验证 P2 增量累加器 vs 完整重建 |

### MNUE 分数转换

MNUE 模块复用 `nnue::WinRateParams` 和 `nnue::WdlTriplet` 类型，提供自己的转换函数：

| 函数 | 描述 |
|------|------|
| `search_score(v, pos)` | 原始值 → 搜索分 |
| `to_cp(v, pos)` | 原始值 → 厘兵 |
| `material_units(pos)` | 当前局面的子力单位 |
| `win_rate_params(pos)` | Sigmoid 参数 |
| `win_rate_model(v, pos)` | 原始值 → 胜率 |
| `search_score_to_cp(score, pos)` | 搜索分 → 厘兵 |
| `search_score_to_winrate(score, pos)` | 搜索分 → 胜率 |
| `search_score_to_wdl(score, pos)` | 搜索分 → 胜率和三元组 |

### MNUE 增量钩子

与 NNUE 相同接口，由 Position 的走法执行/撤销调用：

- `on_position_cleared(pos)` — 清空局面
- `on_piece_added(pos, color, pt, sq)` — 添加棋子
- `on_piece_removed(pos, color, pt, sq)` — 移除棋子
- `on_piece_moved(pos, color, pt, from, to)` — 移动棋子

### 调试工具

- `debug_dump_p2_features(pos, out)` — UCI 命令 `mnuedebug` 的实现，打印 P2 输入桶、活跃特征索引、输出桶和原始 P2 评估值
- `p2_i32_forward_enabled()` — 是否启用 i32 前向传播
- `p2_w1_max_abs()` — 输出权重最大绝对值

### 评估选择逻辑（概念）

```
if (P2 已加载):
    eval = eval_p2(pos)
    if (需要精确评估 且 P4 已加载):
        eval = eval_p4_lazy(pos)
else if (NNUE 已加载):
    eval = nnue::eval(pos)
else:
    eval = eval::evaluate(pos)  // HCE 回退
```

### 设计要点
- P2 使用持久增量累加器（O(隐藏层) 更新而非 O(输入×隐藏层)），适合搜索热路径
- P4 采用惰性重建策略，仅在关键节点（如根节点、PV 节点）使用
- 双网络架构允许在速度和精度之间灵活权衡
- 增量累加器通过 `debug_check_p2_incremental()` 验证正确性
- 训练器/引擎特征索引对齐通过 `debug_dump_p2_features()` 验证

---

## English

### Overview
MagnusChess includes **two** neural network evaluation systems:

1. **NNUE (Chess768)** — Traditional NNUE architecture, 768→128→1, `src/Nnue.cpp`
2. **MNUE-P2/P4** — Next-gen dual-network architecture, P2 fast filter + P4 precision refine, `src/Mnue.cpp`

Both share incremental accumulators in `Position`, updated automatically via hooks during `make_move`/`unmake_move`.

---

## NNUE (Chess768) — `namespace magnus::nnue`

### Network Architecture

```
Input:   768 neurons (dual-perspective board encoding)
Hidden:  128 neurons (fully connected + quantized activation)
Output:  1 scalar (raw eval, not centipawns)
```

- `kInputSize = 768` = 2 perspectives × 6 piece types × 64 squares
- `kHiddenSize = 128` (must be multiple of 16 for AVX2)
- `kActivationClip = 255` (ScreLU clamp threshold)

#### Activation: ScreLU

```
ScreLU(x) = clamp(x, 0, 255)²
```

### Incremental Updates

`Position` maintains one accumulator per side (`nnue_acc[COLOR_NB][kHiddenSize]`), updated incrementally via hooks during move make/unmake.

### Score Conversion Chain

| Function | Input | Output | Description |
|----------|-------|--------|-------------|
| `eval(pos)` | Position | int | Raw NNUE output |
| `search_score(v, pos)` | raw | int | Internal search score |
| `search_score_to_cp(score, pos)` | search | int | UCI centipawn display |
| `search_score_to_wdl(score, pos)` | search | WdlTriplet | Win-draw-loss triplet |
| `win_rate_params(pos)` | Position | WinRateParams | Sigmoid params (a=center, b=slope) |
| `win_rate_model(v, pos)` | raw | int | Direct to win rate |

### Network Lifecycle

| Function | Description |
|----------|-------------|
| `load(path)` | Load network from file (supports .nnue and .bin) |
| `unload()` | Unload current network |
| `loaded()` / `path()` / `description()` / `last_error()` | State queries |

### File Formats

- **Native .nnue** — Magnus proprietary format with header
- **Bullet .bin** — Rust simple.rs quantized format, no header

### AVX2 SIMD

Key computation paths use AVX2: 256-bit vectorized weight accumulation (16 hidden neurons per iteration, 128/16 = 8 rounds).

---

## MNUE-P2/P4 — `namespace magnus::mnue`

### Architecture Overview

MNUE uses a **dual-network cascade**:

| Network | Structure | Hidden | Purpose |
|---------|-----------|--------|---------|
| **P2** (Filter) | 10240 → 1024 → 1 | 1024 | Fast base eval for most search nodes |
| **P4** (Refine) | 20480 → 5120 → 1 | 5120 | Precision refine, selected nodes only |

P4 is intentionally lazy: not written into TT raw eval in first integration stage.

### P2 Specifications

```
Input: 10240 features = 16 buckets × 2 colors × 5 non-king types × 64 squares
Hidden: 1024 neurons (i16)
Arch ID: 2
```

P2 uses persistent incremental accumulator: `Position::mnue_p2_acc[COLOR_NB][1024]`.

### P4 Specifications

```
Input: 20480 features = 32 buckets × 2 colors × 5 non-king types × 64 squares
Hidden: 5120 neurons (i16)
Arch ID: 4
```

P4 uses lazy rebuild: no incremental accumulator, rebuilt from scratch each evaluation.

### Lifecycle

| Function | Description |
|----------|-------------|
| `load_p2(path)` / `load_p4(path)` | Load P2/P4 network |
| `unload_p2()` / `unload_p4()` / `unload_all()` | Unload networks |
| `p2_loaded()` / `p4_loaded()` | Check load state |

### Evaluation

| Function | Description |
|----------|-------------|
| `eval_p2(pos)` | P2 base eval (hot path, incremental accumulator) |
| `eval_p4_lazy(pos)` | P4 lazy eval (cold path, rebuild each time) |
| `debug_eval_p2_reference(pos)` | P2 reference eval (full rebuild, verify incremental correctness) |
| `debug_check_p2_incremental(pos, out)` | Verify P2 incremental accumulator vs full rebuild |

### MNUE Score Conversion

Reuses `nnue::WinRateParams` and `nnue::WdlTriplet` types with its own conversion functions.

### MNUE Incremental Hooks

Same interface as NNUE, called by Position move make/unmake.

### Debug Tools

- `debug_dump_p2_features(pos, out)` — UCI `mnuedebug` command: prints P2 input buckets, active feature indices, output bucket, raw P2 eval
- `p2_i32_forward_enabled()` — Whether i32 forward is enabled
- `p2_w1_max_abs()` — Max absolute output weight

### Evaluation Selection Logic (conceptual)

```
if (P2 loaded):
    eval = eval_p2(pos)
    if (need precise && P4 loaded):
        eval = eval_p4_lazy(pos)
else if (NNUE loaded):
    eval = nnue::eval(pos)
else:
    eval = eval::evaluate(pos)  // HCE fallback
```

### Design Notes
- P2 uses persistent incremental accumulators (O(hidden) update vs O(input×hidden)), ideal for search hot path
- P4 uses lazy rebuild strategy, used only at critical nodes (root, PV)
- Dual-network architecture enables flexible speed/accuracy trade-off
- Incremental accumulator correctness verified via `debug_check_p2_incremental()`
- Trainer/engine feature index alignment verified via `debug_dump_p2_features()`
