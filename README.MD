# MagnusChess

<p align="center">
  <img src="logo.png" alt="MagnusChess logo" width="220">
</p>

<p align="center">
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-blue">
  <img alt="UCI supported" src="https://img.shields.io/badge/UCI-supported-brightgreen">
  <img alt="MNUE embedded" src="https://img.shields.io/badge/MNUE-embedded-purple">
  <img alt="Syzygy supported" src="https://img.shields.io/badge/Syzygy-supported-informational">
  <img alt="License MIT" src="https://img.shields.io/badge/license-MIT-lightgrey">
</p>

MagnusChess is a C++20 UCI chess engine focused on practical playing strength,
reproducible testing, and a conventional high-performance search architecture.

## Project Scope

- UCI-compatible chess engine for GUIs, tournament managers, and automated
  testing.
- Classical alpha-beta search core with iterative deepening, transposition
  table support, move ordering, pruning, reductions, and quiescence search.
- Neural evaluation support through MNUE files, including an embedded default
  MNUE P2 network in the standard build.
- Optional Syzygy tablebase probing through the vendored Fathom backend.
- Built-in validation and benchmark commands for perft, divide, search bench,
  evaluation bench, and transposition-table bench.

## Major Version Strength Comparison

The table below uses the test records supplied with the release notes. These
rows are not one unified rating list. Each Elo number is relative to the listed
opponent, anchor, or direct predecessor test under the stated conditions.

| Release | Comparison | Time Control and Conditions | Games | Result From MagnusChess Perspective | Elo Result | LOS / Draw Ratio | Interpretation |
|---|---|---|---:|---|---:|---|---|
| MagnusChess 1.3.75 | vs Cheese 2.1, listed as 2805 CCRL Blitz | STC 8+0.08 | 1000 | 625W / 317D / 58L, 78.4% | +223.4 +/- 19.1 vs Cheese | LOS 100.0% / 31.7% | Approximately 3028 by this external blitz anchor if Cheese is treated as 2805. |
| [MagnusChess 2.4.102](https://github.com/TMagnusN/MagnusChess/releases/tag/2.4.102) | vs Wahoo v4.0.0 x86_64-v3, listed as CCRL 3058 +/- 9 | STC 8+0.08, 1 thread, 32 MB hash, 5-man Syzygy, `UHO_4060_v4.epd` | 9679 | 3162W / 4013D / 2504L, 53.4% | +23.70 +/- 5.30 | LOS 100.0% / 41.5% | Approximately 3082 by this external anchor. |
| [MagnusChess 2.4.102](https://github.com/TMagnusN/MagnusChess/releases/tag/2.4.102) | vs MagnusChess 1.3.75 | STC 8+0.08, 1 thread, 32 MB hash, `UHO_4060_v4.epd` | 3500 | 1002W / 1996D / 502L, 57.1% | +50.00 +/- 7.50 | LOS 100.0% / 57.0% | Direct short-time-control generation gain over 1.3.75. |
| [MagnusChess 2.4.102](https://github.com/TMagnusN/MagnusChess/releases/tag/2.4.102) | vs MagnusChess 1.3.75 | LTC 30+0.3, 1 thread, 256 MB hash, `UHO_4060_v4.epd` | 356 | 89W / 215D / 52L, 55.2% | +36.20 +/- 22.60 | LOS 99.9% / 60.4% | Longer-time-control confirmation with a smaller sample. |
| [MagnusChess 3.5.121](https://github.com/TMagnusN/MagnusChess/releases/tag/3.5.121) | v3.5.111 test build vs Ice4 v6.1 | STC 10+0.1, 1 thread, 128 MB hash, `UHO_4060_v4.epd` | 1151 | 454W / 444D / 253L, 58.7% | +61.30 +/- 15.80 | LOS 100.0% / 38.6% | Ice4 was used as the external anchor because direct tests against much older MagnusChess versions had become statistically less informative. |
| [MagnusChess 4.5.132](https://github.com/TMagnusN/MagnusChess/releases/tag/4.5.132) | Published STC release result; opponent not named in the supplied excerpt | STC 10+0.1, 1 thread, 128 MB hash, `UHO_4060_v4.epd` | 2095 | 634W / 1209D / 252L, 59.1% | +64.10 +/- 9.60 | LOS 100.0% / 57.7% | Release note records entry into the 3300 Elo class. |
| MagnusChess 5.6.132-dev | vs Avalanche 2.1.0, approximately 3400 CCRL Blitz anchor | STC 10+0.1, 1 thread, 128 MB hash, `UHO_4060_v4.epd` | 2145 | 520W / 861D / 764L, 44.3% | -39.7 +/- 11.4 vs Avalanche | LOS 0.0% / 40.1% | Approximately 3360 by this external blitz anchor. |
| MagnusChess 5.6.132-dev | vs MagnusChess 4.5.132-dev | STC 10+0.1, 1 thread, 128 MB hash, `UHO_4060_v4.epd` | 1163 | 239W / 748D / 176L, 52.7% | +18.8 +/- 11.9 | LOS 99.9% / 64.3% | Direct generation gain over 4.5.132-dev. |

## Technical Overview

### Board and State

- Bitboard-based position representation.
- Incremental state maintenance for keys, evaluation-related state, king
  squares, and search history.
- Core board implementation in `src/board/Attack.*`, `src/board/MoveGen.*`,
  and `src/board/Position.*`.

### Search

- Iterative deepening.
- Principal Variation Search.
- Quiescence search.
- Aspiration windows with widening fallback.
- Transposition table cutoffs with bound-aware storage.
- Internal iterative reductions.
- Razoring, reverse futility pruning, null-move pruning, futility pruning, late
  move pruning, history pruning, and late move reductions.
- UCI `info` output with depth, selective depth, score, nodes, NPS, hashfull,
  tablebase hits, principal variation, and optional telemetry.

### Move Ordering

- Staged move picker:
  `TT move -> good captures -> killer moves -> quiets -> bad captures`.
- Quiet move signals:
  quiet history, countermove history, and continuation history.
- Capture signals:
  capture history, static exchange evaluation, SEE bias, and MVV-LVA support.

### Transposition Table

- Clustered transposition table layout.
- Probe, save, and prefetch support.
- Replacement policy based on depth, age, bound type, and PV status.
- Sampled `hashfull` reporting for UCI output.

### Evaluation

- Hand-crafted evaluation fallback with tapered middle-game and end-game terms.
- MNUE P2 evaluation with a standard embedded network in the build.
- MNUE X1 and MNUEv2 development loaders and diagnostics.
- Legacy NNUE fallback path where supported by the build.
- If no neural evaluator is available, search falls back to HCE.

## MNUE Networks

The standard build embeds the MNUE P2 file named in `src/Makefile`:

```text
MNUE_EMBEDDED_FILE := mm-8adbe41d1.MNUE
```

The build expects this file to exist under `src/build/` so it can be included
through `src/mnue/MnueEmbedded.S`.

An external MNUE file can be selected through UCI:

```text
setoption name MNUEfile value path/to/network.MNUE
```

Use the embedded network again with:

```text
setoption name MNUEfile value <embedded>
```

MNUE networks are trained with the `jw1912/bullet` tooling. External network
files are kept outside the source repository; the historical network repository
is:

```text
https://github.com/TMagnusN/MagnusMNUE
```

## Syzygy Tablebases

MagnusChess probes local Syzygy `.rtbw` and `.rtbz` files through the vendored
MIT-licensed Fathom backend.

Supported behavior includes:

- Root DTZ ranking with WDL fallback.
- Internal WDL cutoffs with configurable piece and depth limits.
- 50-move-rule-aware root scores.
- UCI `tbhits` reporting.

UCI setup example:

```text
setoption name SyzygyPath value D:\tablebases
setoption name SyzygyProbeLimit value 7
setoption name SyzygyProbeDepth value 1
setoption name Syzygy50MoveRule value true
```

## Build

### Requirements

- 64-bit x86-64 compiler target.
- `g++` with C++20 support.
- `make` on Linux, WSL, or MSYS2.
- `mingw32-make` on Windows with MinGW.

The default `auto` profile maps to `native64` and uses `-march=native`. For a
portable x86-64 binary, build the `portable64` profile.

### Windows

```powershell
cd src
mingw32-make clean
mingw32-make auto -j
```

Repository-relative output:

```text
src/build/MagnusChess.exe
```

### Linux, WSL, or MSYS2

```bash
cd src
make clean
make auto -j
```

Repository-relative output:

```text
src/build/MagnusChess
```

### Build Profiles

```bash
# Native build for the current machine.
make auto -j

# Portable x86-64 baseline.
make portable64 -j

# Explicit native x86-64 build.
make native64 -j

# Build both portable64 and native64.
make variants -j

# Print compiler target, selected profile, CPU flags, and output path.
make detect
```

### PGO Build

Set `PGO=1` to build an instrumented executable, run the benchmark workload,
and rebuild with profile-guided optimization:

```bash
cd src
make auto PGO=1 -j
```

Windows uses the same variable:

```powershell
cd src
mingw32-make auto PGO=1 -j
```

PGO output is written as:

```text
src/build/MagnusChess-<profile>-pgo[.exe]
```

The default PGO training command is `bench`. Override it with
`PGO_BENCH_ARGS` if a different workload is required.

## Running

Start UCI mode with no arguments:

```bash
cd src
./build/MagnusChess
```

Or explicitly:

```bash
cd src
./build/MagnusChess uci
```

Windows:

```powershell
cd src
.\build\MagnusChess.exe uci
```

## Command-Line Tools

```bash
# Compact search benchmark.
./build/MagnusChess bench 12 64 1

# Perft benchmark.
./build/MagnusChess perft 6 64 1 magic

# Equivalent perft shorthand.
./build/MagnusChess 6 64 1 pext

# Divide output.
./build/MagnusChess divide 6 64 1 live magic

# Fixed-depth search benchmark.
./build/MagnusChess search 8 64 1

# Evaluation benchmark.
./build/MagnusChess evalbench 1000 64

# Transposition-table benchmark.
./build/MagnusChess -tt -hash 64
```

Available attack backends for perft and divide:

```text
auto
pext
magic
table
classical
```

## UCI Interface

Standard commands:

```text
uci
isready
ucinewgame
position
go
stop
ponderhit
quit
setoption
```

Supported `go` controls include:

```text
depth
nodes
movetime
wtime
btime
winc
binc
movestogo
ponder
infinite
searchmoves
```

Primary UCI options:

| Option | Type | Default | Notes |
|---|---|---:|---|
| Hash | spin | 16 | Megabytes allocated to the transposition table. |
| Threads | spin | 1 | Search thread count, maximum 512. |
| MultiPV | spin | 1 | Multi-principal-variation output, maximum 256. |
| Contempt | spin | 0 | Evaluation offset, range -10000 to 10000. |
| Move Overhead | spin | 10 | Time-management overhead in milliseconds. |
| Clear Hash | button | n/a | Clears the transposition table. |
| UseNNUE | check | true | Enables the neural evaluation path when available. |
| Ponder | check | true | Enables pondering support. |
| FullPV | check | false | Enables extended PV reporting. |
| Singular Telemetry | check | false | Emits singular-search telemetry. |
| UseMsvSmp | check | false | Enables the MSV SMP mode. |
| MsvInfo | check | false | Emits MSV diagnostic information. |
| SyzygyPath | string | `<empty>` | Directory containing Syzygy files. |
| SyzygyProbeDepth | spin | 1 | Minimum depth for tablebase probes, range 1 to 100. |
| Syzygy50MoveRule | check | true | Applies 50-move-rule-aware tablebase scoring. |
| SyzygyProbeLimit | spin | 7 | Maximum tablebase piece count, range 0 to 7. |
| MNUEfile | string | `mm-8adbe41d1.MNUE` | External MNUE file path or `<embedded>`. |

Diagnostic commands include:

```text
d
eval
bench
perft
divide
mnuedebug
mnuecheck
mnueloadp2 <path-to-p2.mnue>
mnuev2load <path-to-v2.mnue>
mnuev2info
mnuev2debug
mnuev2eval
mnuev2check [sequences plies]
mnuev2bench 1000 5
mnuev2scalar on|off
mnuex1load <path-to-x1.mnue>
mnuex1eval
mnuex1bench
mnuex1check
```

## Repository Layout

```text
src/                    Engine source
src/board/              Board representation, attacks, and move generation
src/mnue/               MNUE evaluators, loaders, embedded-network support
src/syzygy/             Syzygy integration
third_party/fathom/     Vendored Fathom tablebase probe
tools/                  MNUE trainer utilities
docs/                   Design notes and MNUE format documentation
```

## License

MagnusChess is distributed under the MIT License. See `LICENSE` for details.
