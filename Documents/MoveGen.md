# MoveGen.cpp - 走法生成 / Move Generation

## 中文

### 概述
`MoveGen.cpp` 实现了 MagnusChess 引擎的完整国际象棋走法生成系统，支持多种生成模式、走法合法性验证、将军检测和特殊走法处理。

### 走法生成模式（GenType）

| 模式 | 描述 |
|------|------|
| `GEN_CAPTURES` | 仅吃子走法（含升变吃子） |
| `GEN_QUIETS` | 仅安静走法（不含吃子） |
| `GEN_NON_EVASIONS` | 非应将状态的所有走法 |
| `GEN_EVASIONS` | 应将走法（王逃、垫、吃攻击子） |
| `GEN_PSEUDO_LEGAL` | 伪合法走法（不做合法性过滤） |
| `GEN_LEGAL` | 仅合法走法 |

### 16 位走法编码

```
bits  0..5   : to (目标格)
bits  6..11  : from (起始格)
bits 12..15  : flag (走法类型)
```

**MoveFlag 枚举**：

| 标志 | 值 | 描述 |
|------|-----|------|
| `MOVE_QUIET` | 0 | 安静走法 |
| `MOVE_DOUBLE_PUSH` | 1 | 兵双步推进 |
| `MOVE_OO` | 2 | 短易位 |
| `MOVE_OOO` | 3 | 长易位 |
| `MOVE_CAPTURE` | 4 | 普通吃子 |
| `MOVE_EP` | 5 | 吃过路兵 |
| `MOVE_PROMO_N/B/R/Q` | 8-11 | 升变（马/象/车/后） |
| `MOVE_CAP_PROMO_N/B/R/Q` | 12-15 | 升变吃子 |

**解码辅助函数**：`from_sq(m)`, `to_sq(m)`, `move_flag(m)`, `make_move(from, to, flag)`, `move_is_capture(m)`, `move_is_promotion(m)`, `move_is_castle(m)`, `move_is_ep(m)`, `move_is_double_push(m)`, `promo_piece(m)`

### 生成信息结构（GenInfo）

| 字段 | 类型 | 描述 |
|------|------|------|
| `us` / `them` | Color | 当前方/对方 |
| `king_sq` | Square | 当前方王的位置 |
| `ep_sq` | Square | 过路兵目标格 |
| `occupied` | Bitboard | 全部占领 |
| `us_occ` / `them_occ` | Bitboard | 当前方/对方占领 |
| `checkers` | Bitboard | 将军攻击者（0/1/2 个） |
| `pinners` | Bitboard | 固定对方的滑行棋子 |
| `pinned` | Bitboard | 被固定棋子 |
| `danger` | Bitboard | 对方攻击的格子（王不能进入） |
| `capture_mask` | Bitboard | 可吃的目标格 |
| `push_mask` | Bitboard | 兵可推进的目标格 |
| `in_check` / `double_check` | bool | 是否被将军/双将 |

### 走法列表结构

| 结构体 | 描述 |
|--------|------|
| `MoveList` | 纯走法容器（`moves[MAX_MOVES]` + `size`），用于 perft 和搜索 |
| `ScoredMove` | 含评分走法（`move`, `score`, `see_value`） |
| `ScoredMoveList` | 评分走法列表 |

`MAX_MOVES = 384`。

### 关键常量/类型

| 符号 | 值 | 描述 |
|------|-----|------|
| `MAX_MOVES` | 384 | 最大走法数 |

### 设计要点
- 模板化架构确保编译器可为每种模式生成最优代码
- 将军检测采用增量方式避免完整走法执行
- 固定检测利用滑行棋子的 X 射线攻击
- 吃过路兵合法性需额外验证（移走兵后是否仍被将军）

---

## English

### Overview
`MoveGen.cpp` implements the complete chess move generation system for the MagnusChess engine, supporting multiple generation modes, legality validation, check detection, and special move handling.

### Generation Modes (GenType)

| Mode | Description |
|------|-------------|
| `GEN_CAPTURES` | Captures only (including promotion captures) |
| `GEN_QUIETS` | Quiets only (non-captures) |
| `GEN_NON_EVASIONS` | All moves when not in check |
| `GEN_EVASIONS` | Check evasion moves |
| `GEN_PSEUDO_LEGAL` | Pseudo-legal moves (no legality filter) |
| `GEN_LEGAL` | Legal moves only |

### 16-bit Move Encoding

`[to:6|from:6|flag:4]`. Flag distinguishes quiet, double push, castling, capture, en passant, promotions (8-11), and promotion captures (12-15).

### GenInfo Structure

Contains board context for move generation: side to move, king square, ep square, occupancy, checkers, pinners, pinned pieces, danger squares, capture/push masks, check state.

### Move List Structures

`MoveList` (plain moves), `ScoredMove` (with score + SEE), `ScoredMoveList`. `MAX_MOVES = 384`.

### Design Notes
- Template architecture for optimal per-mode code generation
- Incremental check detection avoids full move execution
- Pin detection uses X-ray attacks of sliding pieces
- En passant legality requires additional verification
