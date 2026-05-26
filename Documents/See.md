# See.cpp - 静态交换评估 / Static Exchange Evaluation

## 中文

### 概述
`See.cpp` 实现了**静态交换评估（Static Exchange Evaluation, SEE）**算法，用于评估一个吃子走法在经过一系列可能的连续吃子后的净损失或收益。

### 命名空间
所有函数位于 `namespace magnus::search`。

### 棋子价值（用于 SEE 排序）

```cpp
constexpr int piece_order_value[PIECE_TYPE_NB] = { 100, 320, 330, 500, 900, 0 };
//                                                  P     N     B     R     Q    K
```

注意：王的值为 0（用于 MVV-LVA 排序），但在 SEE 模拟中王被视为极高价值（不参与不利交换）。

### 核心函数

| 函数 | 描述 |
|------|------|
| `see_value(pos, mem, move)` | 完整 SEE 计算（含合法性验证，慢速路径） |
| `see_value_fast(pos, mem, move)` | 快速路径 SEE（假设走法合法，热路径） |
| `see_ge(pos, mem, move, threshold)` | 阈值判断 SEE（含验证，提前退出） |
| `see_ge_fast(pos, mem, move, threshold)` | 快速路径阈值判断（最常用） |

### 算法原理

SEE 在目标格上模拟连续的吃子交换，按照**最弱攻击者优先（LVA）**顺序选择吃子者：

1. 起始方走吃子，获得 victim 价值
2. 对方用最小价值攻击者反吃
3. 交替用 LVA 回应
4. 当一方无攻击者或王被暴露时终止
5. 从最深回溯用 minimax 计算最优净交换值

**`see_ge_fast()`** 优化版：当已确定 `balance >= threshold` 时提前终止。

### 在引擎中的用途
- 走法排序：SEE < 0 的吃子分类为"坏吃子"延后搜索
- 裁剪判断：qsearch 和 PVS 中根据 SEE 阈值跳过走法
- 历史训练：SEE 值用于分类走法质量（SeeClass），输入 SeeBiasTable

---

## English

### Overview
`See.cpp` implements the **Static Exchange Evaluation (SEE)** algorithm, evaluating the net loss or gain of a capture move after possible recaptures.

### Namespace
All functions reside in `namespace magnus::search`.

### Piece Ordering Values

```cpp
constexpr int piece_order_value[PIECE_TYPE_NB] = { 100, 320, 330, 500, 900, 0 };
```

Note: King=0 for MVV-LVA ordering; in SEE simulation, king is treated as extremely high value.

### Core Functions

| Function | Description |
|----------|-------------|
| `see_value(pos, mem, move)` | Full SEE (with legality check, slow path) |
| `see_value_fast(pos, mem, move)` | Fast-path SEE (assumes legal, hot path) |
| `see_ge(pos, mem, move, threshold)` | Threshold SEE (with check, early exit) |
| `see_ge_fast(pos, mem, move, threshold)` | Fast-path threshold SEE (most commonly used) |

### Algorithm

Simulates consecutive captures on the target square in LVA (Least Valuable Attacker) order, then minimax rollback for net exchange value. `see_ge_fast()` adds early termination when the threshold can already be determined.

### Usage
- Move ordering: SEE < 0 → bad captures deferred
- Pruning: SEE threshold in qsearch and PVS
- History: SEE values → SeeClass → SeeBiasTable
