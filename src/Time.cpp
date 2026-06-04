/*
MIT License

Copyright (c) 2026 MagnusU0001F984

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

#include "Time.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Mnue.h"
#include "MoveGen.h"
#include "Nnue.h"

/* ===== 繁體中文註釋 =====
 * 本檔案是 MagnusChess 西洋棋引擎的一部分。
 * 實作詳情請參閱對應的 .h 標頭檔案。
 */

namespace magnus::timeman {

namespace {

constexpr int kMoveOverheadMs = 10;
constexpr double kTargetSingleThreadNps = 2'400'000.0;
constexpr u64 kMnueBootstrapNps = 1'100'000ULL;
constexpr u64 kNnueBootstrapNps = 1'700'000ULL;

[[nodiscard]] inline int time_left_overhead(int remaining, int centi_mtg) noexcept {
    const int current_move_overhead = kMoveOverheadMs * 2;
    const int future_move_overhead = (kMoveOverheadMs * centi_mtg) / 100;
    const int future_overhead_cap = std::max(kMoveOverheadMs, remaining / 10);
    return current_move_overhead + std::min(future_move_overhead, future_overhead_cap);
}

[[nodiscard]] inline int explicit_mtg_hard_cap(
    int remaining,
    int increment,
    int movestogo,
    int optimum
) noexcept {
    const int moves = std::max(1, movestogo);
    const i64 period_budget =
        static_cast<i64>(std::max(1, remaining))
        + static_cast<i64>(std::max(0, increment)) * std::max(0, moves - 1);
    const int per_move_budget =
        std::max(1, static_cast<int>(period_budget / moves));
    return std::max(std::max(1, optimum), per_move_budget * 2);
}

[[nodiscard]] inline int game_ply_from_position(const Position& pos) noexcept {
    const int fullmove = std::max(1, pos.fullmove_number);
    return std::max(
        0,
        (fullmove - 1) * 2 + (pos.side_to_move == BLACK ? 1 : 0)
    );
}

[[nodiscard]] inline double safe_log10(double value) noexcept {
    return std::log10(std::max(1.0, value));
}

} // namespace

/*
 * 時間管理實作
 * build_limits() — 從 UCI go 參數計算軟/硬時間限制
 * record_search() — 記錄搜尋結果用於歷史自適應
 * collect_stats() — 從歷史計算平均時間使用率/分數擺動/著法變更率
 */
void TimeManager::new_game() noexcept {
    history_size_ = 0;
    next_index_ = 0;
    original_time_adjust_ = -1.0;
}

std::size_t TimeManager::history_size() const noexcept {
    return history_size_;
}

bool TimeManager::build_limits(
    const Position& pos,
    const GoParams& params,
    search::SearchLimits& limits
) noexcept {
    limits.depth = search::MAX_PLY;
    limits.node_limit = 0;
    limits.soft_time_ms = 0;
    limits.hard_time_ms = 0;
    limits.ponder = params.ponder;
    limits.infinite = false;
    limits.use_time_management = false;

    if (params.depth > 0)
        limits.depth = params.depth;

    limits.node_limit = params.nodes;
    limits.infinite = params.infinite;

    if (params.movetime > 0) {
        limits.soft_time_ms = params.movetime;
        limits.hard_time_ms = params.movetime;
        return true;
    }

    const Color side = static_cast<Color>(pos.side_to_move);
    const int remaining = side == WHITE ? params.wtime : params.btime;
    const int increment = side == WHITE ? params.winc : params.binc;

    if (!limits.infinite && remaining > 0) {
        limits.use_time_management = true;
        const int ply = game_ply_from_position(pos);

        int centi_mtg =
            params.movestogo > 0 ? std::min(params.movestogo * 100, 5000) : 5051;
        if (remaining < 1000)
            centi_mtg = std::max(1, static_cast<int>(remaining * 5.051));

        const int time_left = std::max(
            1,
            remaining
                + (increment * (centi_mtg - 100)) / 100
                - time_left_overhead(remaining, centi_mtg)
        );

        double opt_scale = 1.0;
        double max_scale = 1.0;

        if (params.movestogo == 0) {
            if (original_time_adjust_ < 0.0)
                original_time_adjust_ = 0.3128 * safe_log10(double(time_left)) - 0.4354;

            const double log_time_in_sec =
                safe_log10(std::max(1.0, double(remaining)) / 1000.0);
            const double opt_constant =
                std::min(0.0032116 + 0.000321123 * log_time_in_sec, 0.00508017);
            const double max_constant =
                std::max(3.3977 + 3.03950 * log_time_in_sec, 2.94761);

            opt_scale =
                std::min(
                    0.0121431
                        + std::pow(double(ply) + 2.94693, 0.461073) * opt_constant,
                    0.213035 * double(remaining) / double(time_left)
                )
                * original_time_adjust_;
            max_scale = std::min(6.67704, max_constant + double(ply) / 11.9847);
        } else {
            const double mtg = double(centi_mtg) / 100.0;
            opt_scale = std::min(
                0.88 / mtg,
                0.88 * double(remaining) / double(time_left)
            );
            max_scale = 1.3 + 0.11 * mtg;
        }

        int optimum = std::max(1, static_cast<int>(opt_scale * double(time_left)));
        const int maximum_cap = std::max(
            1,
            static_cast<int>(0.825179 * double(remaining) - kMoveOverheadMs)
        );
        const int scaled_maximum = std::max(
            std::max(1, remaining / 4),
            static_cast<int>(max_scale * double(std::max(1, optimum)))
        );
        int maximum = std::max(1, std::min(maximum_cap, scaled_maximum));

        const HistoryStats stats = collect_stats(side);
        u64 effective_nps = stats.avg_nps;
        if (effective_nps == 0) {
            if (limits.use_nnue && mnue::p2_loaded())
                effective_nps = kMnueBootstrapNps;
            else if (limits.use_nnue && nnue::loaded())
                effective_nps = kNnueBootstrapNps;
        }

        double nps_scale = 1.0;
        if (effective_nps > 0) {
            const double ratio = kTargetSingleThreadNps / double(effective_nps);
            nps_scale = std::clamp(
                std::pow(std::clamp(ratio, 0.35, 4.0), 0.55),
                0.72,
                1.85
            );
        }

        double instability_scale = 1.0;
        if (stats.samples >= 3) {
            instability_scale += std::clamp(
                (double(stats.best_move_flip_pct) - 35.0) / 220.0,
                0.0,
                0.25
            );
            instability_scale += std::clamp(
                (double(stats.avg_score_swing_cp) - 25.0) / 320.0,
                0.0,
                0.20
            );
        }

        double node_floor_scale = 1.0;
        if (stats.avg_nodes > 0 && effective_nps > 0) {
            const double expected_nodes =
                double(effective_nps) * double(std::max(1, optimum)) / 1000.0;
            const double target_nodes =
                double(stats.avg_nodes) * (stats.avg_depth < 10 ? 1.10 : 0.95);
            if (expected_nodes > 1.0 && target_nodes > expected_nodes) {
                node_floor_scale = std::clamp(
                    std::sqrt(target_nodes / expected_nodes),
                    1.0,
                    1.30
                );
            }
        }

        const double soft_scale = std::clamp(
            nps_scale * instability_scale * node_floor_scale,
            0.70,
            2.10
        );
        const double hard_scale = std::clamp(
            1.0 + (soft_scale - 1.0) * 0.65,
            0.85,
            1.65
        );

        const auto scale_ms = [](int value, double scale) noexcept {
            const double scaled = std::clamp(
                double(std::max(1, value)) * scale,
                1.0,
                double(std::numeric_limits<int>::max())
            );
            return static_cast<int>(std::llround(scaled));
        };

        optimum = scale_ms(optimum, soft_scale);
        maximum = std::min(maximum_cap, scale_ms(maximum, hard_scale));

        if (params.movestogo > 0)
            maximum = std::min(
                maximum,
                explicit_mtg_hard_cap(
                    remaining,
                    increment,
                    params.movestogo,
                    optimum
                )
            );

        if (params.ponder)
            optimum += optimum / 4;
        else
            optimum = std::min(optimum, maximum);

        limits.soft_time_ms = optimum;
        limits.hard_time_ms = maximum;
    }

    if (!limits.infinite &&
        limits.depth == search::MAX_PLY &&
        limits.node_limit == 0 &&
        limits.soft_time_ms == 0 &&
        limits.hard_time_ms == 0) {
        return false;
    }

    return true;
}

void TimeManager::record_search(
    const Position& root,
    const search::SearchLimits& limits,
    const search::SearchResult& result,
    int elapsed_ms
) noexcept {
    // Keep collecting simple search history even though the current budget
    // model is Stockfish-style and does not directly feed on these samples.
    if (limits.soft_time_ms <= 0 && limits.hard_time_ms <= 0)
        return;

    SearchRecord record{};
    record.side = static_cast<Color>(root.side_to_move);
    record.fullmove_number = std::max(1, root.fullmove_number);
    record.soft_time_ms = std::max(0, limits.soft_time_ms);
    record.hard_time_ms = std::max(0, limits.hard_time_ms);
    record.elapsed_ms = std::max(0, elapsed_ms);
    record.nodes = result.nodes;
    if (elapsed_ms > 0 && result.nodes > 0)
        record.nps = (result.nodes * 1000ULL) / static_cast<u64>(elapsed_ms);
    record.depth = std::max(0, result.depth);
    if (limits.use_nnue && mnue::p2_loaded())
        record.score_cp = mnue::search_score_to_cp(result.score, root);
    else if (limits.use_nnue && nnue::loaded())
        record.score_cp = nnue::search_score_to_cp(result.score, root);
    else
        record.score_cp = result.score;
    record.best_move = result.best_move;

    push_record(record);
}

void TimeManager::push_record(const SearchRecord& record) noexcept {
    history_[next_index_] = record;
    next_index_ = (next_index_ + 1) % MAX_HISTORY;
    if (history_size_ < MAX_HISTORY)
        ++history_size_;
}

const TimeManager::SearchRecord& TimeManager::recent_record(
    std::size_t offset
) const noexcept {
    const std::size_t index =
        (next_index_ + MAX_HISTORY - 1 - (offset % MAX_HISTORY)) % MAX_HISTORY;
    return history_[index];
}

TimeManager::HistoryStats TimeManager::collect_stats(Color side) const noexcept {
    HistoryStats stats{};
    if (history_size_ == 0)
        return stats;

    i64 usage_sum = 0;
    int usage_count = 0;
    i64 swing_sum = 0;
    int swing_count = 0;
    int move_flip_count = 0;
    int move_transition_count = 0;
    u64 node_sum = 0;
    int node_count = 0;
    int depth_sum = 0;
    int depth_count = 0;
    u64 nps_weighted_sum = 0;
    int nps_weight_sum = 0;

    int prev_score = 0;
    Move prev_move = 0;
    bool has_prev = false;

    constexpr std::size_t SAMPLE_CAP = 12;
    for (std::size_t i = 0; i < history_size_; ++i) {
        if (static_cast<std::size_t>(stats.samples) >= SAMPLE_CAP)
            break;

        const SearchRecord& rec = recent_record(i);
        if (rec.side != side)
            continue;

        ++stats.samples;
        const int recency_weight = static_cast<int>(
            SAMPLE_CAP - std::min<std::size_t>(
                static_cast<std::size_t>(stats.samples - 1),
                SAMPLE_CAP - 1
            )
        );

        if (rec.nodes > 0) {
            node_sum += rec.nodes;
            ++node_count;
        }

        if (rec.depth > 0) {
            depth_sum += rec.depth;
            ++depth_count;
        }

        if (rec.nps > 0) {
            nps_weighted_sum += rec.nps * static_cast<u64>(recency_weight);
            nps_weight_sum += recency_weight;
            ++stats.nps_samples;
        }

        if (rec.soft_time_ms > 0 && rec.elapsed_ms > 0) {
            usage_sum += (static_cast<i64>(rec.elapsed_ms) * 100) / rec.soft_time_ms;
            ++usage_count;
        }

        if (has_prev) {
            const int score_diff =
                rec.score_cp >= prev_score
                    ? rec.score_cp - prev_score
                    : prev_score - rec.score_cp;
            swing_sum += score_diff;
            ++swing_count;

            if (!move_is_none(rec.best_move) && !move_is_none(prev_move)) {
                if (rec.best_move != prev_move)
                    ++move_flip_count;
                ++move_transition_count;
            }
        }

        prev_score = rec.score_cp;
        prev_move = rec.best_move;
        has_prev = true;
    }

    if (usage_count > 0)
        stats.avg_usage_pct = static_cast<int>(usage_sum / usage_count);
    if (node_count > 0)
        stats.avg_nodes = node_sum / static_cast<u64>(node_count);
    if (depth_count > 0)
        stats.avg_depth = depth_sum / depth_count;
    if (nps_weight_sum > 0)
        stats.avg_nps = nps_weighted_sum / static_cast<u64>(nps_weight_sum);
    if (swing_count > 0)
        stats.avg_score_swing_cp = static_cast<int>(swing_sum / swing_count);
    if (move_transition_count > 0)
        stats.best_move_flip_pct = (move_flip_count * 100) / move_transition_count;

    return stats;
}

} // namespace magnus::timeman
