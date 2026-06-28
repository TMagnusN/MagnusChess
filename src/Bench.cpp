/*
MIT License

Copyright (c) 2026 TMagnusN

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "board/Attack.h"
#include "Bench.h"
#include "Common.h"
#include "Memory.h"
#include "board/MoveGen.h"
#include "mnue/Mnue.h"
#include "mnue/MnueV2Network.h"
#include "Nnue.h"
#include "Perft.h"
#include "board/Position.h"
#include "Search.h"

#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <streambuf>

/* ===== 繁體中文註釋 =====
 * 本檔案是 MagnusChess 西洋棋引擎的一部分。
 * 實作詳情請參閱對應的 .h 標頭檔案。
 */

namespace magnus {

namespace {

constexpr std::string_view BENCH_SEPARATOR = "===========================";
constexpr int MAX_SEARCH_THREADS = 512;

constexpr std::array<std::string_view, 50> SEARCH_BENCH_FENS{{
    "rnb1k2r/pp2bp1p/2p1pp2/q7/8/1P6/PBPPQPPP/2KR1BNR w kq - 4 9",
    "1r1qk1nr/pppn1ppp/3p4/3Pp1b1/2P5/2N2Q1P/PP2PPP1/R1B1KB1R w KQk - 3 9",
    "r3kbnr/pp3ppp/2n5/2P1pq2/N1Pp2b1/5N2/PP1BPPPP/R2QKB1R w KQkq - 0 9",
    "rnb1k2r/pppp2pp/8/8/2P2Bn1/2q4N/P3PPPP/R2QKB1R w KQkq - 0 9",
    "r2qk2r/ppp1bppp/2n1bn2/8/2NPp3/2P5/PP2BPPP/RNBQ1RK1 w kq - 1 9",
    "r1b1kb1r/1pqnppp1/p2p1n1p/8/3NP1PP/P1N5/1PP2P2/R1BQKB1R w KQkq - 1 9",
    "rnb1kb1r/pp3pp1/1qpnp2p/3p4/3PP2B/P1N2P1N/1PP3PP/R2QKB1R w KQkq - 0 9",
    "r1bqr1k1/pppp1ppp/2n5/3np1N1/4P3/2P5/PPP2PPP/R1BQ1RK1 w - - 0 9",
    "r1bqkb1r/3n1ppp/p1p1pn2/1p6/8/5NP1/PPQPPPBP/RNB2RK1 w kq - 0 9",
    "r2qk2r/ppp1bppp/2np2n1/3N4/2BPPpb1/5N2/PPP3PP/R1BQ1RK1 w kq - 4 9",
    "r1bqkbnr/3n1ppp/p3p3/2p5/Pp1P4/4PN2/1P2BPPP/RNBQ1RK1 w kq - 0 9",
    "r1bqk2r/ppp1bppp/2n1p3/3pP3/2PP1B2/2PQ4/P4PPP/R3KBNR w KQkq d6 0 9",
    "r1bqk1nr/1ppn1pb1/p2p2pp/4p3/P2PP3/2NB1N2/1PP2PPP/R1BQ1RK1 w kq - 0 9",
    "r1bq1rk1/pp1pppbp/5np1/4n3/2PN4/1PN3P1/P3PPBP/R1BQK2R w KQ - 1 9",
    "rn1qkb1r/1p2npp1/4p2p/p2pPb2/3P4/P1N5/1P2NPPP/R1BQKB1R w KQkq - 2 9",
    "r1bqk2r/1p1nbpp1/p2p1n1p/2pPp3/2P5/P1N1PN1P/1P3PP1/R1BQKB1R w KQkq - 1 9",
    "r1bqk2r/2p1bpp1/p1np1n2/1p2p2p/3PP3/2N1BP2/PPPQN1PP/2KR1B1R w kq - 0 9",
    "r1bqk2r/p2nppbp/2pp2p1/1p2Pn2/3P1P2/2N1BN2/PPPQ2PP/R3KB1R w KQkq - 3 9",
    "r1b1k2r/pp1n1ppp/2p1pn2/q2p4/1bPP4/1PN2NP1/P2BPPBP/R2QK2R w KQkq - 3 9",
    "r1bq1bnr/p1p4p/1pk2p2/3pp1pQ/3P4/4P1B1/PPP2PPP/RN2K1NR w KQ - 0 9",
    "rn1qkb1r/1bp2ppp/p3pn2/8/Pp1PP3/1B3P2/1P2N1PP/RNBQK2R w KQkq - 0 9",
    "rnbq1rk1/1p2ppbp/2p3p1/p2n4/3P4/2NB1N1P/PPP2PP1/R1BQ1RK1 w - - 0 9",
    "rn1qkb1r/1p3p2/p1p1pn1p/2Pp1bp1/3P1B2/2N1PN2/PP2BPPP/R2QK2R w KQkq - 0 9",
    "r1bqk2r/pp1n1pp1/3p1n1p/2pPp3/1bP1P3/2N1BP2/PP4PP/R2QKBNR w KQkq - 2 9",
    "r1bqkb1r/1p2np1p/p1npp1p1/8/3NPP2/2NBB3/PPP3PP/R2QK2R w KQkq - 0 9",
    "rnbq1rk1/p1p2pp1/1p3p1p/8/1bBP4/2N1P3/PP2NPPP/R2QK2R w KQ - 0 9",
    "r1bqkb1r/pp4pp/2p1p3/3pnp1n/2PP4/2NBPN2/PP3PPP/R2QK2R w KQkq - 0 9",
    "rn1q1rk1/p1ppbppp/b3pn2/1p6/2PP4/1P3NP1/P2BPPBP/RN1Q1RK1 w - - 0 9",
    "rnbq1rk1/p3npbp/1pp1p1p1/3p4/2PP1B2/2NBPN2/PP3PPP/2RQK2R w K - 0 9",
    "rnbqk2r/1pp2pbp/p2p2p1/3pP3/3P1P2/3B1N2/PPP3PP/R1BQK2R w KQkq - 0 9",
    "rn1q1rk1/pbp1ppbp/1p3np1/3p4/3PPP2/2N1BB1P/PPP3P1/R2QK1NR w KQ - 0 9",
    "rnbqk2r/pp2p1bp/2pn1pp1/3pN3/3P4/2P3P1/PP1NPPBP/R1BQ1RK1 w kq - 0 9",
    "r1bq1rk1/p1pp1ppp/1pn5/4P3/2P1n3/P3PN2/1P1B1PPP/R2QKB1R w KQ - 0 9",
    "r1bqkbnr/ppp2p2/2npp3/8/2PP1P1p/3NP1pP/PP4P1/RNBQKB1R w KQkq - 0 9",
    "r2qk2r/ppp3pp/2n1b3/3n4/1b6/2N1PN2/PP3PPP/R1BQKB1R w KQkq - 0 9",
    "r1bqk2r/pppnn1b1/3pp1pp/5p1P/4PP2/2PP1N2/PP2B1P1/RNBQK2R w KQkq - 1 9",
    "r1b1kb1r/1p1p1ppp/p1q1pn2/8/4P3/1P1B4/P1P2PPP/RNBQ1RK1 w kq - 0 9",
    "r1bqk1nr/pp1p1ppp/1b6/8/1n2P3/1N1B4/PP3PPP/RNBQK2R w KQkq - 5 9",
    "r2q1rk1/ppp1ppb1/2np1np1/6Bp/3PP1bP/2PQ1N2/PP1N1PP1/R3KB1R w KQ - 5 9",
    "rn1qkb1r/1bpp2pp/p3p3/3n1p2/Pp1P4/4PNB1/1PPNBPPP/R2QK2R w KQkq - 0 9",
    "r1b1kbnr/1pq2pp1/p1np3p/4p3/2B1P3/5N2/PPP2PPP/RNBQR1K1 w kq - 2 9",
    "r1b1kb1r/pp3ppp/1q2pn2/2pP4/2pn4/2N2NP1/PP2PPBP/R1BQ1RK1 w kq - 0 9",
    "r3kb1r/ppp1p2p/2np1np1/5q2/3P4/5N2/PPP2PPP/RNBQ1RK1 w kq - 0 9",
    "r1bqnrk1/pp1nbppp/3pp3/2p3B1/3PP3/2PB1N1P/PP3PP1/RN1Q1RK1 w - - 1 9",
    "r1bqr1k1/pp1nbppp/4pn2/2pp4/3P1B1P/2PBPN2/PP1N1PP1/R2QK2R w KQ - 1 9",
    "r1b1kb1r/1pq2ppp/p1np1n2/2p1p3/P3P3/2N2NP1/1PPP1PBP/R1BQR1K1 w kq - 0 9",
    "rnbq1rk1/1p2ppb1/2pp1npp/p7/P2PP3/2N1BN2/1PP1BPPP/R2Q1RK1 w - - 0 9",
    "r1bqk2r/pp1nppbp/2np4/6B1/2P5/2NQPN2/PP3PPP/R3KB1R w KQkq - 1 9",
    "rnbq1rk1/1p2ppb1/p1p2n1p/3p2p1/2PP4/2N1PNBP/PP3PP1/R2QKB1R w KQ - 1 9",
    "rn2k2r/ppq2p1p/2ppbp2/2b1p3/2B1P2N/3P4/PPP2PPP/RN1Q1RK1 w kq - 2 9",
}};

struct SearchBenchResult {
    search::SearchResult search{};
    u64 time_ms = 0;
    double seconds = 0.0;
    u64 nps = 0;
    std::string ponder{};
};

struct TTBenchFingerprint {
    Move move = 0;
    i16 score = 0;
    i16 eval = 0;
    i16 depth = 0;
};

struct TTBenchProbeResult {
    u64 hits = 0;
    u64 misses = 0;
    u64 false_hits = 0;
    u64 checksum = 0;
    double seconds = 0.0;
};

[[nodiscard]] inline Key tt_bench_key(u64 ordinal) noexcept {
    return mix64(0xA0761D6478BD642FULL + ordinal);
}

[[nodiscard]] inline TTBenchFingerprint tt_bench_fingerprint(Key key) noexcept {
    const u64 signature = mix64(key ^ 0xE7037ED1A0B428DBULL);
    TTBenchFingerprint fingerprint{};
    fingerprint.move = static_cast<Move>(
        static_cast<u16>(signature) | static_cast<u16>(1)
    );
    fingerprint.score = static_cast<i16>(signature >> 16);
    fingerprint.eval = static_cast<i16>(signature >> 32);
    fingerprint.depth = static_cast<i16>(1 + ((signature >> 48) & 0x7FFFULL));
    return fingerprint;
}

[[nodiscard]] inline bool tt_bench_payload_matches(
    const memory::TTData& data,
    const TTBenchFingerprint& expected
) noexcept {
    return data.move == expected.move &&
           data.score == expected.score &&
           data.eval == expected.eval &&
           data.depth == expected.depth &&
           (data.flags & 0x3U) == memory::BOUND_EXACT;
}

[[nodiscard]] inline double tt_bench_mops(u64 operations, double seconds) noexcept {
    return seconds > 0.0
        ? static_cast<double>(operations) / seconds / 1'000'000.0
        : 0.0;
}

[[nodiscard]] inline double tt_bench_percent(u64 count, u64 total) noexcept {
    return total > 0
        ? 100.0 * static_cast<double>(count) / static_cast<double>(total)
        : 0.0;
}

[[nodiscard]] TTBenchProbeResult benchmark_tt_probes(
    memory::TT& tt,
    u64 first_ordinal,
    u64 count,
    bool keys_were_inserted
) {
    using clock = std::chrono::steady_clock;

    TTBenchProbeResult result{};
    const auto start = clock::now();
    for (u64 i = 0; i < count; ++i) {
        const Key key = tt_bench_key(first_ordinal + i);
        const memory::TTProbe probe = memory::tt_probe(tt, key);
        if (!probe.hit) {
            ++result.misses;
            continue;
        }

        result.checksum +=
            static_cast<u64>(probe.data.move) +
            static_cast<u64>(static_cast<u16>(probe.data.score)) +
            static_cast<u64>(static_cast<u16>(probe.data.eval)) +
            static_cast<u64>(static_cast<u16>(probe.data.depth));

        if (keys_were_inserted &&
            tt_bench_payload_matches(probe.data, tt_bench_fingerprint(key))) {
            ++result.hits;
        } else {
            ++result.false_hits;
        }
    }
    const auto end = clock::now();
    result.seconds = std::chrono::duration<double>(end - start).count();
    return result;
}

[[nodiscard]] bool ensure_nnue_loaded(
    const std::string& eval_file,
    std::ostream* out
) {
    if (nnue::loaded() && nnue::path() == eval_file)
        return true;

    if (nnue::load(eval_file)) {
        if (out)
            *out << "info string loaded nnue " << eval_file << '\n';
        return true;
    }

    if (out)
        *out << "info string failed to load nnue: " << nnue::last_error() << '\n';
    return false;
}

[[nodiscard]] bool ensure_mnue_p2_loaded(
    const std::string& eval_file,
    std::ostream* out
) {
    // External file explicitly requested.
    if (!eval_file.empty()) {
        if (mnue::p2_loaded() && mnue::p2_path() == eval_file)
            return true;

        if (mnue::load_p2(eval_file)) {
            if (out)
                *out << "info string loaded mnue p2 " << eval_file << '\n';
            return true;
        }

        if (out)
            *out << "info string failed to load mnue p2: " << mnue::last_error() << '\n';
        return false;
    }

    // Default: compile-time embedded P2 network.
    if (mnue::p2_embedded_available() && mnue::load_p2_embedded()) {
        if (out)
            *out << "info string loaded embedded mnue p2\n";
        return true;
    }

    if (out)
        *out << "info string no MNUE network available\n";
    return false;
}

struct EvalBenchTiming {
    u64 micros = 0;
    i64 checksum = 0;
    std::size_t evals = 0;
};

template<typename EvalFn>
[[nodiscard]] EvalBenchTiming benchmark_eval_batch(
    const std::array<Position, SEARCH_BENCH_FENS.size()>& positions,
    int iterations,
    EvalFn eval_fn
) {
    using clock = std::chrono::steady_clock;

    EvalBenchTiming result;
    result.evals = static_cast<std::size_t>(iterations) * positions.size();

    const auto start = clock::now();
    for (int iter = 0; iter < iterations; ++iter) {
        for (const Position& pos : positions)
            result.checksum += static_cast<i64>(eval_fn(pos));
    }
    const auto end = clock::now();

    result.micros = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()
    );
    return result;
}

[[nodiscard]] EvalBenchTiming benchmark_p2_stack_batch(
    const std::array<Position, SEARCH_BENCH_FENS.size()>& positions,
    int iterations
) {
    using clock = std::chrono::steady_clock;

    std::array<mnue::P2AccumulatorStack, SEARCH_BENCH_FENS.size()> stacks{};
    for (std::size_t i = 0; i < positions.size(); ++i) {
        stacks[i].reset();
        (void)mnue::eval_p2(positions[i], stacks[i]);
    }

    EvalBenchTiming result;
    result.evals = static_cast<std::size_t>(iterations) * positions.size();
    const auto start = clock::now();
    for (int iter = 0; iter < iterations; ++iter)
        for (std::size_t i = 0; i < positions.size(); ++i)
            result.checksum += static_cast<i64>(mnue::eval_p2(positions[i], stacks[i]));
    const auto end = clock::now();

    result.micros = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()
    );
    return result;
}

[[nodiscard]] EvalBenchTiming benchmark_v2_stack_batch(
    const std::array<Position, SEARCH_BENCH_FENS.size()>& positions,
    const memory::Memory& mem,
    int iterations
) {
    using clock = std::chrono::steady_clock;

    std::array<
        mnue::v2::AccumulatorStack,
        SEARCH_BENCH_FENS.size()
    > stacks{};
    for (std::size_t i = 0; i < positions.size(); ++i) {
        stacks[i].reset();
        (void)mnue::v2::evaluate_incremental(
            positions[i],
            mem,
            stacks[i]
        );
    }

    EvalBenchTiming result;
    result.evals = static_cast<std::size_t>(iterations) * positions.size();
    const auto start = clock::now();
    for (int iter = 0; iter < iterations; ++iter) {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            result.checksum += static_cast<i64>(
                mnue::v2::evaluate_incremental(
                    positions[i],
                    mem,
                    stacks[i]
                )
            );
        }
    }
    const auto end = clock::now();

    result.micros = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            end - start
        ).count()
    );
    return result;
}

void render_eval_bench_timing(
    std::ostream& out,
    std::string_view name,
    const EvalBenchTiming& timing
) {
    const double seconds = static_cast<double>(timing.micros) / 1000000.0;
    const double evals_per_second = seconds > 0.0
        ? static_cast<double>(timing.evals) / seconds
        : 0.0;

    out << "  " << name
        << " time_us " << timing.micros
        << " evals_per_second " << static_cast<u64>(evals_per_second)
        << " checksum " << timing.checksum
        << '\n';
}

[[nodiscard]] bool prepare_eval_positions(
    memory::Memory& mem,
    std::array<Position, SEARCH_BENCH_FENS.size()>& positions,
    std::ostream& out
) {
    for (std::size_t i = 0; i < SEARCH_BENCH_FENS.size(); ++i) {
        if (!parse_fen(positions[i], mem, SEARCH_BENCH_FENS[i])) {
            out << "info string failed to parse evalbench FEN: "
                << SEARCH_BENCH_FENS[i] << '\n';
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool check_p2_incremental(
    const Position& pos,
    mnue::P2AccumulatorStack& stack,
    std::ostream& out
) {
    std::ostringstream check_log;
    if (mnue::debug_check_p2_incremental(pos, stack, check_log))
        return true;

    out << check_log.str();
    return false;
}

[[nodiscard]] Move find_legal_move_by_uci(
    const Position& pos,
    const memory::Memory& mem,
    std::string_view move_text
) {
    MoveList list{};
    generate_legal(pos, mem, list);

    for (int i = 0; i < list.size; ++i) {
        const std::string generated = search::move_to_uci(list.moves[i]);
        if (std::string_view(generated) == move_text)
            return list.moves[i];
    }

    return Move(0);
}

[[nodiscard]] bool check_p2_move_roundtrip(
    Position& pos,
    mnue::P2AccumulatorStack& stack,
    const memory::Memory& mem,
    Move move,
    std::ostream& out
) {
    if (!check_p2_incremental(pos, stack, out))
        return false;

    StateInfo st{};
    stack.push(pos, move);
    make_move(pos, move, mem.tables, st);
    if (!check_p2_incremental(pos, stack, out))
        return false;

    unmake_move(pos, move, mem.tables, st);
    stack.pop();
    return check_p2_incremental(pos, stack, out);
}

[[nodiscard]] bool check_p2_named_move(
    std::string_view fen,
    std::string_view move_text,
    const memory::Memory& mem,
    std::ostream& out
) {
    Position pos{};
    if (!parse_fen(pos, mem, fen)) {
        out << "info string failed to parse p2 check FEN: " << fen << '\n';
        return false;
    }

    const Move move = find_legal_move_by_uci(pos, mem, move_text);
    if (move_is_none(move)) {
        out << "info string p2 check move not legal: " << move_text
            << " in " << fen << '\n';
        return false;
    }

    mnue::P2AccumulatorStack stack{};
    stack.reset();
    return check_p2_move_roundtrip(pos, stack, mem, move, out);
}

[[nodiscard]] bool run_p2_incremental_smoke(
    const std::array<Position, SEARCH_BENCH_FENS.size()>& positions,
    const memory::Memory& mem,
    std::ostream& out
) {
    int checks = 0;
    for (std::size_t root_index = 0; root_index < positions.size(); ++root_index) {
        const Position& root = positions[root_index];
        Position pos = root;
        mnue::P2AccumulatorStack stack{};
        stack.reset();
        if ((root_index & 1u) == 0) {
            if (!check_p2_incremental(pos, stack, out))
                return false;
            ++checks;
        }

        u32 rng = static_cast<u32>(pos.key ^ (pos.key >> 32) ^ 0x9E3779B9u);
        std::array<Move, 12> moves{};
        std::array<StateInfo, 12> states{};
        int depth = 0;
        for (int step = 0; step < static_cast<int>(moves.size()); ++step) {
            MoveList list{};
            generate_legal(pos, mem, list);
            if (list.size == 0)
                break;

            rng = rng * 1664525u + 1013904223u;
            const Move move = list.moves[rng % static_cast<u32>(list.size)];
            moves[depth] = move;
            stack.push(pos, move);
            make_move(pos, move, mem.tables, states[depth]);
            ++depth;

            if ((step & 3) == 3) {
                if (!check_p2_incremental(pos, stack, out))
                    return false;
                ++checks;
            }
        }

        if (!check_p2_incremental(pos, stack, out))
            return false;
        ++checks;

        while (depth > 0) {
            --depth;
            unmake_move(pos, moves[depth], mem.tables, states[depth]);
            stack.pop();
            if (!check_p2_incremental(pos, stack, out))
                return false;
            ++checks;
        }

        if (stack.size() != 1) {
            out << "info string p2 stack did not return to root\n";
            return false;
        }
    }

    const bool scenarios_ok =
        check_p2_named_move(
            "7k/8/8/8/8/8/8/4K3 w - - 0 1",
            "e1f1",
            mem,
            out
        ) &&
        check_p2_named_move(
            "7k/8/8/8/8/8/8/4K3 w - - 0 1",
            "e1d1",
            mem,
            out
        ) &&
        check_p2_named_move(
            "7k/8/8/8/8/8/4p3/3QK3 w - - 0 1",
            "d1e2",
            mem,
            out
        ) &&
        check_p2_named_move(
            "7k/P7/8/8/8/8/8/4K3 w - - 0 1",
            "a7a8q",
            mem,
            out
        ) &&
        check_p2_named_move(
            "1r5k/P7/8/8/8/8/8/4K3 w - - 0 1",
            "a7b8q",
            mem,
            out
        ) &&
        check_p2_named_move(
            "7k/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
            "e5d6",
            mem,
            out
        ) &&
        check_p2_named_move(
            "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
            "e1g1",
            mem,
            out
        );

    if (!scenarios_ok)
        return false;

    out << "  p2_incremental_checks " << (checks + 7) << " ok\n";
    return true;
}

[[nodiscard]] bool check_p2_generation_reload(
    const Position& pos,
    std::ostream& out
) {
    mnue::P2AccumulatorStack stack{};
    stack.reset();
    if (!check_p2_incremental(pos, stack, out))
        return false;

    const std::string path = mnue::p2_path();
    if (!mnue::load_p2(path)) {
        out << "info string p2 generation reload failed: "
            << mnue::last_error() << '\n';
        return false;
    }

    if (!check_p2_incremental(pos, stack, out))
        return false;

    out << "  p2_generation_reload ok\n";
    return true;
}

[[nodiscard]] SearchBenchResult benchmark_search_position(
    const Position& pos,
    memory::Memory& mem,
    const search::SearchLimits& limits,
    std::ostream* out
) {
    using clock = std::chrono::steady_clock;

    memory::memory_clear_hash(mem);

    const auto start = clock::now();
    SearchBenchResult result;
    if (out != nullptr) {
        PvTrackingStreamBuf pv_tracking_buf(out->rdbuf());
        std::ostream tracked_out(&pv_tracking_buf);
        result.search = search::iterative_deepening(pos, mem, limits, &tracked_out);
        tracked_out.flush();
    } else {
        result.search = search::iterative_deepening(pos, mem, limits, nullptr);
    }
    if (limits.recover_ponder_pv)
        result.ponder = ponder_move_from_search_result(pos, mem, result.search);
    const auto end = clock::now();

    const u64 time_ms = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count()
    );
    const double seconds = std::chrono::duration<double>(end - start).count();

    result.time_ms = time_ms;
    result.seconds = seconds;
    result.nps = seconds > 0.0
        ? static_cast<u64>(static_cast<double>(result.search.nodes) / seconds)
        : 0ULL;
    return result;
}

void render_search_bench_summary(
    std::ostream& out,
    u64 total_time_ms,
    u64 total_nodes
) {
    const u64 total_nps = total_time_ms > 0
        ? static_cast<u64>((total_nodes * 1000ULL) / total_time_ms)
        : 0ULL;

    out << BENCH_SEPARATOR << "\n";
    out << "Total time (ms) : " << total_time_ms << "\n";
    out << "Nodes searched  : " << total_nodes << "\n";
    out << "Nodes/second    : " << total_nps << "\n";
}

} // namespace

/*
Bench mode is deliberately tiny: create a start position, initialize shared
tables, then route either to perft/divide or to the fixed-depth search smoke test.
*/

void set_start_position(Position& pos) noexcept {
    // Rebuild the standard initial chess position through the public mutators so
    // all caches and bitboards are initialized exactly as search expects.
    position_clear(pos);

    pos.side_to_move = WHITE;
    pos.ep_sq = NO_SQ;
    pos.castling_rights = ANY_CASTLING;
    pos.halfmove_clock = 0;
    pos.fullmove_number = 1;

    for (int sq = 8; sq < 16; ++sq)
        position_put_piece(pos, WHITE, PAWN, sq);
    for (int sq = 48; sq < 56; ++sq)
        position_put_piece(pos, BLACK, PAWN, sq);

    position_put_piece(pos, WHITE, ROOK, 0);
    position_put_piece(pos, WHITE, KNIGHT, 1);
    position_put_piece(pos, WHITE, BISHOP, 2);
    position_put_piece(pos, WHITE, QUEEN, 3);
    position_put_piece(pos, WHITE, KING, 4);
    position_put_piece(pos, WHITE, BISHOP, 5);
    position_put_piece(pos, WHITE, KNIGHT, 6);
    position_put_piece(pos, WHITE, ROOK, 7);

    position_put_piece(pos, BLACK, ROOK, 56);
    position_put_piece(pos, BLACK, KNIGHT, 57);
    position_put_piece(pos, BLACK, BISHOP, 58);
    position_put_piece(pos, BLACK, QUEEN, 59);
    position_put_piece(pos, BLACK, KING, 60);
    position_put_piece(pos, BLACK, BISHOP, 61);
    position_put_piece(pos, BLACK, KNIGHT, 62);
    position_put_piece(pos, BLACK, ROOK, 63);
}

PerftBenchResult benchmark_perft(
    const Position& pos,
    const memory::Memory& mem,
    int depth,
    std::size_t threads
) {
    using clock = std::chrono::steady_clock;

    const auto start = clock::now();
    const NodeCount nodes = threads > 1
        ? perft_mt(pos, mem, depth, threads)
        : perft(pos, mem, depth);
    const auto end = clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();

    PerftBenchResult r;
    r.depth = depth;
    r.nodes = nodes;
    r.seconds = seconds;
    r.nps = seconds > 0.0 ? static_cast<double>(nodes) / seconds : 0.0;
    r.threads = threads;
    return r;
}

bool run_tt_bench(
    std::size_t hash_mb,
    u64 entries,
    std::ostream& out
) {
    using clock = std::chrono::steady_clock;

    memory::TT tt{};
    memory::tt_resize_mb(tt, hash_mb);
    const u64 slot_count =
        static_cast<u64>(tt.cluster_count) * 4ULL;
    const u64 operation_count = entries > 0 ? entries : slot_count;
    const u64 allocated_bytes =
        static_cast<u64>(tt.cluster_count) * sizeof(memory::TTCluster);

    const auto clear_start = clock::now();
    memory::tt_clear(tt);
    const auto clear_end = clock::now();
    const double clear_seconds =
        std::chrono::duration<double>(clear_end - clear_start).count();

    const auto store_start = clock::now();
    for (u64 i = 0; i < operation_count; ++i) {
        const Key key = tt_bench_key(i);
        const TTBenchFingerprint payload = tt_bench_fingerprint(key);
        memory::tt_save(
            tt,
            key,
            payload.move,
            payload.score,
            payload.eval,
            payload.depth,
            memory::BOUND_EXACT,
            false
        );
    }
    const auto store_end = clock::now();
    const double store_seconds =
        std::chrono::duration<double>(store_end - store_start).count();

    const TTBenchProbeResult inserted =
        benchmark_tt_probes(tt, 0, operation_count, true);
    const TTBenchProbeResult absent =
        benchmark_tt_probes(tt, operation_count, operation_count, false);

    u64 occupied_slots = 0;
    u64 empty_clusters = 0;
    u64 full_clusters = 0;
    for (std::size_t cluster_index = 0;
         cluster_index < tt.cluster_count;
         ++cluster_index) {
        int occupancy = 0;
        const memory::TTCluster& cluster = tt.clusters[cluster_index];
        for (int lane = 0; lane < 4; ++lane)
            occupancy += memory::tt_cluster_load(cluster, lane).age != 0;
        occupied_slots += static_cast<u64>(occupancy);
        empty_clusters += occupancy == 0;
        full_clusters += occupancy == 4;
    }

    const u64 unavailable = inserted.misses + inserted.false_hits;
    out << std::fixed << std::setprecision(3);
    out << "tt benchmark\n";
    out << "  requested hash mb    : " << hash_mb << '\n';
    out << "  allocated hash mb    : "
        << static_cast<double>(allocated_bytes) / (1024.0 * 1024.0) << '\n';
    out << "  cluster bytes        : " << sizeof(memory::TTCluster) << '\n';
    out << "  clusters             : " << tt.cluster_count << '\n';
    out << "  slots (4-way)        : " << slot_count << '\n';
    out << "  inserted entries     : " << operation_count << '\n';
    out << "  nominal load         : "
        << tt_bench_percent(operation_count, slot_count) << " %\n";
    out << '\n';
    out << "speed\n";
    out << "  clear time           : " << clear_seconds * 1000.0 << " ms\n";
    out << "  store                : " << tt_bench_mops(operation_count, store_seconds)
        << " Mops/s (" << store_seconds * 1000.0 << " ms)\n";
    out << "  probe inserted       : " << tt_bench_mops(operation_count, inserted.seconds)
        << " Mops/s (" << inserted.seconds * 1000.0 << " ms)\n";
    out << "  probe absent         : " << tt_bench_mops(operation_count, absent.seconds)
        << " Mops/s (" << absent.seconds * 1000.0 << " ms)\n";
    out << '\n';
    out << "occupancy\n";
    out << "  occupied slots       : " << occupied_slots
        << " (" << tt_bench_percent(occupied_slots, slot_count) << " %)\n";
    out << "  empty clusters       : " << empty_clusters
        << " (" << tt_bench_percent(
            empty_clusters,
            static_cast<u64>(tt.cluster_count)
        ) << " %)\n";
    out << "  full clusters        : " << full_clusters
        << " (" << tt_bench_percent(
            full_clusters,
            static_cast<u64>(tt.cluster_count)
        ) << " %)\n";
    out << '\n';
    out << "collisions and retention\n";
    out << "  retained hits        : " << inserted.hits
        << " (" << tt_bench_percent(inserted.hits, operation_count) << " %)\n";
    out << "  replacement misses   : " << inserted.misses
        << " (" << tt_bench_percent(inserted.misses, operation_count) << " %)\n";
    out << "  inserted false hits  : " << inserted.false_hits
        << " (" << tt_bench_percent(inserted.false_hits, operation_count) << " %)\n";
    out << "  total unavailable    : " << unavailable
        << " (" << tt_bench_percent(unavailable, operation_count) << " %)\n";
    out << "  absent false hits    : " << absent.false_hits
        << " (" << tt_bench_percent(absent.false_hits, operation_count) << " %)\n";
    out << "  probe checksum       : "
        << (inserted.checksum ^ absent.checksum) << '\n';

    memory::tt_free(tt);
    return true;
}

bool run_search_bench(
    memory::Memory& mem,
    int depth,
    std::size_t threads,
    bool use_nnue,
    bool emit_ponder,
    std::ostream& out
) {
    u64 total_time_ms = 0;
    u64 total_nodes = 0;

    const int search_threads = std::clamp<int>(static_cast<int>(threads), 1, MAX_SEARCH_THREADS);
    out << "info string Using " << search_threads << " thread"
        << (search_threads == 1 ? "" : "s") << "\n\n";

    for (std::size_t i = 0; i < SEARCH_BENCH_FENS.size(); ++i) {
        Position bench_pos{};
        const std::string_view fen = SEARCH_BENCH_FENS[i];
        if (!parse_fen(bench_pos, mem, fen)) {
            out << "info string failed to parse bench FEN: " << fen << '\n';
            return false;
        }

        out << "Position: " << (i + 1) << '/' << SEARCH_BENCH_FENS.size()
            << " (" << fen << ")\n";

        search::SearchLimits limits{};
        limits.depth = depth;
        limits.use_nnue = use_nnue;
        limits.thread_count = search_threads;
        limits.thread_id = 0;
        limits.report_info = true;
        limits.recover_ponder_pv = emit_ponder;

        const SearchBenchResult res = benchmark_search_position(
            bench_pos,
            mem,
            limits,
            &out
        );

        total_time_ms += res.time_ms;
        total_nodes += res.search.nodes;

        out << "bestmove " << search::move_to_uci(res.search.best_move);
        if (emit_ponder && !res.ponder.empty())
            out << " ponder " << res.ponder;
        out << "\n";
        if (i + 1 != SEARCH_BENCH_FENS.size())
            out << "\n";
    }

    render_search_bench_summary(out, total_time_ms, total_nodes);
    return true;
}

bool run_compact_search_bench(
    memory::Memory& mem,
    int depth,
    std::size_t hash_mb,
    std::size_t threads,
    bool use_nnue,
    std::ostream& out
) {
    const int search_threads = std::clamp<int>(
        static_cast<int>(threads),
        1,
        MAX_SEARCH_THREADS
    );

    u64 total_nodes = 0;
    double total_seconds = 0.0;

    out << "--------------------------------------------------\n";
    out << "          Nodes       Elapsed             NPS\n";
    out << "--------------------------------------------------\n";

    for (std::size_t i = 0; i < SEARCH_BENCH_FENS.size(); ++i) {
        Position bench_pos{};
        const std::string_view fen = SEARCH_BENCH_FENS[i];
        if (!parse_fen(bench_pos, mem, fen)) {
            out << "info string failed to parse bench FEN: " << fen << '\n';
            return false;
        }

        search::SearchLimits limits{};
        limits.depth = depth;
        limits.use_nnue = use_nnue;
        limits.thread_count = search_threads;
        limits.thread_id = 0;
        limits.report_info = false;
        limits.recover_ponder_pv = false;

        const SearchBenchResult res = benchmark_search_position(
            bench_pos,
            mem,
            limits,
            nullptr
        );

        total_nodes += res.search.nodes;
        total_seconds += res.seconds;

        out << std::setw(3) << i
            << std::setw(12) << res.search.nodes
            << std::setw(13) << std::fixed << std::setprecision(3)
            << res.seconds << "s"
            << std::setw(16) << res.nps << " N/s\n";
    }

    const u64 total_nps = total_seconds > 0.0
        ? static_cast<u64>(static_cast<double>(total_nodes) / total_seconds)
        : 0ULL;

    out << "--------------------------------------------------\n";
    out << std::setw(15) << total_nodes
        << std::setw(13) << std::fixed << std::setprecision(3)
        << total_seconds << "s"
        << std::setw(16) << total_nps << " N/s\n";
    out << "--------------------------------------------------\n";
    out << "depth " << depth
        << " hash " << hash_mb
        << " threads " << search_threads
        << " evaluator " << (use_nnue ? "nnue" : "hce")
        << '\n';
    return true;
}

bool run_timed_search_bench(
    memory::Memory& mem,
    int movetime_ms,
    std::size_t threads,
    bool use_nnue,
    bool emit_ponder,
    std::ostream& out
) {
    u64 total_time_ms = 0;
    u64 total_nodes = 0;

    const int search_threads = std::clamp<int>(static_cast<int>(threads), 1, MAX_SEARCH_THREADS);
    out << "info string Using " << search_threads << " thread"
        << (search_threads == 1 ? "" : "s") << "\n\n";

    for (std::size_t i = 0; i < SEARCH_BENCH_FENS.size(); ++i) {
        Position bench_pos{};
        const std::string_view fen = SEARCH_BENCH_FENS[i];
        if (!parse_fen(bench_pos, mem, fen)) {
            out << "info string failed to parse bench FEN: " << fen << '\n';
            return false;
        }

        out << "Position: " << (i + 1) << '/' << SEARCH_BENCH_FENS.size()
            << " (" << fen << ")\n";

        search::SearchLimits limits{};
        limits.depth = search::MAX_SEARCH_DEPTH;
        limits.soft_time_ms = movetime_ms;
        limits.hard_time_ms = movetime_ms;
        limits.use_nnue = use_nnue;
        limits.thread_count = search_threads;
        limits.thread_id = 0;
        limits.report_info = true;
        limits.recover_ponder_pv = emit_ponder;

        const SearchBenchResult res = benchmark_search_position(
            bench_pos,
            mem,
            limits,
            &out
        );

        total_time_ms += res.time_ms;
        total_nodes += res.search.nodes;

        out << "bestmove " << search::move_to_uci(res.search.best_move);
        if (emit_ponder && !res.ponder.empty())
            out << " ponder " << res.ponder;
        out << "\n";
        if (i + 1 != SEARCH_BENCH_FENS.size())
            out << "\n";
    }

    render_search_bench_summary(out, total_time_ms, total_nodes);
    return true;
}

bool run_eval_bench(
    memory::Memory& mem,
    int iterations,
    std::ostream& out
) {
    std::array<Position, SEARCH_BENCH_FENS.size()> positions{};
    if (!prepare_eval_positions(mem, positions, out))
        return false;

    const std::string nnue_file = default_eval_file();
    const bool nnue_ok = ensure_nnue_loaded(nnue_file, &out);

    const std::string mnue_file{}; // use embedded
    const bool mnue_ok = ensure_mnue_p2_loaded(mnue_file, &out);
    const bool mnue_v2_ok = mnue::v2::loaded();

    if (!nnue_ok && !mnue_ok && !mnue_v2_ok) {
        out << "info string evalbench has no loaded evaluator\n";
        return false;
    }

    out << "evalbench\n";
    out << "  fens " << positions.size()
        << " iterations " << iterations
        << " evals " << (static_cast<std::size_t>(iterations) * positions.size())
        << '\n';

    if (mnue_ok) {
        int mismatches = 0;
        for (const Position& pos : positions) {
            mnue::P2AccumulatorStack stack{};
            stack.reset();
            const int fast = mnue::eval_p2(pos, stack);
            const int reference = mnue::debug_eval_p2_reference(pos);
            if (fast != reference)
                ++mismatches;
        }

        out << "  mnue_p2_w1_max_abs " << mnue::p2_w1_max_abs()
            << " i32_fast " << (mnue::p2_i32_forward_enabled() ? 1 : 0)
            << " reference_mismatches " << mismatches
            << '\n';

        if (mismatches != 0)
            return false;

        if (!run_p2_incremental_smoke(positions, mem, out))
            return false;
        if (!check_p2_generation_reload(positions[0], out))
            return false;
    }

    if (mnue_v2_ok) {
        int mismatches = 0;
        for (const Position& pos : positions) {
            mnue::v2::AccumulatorStack stack{};
            const int incremental =
                mnue::v2::evaluate_incremental(pos, mem, stack);
            const int reference =
                mnue::v2::evaluate_reference(pos, mem);
            if (incremental != reference)
                ++mismatches;
        }
        out << "  mnue_v2 backend " << mnue::v2::backend_name()
            << " network_bytes " << mnue::v2::network_bytes()
            << " stack_bytes " << mnue::v2::accumulator_stack_bytes()
            << " reference_mismatches " << mismatches
            << '\n';
        if (mismatches != 0)
            return false;
    }

    if (nnue_ok) {
        for (const Position& pos : positions)
            (void)nnue::eval(pos);

        const EvalBenchTiming nnue_timing = benchmark_eval_batch(
            positions,
            iterations,
            [](const Position& pos) noexcept { return nnue::eval(pos); }
        );
        render_eval_bench_timing(out, "nnue", nnue_timing);
    }

    if (mnue_ok) {
        const EvalBenchTiming mnue_fast_timing =
            benchmark_p2_stack_batch(positions, iterations);
        render_eval_bench_timing(out, "mnue_p2_lazy_stack", mnue_fast_timing);

        const EvalBenchTiming mnue_reference_timing = benchmark_eval_batch(
            positions,
            iterations,
            [](const Position& pos) noexcept { return mnue::debug_eval_p2_reference(pos); }
        );
        render_eval_bench_timing(out, "mnue_p2_reference", mnue_reference_timing);
    }

    if (mnue_v2_ok) {
        const EvalBenchTiming v2_fast_timing =
            benchmark_v2_stack_batch(positions, mem, iterations);
        render_eval_bench_timing(
            out,
            "mnue_v2_lazy_stack",
            v2_fast_timing
        );
        const EvalBenchTiming v2_reference_timing =
            benchmark_eval_batch(
                positions,
                iterations,
                [&](const Position& pos) noexcept {
                    return mnue::v2::evaluate_reference(pos, mem);
                }
            );
        render_eval_bench_timing(
            out,
            "mnue_v2_reference",
            v2_reference_timing
        );
    }

    return true;
}

/*
 * 基準測試實作
 * parse_config() — 解析命令列參數（perft/search/timed_search 模式）
 * benchmark_perft() — Perft 節點計數基準測試
 * run_search_bench() — 固定深度搜尋基準測試
 * run_timed_search_bench() — 定時搜尋基準測試（模擬真實對局）
 */
BenchConfig parse_config(int argc, char** argv) noexcept {
    BenchConfig cfg;
    int argi = 1;

    if (argc > 1 && std::string_view(argv[1]) == "divide") {
        cfg.divide = true;
        argi = 2;
    }
    else if (argc > 1 && std::string_view(argv[1]) == "perft") {
        argi = 2;
    }
    else if (argc > 1 && std::string_view(argv[1]) == "search") {
        cfg.search = true;
        argi = 2;
    }
    else if (argc > 1 && std::string_view(argv[1]) == "bench") {
        cfg.search = true;
        cfg.timed_search = false;
        cfg.search_depth = 12;
        cfg.hash_mb = 16;
        cfg.threads = 1;
        argi = 2;
    }
    else if (argc > 1 && std::string_view(argv[1]) == "evalbench") {
        cfg.evalbench = true;
        argi = 2;
    }
    else if (argc > 1 &&
             (std::string_view(argv[1]) == "-tt" ||
              std::string_view(argv[1]) == "--tt" ||
              std::string_view(argv[1]) == "tt")) {
        cfg.tt_bench = true;
        argi = 2;
    }

    auto parse_arg_int = [&](int idx, int default_val) noexcept {
        if (idx >= argc || !argv[idx] || !argv[idx][0])
            return default_val;
        int value = 0;
        std::string_view sv(argv[idx]);
        if (std::from_chars(sv.data(), sv.data() + sv.size(), value).ec == std::errc{})
            return value;
        return default_val;
    };
    auto parse_arg_u64 = [&](int idx, std::size_t default_val) noexcept -> std::size_t {
        if (idx >= argc || !argv[idx] || !argv[idx][0])
            return default_val;
        u64 value = 0;
        std::string_view sv(argv[idx]);
        if (std::from_chars(sv.data(), sv.data() + sv.size(), value).ec == std::errc{})
            return static_cast<std::size_t>(value);
        return default_val;
    };

    if (cfg.tt_bench) {
        for (int i = argi; i < argc; ++i) {
            const std::string_view option(argv[i]);
            if (option == "-hash" || option == "--hash") {
                if (++i >= argc) {
                    cfg.valid = false;
                    break;
                }
                const std::size_t parsed = parse_arg_u64(i, 0);
                if (parsed == 0) {
                    cfg.valid = false;
                    break;
                }
                cfg.hash_mb = parsed;
            } else if (option == "-entries" || option == "--entries") {
                if (++i >= argc) {
                    cfg.valid = false;
                    break;
                }
                cfg.tt_entries = static_cast<u64>(parse_arg_u64(i, 0));
                if (cfg.tt_entries == 0) {
                    cfg.valid = false;
                    break;
                }
            } else {
                cfg.valid = false;
                break;
            }
        }
        cfg.threads = 1;
    } else if (cfg.evalbench) {
        cfg.eval_iterations = std::max(1, parse_arg_int(argi, cfg.eval_iterations));
        cfg.hash_mb = parse_arg_u64(argi + 1, cfg.hash_mb);
        cfg.threads = 1;
    } else if (cfg.search) {
        if (cfg.timed_search)
            cfg.search_movetime_ms = std::max(1, parse_arg_int(argi, cfg.search_movetime_ms));
        else
            cfg.search_depth = std::max(1, parse_arg_int(argi, cfg.search_depth));
        cfg.hash_mb = parse_arg_u64(argi + 1, cfg.hash_mb);
        cfg.threads = parse_arg_u64(argi + 2, cfg.threads);
    } else {
        cfg.perft_depth = std::max(0, parse_arg_int(argi, cfg.perft_depth));
        cfg.hash_mb = parse_arg_u64(argi + 1, cfg.hash_mb);
        cfg.threads = parse_arg_u64(argi + 2, cfg.threads);

        bool backend_seen = false;
        bool live_seen = false;
        for (int i = argi + 3; i < argc; ++i) {
            const std::string_view value(argv[i]);
            const bool is_backend =
                value == "auto"
                || value == "pext"
                || value == "magic"
                || value == "table"
                || value == "classical";

            if (value == "live" && !live_seen) {
                cfg.live_divide = true;
                live_seen = true;
            } else if (is_backend && !backend_seen) {
                cfg.attack_backend = value;
                backend_seen = true;
            } else {
                cfg.valid = false;
            }
        }
    }
    if (cfg.hash_mb == 0) cfg.hash_mb = 1;
    if (cfg.threads == 0) cfg.threads = 1;

    return cfg;
}

int run_bench(int argc, char** argv) { 
    const BenchConfig cfg = parse_config(argc, argv);
    const bool compact_bench =
        argc > 1 && std::string_view(argv[1]) == "bench";
    bool use_nnue = false;

    if (!cfg.valid) {
        std::cerr
            << "usage: MagnusChess [perft] <depth> <hash_mb> <threads> "
               "[auto|pext|magic|table|classical]\n"
            << "       MagnusChess divide <depth> <hash_mb> <threads> "
               "[live] [auto|pext|magic|table|classical]\n"
            << "       MagnusChess bench [depth=12] [hash_mb=16] [threads=1]\n"
            << "       MagnusChess -tt -hash <mb> [-entries <count>]\n";
        return 1;
    }

    if (cfg.tt_bench)
        return run_tt_bench(cfg.hash_mb, cfg.tt_entries, std::cout) ? 0 : 1;

    memory::Memory mem{};
    memory_init(mem, cfg.hash_mb, 8, 2);
    attack_init_backend(mem);

    if (!cfg.search && !cfg.evalbench &&
        !attack_select_backend(cfg.attack_backend)) {
        std::cerr << "invalid attack backend: " << cfg.attack_backend << '\n';
        memory_free(mem);
        return 1;
    }

    Position pos{};
    set_start_position(pos);

    if (!position_has_valid_kings(pos) || !position_board_matches_bitboards(pos)) {
        std::cerr << "position bootstrap failed\n";
        memory_free(mem);
        return 1;
    }

    if (cfg.evalbench) {
        const bool ok = run_eval_bench(mem, cfg.eval_iterations, std::cout);
        memory_free(mem);
        return ok ? 0 : 1;
    }

    if (cfg.search) {
        if (cfg.timed_search || compact_bench) {
            const std::string mnue_file{}; // use embedded
            use_nnue = ensure_mnue_p2_loaded(mnue_file, &std::cout);

            if (!use_nnue) {
                const std::string eval_file = default_eval_file();
                use_nnue = ensure_nnue_loaded(eval_file, &std::cout);
            }

            if (!use_nnue)
                std::cout << "info string nnue unavailable, bench will use hce\n";
        }

        const bool ok = compact_bench
            ? run_compact_search_bench(
                mem,
                cfg.search_depth,
                cfg.hash_mb,
                cfg.threads,
                use_nnue,
                std::cout
            )
            : cfg.timed_search
                ? run_timed_search_bench(
                    mem,
                    cfg.search_movetime_ms,
                    cfg.threads,
                    use_nnue,
                    true,
                    std::cout
                )
                : run_search_bench(
                    mem,
                    cfg.search_depth,
                    cfg.threads,
                    false,
                    true,
                    std::cout
                );
        memory_free(mem);
        return ok ? 0 : 1;
    }

    if (cfg.divide) {
        divide(pos, mem, cfg.perft_depth, std::cout, cfg.threads, cfg.live_divide);
        memory_free(mem);
        return 0;
    }

    const PerftBenchResult perft_res = benchmark_perft(pos, mem, cfg.perft_depth, cfg.threads);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "perft\n";
    std::cout << "  depth               : " << perft_res.depth << "\n";
    std::cout << "  nodes               : " << perft_res.nodes << "\n";
    std::cout << "  time                : " << perft_res.seconds << " s\n";
    std::cout << "  nps                 : " << perft_res.nps << "\n";
    std::cout << "  hash mb             : " << cfg.hash_mb << "\n";
    std::cout << "  threads             : " << perft_res.threads << "\n";
    std::cout << "  attack backend      : " << attack_backend_name() << "\n";

    memory_free(mem);
    return 0;
}

} // namespace magnus
