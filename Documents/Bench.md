# Bench.cpp - 基准测试 / Benchmark

## 中文

### 概述
`Bench.cpp` 实现了 MagnusChess 引擎的**基准测试（Benchmark）**系统。通过在一组预定义局面上运行固定深度或固定时间的搜索，量化引擎的搜索速度和性能。同时集成了 perft 验证和走法生成速度测试。

### BenchConfig 配置结构

| 字段 | 类型 | 默认 | 描述 |
|------|------|------|------|
| `perft_depth` | int | 10 | Perft 深度 |
| `search_depth` | int | 6 | 搜索基准深度 |
| `search_movetime_ms` | int | 1000 | 搜索基准时间（ms） |
| `eval_iterations` | int | 20000 | 评估基准迭代次数 |
| `hash_mb` | size_t | 16 | 置换表大小（MB） |
| `threads` | size_t | 1 | 线程数 |
| `divide` | bool | false | Perft divide 模式 |
| `evalbench` | bool | false | 评估速度基准 |
| `search` | bool | false | 搜索基准模式 |
| `timed_search` | bool | false | 定时搜索基准模式 |
| `live_divide` | bool | false | 实时 divide 输出 |

### 核心函数

| 函数 | 签名 | 描述 |
|------|------|------|
| `set_start_position(pos)` | `(Position& pos)` | 设置标准初始局面 |
| `benchmark_perft(pos, mem, depth, threads)` | → `PerftBenchResult` | Perft 性能基准 |
| `run_search_bench(mem, depth, threads, use_nnue, emit_ponder, out)` | → `bool` | 固定深度搜索基准 |
| `run_timed_search_bench(mem, movetime_ms, threads, use_nnue, emit_ponder, out)` | → `bool` | 固定时间搜索基准 |
| `parse_config(argc, argv)` | → `BenchConfig` | 解析 CLI 参数 |
| `run_bench(argc, argv)` | → `int` | 基准测试入口（main 调用） |

### PerftBenchResult

| 字段 | 类型 | 描述 |
|------|------|------|
| `depth` | int | Perft 深度 |
| `nodes` | NodeCount (u64) | 总节点数 |
| `seconds` | double | 耗时（秒） |
| `nps` | double | 每秒节点数 |
| `threads` | size_t | 使用线程数 |

### 基准测试模式

| 模式 | 触发条件 | 描述 |
|------|---------|------|
| Perft | `BenchConfig::perft_depth > 0` | 走法生成正确性+速度验证 |
| 搜索基准 | `BenchConfig::search == true` | 固定深度搜索计时 |
| 定时搜索 | `BenchConfig::timed_search == true` | 固定时间搜索计时 |
| 评估基准 | `BenchConfig::evalbench == true` | 评估函数速度测试 |
| Divide | `BenchConfig::divide == true` | 分行 perft 输出 |

### CLI 用法

```
magnus bench [options]
```

选项由 `parse_config(argc, argv)` 解析。具体参数格式参见 `main()` 调用 `run_bench(argc, argv)`。

### 设计要点
- 支持单线程和多线程基准模式
- `set_start_position()` 仅设置初始局面（不接受 FEN 参数）
- Perft 结果通过 `PerftBenchResult` 结构体返回（含 nps）
- 评估基准 (`evalbench`) 用于测量 HCE/NNUE 的纯评估速度

---

## English

### Overview
`Bench.cpp` implements the **benchmark** system for the MagnusChess engine. It quantifies search speed and performance by running fixed-depth or fixed-time searches on a set of predefined positions, and also integrates perft verification and move generation speed testing.

### BenchConfig

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `perft_depth` | int | 10 | Perft depth |
| `search_depth` | int | 6 | Search bench depth |
| `search_movetime_ms` | int | 1000 | Search bench time (ms) |
| `eval_iterations` | int | 20000 | Eval bench iterations |
| `hash_mb` | size_t | 16 | TT size (MB) |
| `threads` | size_t | 1 | Thread count |
| `divide` | bool | false | Perft divide mode |
| `evalbench` | bool | false | Eval speed benchmark |
| `search` | bool | false | Search benchmark mode |
| `timed_search` | bool | false | Timed search benchmark |
| `live_divide` | bool | false | Live divide output |

### Core Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `set_start_position(pos)` | `(Position& pos)` | Set starting position |
| `benchmark_perft(pos, mem, depth, threads)` | → `PerftBenchResult` | Perft benchmark |
| `run_search_bench(mem, depth, threads, use_nnue, emit_ponder, out)` | → `bool` | Fixed-depth search bench |
| `run_timed_search_bench(mem, movetime_ms, threads, use_nnue, emit_ponder, out)` | → `bool` | Fixed-time search bench |
| `parse_config(argc, argv)` | → `BenchConfig` | Parse CLI args |
| `run_bench(argc, argv)` | → `int` | Benchmark entry (called by main) |

### Benchmark Modes

| Mode | Trigger | Description |
|------|---------|-------------|
| Perft | `perft_depth > 0` | Move gen correctness + speed |
| Search | `search == true` | Fixed-depth search timing |
| Timed search | `timed_search == true` | Fixed-time search timing |
| Eval | `evalbench == true` | Eval function speed test |
| Divide | `divide == true` | Per-line perft output |

### Design Notes
- Supports single and multi-threaded benchmark modes
- `set_start_position()` only sets the initial position (no FEN parameter)
- Perft results returned via `PerftBenchResult` struct (includes nps)
- Eval benchmark (`evalbench`) measures pure HCE/NNUE evaluation speed
