# Uci.cpp - UCI 协议前端 / UCI Protocol Frontend

## 中文

### 概述
`Uci.cpp` 实现了 MagnusChess 引擎的 **UCI（通用国际象棋接口）**协议前端。它是引擎与外界的通信桥梁，负责解析 GUI 命令、管理搜索会话、处理选项配置以及格式化输出。

### 公开 API

唯一的公开入口点：

| 函数 | 描述 |
|------|------|
| `magnus::run_uci()` | 启动标准输入/输出 UCI 命令循环，接收到 `quit` 命令时返回 |

### UCI 协议命令

#### 标准命令

| 命令 | 描述 |
|------|------|
| `uci` | 输出引擎标识和选项列表 |
| `isready` | 输出 `readyok` |
| `setoption name <n> value <v>` | 设置引擎参数 |
| `position [fen <f> \| startpos] moves <m>...` | 设置棋盘局面 |
| `go <params>` | 启动搜索 |
| `stop` | 停止搜索 |
| `ponderhit` | 沉思命中（切换为正常搜索） |
| `ucinewgame` | 新对局开始 |
| `quit` | 退出引擎 |

#### `go` 命令参数

| 参数 | 描述 |
|------|------|
| `wtime/btime` | 白方/黑方剩余时间（ms） |
| `winc/binc` | 白方/黑方增量（ms） |
| `movestogo` | 到时间控制的剩余步数 |
| `depth` | 固定搜索深度 |
| `nodes` | 固定搜索节点数 |
| `movetime` | 固定搜索时间 |
| `infinite` | 无限搜索 |
| `ponder` | 沉思模式 |

### UCI 选项（参见 README）

| 选项 | 类型 | 描述 |
|------|------|------|
| `Hash` | Spin | 置换表大小（MB） |
| `Threads` | Spin | 搜索线程数 |
| `Contempt` | Spin | 轻视值 |
| `Clear Hash` | Button | 清除置换表 |
| `UseNNUE` | Check | 是否使用 NNUE |
| `Ponder` | Check | 沉思模式 |
| `EvalFile` | String | NNUE/MNUE 网络文件路径 |

### 搜索线程管理

- 搜索在独立线程中运行 `iterative_deepening()`
- 通过 `std::atomic<bool>` 标志实现线程安全终止
- 支持 `stop` 和 `ponderhit` 命令的线程协调

### 输出格式

**`info` 输出**：
```
info depth <d> seldepth <s> score cp <x> nodes <n> nps <n> time <t> pv <moves>
```

**`bestmove` 输出**：
```
bestmove <move> [ponder <move>]
```

### 设计要点
- 完整的 UCI 协议兼容
- 支持 FEN 解析和 `startpos` + moves 的局面设置
- NNUE/MNUE 网络文件通过 `EvalFile` 选项加载
- 搜索线程与主线程通过原子变量通信

---

## English

### Overview
`Uci.cpp` implements the **UCI (Universal Chess Interface)** protocol frontend for the MagnusChess engine. It is the communication bridge between the engine and the outside world.

### Public API

| Function | Description |
|----------|-------------|
| `magnus::run_uci()` | Start stdin/stdout UCI command loop, returns on `quit` |

### UCI Protocol Commands

Standard: `uci`, `isready`, `setoption`, `position`, `go`, `stop`, `ponderhit`, `ucinewgame`, `quit`.

`go` parameters: `wtime/btime`, `winc/binc`, `movestogo`, `depth`, `nodes`, `movetime`, `infinite`, `ponder`.

### UCI Options (see README)

`Hash`, `Threads`, `Contempt`, `Clear Hash`, `UseNNUE`, `Ponder`, `EvalFile`.

### Search Thread Management

Search runs in a separate thread calling `iterative_deepening()`, with `std::atomic<bool>` flags for thread-safe termination.

### Output Format

`info depth <d> seldepth <s> score cp <x> nodes <n> nps <n> time <t> pv <moves>`
`bestmove <move> [ponder <move>]`

### Design Notes
- Full UCI protocol compatibility
- FEN parsing and `startpos` + moves position setup
- NNUE/MNUE network files loaded via `EvalFile` option
- Search thread communicates with main thread via atomic variables
