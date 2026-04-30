# See.cpp - 静态交换评估 / Static Exchange Evaluation

## 中文

### 概述
`See.cpp` 实现了**静态交换评估（Static Exchange Evaluation, SEE）**算法，用于评估一个吃子走法在经过一系列可能的连续吃子后的净损失或收益。提供两个版本：完整计算版（`see_value_fast`）和带阈值的早期截断版（`see_ge_fast`）。

### 算法原理

SEE 在目标格上模拟连续的吃子交换，按照**最弱攻击者优先（Least Valuable Attacker, LVA）**的顺序选择吃子者，最终得出该交换的净得分。

#### 棋子价值（用于 SEE）
```
兵 = 100, 马 = 320, 象 = 330, 车 = 500, 后 = 900, 王 = 20000
```
注意王的极高估值确保王不会参与到不利的交换中。

#### 核心流程

**`see_value_fast()`**：
1. 从 `gain[0]` = 被吃棋子价值开始
2. 如果是升变走法，加上升变获得的价值
3. 模拟执行走法后的棋盘状态（更新 occupied 位棋盘）
4. 计算所有攻击目标格的棋子
5. 交替选择双方最弱攻击者执行吃子：
   - `gain[depth+1]` = 当前受害者价值 − `gain[depth]`
   - `next_victim` = 刚被选中的攻击者
   - 移除该攻击者，更新 X 射线攻击
   - 如果攻击者是王且对方仍有攻击者，停止交换
6. 最终通过 mini-max 回退计算净得分：`gain[i] = -max(-gain[i], gain[i+1])`

**`see_ge_fast()`** — 优化版，当已确定 `balance >= threshold` 时提前终止：
- 计算初始平衡值 `balance = capture_value - threshold`
- 减去己方走子价值；若仍 ≥ 0，直接返回 true
- 交替加入对方 LVA 攻击者：`balance = attacker_value - balance`
- 一旦 `balance >= 0`，确定胜方并返回

#### LVA 攻击者选择（`pick_lva_attacker()`）
按兵→马→象→车→后→王的顺序搜索对方攻击者集合，始终优先选择价值最低的棋子执行交换。

### 关键函数

| 函数 | 描述 |
|------|------|
| `see_value()` | 完整 SEE 计算（包装器，验证是否吃子） |
| `see_value_fast()` | 完整 SEE 计算（核心实现） |
| `see_ge()` | 阈值比较 SEE（包装器） |
| `see_ge_fast()` | 阈值比较 SEE（核心实现，带提前终止） |
| `pick_lva_attacker()` | 从攻击者集合中选择最弱攻击者 |

### 在引擎中的用途
- 走法排序：将 SEE < 0 的吃子分类为"坏吃子"延后搜索
- 裁剪判断：在 qsearch 和 PVS 中根据 SEE 阈值决定是否跳过某走法
- 历史训练：SEE 值用于分类走法质量（`SeeClass`），输入 `SeeBiasTable`

---

## English

### Overview
`See.cpp` implements the **Static Exchange Evaluation (SEE)** algorithm, which evaluates the net loss or gain of a capture move after a possible sequence of recaptures. Two versions are provided: a full computation version (`see_value_fast`) and a threshold-based early cutoff version (`see_ge_fast`).

### Algorithm

SEE simulates consecutive captures on the target square, selecting attackers in **Least Valuable Attacker (LVA)** order, to determine the net score of the exchange.

#### Piece Values (for SEE)
```
Pawn = 100, Knight = 320, Bishop = 330, Rook = 500, Queen = 900, King = 20000
```
Note the extremely high king value ensures kings do not participate in unfavorable exchanges.

#### Core Flow

**`see_value_fast()`**:
1. Start with `gain[0]` = value of captured piece
2. If promotion, add the promotion gain in value
3. Simulate board state after the move (update `occupied` bitboard)
4. Compute all attackers to the target square
5. Alternate picking each side's weakest attacker:
   - `gain[depth+1]` = current victim value − `gain[depth]`
   - `next_victim` = the attacker just selected
   - Remove the attacker, update X-ray attackers
   - If attacker is a king and opponent still has attackers, stop the exchange
6. Mini-max rollback to compute net score: `gain[i] = -max(-gain[i], gain[i+1])`

**`see_ge_fast()`** — Optimized version with early termination when `balance >= threshold` can already be determined:
- Compute initial `balance = capture_value - threshold`
- Subtract own moving piece value; if still ≥ 0, return true immediately
- Alternate adding opponent's LVA attacker: `balance = attacker_value - balance`
- Once `balance >= 0`, determine the winning side and return

#### LVA Attacker Selection (`pick_lva_attacker()`)
Searches the opponent's attacker set in pawn→knight→bishop→rook→queen→king order, always preferring the least valuable piece to perform the exchange.

### Key Functions

| Function | Description |
|----------|-------------|
| `see_value()` | Full SEE computation (wrapper, validates capture) |
| `see_value_fast()` | Full SEE computation (core implementation) |
| `see_ge()` | Threshold comparison SEE (wrapper) |
| `see_ge_fast()` | Threshold comparison SEE (core, early termination) |
| `pick_lva_attacker()` | Selects weakest attacker from attacker set |

### Usage in the Engine
- Move ordering: Captures with SEE < 0 are classified as "bad captures" and searched later
- Pruning decisions: In qsearch and PVS, SEE threshold determines whether to skip a move
- History training: SEE values classify move quality (`SeeClass`), fed into the `SeeBiasTable`
