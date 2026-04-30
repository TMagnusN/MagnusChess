# Attack.cpp - 攻击生成 / Attack Generation

## 中文

### 概述
`Attack.cpp` 实现了国际象棋中滑行棋子（象、车、后）的攻击掩码生成，以及 X 射线攻击和几何对齐检查。它支持三种后端实现，可在编译期或运行时选择。

### 核心技术

#### 三种后端实现

1. **Classical（经典）** — 纯射线扫描，直接循环扫描棋子的对角线/直线方向，遇到阻挡即停止。无预计算表，内存占用为零。

2. **Dense Table（密集表）** — 预计算 `AttackTable`，存储每个格子每种 ocupancy 组合的攻击掩码。查找时使用 `(occupancy * magic) >> shift` 作为索引。占用内存约 107KB。

3. **BMI2 / PEXT** — 使用 BMI2 指令集的 `_pext_u64` 提取相关占领位，直接作为查找表索引。需要 x86 BMI2 硬件支持。这是三种方案中最快的。

#### 后端选择
- `attack_init_backend()` — 根据编译宏（`ATTACK_BACKEND_CLASSICAL` / `ATTACK_BACKEND_DENSE` 或 `ATTACK_BACKEND_AUTO`）初始化对应的后端函数指针
- 运行时动态分派：`bishop_attacks_fn()`、`rook_attacks_fn()` 等函数指针在初始化后指向选定的实现

### 关键函数

| 函数 | 描述 |
|------|------|
| `bishop_attacks()` / `rook_attacks()` | 生成象/车的攻击掩码（通过分派表） |
| `queen_attacks()` | 象 + 车的并集 |
| `xray_bishop_attacks()` / `xray_rook_attacks()` | X 射线攻击，穿透被阻挡的格子 |
| `aligned()` | 检查两个格子是否在同一对角线或直线上 |
| `between()` | 返回两格之间（不含端点）的所有格子 |
| `ray()` | 返回从一格出发经另一格延伸的射线 |

### 内存布局
- `BishopTable`：64 个格子 × 512 个条目（9 位索引）= 32K 条目
- `RookTable`：64 个格子 × 4096 个条目（12 位索引）= 256K 条目

---

## English

### Overview
`Attack.cpp` implements attack mask generation for sliding pieces (bishop, rook, queen), as well as X-ray attacks and geometric alignment checks for a chess engine. It supports three backend implementations selectable at compile time or runtime.

### Core Technology

#### Three Backend Implementations

1. **Classical** — Pure ray scanning, directly loops through diagonal/orthogonal directions, stopping at the first blocker. Zero precomputation tables, zero memory footprint.

2. **Dense Table** — Precomputed `AttackTable` storing attack masks for every square × occupancy combination. Lookup uses `(occupancy * magic) >> shift` as the index. Memory footprint ~107KB.

3. **BMI2 / PEXT** — Uses the BMI2 instruction set's `_pext_u64` to extract relevant occupancy bits, directly used as a lookup table index. Requires x86 BMI2 hardware support. Fastest of the three.

#### Backend Selection
- `attack_init_backend()` — Initializes backend function pointers based on compile macros (`ATTACK_BACKEND_CLASSICAL`, `ATTACK_BACKEND_DENSE`, or `ATTACK_BACKEND_AUTO`)
- Runtime dispatch: function pointers like `bishop_attacks_fn()` and `rook_attacks_fn()` point to the selected implementation after initialization

### Key Functions

| Function | Description |
|----------|-------------|
| `bishop_attacks()` / `rook_attacks()` | Generate bishop/rook attack masks (via dispatch table) |
| `queen_attacks()` | Union of bishop + rook |
| `xray_bishop_attacks()` / `xray_rook_attacks()` | X-ray attacks, penetrating through an occupied square |
| `aligned()` | Checks if two squares lie on the same diagonal or orthogonal |
| `between()` | Returns all squares between two squares (exclusive) |
| `ray()` | Returns the ray from one square extending through another |

### Memory Layout
- `BishopTable`: 64 squares × 512 entries (9-bit index) = 32K entries
- `RookTable`: 64 squares × 4096 entries (12-bit index) = 256K entries
