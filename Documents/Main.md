# Main.cpp - 程序入口 / Program Entry Point

## 中文

### 概述
`Main.cpp` 是 MagnusChess 国际象棋引擎的入口文件。它负责解析命令行参数，根据子命令将控制流分派到对应的子系统。

### 核心功能

**`main()`** 函数直接包含所有分发逻辑：

1. 在 Windows 上设置控制台编码为 UTF-8 并启用虚拟终端处理（ANSI 转义序列支持）
2. 如果 `argc <= 1`（无子命令）→ 调用 `magnus::run_uci()` 进入 UCI 协议模式
3. 如果 `argv[1] == "uci"` → 调用 `magnus::run_uci()`
4. 其他所有情况 → 调用 `magnus::run_bench(argc, argv)` 进入基准测试模式

注意：`perft` 不是独立的 CLI 子命令；perft 功能通过 `bench` 命令内部的 `BenchConfig::perft_depth` 参数使用。

### 运行模式

| 模式 | 触发条件 | 入口函数 |
|------|---------|---------|
| UCI 通信 | 无参数 或 `uci` | `magnus::run_uci()` |
| 基准测试 | 其他所有参数 | `magnus::run_bench(argc, argv)` |

### Windows 平台初始化

在 `_WIN32` 平台上，`main()` 执行以下初始化：
- `SetConsoleOutputCP(CP_UTF8)` — 控制台输出 UTF-8
- `SetConsoleCP(CP_UTF8)` — 控制台输入 UTF-8
- `SetConsoleMode(..., ENABLE_VIRTUAL_TERMINAL_PROCESSING)` — 启用 ANSI 转义序列

### 设计要点
- 极简设计：`main()` 只做平台初始化和命令路由，所有具体逻辑委托给 `run_uci()` 和 `run_bench()`
- 两种运行模式：UCI 通信模式 和 基准测试模式（bench 内部支持 perft、divide、搜索基准等多种子模式）

---

## English

### Overview
`Main.cpp` is the entry point of the MagnusChess chess engine. It parses command-line arguments and dispatches control flow to the corresponding subsystem based on the subcommand.

### Core Functionality

**`main()`** contains all dispatch logic directly:

1. On Windows, sets console encoding to UTF-8 and enables virtual terminal processing (ANSI escape sequence support)
2. If `argc <= 1` (no subcommand) → calls `magnus::run_uci()` to enter UCI protocol mode
3. If `argv[1] == "uci"` → calls `magnus::run_uci()`
4. All other cases → calls `magnus::run_bench(argc, argv)` to enter benchmark mode

Note: `perft` is not a standalone CLI subcommand; perft functionality is accessed through the `bench` command via `BenchConfig::perft_depth`.

### Runtime Modes

| Mode | Trigger | Entry function |
|------|---------|---------------|
| UCI communication | No arguments or `uci` | `magnus::run_uci()` |
| Benchmark | Everything else | `magnus::run_bench(argc, argv)` |

### Windows Platform Initialization

On `_WIN32`, `main()` performs:
- `SetConsoleOutputCP(CP_UTF8)` — UTF-8 console output
- `SetConsoleCP(CP_UTF8)` — UTF-8 console input
- `SetConsoleMode(..., ENABLE_VIRTUAL_TERMINAL_PROCESSING)` — Enable ANSI escape sequences

### Design Notes
- Minimal design: `main()` only does platform initialization and command routing; all concrete logic is delegated to `run_uci()` and `run_bench()`
- Two runtime modes: UCI communication mode and benchmark mode (bench internally supports perft, divide, search benchmarks, and more sub-modes)
