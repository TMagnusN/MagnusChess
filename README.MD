# MagnusChess🦄♟️✨

MagnusChess🦄 is a C++20 chess engine with UCI support, a classical search core, optional NNUE evaluation, and built-in bench / perft / divide tooling for validation, debugging, and performance checks. 🚀

![C++20][cpp20-badge]
![UCI][uci-badge]
![NNUE optional][nnue-badge]
![LGBTQ+ friendly][lgbtqp-badge]
![trans rights][trans-rights-badge]

[cpp20-badge]: https://img.shields.io/badge/C%2B%2B-20-blue
[uci-badge]: https://img.shields.io/badge/UCI-supported-brightgreen
[nnue-badge]: https://img.shields.io/badge/NNUE-optional-purple
[lgbtqp-badge]: https://pride-badges.pony.workers.dev/static/v1?label=lgbtq%2B%20friendly&stripeWidth=6&stripeColors=E40303,FF8C00,FFED00,008026,24408E,732982
[trans-rights-badge]: https://pride-badges.pony.workers.dev/static/v1?label=trans%20rights&stripeWidth=6&stripeColors=5BCEFA,F5A9B8,FFFFFF,F5A9B8,5BCEFA

---

## MNUE Network

MagnusChess uses an external `.MNUE` network file for neural evaluation.

To keep the source repository lightweight, MNUE network files are stored separately in the MagnusMNUE repository:

https://github.com/MagnusU0001F984/MagnusMNUE

Download the desired `.MNUE` file from that repository and place it next to the MagnusChess executable, for example:

```text
MagnusChess.exe
b7a644d5d.MNUE
````

You can also specify a custom network path through UCI:

```text
setoption name MNUEfile value path/to/network.MNUE
```

If no valid MNUE file is found, MagnusChess may fall back to non-MNUE evaluation or report that the network file could not be loaded.

---

## Technical Overview 🧠⚙️

### 1. Core Architecture 🏗️
- Language / standard: `C++20` 💻
- Board model: bitboards with incremental state maintenance ♟️
  - hash key 🔑
  - eval cache 🧮
  - king-square cache 👑
  - other hot search state ⚡
- Main modules:
  - `MoveGen` ♞
  - `Search` 🔍
  - `TT` 🧊
  - `History` 📚
  - `Evaluation` 🧠
  - `NNUE` 🤖
  - `UCI` 🗣️
  - `Time` ⏱️

### 2. Search Framework 🔎🔥
- Iterative Deepening 🪜
- Main search: PVS / Principal Variation Search 🎯
- Tactical frontier: Quiescence Search ⚔️
- Aspiration Windows with fallback widening 🪟
- Full `seldepth` tracking in UCI `info` output 📡

### 3. Move Ordering and Learned Signals 🧭📈
- Staged MovePicker in main search:
  - `TT move -> good captures -> killer1/2 -> quiets -> bad captures` 🧩
- Quiet ordering signals 🤫:
  - quiet history 📚
  - countermove 🔁
  - continuation history: `prev / prev2` 🧬
- Capture ordering signals ⚔️:
  - capture history 🩸
  - immediate SEE term 👁️
  - see-bias term ⚖️
  - MVV-LVA support term ♟️

### 4. Implemented Pruning / Reductions 🌲✂️
- TT cutoffs, bound-aware 🧊
- IIR / Internal Iterative Reduction 🪜
- Razoring 🪒
- Reverse Futility 🔙
- Null Move Pruning 🚫
- SEE late bad-capture gate 👁️🚪
- Futility pruning for shallow quiets 🤫
- LMP / Late Move Pruning ⏳
- History pruning 📚✂️
- Partial LMR:
  - quiet LMR with quiet strength + improving signal 🤫📈
  - capture LMR with simple-capture branch + fail-high re-search ⚔️🔁

### 5. History / Experience System 🧠📚
- killer table ☠️
- quiet history table 🤫
- capture history table ⚔️
- countermove table 🔁
- continuation history table 🧬
- see-bias table 👁️⚖️
- reward / penalty paths for cutoff winners and failed alternatives 🏆📉
  - includes continuation updates 🧬
  - includes see-bias updates 👁️

### 6. Transposition Table / TT 🧊🔑
- 64-byte clusters, 4 lanes 🛣️
- probe / save + prefetch ⚡
- replacement policy aware of:
  - depth 🕳️
  - age ⏳
  - bound 📐
  - PV status 🎯
- sampled `hashfull` reporting with reduced reporting frequency 📊

### 7. Evaluation 🧮♟️
- HCE: tapered evaluation with MG / EG interpolation ⚖️
- PST support, incrementally maintained 🗺️
- NNUE: optional neural evaluation 🤖
  - `UseNNUE`
  - `EvalFile`
- Automatic fallback to HCE if NNUE is unavailable 🛟

### 8. Syzygy Tablebases
- Native local `.rtbw` / `.rtbz` probing through the vendored MIT-licensed Fathom probe
- Root DTZ ranking with WDL fallback
- Internal WDL cutoffs with configurable piece and depth limits
- 50-move-rule-aware root scores and UCI `tbhits` reporting

UCI setup example:

```text
setoption name SyzygyPath value D:\tablebases
setoption name SyzygyProbeLimit value 7
setoption name SyzygyProbeDepth value 1
setoption name Syzygy50MoveRule value true
```

### 9. Time Management and UCI ⏱️🗣️
- Standard UCI commands:
  - `uci`
  - `isready`
  - `position`
  - `go`
  - `stop`
  - `quit`
  - `setoption`
- Exposed UCI options:
  - `Hash` 🧊
  - `Threads` 🧵
  - `Contempt` 😈
  - `Clear Hash` 🧹
  - `UseNNUE` 🤖
  - `Ponder` 💭
  - `Singular Telemetry` (TT trust, exclusion cost, and extension outcomes; default off)
  - `SyzygyPath`
  - `SyzygyProbeDepth`
  - `Syzygy50MoveRule`
  - `SyzygyProbeLimit`
  - `EvalFile` 📁
- `go` supports:
  - depth 🕳️
  - nodes 🌐
  - movetime ⏲️
  - wtime / btime / inc / movestogo ♟️
- TimeManager derives soft/hard budgets from clock, increment, moves-to-go, and game ply 📈

---

## Build 🛠️🚀

### Requirements 📦
- Compiler: `g++` with `-std=c++20` 💻
- Build tool: `make`  
  - `mingw32-make` on Windows 🪟
- Default target CPU flags in `src/Makefile`:
  - `AVX2 + BMI2 + POPCNT + LZCNT` ⚡
- If your CPU does not support these, adjust `ARCH_FLAGS` in `src/Makefile`. 🧰

### Directory Layout 📁
- Source: `src/` 🧾
- Binaries: `src/bin/` 🚀
- Build artifacts: `src/build/` 🏗️

### Windows / PowerShell + MinGW 🪟⚙️
```powershell
cd src
mingw32-make clean
mingw32-make -j
````

Output:

* `src/bin/magnus.exe` 🦄

### Linux / WSL / MSYS2 🐧⚙️

```bash
cd src
make clean
make -j
```

Output:

* `src/bin/magnus` 🦄

### Optional PGO Flow 🔥📈

1. Build an instrumented binary:

```bash
cd src
make pgo-generate
```

2. Run representative workloads to generate profile data:

```bash
./bin/magnus_pgo search 20 64
```

3. Rebuild with profile use:

```bash
make pgo-use
```

---

## Run Examples 🎮♟️

### UCI Mode 🗣️

```bash
# No arguments starts UCI mode
./src/bin/magnus

# Equivalent explicit form
./src/bin/magnus uci
```

### Bench / Perft / Divide / Fixed-Depth Search 🧪⚡

```bash
# perft: <depth> <hash_mb> <threads>
./src/bin/magnus 6 64 1

# divide: divide <depth> <hash_mb> <threads> [live]
./src/bin/magnus divide 6 64 1

# search: search <depth> <hash_mb>
./src/bin/magnus search 8 64
```

---

## MagnusChess🦄

Fast search.
Clean engineering.
A little bit of unicorn energy. 🦄♟️✨
