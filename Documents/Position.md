# Position.cpp - 棋盘状态管理 / Board State Management

## 中文

### 概述
`Position.cpp` 实现了 MagnusChess 引擎的棋盘状态表示与操作。它采用混合架构，将邮箱数组（mailbox）、位棋盘（bitboard）、增量评估、Zobrist 哈希和 NNUE/MNUE 累加器结合为一体，支持完整的走法执行与撤销。

### Position 结构体核心字段

| 字段 | 类型 | 描述 |
|------|------|------|
| `side_to_move` | int | 当前走子方（WHITE/BLACK） |
| `ep_sq` | Square | 过路兵目标格（NO_SQ=无） |
| `castling_rights` | int | 王车易位权限位掩码 |
| `halfmove_clock` | int | 半回合计数器（50 步规则） |
| `fullmove_number` | int | 完整回合数 |
| `king_sq[COLOR_NB]` | Square[] | 双方王的位置缓存 |
| `color_bb[COLOR_NB]` | Bitboard[] | 按颜色占领位棋盘 |
| `piece_bb[PIECE_NB]` | Bitboard[] | 按棋子类型位棋盘 |
| `occupied` | Bitboard | 所有棋子并集 |
| `piece_counts[COLOR_NB][PIECE_NB]` | u8[][] | 按颜色+类型的棋子计数 |
| `non_king_material` | u8 | 非王子力标志 |
| `material_signature` | Key | 材料配置哈希（材料表索引） |
| `eval_mg[COLOR_NB]` | int[] | 中局增量评估（按颜色） |
| `eval_eg[COLOR_NB]` | int[] | 残局增量评估（按颜色） |
| `eval_phase` | int | 局面阶段值（MG/EG 插值） |
| `key` | Key | 完整 Zobrist 哈希键 |
| `board[SQ_NB]` | int[] | 邮箱数组（O(1) 格子查询） |

#### NNUE 累加器

| 字段 | 类型 | 描述 |
|------|------|------|
| `nnue_generation` | u32 | NNUE 累加器代数（惰性刷新） |
| `nnue_acc_valid` | bool | NNUE 累加器是否有效 |
| `nnue_acc[COLOR_NB][kHiddenSize]` | i16[][] | NNUE 双视角隐藏层累加器（alignas(64)） |

#### MNUE-P2 累加器

| 字段 | 类型 | 描述 |
|------|------|------|
| `mnue_p2_generation` | u32 | P2 累加器代数 |
| `mnue_p2_acc_valid_mask` | u8 | P2 累加器有效掩码（按颜色） |
| `mnue_p2_acc[COLOR_NB][1024]` | i16[][] | P2 增量累加器（alignas(64)） |

### StateInfo 结构体

走法撤销信息，存储在 `make_move`/`unmake_move` 的栈中：

| 字段 | 类型 | 描述 |
|------|------|------|
| `castling_rights` | int | 走法前的易位权限 |
| `ep_sq` | Square | 走法前的过路兵格 |
| `halfmove_clock` | int | 走法前的半回合计数 |
| `fullmove_number` | int | 走法前的完整回合数 |
| `key` | Key | 走法前的 Zobrist 键 |
| `captured` | Piece | 被吃的棋子（PIECE_NONE=无） |
| `captured_sq` | Square | 被吃棋子所在格 |

### 便捷访问器（内联函数）

| 函数 | 描述 |
|------|------|
| `us(pos)` / `them(pos)` | 当前走子方 / 对方 |
| `pieces(pos)` | 全部占领位棋盘 |
| `pieces(pos, color)` | 某颜色占领位棋盘 |
| `pieces_of_type(pos, pt)` | 某棋子类型位棋盘 |
| `pieces(pos, color, pt)` | 某颜色+类型位棋盘 |
| `piece_count(pos, color, pt)` | 某颜色+类型的棋子数 |
| `non_king_material(pos)` | 非王子力标志 |
| `packed_material_signature(pos)` | 材料签名 |
| `king_square(pos, color)` | 某方王的位置 |
| `has_ep(pos)` | 是否有过路兵 |
| `piece_on(pos, sq)` | 格上棋子（Piece） |
| `color_on(pos, sq)` | 格上棋子的颜色 |
| `piece_type_on(pos, sq)` | 格上棋子的类型 |
| `empty_on(pos, sq)` / `occupied_on(pos, sq)` | 格子是否空/有子 |

### 走法执行

| 函数 | 描述 |
|------|------|
| `make_move(pos, m, st)` | 执行走法，StateInfo 入栈 |
| `unmake_move(pos, m)` | 撤销走法，StateInfo 出栈 |
| `do_move_copy(pos, m)` | 创建副本执行走法（perft 用） |

### 局面操作

| 函数 | 描述 |
|------|------|
| `position_clear(pos)` | 清空局面 |
| `position_put_piece(pos, color, pt, sq)` | 放置棋子（更新位棋盘+评估） |
| `position_remove_piece(pos, color, pt, sq)` | 移除棋子 |
| `position_move_piece(pos, color, pt, from, to)` | 移动棋子 |
| `position_recompute_occupied(pos)` | 从 piece_bb 重建 occupied |
| `position_refresh_king_squares(pos)` | 从 board 重建 king_sq |
| `position_compute_key(pos, tables)` | 从零计算 Zobrist 键 |
| `position_refresh_key(pos, tables)` | 刷新 Zobrist 键 |

### 验证

| 函数 | 描述 |
|------|------|
| `position_has_valid_kings(pos)` | 双方各有一个王 |
| `position_board_matches_bitboards(pos)` | board 与 bitboard 一致性检查 |

### 走法合法性

| 函数 | 描述 |
|------|------|
| `legal(pos, mem, move)` | 完整合法性检查（执行走法后验证） |
| `pseudo_legal_fast(pos, mem, info, move)` | 伪合法性快速检查 |

### 设计要点
- 所有更新是增量的：`make_move`/`unmake_move` 仅修改涉及到的字段
- Zobrist 键通过异或运算增量更新
- NNUE/MNUE-P2 累加器存储在 Position 中，走法执行时自动增量更新
- `StateInfo` 栈支持完整的走法撤销
- 支持 FEN 字符串解析和导出

---

## English

### Overview
`Position.cpp` implements the board state representation and operations for the MagnusChess engine. It uses a hybrid architecture combining mailbox arrays, bitboards, incremental evaluation, Zobrist hashing, and NNUE/MNUE accumulators.

### Position Struct Core Fields

Key fields include: `side_to_move`, `ep_sq`, `castling_rights`, `halfmove_clock`, `fullmove_number`, `king_sq[COLOR_NB]`, `color_bb[COLOR_NB]`, `piece_bb[PIECE_NB]`, `occupied`, `piece_counts`, `non_king_material`, `material_signature`, `eval_mg[COLOR_NB]`/`eval_eg[COLOR_NB]`, `eval_phase`, `key`, `board[SQ_NB]`.

NNUE accumulators: `nnue_generation`, `nnue_acc_valid`, `nnue_acc[COLOR_NB][128]` (alignas(64)).

MNUE-P2 accumulators: `mnue_p2_generation`, `mnue_p2_acc_valid_mask`, `mnue_p2_acc[COLOR_NB][1024]` (alignas(64)).

### StateInfo Struct

Move undo information stored on the `make_move`/`unmake_move` stack: castling rights, ep square, halfmove clock, fullmove number, key, captured piece, captured square.

### Convenience Accessors

`us`, `them`, `pieces`, `pieces_of_type`, `piece_count`, `non_king_material`, `packed_material_signature`, `king_square`, `has_ep`, `piece_on`, `color_on`, `piece_type_on`, `empty_on`, `occupied_on`.

### Move Execution

`make_move`, `unmake_move`, `do_move_copy`.

### Position Operations

`position_clear`, `position_put_piece`, `position_remove_piece`, `position_move_piece`, `position_recompute_occupied`, `position_refresh_king_squares`, `position_compute_key`, `position_refresh_key`.

### Validation

`position_has_valid_kings`, `position_board_matches_bitboards`.

### Move Legality

`legal`, `pseudo_legal_fast`.

### Design Notes
- All updates are incremental; `make_move`/`unmake_move` only modifies affected fields
- Zobrist key is updated incrementally via XOR
- NNUE/MNUE-P2 accumulators stored in Position, auto-updated during move execution
- `StateInfo` stack supports full move undo
- Supports FEN string parsing and export
