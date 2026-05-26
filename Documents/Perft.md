# Perft.cpp - 走法生成性能测试 / Perft Testing

## 中文

### 概述
`Perft.cpp` 实现了 MagnusChess 引擎的 **Perft（走法生成验证）**系统，通过计算给定深度下所有可能走法序列的总数来验证走法生成器和走法执行/撤销的正确性，同时作为性能基准测试工具。

### 核心函数

| 函数 | 签名 | 描述 |
|------|------|------|
| `perft(pos, mem, depth)` | → `NodeCount` | 单线程递归 Perft |
| `perft_mt(pos, mem, depth, threads)` | → `NodeCount` | 多线程 Perft（前沿扩展 + 工作窃取） |
| `divide(pos, mem, depth, os, threads, live)` | — | 分行输出 perft 统计 |
| `benchmark_generation(pos, mem, iterations)` | → `GenSpeedResult` | 走法生成速度基准 |

### PerftDivideRow

| 字段 | 类型 | 描述 |
|------|------|------|
| `index` | int | 走法序号 |
| `move` | string | 走法 UCI 表示 |
| `nodes` | NodeCount (u64) | 子树节点数 |
| `seconds` | double | 耗时 |
| `knps` | double | 每秒千节点 |

### GenSpeedResult

| 字段 | 类型 | 描述 |
|------|------|------|
| `iterations` | u64 | 迭代次数 |
| `total_moves` | u64 | 生成的总走法数 |
| `seconds` | double | 耗时 |
| `generations_per_second` | double | 每秒生成次数 |
| `moves_per_second` | double | 每秒生成走法数 |

### 标准 Perft 结果（初始局面）

| 深度 | 节点数 |
|------|--------|
| 0 | 1 |
| 1 | 20 |
| 2 | 400 |
| 3 | 8,902 |
| 4 | 197,281 |
| 5 | 4,865,609 |
| 6 | 119,060,324 |
| 7 | 3,195,901,860 |

### 多线程 Perft

采用**前沿扩展（Frontier Expansion）**策略：
1. 主线程将第一层走法分布到工作队列
2. 工作线程独立计算各自子树
3. 空闲线程从其他线程"窃取"排队任务（工作窃取）

### 设计要点
- 与已知标准值比较验证走法生成正确性
- 工作窃取策略在多核系统上实现接近线性的加速
- Divide 支持实时和离线两种输出模式

---

## English

### Overview
`Perft.cpp` implements the **Perft (move generation verification)** system for the MagnusChess engine, validating move generator and move execution/undo correctness by counting all possible move sequences at a given depth.

### Core Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `perft(pos, mem, depth)` | → `NodeCount` | Single-threaded recursive perft |
| `perft_mt(pos, mem, depth, threads)` | → `NodeCount` | Multi-threaded perft (frontier expansion + work stealing) |
| `divide(pos, mem, depth, os, threads, live)` | — | Per-line perft output |
| `benchmark_generation(pos, mem, iterations)` | → `GenSpeedResult` | Move generation speed benchmark |

### Key Types

`NodeCount` = `u64`, `PerftDivideRow` (index, move string, nodes, seconds, knps), `GenSpeedResult` (iterations, total_moves, seconds, generations/sec, moves/sec).

### Standard Perft Results (Starting Position)

Depths 0-7: 1, 20, 400, 8,902, 197,281, 4,865,609, 119,060,324, 3,195,901,860.

### Multi-Threaded Perft

Frontier expansion + work stealing for near-linear multi-core speedup.

### Design Notes
- Compare against known standard values for correctness validation
- Work stealing achieves near-linear speedup
- Divide supports live and offline output modes
