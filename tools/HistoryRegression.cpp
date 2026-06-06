#include <iostream>

#include "../src/History.h"

namespace {

using namespace magnus;
using namespace magnus::search;

bool continuation_uses_snapshot() {
    Position pos{};
    for (Square sq = 0; sq < SQ_NB; ++sq)
        pos.board[sq] = PIECE_NONE;

    pos.side_to_move = WHITE;
    constexpr Square current_from = 1;
    constexpr Square current_to = 18;
    constexpr Square previous_to = 24;
    pos.board[current_from] = W_KNIGHT;

    const Move current_move = make_move(current_from, current_to);
    const ContinuationHistoryContext previous{ ROOK, previous_to };

    HistoryTables history{};
    history.bonus_continuation_fast(
        pos,
        previous,
        current_move,
        4,
        CONTINUATION_PLY2_SLOT
    );

    // The historical rook has since moved away and another piece now occupies
    // its destination. Lookup must still use the recorded move-time snapshot.
    pos.board[previous_to] = B_BISHOP;

    const int stored = history.continuation_value_fast(
        pos,
        current_move,
        previous,
        CONTINUATION_PLY2_SLOT
    );
    const int wrong_piece = history.continuation_value_fast(
        pos,
        current_move,
        ContinuationHistoryContext{ BISHOP, previous_to },
        CONTINUATION_PLY2_SLOT
    );

    return stored == history_bonus(4) && wrong_piece == 0;
}

bool history_update_has_gravity() {
    constexpr int bonus = 100;
    i16 rewarded = 16000;
    i16 penalized = 16000;

    update_history_value(rewarded, bonus);
    update_history_value(penalized, -bonus);

    const int reward_step = static_cast<int>(rewarded) - 16000;
    const int penalty_step = 16000 - static_cast<int>(penalized);
    return reward_step > 0
        && reward_step < bonus
        && penalty_step > bonus
        && rewarded <= HISTORY_VALUE_LIMIT
        && penalized >= -HISTORY_VALUE_LIMIT;
}

bool repeated_updates_do_not_saturate_early() {
    i16 value = 0;
    constexpr int update = 64;
    constexpr int repetitions = HISTORY_VALUE_LIMIT / update + 1;

    for (int i = 0; i < repetitions; ++i)
        update_history_value(value, update);

    const int before_reverse = static_cast<int>(value);
    update_history_value(value, -update);
    const int reverse_step = before_reverse - static_cast<int>(value);

    return before_reverse > 0
        && before_reverse < HISTORY_VALUE_LIMIT
        && reverse_step > update;
}

bool quiet_history_uses_gravity() {
    Position pos{};
    for (Square sq = 0; sq < SQ_NB; ++sq)
        pos.board[sq] = PIECE_NONE;

    pos.side_to_move = WHITE;
    constexpr Square from = 1;
    constexpr Square to = 18;
    pos.board[from] = W_KNIGHT;

    HistoryTables history{};
    history.quiet.value[WHITE][from][to] = 16000;

    i16 expected = 16000;
    update_history_value(expected, history_bonus(10));
    history.bonus_fast(pos, make_move(from, to), 10);

    return history.quiet.value[WHITE][from][to] == expected;
}

} // namespace

int main() {
    if (!continuation_uses_snapshot()) {
        std::cerr << "History continuation snapshot regression failed\n";
        return 1;
    }
    if (!history_update_has_gravity()) {
        std::cerr << "History gravity update regression failed\n";
        return 1;
    }
    if (!repeated_updates_do_not_saturate_early()) {
        std::cerr << "History early saturation regression failed\n";
        return 1;
    }
    if (!quiet_history_uses_gravity()) {
        std::cerr << "Quiet history integration regression failed\n";
        return 1;
    }

    std::cout << "History regressions passed\n";
    return 0;
}
