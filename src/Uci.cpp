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

#include "Uci.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <charconv>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Attack.h"
#include "Bench.h"
#include "Common.h"
#include "Evaluation.h"
#include "Memory.h"
#include "Mnue.h"
#include "Nnue.h"
#include "Search.h"
#include "Syzygy.h"
#include "Time.h"

#ifdef _WIN32
#include <windows.h>
#endif

/*
 * MagnusChess UCI 前端 — Universal Chess Interface 實作
 *
 * 完整的 UCI 協定實作，包含以下功能模組：
 *
 * 1. UCI 命令解析與派發
 *    - uci / isready / ucinewgame / quit — 標準 UCI 握手
 *    - position [startpos|fen ...] [moves ...] — 局面設定
 *    - go [depth|movetime|wtime|btime|...] — 搜尋控制
 *    - stop / ponderhit — 非同步搜尋控制
 *    - setoption name [Hash|Threads|EvalFile|...] value — 引擎選項
 *
 * 2. FEN 解析 (parse_fen)
 *    - 驗證棋盤結構、走子方、易位權、過路兵
 *    - 通過標準 piece-placement API 建構局面
 *
 * 3. 搜尋線程管理 (UciSession)
 *    - 合作式停止 (stop_requested 原子變量)
 *    - 沉思模式支援 (ponder / ponderhit 時間追蹤)
 *    - 背景 NNUE 預加載
 *
 * 4. PvTrackingStreamBuf — 自訂 streambuf
 *    - 攔截搜尋輸出的 "info ... pv ..." 行
 *    - 提取 PV 用於沉思著法計算
 */

namespace magnus {

namespace {

constexpr int DEFAULT_UCI_DEPTH = 8;
constexpr int DEFAULT_UCI_HASH_MB = 16;
constexpr int DEFAULT_UCI_THREADS = 1;
constexpr int MAX_UCI_THREADS = 512;
constexpr int DEFAULT_UCI_MULTIPV = 1;
constexpr int DEFAULT_UCI_CONTEMPT = 0;
constexpr int MIN_UCI_CONTEMPT = -10000;
constexpr int MAX_UCI_CONTEMPT = 10000;

static std::string default_mnue_p2_file() {
    return "b7a644d5d.MNUE";
}

[[nodiscard]] const std::string& get_executable_dir() {
    static const std::string dir = []() -> std::string {
#ifdef _WIN32
        char buf[MAX_PATH];
        const DWORD len = GetModuleFileNameA(nullptr, buf, sizeof(buf));
        if (len > 0 && len < sizeof(buf))
            return std::filesystem::path(buf).parent_path().string();
#endif
        return std::filesystem::current_path().string();
    }();
    return dir;
}

[[nodiscard]] std::string resolve_file_path(const std::string& filename) {
    std::error_code ec;

    // 1. Absolute path or CWD-relative — if it already exists, use as-is.
    if (std::filesystem::exists(filename, ec) && !ec)
        return filename;

    // 2. Relative to executable directory.
    const std::string& exe_dir = get_executable_dir();
    for (const char* subdir : {"", "/bin/", "/../", "/../../", "/../bin/", "/../../bin/"}) {
        const std::string candidate = exe_dir + subdir + filename;
        if (std::filesystem::exists(candidate, ec) && !ec)
            return candidate;
    }

    // 3. Give up — fall back to the original filename so the error message
    //    still shows what was requested.
    return filename;
}

[[nodiscard]] bool has_mnue_extension(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(
        ext.begin(),
        ext.end(),
        ext.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );
    return ext == ".mnue";
}

struct PositionHistory {
    Key keys[search::MAX_GAME_HISTORY]{};
    int count = 0;
    int head = 0; // circular-buffer write cursor
};

inline void clear_position_history(PositionHistory& history) noexcept {
    history.count = 0;
    history.head = 0;
}

inline void push_position_history(
    PositionHistory& history,
    Key key
) noexcept {
    if (history.count < search::MAX_GAME_HISTORY) {
        history.keys[history.count] = key;
        ++history.count;
        return;
    }
    // Circular overwrite: O(1) instead of O(n) shift.
    history.keys[history.head] = key;
    history.head = (history.head + 1) % search::MAX_GAME_HISTORY;
}

[[nodiscard]] constexpr const char* go_usage_hint() noexcept {
    return "go <depth/movetime/nodes/ponder>";
}

[[nodiscard]] constexpr const char* go_usage_examples() noexcept {
    return "examples: go depth 8 | go movetime 1000 | go nodes 50000 | go wtime 15000 btime 15000 | go ponder wtime 15000 btime 15000";
}

[[nodiscard]] constexpr const char* perft_usage_hint() noexcept {
    return "perft <depth> <threads>";
}

[[nodiscard]] constexpr const char* divide_usage_hint() noexcept {
    return "divide <depth> <threads> [live]";
}

[[nodiscard]] constexpr const char* bench_usage_hint() noexcept {
    return "bench";
}

[[nodiscard]] constexpr const char* sort_usage_hint() noexcept {
    return "sort [depth] [threads] | sort depth N threads M";
}

constexpr const char* kAnsiReset = "\x1b[0m";
constexpr const char* kAnsiRed = "\x1b[31m";
constexpr const char* kAnsiGreen = "\x1b[32m";
constexpr const char* kAnsiYellow = "\x1b[33m";
constexpr const char* kAnsiBlue = "\x1b[34m";
constexpr const char* kAnsiMagenta = "\x1b[35m";
constexpr const char* kAnsiCyan = "\x1b[36m";
constexpr const char* kAnsiWhite = "\x1b[37m";
constexpr const char* kAnsiBold = "\x1b[1m";

[[nodiscard]] std::string debug_separator() {
    return std::string(88, '-');
}

[[nodiscard]] std::string color_text(std::string_view text, const char* color) {
    std::string result{color};
    result.append(text.data(), text.size());
    result += kAnsiReset;
    return result;
}

[[nodiscard]] std::string debug_marker(bool highlight) {
    return std::string(highlight ? kAnsiMagenta : kAnsiGreen)
        + (highlight ? "[*]" : "[-]")
        + kAnsiReset;
}

[[nodiscard]] const char* score_color(int score) noexcept {
    if (score > 0)
        return kAnsiGreen;
    if (score < 0)
        return kAnsiRed;
    return kAnsiYellow;
}

[[nodiscard]] const char* move_color(Move move) noexcept {
    if (move_is_promotion(move))
        return kAnsiMagenta;
    if (move_is_capture(move))
        return kAnsiYellow;
    if (move_is_castle(move))
        return kAnsiCyan;
    return kAnsiWhite;
}

[[nodiscard]] std::string format_u64_with_commas(u64 value) {
    std::string digits = std::to_string(value);
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<std::size_t>(i), ",");
    return digits;
}

[[nodiscard]] std::string format_fixed_with_commas(double value, int decimals) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(decimals) << value;
    std::string result = oss.str();

    std::size_t dot = result.find('.');
    if (dot == std::string::npos)
        dot = result.size();

    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(dot) - 3; i > 0; i -= 3)
        result.insert(static_cast<std::size_t>(i), ",");

    return result;
}

[[nodiscard]] std::string format_seconds(double seconds) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << seconds << " s";
    return oss.str();
}

[[nodiscard]] std::string format_knps(u64 nodes, double seconds) {
    const double knps = seconds > 0.0 ? (static_cast<double>(nodes) / seconds) / 1000.0 : 0.0;
    return format_fixed_with_commas(knps, 1) + " kN/s";
}

[[nodiscard]] std::string progress_bar(int completed, int total, int width = 28) {
    completed = std::clamp(completed, 0, std::max(0, total));
    const int filled = total > 0 ? (completed * width) / total : width;

    std::string result;
    result.reserve(static_cast<std::size_t>(width) + 32);
    result.push_back('[');
    result += kAnsiGreen;
    result.append(static_cast<std::size_t>(filled), '#');
    result += kAnsiReset;
    result.append(static_cast<std::size_t>(width - filled), '-');
    result.push_back(']');
    return result;
}

void render_sort_progress(
    std::ostream& out,
    int completed,
    int total,
    std::string_view move,
    std::string_view score,
    double elapsed_seconds,
    bool searching
) {
    const int percent = total > 0 ? (completed * 100) / total : 100;

    out << '\r' << "\x1b[2K"
        << " " << debug_marker(false) << ' '
        << (searching ? "⏳" : "✅") << ' '
        << progress_bar(completed, total)
        << ' ' << std::setw(3) << std::right << percent << "%  "
        << std::setw(3) << std::right << completed << '/' << std::setw(3) << std::left << total
        << "  move " << color_text(move, kAnsiWhite);

    if (!score.empty())
        out << "  score " << score;

    out << "  elapsed " << format_seconds(elapsed_seconds)
        << std::flush;
}

[[nodiscard]] const char* side_name(Color color) noexcept {
    return color == WHITE ? "white" : "black";
}

[[nodiscard]] char display_piece_char(Piece pc) noexcept {
    switch (pc) {
        case W_PAWN:   return 'P';
        case W_KNIGHT: return 'N';
        case W_BISHOP: return 'B';
        case W_ROOK:   return 'R';
        case W_QUEEN:  return 'Q';
        case W_KING:   return 'K';
        case B_PAWN:   return 'p';
        case B_KNIGHT: return 'n';
        case B_BISHOP: return 'b';
        case B_ROOK:   return 'r';
        case B_QUEEN:  return 'q';
        case B_KING:   return 'k';
        default:       return '.';
    }
}

[[nodiscard]] std::string display_square(Square sq) {
    if (!is_ok(sq))
        return "-";

    std::string result;
    result.push_back(static_cast<char>('a' + file_of(sq)));
    result.push_back(static_cast<char>('1' + rank_of(sq)));
    return result;
}

[[nodiscard]] std::string display_castling_rights(int rights) {
    std::string result;
    if ((rights & WHITE_OO) != 0)
        result.push_back('K');
    if ((rights & WHITE_OOO) != 0)
        result.push_back('Q');
    if ((rights & BLACK_OO) != 0)
        result.push_back('k');
    if ((rights & BLACK_OOO) != 0)
        result.push_back('q');
    return result.empty() ? "-" : result;
}

[[nodiscard]] std::string display_fen(const Position& pos) {
    std::string fen;

    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            const Square sq = rank * 8 + file;
            const Piece pc = piece_on(pos, sq);
            if (pc == PIECE_NONE) {
                ++empty;
                continue;
            }

            if (empty != 0) {
                fen.push_back(static_cast<char>('0' + empty));
                empty = 0;
            }
            fen.push_back(display_piece_char(pc));
        }

        if (empty != 0)
            fen.push_back(static_cast<char>('0' + empty));
        if (rank != 0)
            fen.push_back('/');
    }

    fen += pos.side_to_move == WHITE ? " w " : " b ";
    fen += display_castling_rights(pos.castling_rights);
    fen.push_back(' ');
    fen += display_square(pos.ep_sq);
    fen.push_back(' ');
    fen += std::to_string(pos.halfmove_clock);
    fen.push_back(' ');
    fen += std::to_string(pos.fullmove_number);
    return fen;
}

[[nodiscard]] std::string display_key_hex(Key key) {
    std::ostringstream oss;
    oss << "0x"
        << std::uppercase
        << std::hex
        << std::setw(16)
        << std::setfill('0')
        << key;
    return oss.str();
}

void display_position_snapshot(
    const Position& pos,
    std::ostream& out,
    std::string_view changed = {}
) {
    if (!changed.empty())
        out << "changed -> " << changed << '\n';
    out << "pos -> " << display_fen(pos) << '\n';
    out << "hash -> " << pos.key << " (" << display_key_hex(pos.key) << ")\n";
}

void display_position(const Position& pos, std::ostream& out) {
    out << '\n';
    for (int rank = 7; rank >= 0; --rank) {
        out << "  +---+---+---+---+---+---+---+---+\n";
        out << (rank + 1) << " |";
        for (int file = 0; file < 8; ++file) {
            const Square sq = rank * 8 + file;
            out << ' ' << display_piece_char(piece_on(pos, sq)) << " |";
        }
        out << '\n';
    }

    out << "  +---+---+---+---+---+---+---+---+\n";
    out << "    a   b   c   d   e   f   g   h\n";
    out << "Fen: " << display_fen(pos) << '\n';
}

enum class DisplayScoreModel {
    Hce,
    Nnue,
    Mnue
};

[[nodiscard]] std::string display_score_text(
    const Position& root,
    int score,
    DisplayScoreModel score_model
) {
    constexpr int DISPLAY_VALUE_MATE = 31000;

    if (score >= DISPLAY_VALUE_MATE - search::MAX_PLY) {
        const int plies_to_mate = DISPLAY_VALUE_MATE - score;
        return "mate " + std::to_string((plies_to_mate + 1) / 2);
    }

    if (score <= -DISPLAY_VALUE_MATE + search::MAX_PLY) {
        const int plies_to_mate = DISPLAY_VALUE_MATE + score;
        return "mate -" + std::to_string((plies_to_mate + 1) / 2);
    }

    int cp = score;
    if (score_model == DisplayScoreModel::Mnue)
        cp = mnue::search_score_to_cp(score, root);
    else if (score_model == DisplayScoreModel::Nnue)
        cp = nnue::search_score_to_cp(score, root);
    return "cp " + std::to_string(cp);
}

[[nodiscard]] std::string display_move_kind(
    const Position& pos,
    const memory::Memory& mem,
    Move move
) {
    std::string kind;
    if (move_is_castle(move)) {
        kind = "🏰 castle";
    } else if (move_is_ep(move)) {
        kind = "⚔ ep";
    } else if (move_is_promotion(move)) {
        kind = move_is_capture(move) ? "✨ cap-promo" : "✨ promo";
    } else if (move_is_capture(move)) {
        kind = "⚔ capture";
    } else if (move_is_double_push(move)) {
        kind = "⇈ double";
    } else {
        kind = "· quiet";
    }

    if (move_gives_check(pos, mem, move))
        kind += " 🎯 check";
    return kind;
}

[[nodiscard]] bool parse_sort_command(
    std::string_view command,
    int& depth,
    int& thread_count
) noexcept {
    std::istringstream iss{std::string(command)};
    std::string token;

    depth = DEFAULT_UCI_DEPTH;
    thread_count = std::clamp(thread_count, 1, MAX_UCI_THREADS);
    iss >> token; // sort

    bool have_positional_depth = false;
    bool have_positional_threads = false;

    while (iss >> token) {
        if (token == "depth") {
            std::string value;
            if (!(iss >> value) || !parse_int(value, depth))
                return false;
            have_positional_depth = true;
            continue;
        }

        if (token == "threads" || token == "thread") {
            std::string value;
            if (!(iss >> value) || !parse_int(value, thread_count))
                return false;
            have_positional_threads = true;
            continue;
        }

        int parsed = 0;
        if (!parse_int(token, parsed))
            return false;

        if (!have_positional_depth) {
            depth = parsed;
            have_positional_depth = true;
        } else if (!have_positional_threads) {
            thread_count = parsed;
            have_positional_threads = true;
        } else {
            return false;
        }
    }

    return depth > 0 &&
           depth <= search::MAX_PLY &&
           thread_count > 0 &&
           thread_count <= MAX_UCI_THREADS;
}

[[nodiscard]] std::string_view trim_ascii(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::string_view arrow_arguments(std::string_view args) noexcept {
    args = trim_ascii(args);
    if (args.size() >= 2 && args[0] == '-' && args[1] == '>')
        args.remove_prefix(2);
    return trim_ascii(args);
}

[[nodiscard]] bool command_starts_with(
    std::string_view line,
    std::string_view command
) noexcept {
    return line == command ||
           (line.size() > command.size() &&
            line.substr(0, command.size()) == command &&
            line[command.size()] == ' ');
}

[[nodiscard]] std::string_view command_arguments(
    std::string_view line,
    std::string_view command
) noexcept {
    return line.size() > command.size() ? line.substr(command.size() + 1) : std::string_view{};
}

[[nodiscard]] inline long long steady_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

[[nodiscard]] bool parse_u64(std::string_view sv, u64& value) noexcept {
    const char* first = sv.data();
    const char* last = sv.data() + sv.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    return ec == std::errc{} && ptr == last;
}

[[nodiscard]] bool parse_bool(std::string_view sv, bool& value) noexcept {
    if (sv == "true" || sv == "1") {
        value = true;
        return true;
    }

    if (sv == "false" || sv == "0") {
        value = false;
        return true;
    }

    return false;
}

[[nodiscard]] bool apply_move_list(
    Position& pos,
    const memory::Memory& mem,
    std::istringstream& iss,
    PositionHistory& history
) noexcept {
    std::string move_token;
    while (iss >> move_token) {
        Move move = 0;
        if (!find_uci_move(pos, mem, move_token, move))
            return false;

        const Key prev_key = pos.key;
        do_move_copy(pos, move, mem.tables);
        if (pos.halfmove_clock == 0)
            clear_position_history(history);
        else
            push_position_history(history, prev_key);
    }

    return true;
}

[[nodiscard]] bool set_position_from_command(
    Position& pos,
    const memory::Memory& mem,
    std::string_view command,
    PositionHistory& history
) noexcept {
    std::istringstream iss{std::string(command)};
    std::string token;

    if (!(iss >> token))
        return false;

    if (token == "startpos") {
        set_start_position(pos);
        position_refresh_key(pos, mem.tables);
    } else if (token == "fen") {
        std::string fen;
        std::string part;

        for (int i = 0; i < 6; ++i) {
            if (!(iss >> part))
                return false;

            if (i != 0)
                fen.push_back(' ');
            fen += part;
        }

        if (!parse_fen(pos, mem, fen))
            return false;
    } else {
        return false;
    }

    clear_position_history(history);

    if (!(iss >> token))
        return true;

    if (token != "moves")
        return false;

    return apply_move_list(pos, mem, iss, history);
}


[[nodiscard]] const char* active_eval_name(bool use_nnue) noexcept {
    if (!use_nnue)
        return "hce";
    if (mnue::p2_loaded())
        return "mnue-p2";
    if (nnue::loaded())
        return "nnue";
    return "hce";
}


[[nodiscard]] inline int white_pov_score(const Position& pos, int stm_score) noexcept {
    return pos.side_to_move == WHITE ? stm_score : -stm_score;
}

[[nodiscard]] inline int white_pov_winrate(const Position& pos, int stm_wr) noexcept {
    return pos.side_to_move == WHITE ? stm_wr : (1000 - stm_wr);
}

[[nodiscard]] inline nnue::WdlTriplet white_pov_wdl(
    const Position& pos,
    nnue::WdlTriplet stm_wdl
) noexcept {
    if (pos.side_to_move == WHITE)
        return stm_wdl;

    return {
        .win = stm_wdl.loss,
        .draw = stm_wdl.draw,
        .loss = stm_wdl.win
    };
}
[[nodiscard]] bool parse_go_command(
    const Position& pos,
    const memory::Memory& mem,
    timeman::TimeManager& time_manager,
    std::string_view command,
    search::SearchLimits& limits
) noexcept {
    // Convert the UCI go command into normalized parameters, then let the time
    // manager derive final soft/hard budgets (with historical adjustment).
    std::istringstream iss{std::string(command)};
    std::string token;

    timeman::GoParams params{};
    bool has_limit = false;
    limits.root_move_count = 0;

    iss >> token; // go

    const auto parse_next_int = [&](int& value, bool require_positive = false) noexcept {
        std::string raw;
        return (iss >> raw) &&
               parse_int(raw, value) &&
               (!require_positive || value > 0);
    };

    const auto parse_next_u64 = [&](u64& value, bool require_positive = false) noexcept {
        std::string raw;
        return (iss >> raw) &&
               parse_u64(raw, value) &&
               (!require_positive || value > 0);
    };

    while (iss >> token) {
        if (token == "depth") {
            if (!parse_next_int(params.depth, true))
                return false;
            has_limit = true;
        }
        else if (token == "nodes") {
            if (!parse_next_u64(params.nodes, true))
                return false;
            has_limit = true;
        }
        else if (token == "movetime") {
            if (!parse_next_int(params.movetime, true))
                return false;
            has_limit = true;
        }
        else if (token == "wtime") {
            if (!parse_next_int(params.wtime))
                return false;
            has_limit = true;
        }
        else if (token == "btime") {
            if (!parse_next_int(params.btime))
                return false;
            has_limit = true;
        }
        else if (token == "winc") {
            if (!parse_next_int(params.winc))
                return false;
            has_limit = true;
        }
        else if (token == "binc") {
            if (!parse_next_int(params.binc))
                return false;
            has_limit = true;
        }
        else if (token == "movestogo") {
            if (!parse_next_int(params.movestogo))
                return false;
            has_limit = true;
        }
        else if (token == "ponder") {
            params.ponder = true;
        }
        else if (token == "infinite") {
            params.infinite = true;
            has_limit = true;
        }
        else if (token == "searchmoves") {
            std::string move_token;
            while (iss >> move_token) {
                Move move = 0;
                if (!find_uci_move(pos, mem, move_token, move))
                    return false;

                bool duplicate = false;
                for (int i = 0; i < limits.root_move_count; ++i) {
                    if (limits.root_moves[i] == move) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                    continue;

                if (limits.root_move_count >= 256)
                    return false;
                limits.root_moves[limits.root_move_count++] = move;
            }

            if (limits.root_move_count == 0)
                return false;
            break;
        }
    }

    if (!has_limit)
        return false;

    return time_manager.build_limits(pos, params, limits);
}

[[nodiscard]] bool parse_divide_command(
    std::string_view command,
    int& depth,
    std::size_t& threads,
    bool& live
) noexcept {
    std::istringstream iss{std::string(command)};
    std::string token;
    std::string value;

    depth = -1;
    threads = 0;
    live = false;

    iss >> token; // divide

    const auto parse_next_int = [&](int& parsed, bool require_positive) noexcept {
        return (iss >> value) &&
               parse_int(value, parsed) &&
               (!require_positive || parsed > 0);
    };

    if (!parse_next_int(depth, false) || depth < 0)
        return false;

    int parsed_threads = 0;
    if (!parse_next_int(parsed_threads, true))
        return false;

    threads = static_cast<std::size_t>(parsed_threads);

    if (iss >> value) {
        if (value == "live")
            live = true;
        else
            return false;
    }

    return true;
}

struct UciSession {
    memory::Memory mem{};
    Position pos{};
    PositionHistory position_history{};
    timeman::TimeManager time_manager{};
    bool use_nnue = true;
    bool enable_ponder = true;
    bool singular_telemetry = false;
    int threads = DEFAULT_UCI_THREADS;
    int contempt = DEFAULT_UCI_CONTEMPT;
    int syzygy_probe_depth = syzygy::DEFAULT_PROBE_DEPTH;
    int syzygy_probe_limit = syzygy::DEFAULT_PROBE_LIMIT;
    bool syzygy_50_move_rule = true;
    std::string eval_file = default_eval_file();
    std::string eval_file_p2 = default_mnue_p2_file();
    std::string syzygy_path{};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> search_running{false};
    std::atomic<bool> ponder_search{false};
    std::atomic<bool> pondering{false};
    std::atomic<bool> ponder_hit_received{false};
    std::atomic<int> ponder_time_offset_ms{0};
    std::atomic<long long> search_start_ms{0};
    std::thread search_thread;

    UciSession() {
        memory::memory_init(mem, DEFAULT_UCI_HASH_MB, 8, 2);
        // attack_init_backend deferred to first command that needs it.
        set_start_position(pos);
        position_refresh_key(pos, mem.tables);
        if (use_nnue)
            (void)ensure_eval_loaded(nullptr);
    }

    [[nodiscard]] bool mnue_active() const noexcept {
        return use_nnue && mnue::p2_loaded();
    }

    [[nodiscard]] std::string mnue_name() const {
        return mnue::p2_loaded() ? mnue::p2_path() : std::string{};
    }

    void ensure_attack_ready() {
        attack_init_backend(mem);
    }

    ~UciSession() {
        stop_search();
        syzygy::shutdown();
        memory::memory_free(mem);
    }

    void join_finished_search() {
        if (search_thread.joinable() &&
            !search_running.load(std::memory_order_acquire)) {
            search_thread.join();
        }
    }

    void stop_search() {
        stop_requested.store(true, std::memory_order_release);
        pondering.store(false, std::memory_order_release);
        if (search_thread.joinable())
            search_thread.join();
        ponder_search.store(false, std::memory_order_release);
        pondering.store(false, std::memory_order_release);
        ponder_hit_received.store(false, std::memory_order_release);
        ponder_time_offset_ms.store(0, std::memory_order_release);
        search_start_ms.store(0, std::memory_order_release);
        search_running.store(false, std::memory_order_release);
    }

    void handle_ponderhit() noexcept {
        if (!search_running.load(std::memory_order_acquire) ||
            !ponder_search.load(std::memory_order_acquire) ||
            !pondering.load(std::memory_order_acquire)) {
            return;
        }

        const long long start_ms = search_start_ms.load(std::memory_order_acquire);
        const long long now_ms = steady_now_ms();
        const long long elapsed = start_ms > 0 ? std::max(0LL, now_ms - start_ms) : 0LL;
        const int offset_ms = static_cast<int>(
            std::min<long long>(elapsed, std::numeric_limits<int>::max())
        );

        ponder_time_offset_ms.store(offset_ms, std::memory_order_release);
        ponder_hit_received.store(true, std::memory_order_release);
        pondering.store(false, std::memory_order_release);
    }

    const bool is_beta = true;

    void emit_banner(std::ostream& out) const {
        out << "MagnusChess\U0001F984 2.4.102 by the Magnus developer ";
        if (is_beta) {
            out << "& This is a beta version";
        }
        out << std::endl;
    }

    void emit_uci_id(std::ostream& out) const {
        out << "id name MagnusChess 2.4.102 ";
        
        if(is_beta) {
            out << "for Beta Testing";
        }
        
        out << '\n';

        out << "id author Magnus\U0001F984(gitvalerain@gmail.com)\n";
        out << "option name Hash type spin default " << DEFAULT_UCI_HASH_MB
            << " min 1 max 1048576\n";
        out << "option name Threads type spin default 1 min 1 max " << MAX_UCI_THREADS << "\n";
        out << "option name MultiPV type spin default " << DEFAULT_UCI_MULTIPV
            << " min " << DEFAULT_UCI_MULTIPV
            << " max " << DEFAULT_UCI_MULTIPV << "\n";
        out << "option name Contempt type spin default " << DEFAULT_UCI_CONTEMPT
            << " min " << MIN_UCI_CONTEMPT
            << " max " << MAX_UCI_CONTEMPT << "\n";
        out << "option name Move Overhead type spin default "
            << timeman::DEFAULT_MOVE_OVERHEAD_MS
            << " min " << timeman::MIN_MOVE_OVERHEAD_MS
            << " max " << timeman::MAX_MOVE_OVERHEAD_MS << "\n";
        out << "option name Clear Hash type button\n";
        out << "option name UseNNUE type check default true\n";
        out << "option name Ponder type check default true\n";
        out << "option name Singular Telemetry type check default false\n";
        out << "option name SyzygyPath type string default <empty>\n";
        out << "option name SyzygyProbeDepth type spin default "
            << syzygy::DEFAULT_PROBE_DEPTH
            << " min " << syzygy::MIN_PROBE_DEPTH
            << " max " << syzygy::MAX_PROBE_DEPTH << "\n";
        out << "option name Syzygy50MoveRule type check default true\n";
        out << "option name SyzygyProbeLimit type spin default "
            << syzygy::DEFAULT_PROBE_LIMIT
            << " min " << syzygy::MIN_PROBE_LIMIT
            << " max " << syzygy::MAX_PROBE_LIMIT << "\n";
        out << "option name MNUEfile type string default " << eval_file_p2 << '\n';
        out << "uciok" << std::endl;
    }

    void reset_new_game() {
        memory::memory_clear_hash(mem);
        set_start_position(pos);
        position_refresh_key(pos, mem.tables);
        clear_position_history(position_history);
        time_manager.new_game();
    }

    void copy_history_to_limits(search::SearchLimits& limits) const {
        limits.game_history_count = position_history.count;
        if (position_history.count < search::MAX_GAME_HISTORY) {
            for (int i = 0; i < position_history.count; ++i)
                limits.game_history_keys[i] = position_history.keys[i];
            return;
        }
        // Circular buffer wrap: copy [head..end] then [0..head-1].
        const int head = position_history.head;
        int dst = 0;
        for (int i = head; i < search::MAX_GAME_HISTORY; ++i)
            limits.game_history_keys[dst++] = position_history.keys[i];
        for (int i = 0; i < head; ++i)
            limits.game_history_keys[dst++] = position_history.keys[i];
    }

    [[nodiscard]] bool ensure_nnue_loaded(std::ostream* out) {
        if (has_mnue_extension(eval_file)) {
            if (out)
                *out << "info string NNUE loader got .mnue path; use MNUEfile option\n";
            return false;
        }

        const std::string resolved = resolve_file_path(eval_file);

        if (nnue::loaded() && nnue::path() == resolved)
            return true;

        if (nnue::load(resolved)) {
            if (out)
                *out << "info string loaded nnue " << resolved << '\n';
            return true;
        }

        if (out)
            *out << "info string failed to load nnue: " << nnue::last_error() << '\n';
        return false;
    }


    [[nodiscard]] bool ensure_mnue_loaded(std::ostream* out) {
        if (eval_file_p2.empty())
            return false;

        const std::string p2_resolved = resolve_file_path(eval_file_p2);

        if (mnue::p2_loaded() && mnue::p2_path() == p2_resolved)
            return true;

        if (!mnue::load_p2(p2_resolved)) {
            if (out)
                *out << "info string failed to load mnue p2: "
                     << mnue::last_error() << '\n';
            return false;
        }

        if (out)
            *out << "info string loaded mnue p2 " << p2_resolved << '\n';

        return true;
    }

    [[nodiscard]] bool ensure_eval_loaded(std::ostream* out) {
        if (!use_nnue)
            return false;

        if (ensure_mnue_loaded(out))
            return true;

        return ensure_nnue_loaded(out);
    }

    void handle_setoption(std::ostream& out, std::string_view command) {
        std::istringstream iss{std::string(command)};
        std::string token;
        std::string name;
        std::string value;

        iss >> token; // setoption
        iss >> token; // name

        while (iss >> token) {
            if (token == "value")
                break;

            if (!name.empty())
                name.push_back(' ');
            name += token;
        }

        std::getline(iss, value);
        if (!value.empty() && value.front() == ' ')
            value.erase(value.begin());

        if (name == "Hash") {
            int mb = 0;
            if (parse_int(value, mb) && mb > 0) {
                // Safety cap: prevent OOM crash from -fno-exceptions + huge allocation.
                constexpr std::size_t MAX_HASH_MB = 1ULL << 20; // 1 TB
                const std::size_t clamped_mb = std::min(
                    static_cast<std::size_t>(mb), MAX_HASH_MB);
                memory::tt_resize_mb(mem.tt, clamped_mb);
            }
        }
        else if (name == "Threads") {
            int parsed_threads = 0;
            if (parse_int(value, parsed_threads))
                threads = std::clamp(parsed_threads, 1, MAX_UCI_THREADS);
        }
        else if (name == "MultiPV") {
            // Advertised for GUI compatibility; search currently supports one principal variation.
        }
        else if (name == "Contempt") {
            int parsed_contempt = 0;
            if (parse_int(value, parsed_contempt)) {
                const int clamped = std::clamp(parsed_contempt, MIN_UCI_CONTEMPT, MAX_UCI_CONTEMPT);
                if (contempt != clamped)
                    memory::memory_clear_hash(mem);
                contempt = clamped;
            }
        }
        else if (name == "Clear Hash") {
            memory::memory_clear_hash(mem);
        }
        else if (name == "Move Overhead") {
            int parsed_overhead = 0;
            if (parse_int(value, parsed_overhead))
                time_manager.set_move_overhead_ms(parsed_overhead);
        }
        else if (name == "UseNNUE") {
            bool parsed = false;
            if (parse_bool(value, parsed)) {
                if (use_nnue != parsed)
                    memory::memory_clear_hash(mem);

                use_nnue = parsed;
                if (use_nnue && !ensure_eval_loaded(&out))
                    out << "info string eval unavailable, search will fall back to hce\n";
            }
        }
        else if (name == "Ponder") {
            bool parsed = false;
            if (parse_bool(value, parsed))
                enable_ponder = parsed;
        }
        else if (name == "Singular Telemetry") {
            bool parsed = false;
            if (parse_bool(value, parsed))
                singular_telemetry = parsed;
        }
        else if (name == "SyzygyPath") {
            syzygy_path = value == "<empty>" ? std::string{} : value;
            if (!syzygy::init(syzygy_path)) {
                out << "info string failed to initialize Syzygy tablebases\n";
            } else if (syzygy::max_cardinality() > 0) {
                out << "info string Syzygy tablebases loaded up to "
                    << syzygy::max_cardinality() << " pieces\n";
            } else {
                out << "info string no Syzygy tablebases found\n";
            }
            memory::memory_clear_hash(mem);
        }
        else if (name == "SyzygyProbeDepth") {
            int parsed_depth = 0;
            if (parse_int(value, parsed_depth)) {
                syzygy_probe_depth = std::clamp(
                    parsed_depth,
                    syzygy::MIN_PROBE_DEPTH,
                    syzygy::MAX_PROBE_DEPTH
                );
            }
        }
        else if (name == "Syzygy50MoveRule") {
            bool parsed = false;
            if (parse_bool(value, parsed))
                syzygy_50_move_rule = parsed;
        }
        else if (name == "SyzygyProbeLimit") {
            int parsed_limit = 0;
            if (parse_int(value, parsed_limit)) {
                syzygy_probe_limit = std::clamp(
                    parsed_limit,
                    syzygy::MIN_PROBE_LIMIT,
                    syzygy::MAX_PROBE_LIMIT
                );
            }
        }
        else if (name == "MNUEfile") {
            if (!value.empty()) {
                std::error_code ec;
                if (!std::filesystem::exists(value, ec) || ec) {
                    out << "info string MNUEfile not found: " << value << '\n';
                    return;
                }
                eval_file_p2 = value;
                memory::memory_clear_hash(mem);

                if (use_nnue && !ensure_eval_loaded(&out))
                    out << "info string eval unavailable, search will fall back to hce\n";
            }
        }
    }

    void ensure_search_eval_ready(
        std::ostream& out,
        const char* fallback_message
    ) {
        if (use_nnue && !ensure_eval_loaded(&out))
            out << fallback_message << '\n';
    }


    void handle_eval(std::ostream& out) {
        if (use_nnue && !ensure_eval_loaded(&out))
            out << "info string eval unavailable, using hce\n";

        out << "info string eval " << active_eval_name(use_nnue) << '\n';
        const int hce_stm = eval::evaluate(pos);
        out << "info string hce cp " << white_pov_score(pos, hce_stm) << '\n';


        if (mnue::p2_loaded()) {
            const int raw_stm = mnue::eval_p2(pos);
            const int search_stm = mnue::search_score(raw_stm, pos);
            const int search_cp_stm = mnue::search_score_to_cp(search_stm, pos);
            const mnue::WdlTriplet wdl_white =
                white_pov_wdl(pos, mnue::search_score_to_wdl(search_stm, pos));

            out << "info string mnue p2 path " << mnue::p2_path() << '\n';
            out << "info string mnue p2 material " << mnue::material_units(pos) << '\n';
            out << "info string mnue p2 raw " << white_pov_score(pos, raw_stm) << '\n';
            out << "info string mnue p2 search " << white_pov_score(pos, search_stm) << '\n';
            out << "info string mnue p2 searchcp " << white_pov_score(pos, search_cp_stm) << '\n';
            out << "info string mnue p2 winrate "
                << wdl_white.win << '\n';
            out << "info string mnue p2 wdl "
                << wdl_white.win << ' '
                << wdl_white.draw << ' '
                << wdl_white.loss << '\n';

        }

        if (nnue::loaded()) {
            const int raw_stm = nnue::eval(pos);
            const int cp_stm = nnue::to_cp(raw_stm, pos);
            const int winrate_stm = nnue::win_rate_model(raw_stm, pos);
            const int search_stm = nnue::search_score(raw_stm, pos);
            const int search_cp_stm = nnue::search_score_to_cp(search_stm, pos);
            const nnue::WdlTriplet wdl_white =
                white_pov_wdl(pos, nnue::search_score_to_wdl(search_stm, pos));

            out << "info string nnue raw " << white_pov_score(pos, raw_stm) << '\n';
            out << "info string nnue cp " << white_pov_score(pos, cp_stm) << '\n';
            out << "info string nnue search " << white_pov_score(pos, search_stm) << '\n';
            out << "info string nnue searchcp " << white_pov_score(pos, search_cp_stm) << '\n';
            out << "info string nnue winrate "
                << white_pov_winrate(pos, winrate_stm) << '\n';
            out << "info string nnue wdl "
                << wdl_white.win << ' '
                << wdl_white.draw << ' '
                << wdl_white.loss << '\n';
        } else {
            out << "info string nnue unavailable\n";
        }
    }

    void handle_movegen(std::ostream& out) {
        ensure_attack_ready();

        MoveList list{};
        generate_legal(pos, mem, list);

        out << debug_separator() << '\n';
        out << " " << debug_marker(true) << ' '
            << kAnsiBold << kAnsiCyan << "🔎 movegen" << kAnsiReset
            << "   side: " << side_name(static_cast<Color>(pos.side_to_move))
            << "   legal: " << list.size
            << "   backend: " << attack_backend_name() << '\n';
        out << " " << debug_marker(false) << " fen: " << display_fen(pos) << '\n';
        out << debug_separator() << '\n';
        out << " " << debug_marker(true) << " "
            << std::setw(3) << std::right << "#"
            << "   "
            << std::setw(8) << std::left << "Move"
            << std::setw(6) << std::left << "From"
            << std::setw(6) << std::left << "To"
            << "Kind" << '\n';
        out << debug_separator() << '\n';

        for (int i = 0; i < list.size; ++i) {
            const Move move = list.moves[i];
            const std::string move_text = search::move_to_uci(move);
            out << " " << debug_marker(false) << " "
                << std::setw(3) << std::right << (i + 1)
                << "   "
                << move_color(move) << std::setw(8) << std::left << move_text << kAnsiReset
                << std::setw(6) << std::left << display_square(from_sq(move))
                << std::setw(6) << std::left << display_square(to_sq(move))
                << display_move_kind(pos, mem, move)
                << '\n';
        }
        out << debug_separator() << '\n' << std::flush;
    }

    void handle_sort(std::string_view line, std::ostream& out) {
        ensure_attack_ready();

        int depth = DEFAULT_UCI_DEPTH;
        int sort_threads = threads;
        if (!parse_sort_command(line, depth, sort_threads)) {
            out << "info string usage: " << sort_usage_hint() << '\n';
            return;
        }

        MoveList list{};
        generate_legal(pos, mem, list);
        if (list.size == 0) {
            out << debug_separator() << '\n';
            out << " " << debug_marker(true) << ' '
                << kAnsiBold << kAnsiMagenta << "🧠 sort" << kAnsiReset
                << "   depth: " << depth
                << "   threads: " << sort_threads
                << "   legal: 0\n";
            out << debug_separator() << '\n' << std::flush;
            return;
        }

        ensure_search_eval_ready(out, "info string nnue unavailable, sort will use hce");
        const DisplayScoreModel score_model =
            use_nnue && mnue::p2_loaded()
                ? DisplayScoreModel::Mnue
                : (use_nnue && nnue::loaded() ? DisplayScoreModel::Nnue : DisplayScoreModel::Hce);

        struct SortEntry {
            Move move = 0;
            std::string move_text{};
            std::string score_text{};
            std::string pv{};
            int score = 0;
            int depth = 0;
            int seldepth = 0;
            u64 nodes = 0;
            double seconds = 0.0;
        };

        std::vector<SortEntry> entries;
        entries.reserve(static_cast<std::size_t>(list.size));

        out << debug_separator() << '\n';
        out << " " << debug_marker(true) << ' '
            << kAnsiBold << kAnsiMagenta << "🧠 sort" << kAnsiReset
            << "   depth: " << depth
            << "   threads: " << sort_threads
            << "   legal: " << list.size
            << "   eval: " << active_eval_name(use_nnue)
            << "   side: " << side_name(static_cast<Color>(pos.side_to_move))
            << '\n';
        out << " " << debug_marker(false) << " fen: " << display_fen(pos) << '\n';
        out << debug_separator() << '\n' << std::flush;

        using clock = std::chrono::steady_clock;
        const auto total_start = clock::now();

        for (int i = 0; i < list.size; ++i) {
            const Move move = list.moves[i];
            const std::string move_text = search::move_to_uci(move);

            render_sort_progress(
                out,
                i,
                list.size,
                move_text,
                std::string_view{},
                std::chrono::duration<double>(clock::now() - total_start).count(),
                true
            );

            search::SearchLimits limits{};
            limits.depth = depth;
            limits.use_nnue = use_nnue;
            limits.contempt = contempt;
            limits.singular_telemetry = singular_telemetry;
            limits.thread_count = sort_threads;
            limits.thread_id = 0;
            limits.report_info = true;
            limits.root_moves[0] = move;
            limits.root_move_count = 1;
            copy_history_to_limits(limits);

            std::atomic<bool> local_stop{false};
            limits.stop = &local_stop;

            PvTrackingStreamBuf pv_tracking_buf(nullptr);
            std::ostream tracked_out(&pv_tracking_buf);
            const auto move_start = clock::now();
            const search::SearchResult result =
                search::iterative_deepening(pos, mem, limits, &tracked_out);
            const auto move_end = clock::now();
            tracked_out.flush();
            const double seconds = std::chrono::duration<double>(move_end - move_start).count();

            std::string pv{pv_tracking_buf.last_pv()};
            if (pv.empty())
                pv = move_text;

            const std::string score_text =
                display_score_text(pos, result.score, score_model);

            entries.push_back({
                move,
                move_text,
                score_text,
                pv,
                result.score,
                result.depth,
                result.seldepth,
                result.nodes,
                seconds
            });

            const std::string colored_score = color_text(score_text, score_color(result.score));
            render_sort_progress(
                out,
                i + 1,
                list.size,
                move_text,
                colored_score,
                std::chrono::duration<double>(clock::now() - total_start).count(),
                false
            );
        }
        out << '\n';

        std::stable_sort(
            entries.begin(),
            entries.end(),
            [](const SortEntry& lhs, const SortEntry& rhs) noexcept {
                if (lhs.score != rhs.score)
                    return lhs.score > rhs.score;
                return lhs.move_text < rhs.move_text;
            }
        );

        const double total_seconds =
            std::chrono::duration<double>(clock::now() - total_start).count();
        u64 total_nodes = 0;
        for (const SortEntry& entry : entries)
            total_nodes += entry.nodes;

        out << debug_separator() << '\n';
        out << " " << debug_marker(true) << ' '
            << kAnsiBold << kAnsiMagenta << "🏁 sorted root moves" << kAnsiReset
            << "   depth: " << depth
            << "   threads: " << sort_threads
            << "   moves: " << entries.size()
            << "   nodes: " << format_u64_with_commas(total_nodes)
            << "   time: " << format_seconds(total_seconds)
            << "   nps: " << format_knps(total_nodes, total_seconds)
            << '\n';
        out << debug_separator() << '\n';
        out << " " << debug_marker(true) << " "
            << std::setw(3) << std::right << "#"
            << "   "
            << std::setw(8) << std::left << "Move"
            << std::setw(11) << std::left << "Score"
            << std::setw(7) << std::right << "Depth"
            << std::setw(8) << std::right << "Sel"
            << std::setw(13) << std::right << "Nodes"
            << std::setw(11) << std::right << "Time"
            << "  PV" << '\n';
        out << debug_separator() << '\n';

        for (std::size_t i = 0; i < entries.size(); ++i) {
            const SortEntry& entry = entries[i];
            out << " " << debug_marker(false) << " "
                << std::setw(3) << std::right << (i + 1)
                << "   "
                << move_color(entry.move)
                << std::setw(8) << std::left << entry.move_text << kAnsiReset
                << score_color(entry.score) << std::setw(11) << std::left
                << entry.score_text << kAnsiReset
                << std::setw(7) << std::right << entry.depth
                << std::setw(8) << std::right << entry.seldepth
                << std::setw(13) << std::right << format_u64_with_commas(entry.nodes)
                << std::setw(11) << std::right << format_seconds(entry.seconds)
                << "  " << color_text(entry.pv, kAnsiBlue)
                << '\n';
        }
        out << debug_separator() << '\n' << std::flush;
    }

    void handle_bench(std::string_view line, std::ostream& out) {
        ensure_attack_ready();
        if (line != "bench") {
            out << "info string usage: " << bench_usage_hint() << '\n';
            return;
        }

        ensure_search_eval_ready(out, "info string nnue unavailable, bench will use hce");

        if (!run_timed_search_bench(
                mem,
                DEFAULT_BENCH_MOVETIME_MS,
                static_cast<std::size_t>(threads),
                use_nnue,
                enable_ponder,
                out
            ))
            out << "info string bench failed\n";
    }

    void handle_position(std::string_view line, std::ostream& out) {
        ensure_attack_ready();
        if (!set_position_from_command(
                pos,
                mem,
                command_arguments(line, "position"),
                position_history
            )) {
            out << "info string invalid position command\n";
            return;
        }

        display_position_snapshot(pos, out, "position");
    }

    void handle_fen_shortcut(
        std::string_view line,
        std::string_view command,
        std::ostream& out
    ) {
        std::string_view fen = arrow_arguments(command_arguments(line, command));
        if (fen.empty()) {
            if (command == "fen")
                out << "fen -> " << display_fen(pos) << '\n';
            else
                display_position_snapshot(pos, out);
            return;
        }

        if (!parse_fen(pos, mem, fen)) {
            out << "invalid -> " << command << '\n';
            out << "usage -> " << command << " [->] <fen>\n";
            return;
        }

        clear_position_history(position_history);
        display_position_snapshot(pos, out, command);
    }

    void handle_go(std::string_view line, std::ostream& out) {
        ensure_attack_ready();
        ensure_search_eval_ready(out, "info string nnue unavailable, search will use hce");

        search::SearchLimits limits{};
        limits.use_nnue = use_nnue;
        if (!parse_go_command(pos, mem, time_manager, line, limits)) {
            out << "info string usage: " << go_usage_hint() << '\n';
            out << "info string " << go_usage_examples() << '\n';
            return;
        }

        if (mnue_active()){
            std :: cout << "info string MNUE evaluation using " << mnue_name() << "(20.1 MB, (1,32,16,1024,10240))\n";
        }

        limits.contempt = contempt;
        limits.singular_telemetry = singular_telemetry;
        limits.stop = &stop_requested;
        limits.pondering = &pondering;
        limits.ponder_time_offset_ms = &ponder_time_offset_ms;
        limits.thread_count = threads;
        limits.thread_id = 0;
        limits.report_info = true;
        limits.recover_ponder_pv = enable_ponder || limits.ponder;
        limits.syzygy_probe_depth = syzygy_probe_depth;
        limits.syzygy_probe_limit = syzygy_probe_limit;
        limits.syzygy_50_move_rule = syzygy_50_move_rule;
        copy_history_to_limits(limits);
        stop_requested.store(false, std::memory_order_release);
        ponder_search.store(limits.ponder, std::memory_order_release);
        pondering.store(limits.ponder, std::memory_order_release);
        ponder_hit_received.store(false, std::memory_order_release);
        ponder_time_offset_ms.store(0, std::memory_order_release);
        search_start_ms.store(steady_now_ms(), std::memory_order_release);

        const Position root = pos;
        search_running.store(true, std::memory_order_release);
        search_thread = std::thread([this, root, limits]() {
            PvTrackingStreamBuf pv_tracking_buf(std::cout.rdbuf());
            std::ostream tracked_out(&pv_tracking_buf);
            const auto search_start = std::chrono::steady_clock::now();
            const bool started_as_ponder = limits.ponder;
            const search::SearchResult result =
                search::iterative_deepening(root, mem, limits, &tracked_out);
            tracked_out.flush();
            const auto search_end = std::chrono::steady_clock::now();
            const int wall_elapsed_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    search_end - search_start
                ).count()
            );
            const bool ponder_was_hit =
                !started_as_ponder || ponder_hit_received.load(std::memory_order_acquire);
            const int record_elapsed_ms = started_as_ponder
                ? std::max(
                    0,
                    wall_elapsed_ms - ponder_time_offset_ms.load(std::memory_order_acquire)
                )
                : wall_elapsed_ms;
            if (ponder_was_hit)
                time_manager.record_search(root, limits, result, record_elapsed_ms);

            const std::string ponder = ponder_move_from_search_result(
                root,
                mem,
                result
            );

            std::cout << "bestmove " << search::move_to_uci(result.best_move);
            if (enable_ponder && !ponder.empty())
                std::cout << " ponder " << ponder;
            std::cout << std::endl;
            ponder_search.store(false, std::memory_order_release);
            pondering.store(false, std::memory_order_release);
            ponder_hit_received.store(false, std::memory_order_release);
            ponder_time_offset_ms.store(0, std::memory_order_release);
            search_start_ms.store(0, std::memory_order_release);
            search_running.store(false, std::memory_order_release);
        });
    }

    void handle_perft(std::string_view line, std::ostream& out) {
        ensure_attack_ready();
        int depth = -1;
        std::size_t perft_threads = 0;
        bool live = false;
        if (!parse_divide_command(line, depth, perft_threads, live)) {
            out << divide_usage_hint() << '\n';
            return;
        }

        divide(pos, mem, depth, out, perft_threads, true);
    }

    [[nodiscard]] bool process_command(
        std::string_view line,
        std::ostream& out
    ) {
        join_finished_search();

        if (line == "uci") {
            emit_uci_id(out);
            return true;
        }

        if (line == "isready") {
            if (use_nnue && !ensure_eval_loaded(&out))
                out << "info string eval unavailable, search will use hce\n";
            out << "readyok\n";
            return true;
        }

        if (line == "stop") {
            stop_search();
            return true;
        }

        if (line == "ponderhit") {
            handle_ponderhit();
            return true;
        }

        if (line == "quit") {
            stop_search();
            return false;
        }

        if (search_running.load(std::memory_order_acquire)) {
            out << "info string search busy, send stop first\n";
            return true;
        }

        if (line == "ucinewgame") {
            reset_new_game();
            display_position_snapshot(pos, out, "newgame");
            return true;
        }

        if (command_starts_with(line, "setoption")) {
            handle_setoption(out, line);
            return true;
        }

        if (command_starts_with(line, "mnueloadp2")) {
            const std::string path{arrow_arguments(command_arguments(line, "mnueloadp2"))};
            if (path.empty()) {
                out << "info string usage: mnueloadp2 <path-to-p2.mnue>\n";
                return true;
            }

            if (mnue::load_p2(path)) {
                memory::memory_clear_hash(mem);
                out << "info string loaded mnue p2 " << path << '\n';
            } else {
                out << "info string failed to load mnue p2: " << mnue::last_error() << '\n';
            }
            return true;
        }


        if (line == "eval") {
            handle_eval(out);
            return true;
        }

        if (line == "d") {
            display_position(pos, out);
            return true;
        }

        if (line == "mnuedebug") {
            if (use_nnue)
                (void)ensure_eval_loaded(&out);
            mnue::debug_dump_p2_features(pos, out);
            return true;
        }

        if (line == "mnuecheck") {
            if (use_nnue)
                (void)ensure_eval_loaded(&out);
            (void)mnue::debug_check_p2_incremental(pos, out);
            return true;
        }

        if (command_starts_with(line, "pos")) {
            handle_fen_shortcut(line, "pos", out);
            return true;
        }

        if (command_starts_with(line, "fen")) {
            handle_fen_shortcut(line, "fen", out);
            return true;
        }

        if (command_starts_with(line, "hash") || command_starts_with(line, "key")) {
            const std::string_view command = command_starts_with(line, "hash") ? "hash" : "key";
            if (!arrow_arguments(command_arguments(line, command)).empty())
                out << "readonly -> hash is derived from the current position\n";
            out << "hash -> " << pos.key << " (" << display_key_hex(pos.key) << ")\n";
            return true;
        }

        if (line == "movegen") {
            handle_movegen(out);
            return true;
        }

        if (command_starts_with(line, "sort")) {
            handle_sort(line, out);
            return true;
        }

        if (command_starts_with(line, "bench")) {
            handle_bench(line, out);
            return true;
        }

        if (command_starts_with(line, "position")) {
            handle_position(line, out);
            return true;
        }

        if (command_starts_with(line, "go")) {
            handle_go(line, out);
            return true;
        }

        if (command_starts_with(line, "perft")) {
            handle_perft(line, out);
            return true;
        }

        return true;
    }
};

} // namespace

int run_uci() {
    // UCI command loop with cooperative stop support.
    std::cout << std::unitbuf;
    UciSession session{};
    session.emit_banner(std::cout);

    constexpr std::size_t MAX_UCI_LINE = 8192;
    std::string line;
    line.reserve(MAX_UCI_LINE);
    while (true) {
        line.clear();
        int ch = 0;
        while ((ch = std::cin.get()) != std::char_traits<char>::eof() && ch != '\n') {
            if (line.size() < MAX_UCI_LINE)
                line.push_back(static_cast<char>(ch));
        }
        if (ch == std::char_traits<char>::eof() && line.empty())
            break;
        if (line.size() >= MAX_UCI_LINE)
            std::cout << "info string line too long, ignoring\n";
        else if (!session.process_command(line, std::cout))
            break;
    }

    return 0;
}

} // namespace magnus
