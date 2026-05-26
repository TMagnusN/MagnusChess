# Attack.cpp - 攻击生成 / Attack Generation

## 中文

### 概述
`Attack.cpp` 实现了国际象棋中滑行棋子（象、车、后）的攻击掩码生成，以及非滑行棋子（兵、马、王）的预计算查表和几何工具函数。支持四种后端实现，可在编译期或运行时选择。

### 命名空间
所有攻击函数位于 `namespace magnus`（`Attack.h` 不引入独立命名空间）。

### 四种后端实现

| 后端 | 枚举值 | 描述 |
|------|--------|------|
| **Classical** | `CLASSICAL` | 纯射线扫描，直接循环扫描对角线/直线方向，遇阻挡停止。零预计算表，零内存。 |
| **Dense Table** | `TABLE` | 预计算密度表，`(occupancy * magic) >> shift` 索引。内存约 107KB。 |
| **Magic** | `MAGIC` | 魔法位棋盘变体，使用不同的索引方案。 |
| **BMI2 / PEXT** | `PEXT` | 使用 BMI2 `_pext_u64` 提取相关占领位。需要 x86 BMI2 硬件。最快方案。 |

#### 后端选择 API

| 函数 | 描述 |
|------|------|
| `attack_init_backend(mem)` | 初始化后端（根据编译宏或自动检测） |
| `attack_set_backend(kind)` | 手动设置后端 |
| `attack_auto_select_backend()` | 自动选择最佳后端 |
| `attack_backend_kind()` | 返回当前后端枚举 |
| `attack_backend_name()` | 返回当前后端名称字符串 |
| `attack_backend_uses_slider_tables()` | 是否使用滑子查表 |
| `attack_backend_pext_supported()` | PEXT 指令是否可用 |

### 关键函数签名（所有需要 `const memory::Memory& mem` 参数）

#### 滑子攻击（依赖后端选择）

| 函数 | 描述 |
|------|------|
| `bishop_attacks(mem, sq, occupied)` | 象的攻击掩码 |
| `rook_attacks(mem, sq, occupied)` | 车的攻击掩码 |
| `queen_attacks(mem, sq, occupied)` | 后的攻击掩码（象 ∪ 车） |
| `bishop_rays(sq)` | 象的射线掩码（无阻挡） |
| `rook_rays(sq)` | 车的射线掩码 |
| `queen_rays(sq)` | 后的射线掩码 |
| `bishop_xray_attacks(mem, sq, occupied, blockers)` | 象的 X 射线攻击（穿透 blockers） |
| `rook_xray_attacks(mem, sq, occupied, blockers)` | 车的 X 射线攻击 |

#### 非滑子攻击（查表，O(1)）

| 函数 | 描述 |
|------|------|
| `pawn_attacks(mem, color, sq)` | 兵的吃子掩码 |
| `knight_attacks(mem, sq)` | 马的攻击掩码 |
| `king_attacks(mem, sq)` | 王的攻击掩码 |

#### 几何工具

| 函数 | 描述 |
|------|------|
| `between_bb(mem, a, b)` | 两格之间的格子（不含端点） |
| `line_bb(mem, a, b)` | 两格所在的线（直线或对角线） |
| `chebyshev_distance(mem, a, b)` | 切比雪夫距离 |
| `manhattan_distance(mem, a, b)` | 曼哈顿距离 |

#### 滑子表元数据

| 函数 | 描述 |
|------|------|
| `bishop_slider_entry(sq)` | 返回象在 sq 的滑子表条目 |
| `rook_slider_entry(sq)` | 返回车在 sq 的滑子表条目 |
| `bishop_slider_table_size()` | 象滑子表总条目数 |
| `rook_slider_table_size()` | 车滑子表总条目数 |

### 内部类型

| 类型 | 描述 |
|------|------|
| `AttackBackendKind` | 枚举：CLASSICAL / TABLE / MAGIC / PEXT |
| `SliderAttackEntry` | 滑子表条目：mask, offset, relevant_bits, shift |
| `AttackBitboard` | `memory::Bitboard` 的别名 |
| `AttackColor` | `memory::Color` 的别名 |
| `AttackKey` | `memory::Key` 的别名 |

### 内存布局
- `BishopTable`：64 格 × 512 条目（9 位索引）= 32K 条目
- `RookTable`：64 格 × 4096 条目（12 位索引）= 256K 条目

### 设计要点
- 所有滑子攻击函数需要 `const memory::Memory& mem` 参数（依赖注入模式）
- 非滑子攻击（兵/马/王）从预计算表 O(1) 查表，不依赖后端
- 运行时动态分派：函数指针在后端初始化后指向选定实现
- 滑子表元数据通过 `bishop_slider_entry()` / `rook_slider_entry()` 访问

---

## English

### Overview
`Attack.cpp` implements attack mask generation for sliding pieces (bishop, rook, queen), precomputed lookups for non-sliding pieces (pawn, knight, king), and geometric utility functions. Supports four backend implementations selectable at compile time or runtime.

### Namespace
All attack functions reside in `namespace magnus`.

### Four Backend Implementations

| Backend | Enum | Description |
|---------|------|-------------|
| **Classical** | `CLASSICAL` | Pure ray scanning, zero precomputation |
| **Dense Table** | `TABLE` | Precomputed density table, ~107KB memory |
| **Magic** | `MAGIC` | Magic bitboard variant |
| **BMI2 / PEXT** | `PEXT` | BMI2 `_pext_u64` extraction, requires x86 BMI2 |

#### Backend Selection API

| Function | Description |
|----------|-------------|
| `attack_init_backend(mem)` | Initialize backend (compile macro or auto-detect) |
| `attack_set_backend(kind)` | Manually set backend |
| `attack_auto_select_backend()` | Auto-select best backend |
| `attack_backend_kind()` | Current backend enum |
| `attack_backend_name()` | Current backend name string |
| `attack_backend_uses_slider_tables()` | Whether slider tables are used |
| `attack_backend_pext_supported()` | Whether PEXT is available |

### Key Function Signatures (all require `const memory::Memory& mem`)

#### Slider Attacks (backend-dependent)

| Function | Description |
|----------|-------------|
| `bishop_attacks(mem, sq, occupied)` | Bishop attack mask |
| `rook_attacks(mem, sq, occupied)` | Rook attack mask |
| `queen_attacks(mem, sq, occupied)` | Queen attack mask (bishop ∪ rook) |
| `bishop_rays(sq)` | Bishop ray mask (no blockers) |
| `rook_rays(sq)` | Rook ray mask |
| `queen_rays(sq)` | Queen ray mask |
| `bishop_xray_attacks(mem, sq, occupied, blockers)` | Bishop X-ray (through blockers) |
| `rook_xray_attacks(mem, sq, occupied, blockers)` | Rook X-ray |

#### Non-Sliding Attacks (table lookup, O(1))

| Function | Description |
|----------|-------------|
| `pawn_attacks(mem, color, sq)` | Pawn capture mask |
| `knight_attacks(mem, sq)` | Knight attack mask |
| `king_attacks(mem, sq)` | King attack mask |

#### Geometry Utilities

| Function | Description |
|----------|-------------|
| `between_bb(mem, a, b)` | Squares between a and b (exclusive) |
| `line_bb(mem, a, b)` | Line (rank/file/diagonal) through a and b |
| `chebyshev_distance(mem, a, b)` | Chebyshev distance |
| `manhattan_distance(mem, a, b)` | Manhattan distance |

#### Slider Table Metadata

| Function | Description |
|----------|-------------|
| `bishop_slider_entry(sq)` | Bishop slider entry for square |
| `rook_slider_entry(sq)` | Rook slider entry for square |
| `bishop_slider_table_size()` | Total bishop slider table entries |
| `rook_slider_table_size()` | Total rook slider table entries |

### Internal Types

| Type | Description |
|------|-------------|
| `AttackBackendKind` | Enum: CLASSICAL / TABLE / MAGIC / PEXT |
| `SliderAttackEntry` | Slider entry: mask, offset, relevant_bits, shift |

### Design Notes
- All slider attack functions require `const memory::Memory& mem` (dependency injection)
- Non-slider attacks (pawn/knight/king) use precomputed table lookups, O(1), no backend dependency
- Runtime dispatch: function pointers redirected after backend initialization
- Slider table metadata accessible via `bishop_slider_entry()` / `rook_slider_entry()`
