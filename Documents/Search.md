# Search.cpp - 搜索引擎 / Search Engine

## 中文

### 概述
`Search.cpp` 是 MagnusChess 引擎的核心——完整的**搜索引擎**实现。它实现了主变例搜索（PVS）、Alpha-Beta 剪枝，以及数十种现代搜索优化技术。所有内部实现细节封装在匿名命名空间中。

### 公开 API（`namespace magnus::search`）

| 函数 | 描述 |
|------|------|
| `iterative_deepening(root, mem, limits, out)` | 搜索入口：从根节点启动迭代加深 PVS 搜索 |
| `move_to_uci(m)` | 将内部 16 位走法转为 UCI 坐标字符串 |

### 搜索框架

#### `iterative_deepening()` — 迭代加深
```
for depth in 1..=max_depth:
    aspiration_window = [prev_score - delta, prev_score + delta]
    do:
        score = pvs(alpha, beta, depth, ply=0)
        if fail-low: widen window downward, re-search
        if fail-high: widen window upward, re-search
    while window fails
    输出 info 和 bestmove
```

多线程模式使用 Lazy SMP：所有线程共享置换表，独立搜索同一根局面。

#### `pvs()` — 主变例搜索
```
if first_move:
    score = -pvs(-beta, -alpha, depth-1, ply+1)  // 全窗口
else:
    score = -pvs(-alpha-1, -alpha, depth-1, ply+1)  // 零窗口
    if score > alpha:
        score = -pvs(-beta, -alpha, depth-1, ply+1)  // 全窗口重搜索
```

#### `qsearch()` — 静止搜索
- 仅搜索吃子走法（解决地平线效应）
- 站定截止（stand-pat）评估
- Delta 剪枝：如果 eval + queen_val < alpha，跳过

### 搜索栈（`SearchStackEntry`）

每层 ply 的搜索状态，通过 `search_stack[]` 数组在 ply 之间传递：

| 字段 | 类型 | 描述 |
|------|------|------|
| `current_move` | Move | 当前正在搜索的走法（供历史更新使用） |
| `static_eval` | int | 当前 ply 的静态评估值 |
| `stat_score` | int | 综合历史统计分数（LMR 计算的输出，供子节点参考） |
| `reduction_fp` | int | LMR 最终减免量（固定点格式，子节点用于 hindsight 调整） |
| `move_count` | int | 已搜索的合法走法数量 |
| `cutoff_count` | int | 截断次数（连续截断增加后续走法的 LMR 减免量） |
| `in_check` | bool | 走子方是否被将军 |
| `tt_hit` | bool | 置换表是否命中（用于空步裁剪的阈值调整） |
| `tt_pv` | bool | TT entry 是否来自 PV 节点 |

### 搜索限制（`SearchLimits`）

| 字段 | 类型 | 描述 |
|------|------|------|
| `depth` | int | 最大搜索深度（默认 MAX_PLY=128） |
| `node_limit` | u64 | 最大节点数限制（0=无限制） |
| `soft_time_ms` | int | 软时间限制 |
| `hard_time_ms` | int | 硬时间限制 |
| `ponder` | bool | 沉思模式 |
| `infinite` | bool | 无限搜索 |
| `use_time_management` | bool | 启用时间管理 |
| `contempt` | int | 轻视值 |
| `use_nnue` | bool | 是否使用 NNUE |
| `game_history_keys[]` | Key[] | 历史局面 Zobrist 键（重复检测） |
| `root_moves[]` | Move[] | 限制的根节点走法（UCI searchmoves） |
| `stop` / `external_stop` | atomic<bool>* | 停止信号 |
| `shared_nodes` | atomic<u64>* | 跨线程共享节点计数器 |
| `thread_id` / `thread_count` | int | 线程标识 |
| `report_info` | bool | 是否输出 UCI info（辅助线程设为 false） |

### 搜索结果（`SearchResult`）

| 字段 | 类型 | 描述 |
|------|------|------|
| `best_move` | Move | 最佳走法（0=无合法走法） |
| `score` | Score | 最佳走法的评分（cp 单位，根节点视角） |
| `nodes` | u64 | 搜索探索的总节点数 |
| `depth` | int | 完成的搜索深度 |
| `seldepth` | int | 选择性深度（最深分支实际搜索深度） |

### 搜索优化技术全景

#### 节点进入时的裁剪

| 技术 | 条件 | 动作 |
|------|------|------|
| TT 探测 | TT 命中且深度足够 | 返回缓存分数 |
| Razoring | `eval + margin < alpha`，深度很浅 | 返回 qsearch 结果 |
| 反向无效裁剪（RFP） | `eval - margin >= beta` | 跳过搜索，返回 eval |
| 空走裁剪（NMP） | 见 Nmp.cpp | 让对方走两步测试 |
| Probcut | TT 命中高深度 + 下界 | 缩减深度验证 β 剪枝 |

#### 走法循环中的裁剪

| 技术 | 条件 | 动作 |
|------|------|------|
| SEE 门控 | SEE < 阈值 | 将吃子发送到坏吃子队列 |
| 吃子无效裁剪 | `eval + capture_gain < alpha` | 跳过该吃子 |
| 历史裁剪 | 安静走法历史得分太低 | 跳过该走法 |
| 奇异延展（SE） | 只有一个走法明显好于其他 | 延展搜索深度 |
| 迟走法裁剪（LMR） | 见 Lmr.cpp | 降低非优先走法搜索深度 |

#### 节点完成时的处理

| 技术 | 描述 |
|------|------|
| TT 保存 | 将结果存入置换表 |
| 历史更新 | reward/bonus 截断走法，penalty 未截断走法 |
| 杀手更新 | 更新杀手走法槽位 |
| 反走法更新 | 设置反走法关联 |
| 延续历史更新 | 更新 continuation history |

### 关键辅助系统

#### 静态评估解析

优先级：
1. 如果无现存评估 → NNUE 或 HCE 评估
2. 如果局面被将军 → 重新评估
3. 否则 → 使用增量评估值

#### 改善检测

比较本回合与两回合前的静态评估，判断局面是否改善，用于调整裁剪裕度。

#### 矫正历史（Correction History）

根据历史搜索结果调整静态评估值，使评估更接近实际搜索结果。

### 关键常量

| 常量 | 值 | 描述 |
|------|-----|------|
| `MAX_PLY` | 128 | 最大搜索深度（所有栈数组的固定尺寸） |
| `MAX_GAME_HISTORY` | 128 | 最大对局历史记录数（重复检测） |

### 设计要点
- 所有裁剪裕度随深度和改善状态动态调整
- LMR 使用 FP 精度实现连续裁剪量调整
- 历史更新同时影响安静走法和吃子走法的排序
- 奇异延展提供战术发现能力
- Lazy SMP 多线程通过共享 TT 实现隐式信息传递

---

## English

### Overview
`Search.cpp` is the core of the MagnusChess engine — the complete **search engine** implementation. It implements Principal Variation Search (PVS) with Alpha-Beta pruning and dozens of modern search optimization techniques. All internal details are encapsulated in anonymous namespaces.

### Public API (`namespace magnus::search`)

| Function | Description |
|----------|-------------|
| `iterative_deepening(root, mem, limits, out)` | Search entry: iterative deepening PVS from root |
| `move_to_uci(m)` | Convert 16-bit internal move to UCI coordinate string |

### Search Framework

Multi-threaded mode uses Lazy SMP: all threads share the transposition table, independently searching the same root position.

### Search Stack (`SearchStackEntry`)

Per-ply state passed between plies via the `search_stack[]` array:

| Field | Type | Description |
|-------|------|-------------|
| `current_move` | Move | Move currently being searched (for history updates) |
| `static_eval` | int | Static evaluation at this ply |
| `stat_score` | int | Composite history stat score (LMR output, used by child nodes) |
| `reduction_fp` | int | Final LMR reduction in fixed-point (for child hindsight adjustment) |
| `move_count` | int | Number of legal moves searched so far |
| `cutoff_count` | int | Number of cutoffs (consecutive cutoffs increase LMR for later moves) |
| `in_check` | bool | Whether side to move is in check |
| `tt_hit` | bool | Whether TT was hit (affects NMP threshold) |
| `tt_pv` | bool | Whether TT entry came from a PV node |

### Search Limits (`SearchLimits`)

| Field | Type | Description |
|-------|------|-------------|
| `depth` | int | Max search depth (default MAX_PLY=128) |
| `node_limit` | u64 | Max node count (0=unlimited) |
| `soft_time_ms` / `hard_time_ms` | int | Soft/hard time limits |
| `ponder` / `infinite` | bool | Ponder/infinite mode |
| `use_time_management` | bool | Enable time management |
| `contempt` | int | Contempt value |
| `use_nnue` | bool | Whether NNUE is enabled |
| `game_history_keys[]` | Key[] | Historical position keys (repetition detection) |
| `root_moves[]` | Move[] | Restricted root moves (UCI searchmoves) |
| `stop` / `external_stop` | atomic<bool>* | Stop signals |
| `shared_nodes` | atomic<u64>* | Cross-thread shared node counter |
| `thread_id` / `thread_count` | int | Thread identification |

### Search Result (`SearchResult`)

| Field | Type | Description |
|-------|------|-------------|
| `best_move` | Move | Best move found (0 = no legal moves) |
| `score` | Score | Score of best move (cp, from root perspective) |
| `nodes` | u64 | Total nodes explored |
| `depth` | int | Completed search depth |
| `seldepth` | int | Selective depth (deepest branch actually searched) |

### Search Optimization Techniques

#### Node Entry Pruning: TT Probe, Razoring, Reverse Futility (RFP), NMP, Probcut

#### In-Move-Loop Pruning: SEE Gating, Capture Futility, History Pruning, Singular Extension (SE), LMR

#### Node Completion: TT Save, History Update (bonus/penalty), Killer/Countermove/Continuation History Updates

### Key Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `MAX_PLY` | 128 | Max search depth (fixed size for all stack arrays) |
| `MAX_GAME_HISTORY` | 128 | Max game history entries (repetition detection) |

### Design Notes
- All pruning margins dynamically adjust with depth and improving state
- LMR uses fractional ply (FP) precision for continuous reduction adjustment
- History updates affect both quiet and capture move ordering
- Singular extensions provide tactical discovery capability
- Lazy SMP achieves implicit information sharing through shared TT
