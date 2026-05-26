# Time.cpp - 时间管理 / Time Management

## 中文

### 概述
`Time.cpp` 实现了 MagnusChess 引擎的**时间管理**系统。它通过 `TimeManager` 类解析 UCI `go` 命令的时间参数，使用 Stockfish 风格的动态时间分配公式，并结合搜索历史自适应调整时间预算。

### 命名空间
所有类型和函数位于 `namespace magnus::timeman`。

### 核心类型

#### GoParams — UCI 时间参数

| 字段 | 类型 | 默认 | 描述 |
|------|------|------|------|
| `depth` | int | 0 | 固定深度（go depth N） |
| `nodes` | u64 | 0 | 固定节点数（go nodes N） |
| `movetime` | int | 0 | 固定时间（go movetime N） |
| `wtime` | int | 0 | 白方剩余时间 ms |
| `btime` | int | 0 | 黑方剩余时间 ms |
| `winc` | int | 0 | 白方增量 ms |
| `binc` | int | 0 | 黑方增量 ms |
| `movestogo` | int | 0 | 剩余步数（0=未知） |
| `ponder` | bool | false | 沉思模式 |
| `infinite` | bool | false | 无限搜索 |

#### TimeManager 类

| 方法 | 描述 |
|------|------|
| `new_game()` | 清空历史，开始新对局 |
| `build_limits(pos, params, limits)` | 从 GoParams 构建 SearchLimits（返回 false=参数无效） |
| `record_search(root, limits, result, elapsed_ms)` | 记录搜索结果到历史（用于自适应） |
| `history_size()` | 当前历史记录数 |

#### 内部类型

| 类型 | 描述 |
|------|------|
| `SearchRecord` | 单次搜索记录：side, fullmove_number, soft/hard time, elapsed, depth, score_cp, best_move |
| `HistoryStats` | 历史统计：samples, avg_usage_pct, avg_score_swing_cp, best_move_flip_pct |

### 时间分配流程

1. 解析 `GoParams`
2. 如果指定了 `movetime`/`depth`/`nodes` → 固定限制
3. 否则：
   - 计算可用时间 = `time[side] + inc[side] - overhead`
   - 根据 `movestogo` 或历史预估步数分摊
   - 软限制 = 可用时间 × scale_factor
   - 硬限制 = 可用时间 × max_factor

### 历史自适应

`TimeManager` 维护最近 **64 次**搜索的环形缓冲区，统计：
- 平均时间使用率
- 平均分数摆动
- 最佳走法变更频率

若最佳走法频繁变更 → 分配更多时间；若局势稳定 → 提早停止。

### 搜索限制（SearchLimits，定义在 Search.h）

由 `TimeManager::build_limits()` 填充的关键时间字段：
- `soft_time_ms` — 软时间限制（最佳停止点）
- `hard_time_ms` — 硬时间限制（绝对上限）
- `use_time_management` — 是否启用时间管理

### 设计要点
- 动态分配策略避免固定时间均分的粗暴方式
- 64 条历史记录实现经验驱动的自适应
- 软限制用于"考虑停止"，硬限制用于强制停止
- 与 UCI `go` 和 `ponderhit` 命令紧密集成

---

## English

### Overview
`Time.cpp` implements the **time management** system for the MagnusChess engine. It uses the `TimeManager` class to parse UCI `go` command time parameters, applies a Stockfish-style dynamic time allocation formula, and adaptively adjusts time budgets using search history.

### Namespace
All types and functions reside in `namespace magnus::timeman`.

### Core Types

#### GoParams — UCI Time Parameters

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `depth` | int | 0 | Fixed depth |
| `nodes` | u64 | 0 | Fixed node count |
| `movetime` | int | 0 | Fixed time (ms) |
| `wtime`/`btime` | int | 0 | White/Black remaining time (ms) |
| `winc`/`binc` | int | 0 | White/Black increment (ms) |
| `movestogo` | int | 0 | Moves to time control (0=unknown) |
| `ponder` | bool | false | Ponder mode |
| `infinite` | bool | false | Infinite search |

#### TimeManager Class

| Method | Description |
|--------|-------------|
| `new_game()` | Clear history for new game |
| `build_limits(pos, params, limits)` | Build SearchLimits from GoParams |
| `record_search(root, limits, result, elapsed_ms)` | Record search result for adaptive tuning |
| `history_size()` | Current history count |

### Internals

`SearchRecord` — per-search record (side, time, depth, score, best move).
`HistoryStats` — aggregated stats (usage %, score swing, best move flip rate).

### Time Allocation Flow

1. Parse `GoParams`
2. If fixed limits specified → use directly
3. Otherwise: compute available time, amortize by estimated remaining moves, compute soft/hard limits.

### History Adaptation

64-entry ring buffer tracking: average time usage, score swing, best move flip rate. Unstable best moves → more time; stable positions → stop earlier.

### Design Notes
- Dynamic allocation avoids crude equal time division
- 64-entry history enables empirically-driven adaptation
- Soft limit = "consider stopping", hard limit = forced stop
- Tightly integrated with UCI `go` and `ponderhit` commands
