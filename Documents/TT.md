# TT.cpp - 置换表 / Transposition Table

## 中文

### 概述
`TT.cpp` 实现了 MagnusChess 引擎的**置换表（Transposition Table, TT）**，是搜索系统的核心缓存结构。使用 4 路组相联哈希桶、64 字节对齐的缓存行布局和深度-年龄替换策略。

### 命名空间
所有类型和函数位于 `namespace magnus::memory`。

### 核心结构

#### TTCluster（64 字节对齐）

物理存储 4 个打包的 TT 条目，每个条目按列存储：

| 字段数组 | 类型 | 描述 |
|---------|------|------|
| `tag32[4]` | u32 | 32 位标签（key 的高位，用于快速匹配） |
| `move[4]` | u16 | 最佳走法 |
| `score[4]` | i16 | 搜索分数 |
| `eval[4]` | i16 | 静态评估 |
| `depth[4]` | i16 | 搜索深度 |
| `age[4]` | u8 | 年龄（用于替换决策） |
| `flags[4]` | u8 | 边界类型（NONE/UPPER/LOWER/EXACT） |

#### TTData（逻辑视图）

单个条目的逻辑表示：`tag32, move, score, eval, depth, age, flags, spare`。

#### TT 结构体

| 字段 | 类型 | 描述 |
|------|------|------|
| `clusters` | TTCluster* | 桶数组 |
| `cluster_count` | size_t | 桶数量 |
| `cluster_mask` | size_t | 索引掩码 |
| `locks` | atomic_flag* | 细粒度锁数组 |
| `generation` | u8 | 当前搜索代数 |

#### 边界类型（Bound）

| 枚举值 | 描述 |
|--------|------|
| `BOUND_NONE` | 无效条目 |
| `BOUND_UPPER` | 上界（搜索分数 ≤ 真实值） |
| `BOUND_LOWER` | 下界（搜索分数 ≥ 真实值） |
| `BOUND_EXACT` | 精确值 |

### 核心 API

| 函数 | 描述 |
|------|------|
| `tt_index(tt, key)` | 计算 `key & cluster_mask` 索引 |
| `tt_prefetch(tt, key)` | 软件预取 TT 桶（减少缓存未命中） |
| `tt_probe(tt, key)` | 探测 TT，返回 `TTProbe`（hit, slot, data） |
| `tt_save(tt, key, move, score, eval, depth, bound, pv)` | 保存条目（含替换策略） |
| `tt_hashfull(tt, max_age)` | 估算 TT 占用率（千分比） |
| `tt_clear(tt)` | 清空 TT |
| `tt_resize_mb(tt, mb)` | 调整 TT 大小（MB） |
| `tt_new_search(tt)` | 新搜索开始（递增 generation） |
| `tt_free(tt)` | 释放 TT 内存 |

### 替换策略

`tt_replacement_score(cluster, lane, current_age)` 计算替换优先级：
- 优先覆盖空槽位
- 旧世代条目优先替换
- 浅深度条目优先替换

### 设计要点
- 64 字节对齐的集群布局确保一个缓存行内完成探测/存储
- 细粒度锁数组支持多线程并发
- `tt_prefetch()` 软件预取减少搜索热路径的缓存延迟
- `tt_hashfull()` 采样前 1000 个桶估算占用率

---

## English

### Overview
`TT.cpp` implements the **Transposition Table (TT)** for the MagnusChess engine, the core caching structure of the search system. Uses 4-way set-associative hash buckets with 64-byte aligned cache-line layout and depth-age replacement policy.

### Namespace
All types and functions reside in `namespace magnus::memory`.

### Core Structures

#### TTCluster (alignas 64)

Columnar storage of 4 packed entries: `tag32[4]`, `move[4]`, `score[4]`, `eval[4]`, `depth[4]`, `age[4]`, `flags[4]`.

#### TTData (Logical View)

Single entry: `tag32, move, score, eval, depth, age, flags, spare`.

#### Bound Types

`BOUND_NONE`, `BOUND_UPPER`, `BOUND_LOWER`, `BOUND_EXACT`.

### Core API

| Function | Description |
|----------|-------------|
| `tt_index(tt, key)` | Compute `key & cluster_mask` |
| `tt_prefetch(tt, key)` | Software prefetch TT bucket |
| `tt_probe(tt, key)` | Probe TT, returns `TTProbe` (hit, slot, data) |
| `tt_save(tt, key, move, score, eval, depth, bound, pv)` | Save entry with replacement policy |
| `tt_hashfull(tt)` | Estimate occupancy (permille) |
| `tt_clear` / `tt_resize_mb` / `tt_new_search` / `tt_free` | Lifecycle management |

### Replacement Policy

Prefer empty slots, then older generations, then shallower depth.

### Design Notes
- 64-byte aligned clusters: probe/store within one cache line
- Fine-grained lock array for multi-threaded concurrency
- `tt_prefetch()` software prefetch reduces cache latency in search hot path
- `tt_hashfull()` samples first 1000 buckets for occupancy estimation
