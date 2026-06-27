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
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "board/Attack.h"
#include "board/MoveGen.h"

#include "Bench.h"
#include "Common.h"
#include "Evaluation.h"
#include "Memory.h"
#include "mnue/Mnue.h"
#include "mnue/MnueX1Features.h"
#include "mnue/MnueX1Network.h"
#include "mnue/MnueV2Features.h"
#include "mnue/MnueV2Network.h"
#include "mnue/MnueV2Telemetry.h"
#include "Nnue.h"
#include "Search.h"
#include "syzygy/Syzygy.h"
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
    const std::filesystem::path exe_dir = get_executable_dir();
    const std::filesystem::path exe_parent = exe_dir.parent_path();
    const std::filesystem::path search_dirs[] = {
        exe_dir,
        exe_dir / "build",
        exe_parent,
        exe_parent.parent_path(),
        exe_parent / "build",
        exe_parent.parent_path() / "build"
    };

    for (const std::filesystem::path& dir : search_dirs) {
        const std::filesystem::path candidate =
            (dir / filename).lexically_normal();
        if (std::filesystem::exists(candidate, ec) && !ec)
            return candidate.string();
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

[[nodiscard]] bool is_mnue_v2_file(const std::string& path) {
    constexpr std::array<char, 8> Magic{
        'M', 'N', 'U', 'E', 'V', '2', '\0', '\0'
    };
    std::ifstream input(path, std::ios::binary);
    std::array<char, 8> actual{};
    return input
        && static_cast<bool>(input.read(actual.data(), actual.size()))
        && actual == Magic;
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
    return "perft <depth> <threads> [auto|pext|magic|table|classical]";
}

[[nodiscard]] constexpr const char* divide_usage_hint() noexcept {
    return "divide <depth> <threads> [live] [auto|pext|magic|table|classical]";
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
           depth <= search::MAX_SEARCH_DEPTH &&
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
    const auto equals_ci = [sv](std::string_view expected) noexcept {
        if (sv.size() != expected.size())
            return false;

        for (std::size_t i = 0; i < sv.size(); ++i) {
            const char lhs = static_cast<char>(
                std::tolower(static_cast<unsigned char>(sv[i]))
            );
            if (lhs != expected[i])
                return false;
        }
        return true;
    };

    if (equals_ci("true") || sv == "1") {
        value = true;
        return true;
    }

    if (equals_ci("false") || sv == "0") {
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
    if (mnue::x1::loaded())
        return "mnue-x1";
    if (mnue::v2::loaded())
        return "mnue-v2";
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

template<std::size_t Capacity>
[[nodiscard]] bool feature_list_equals(
    const mnue::v2::FeatureList<Capacity>& actual,
    const std::vector<u32>& expected
) noexcept {
    if (actual.count != expected.size())
        return false;
    for (std::size_t i = 0; i < actual.count; ++i) {
        if (actual.indices[i] != expected[i])
            return false;
    }
    return true;
}

[[nodiscard]] bool parse_golden_indices(
    const std::string& line,
    std::string_view label,
    std::vector<u32>& output
) {
    std::istringstream input(line);
    std::string actual_label;
    std::size_t count = 0;
    if (!(input >> actual_label >> count) || actual_label != label)
        return false;
    output.clear();
    output.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        u32 index = 0;
        if (!(input >> index))
            return false;
        output.push_back(index);
    }
    return true;
}

[[nodiscard]] bool run_mnue_v2_golden_check(
    const std::string& path,
    memory::Memory& mem,
    std::ostream& output
) {
    std::ifstream input(path);
    if (!input) {
        output << "info string could not open MNUEv2 golden fixture "
            << path << '\n';
        return false;
    }
    std::string line;
    if (!std::getline(input, line) || line != "MNUEV2_GOLDEN 1") {
        output << "info string invalid MNUEv2 golden fixture header\n";
        return false;
    }
    if (!std::getline(input, line)
        || !line.starts_with("NETWORK ")) {
        return false;
    }

    u64 cases = 0;
    u64 mismatches = 0;
    double total_output_error = 0.0;
    double maximum_output_error = 0.0;
    while (std::getline(input, line)) {
        if (line.empty())
            continue;
        if (line != "CASE")
            return false;

        std::array<std::string, 16> fields{};
        for (std::string& field : fields) {
            if (!std::getline(input, field))
                return false;
        }
        if (!fields[0].starts_with("FEN ")
            || !fields[1].starts_with("SIDE ")
            || !fields[2].starts_with("MATERIAL ")
            || !fields[3].starts_with("BUCKET ")
            || !fields[13].starts_with("OUTPUT ")
            || !fields[14].starts_with("SCORE ")
            || fields[15] != "END") {
            return false;
        }

        const std::string fen = fields[0].substr(4);
        const char side = fields[1].size() > 5 ? fields[1][5] : 'w';
        int expected_material = 0;
        int expected_bucket = 0;
        {
            std::istringstream material(fields[2].substr(9));
            std::istringstream bucket(fields[3].substr(7));
            if (!(material >> expected_material)
                || !(bucket >> expected_bucket)) {
                return false;
            }
        }

        std::array<std::vector<u32>, 6> expected_features{};
        constexpr std::array<std::string_view, 6> Labels{{
            "POSITION_STM",
            "POSITION_NTM",
            "ATTACK_STM",
            "ATTACK_NTM",
            "STRUCTURE_STM",
            "STRUCTURE_NTM"
        }};
        for (std::size_t i = 0; i < Labels.size(); ++i) {
            if (!parse_golden_indices(
                    fields[4 + i],
                    Labels[i],
                    expected_features[i]
                )) {
                return false;
            }
        }

        std::array<std::array<u64, 2>, 3> expected_hashes{};
        constexpr std::array<std::string_view, 3> HashLabels{{
            "POSITION_HASH",
            "ATTACK_HASH",
            "STRUCTURE_HASH"
        }};
        for (std::size_t i = 0; i < HashLabels.size(); ++i) {
            std::istringstream hashes(fields[10 + i]);
            std::string label;
            if (!(hashes >> label)
                || label != HashLabels[i]
                || !(hashes >> std::hex
                    >> expected_hashes[i][0]
                    >> expected_hashes[i][1])) {
                return false;
            }
        }

        double expected_output = 0.0;
        int expected_score = 0;
        {
            std::istringstream value(fields[13].substr(7));
            std::istringstream score(fields[14].substr(6));
            if (!(value >> expected_output)
                || !(score >> expected_score)) {
                return false;
            }
        }

        Position position{};
        if (!parse_fen(position, mem, fen))
            return false;
        mnue::v2::GoldenSnapshot actual{};
        if (!mnue::v2::make_golden_snapshot(position, mem, actual))
            return false;

        const Color stm = side == 'b' ? BLACK : WHITE;
        const Color ntm = ~stm;
        const double output_error =
            std::abs(actual.output - expected_output);
        total_output_error += output_error;
        maximum_output_error =
            std::max(maximum_output_error, output_error);
        bool matches =
            actual.material == expected_material
            && actual.bucket == expected_bucket
            && feature_list_equals(
                actual.features.position[stm],
                expected_features[0]
            )
            && feature_list_equals(
                actual.features.position[ntm],
                expected_features[1]
            )
            && feature_list_equals(
                actual.features.attack[stm],
                expected_features[2]
            )
            && feature_list_equals(
                actual.features.attack[ntm],
                expected_features[3]
            )
            && feature_list_equals(
                actual.features.structure[stm],
                expected_features[4]
            )
            && feature_list_equals(
                actual.features.structure[ntm],
                expected_features[5]
            )
            && actual.position_hash[stm] == expected_hashes[0][0]
            && actual.position_hash[ntm] == expected_hashes[0][1]
            && actual.attack_hash[stm] == expected_hashes[1][0]
            && actual.attack_hash[ntm] == expected_hashes[1][1]
            && actual.structure_hash[stm] == expected_hashes[2][0]
            && actual.structure_hash[ntm] == expected_hashes[2][1]
            && output_error <= 1.0e-5
            && actual.score == expected_score;
        if (!matches) {
            ++mismatches;
            output << "info string MNUEv2 golden mismatch case "
                << cases << " fen " << fen
                << " expected_output " << expected_output
                << " actual_output " << actual.output
                << " expected_score " << expected_score
                << " actual_score " << actual.score
                << " expected_phash " << std::hex
                << expected_hashes[0][0] << ':' << expected_hashes[0][1]
                << " actual_phash "
                << actual.position_hash[stm] << ':'
                << actual.position_hash[ntm]
                << " expected_ahash "
                << expected_hashes[1][0] << ':' << expected_hashes[1][1]
                << " actual_ahash "
                << actual.attack_hash[stm] << ':'
                << actual.attack_hash[ntm]
                << " expected_shash "
                << expected_hashes[2][0] << ':' << expected_hashes[2][1]
                << " actual_shash "
                << actual.structure_hash[stm] << ':'
                << actual.structure_hash[ntm]
                << std::dec << '\n';
        }
        ++cases;
    }

    output << "mnue v2 golden cases " << cases
        << " mismatches " << mismatches
        << " max_output_error " << maximum_output_error
        << " mean_output_error "
        << (cases == 0
            ? 0.0
            : total_output_error / static_cast<double>(cases))
        << " ok " << (mismatches == 0 ? 1 : 0) << '\n';
    return mismatches == 0;
}

[[nodiscard]] bool same_position_state(
    const Position& left,
    const Position& right
) noexcept {
    if (left.side_to_move != right.side_to_move
        || left.ep_sq != right.ep_sq
        || left.castling_rights != right.castling_rights
        || left.halfmove_clock != right.halfmove_clock
        || left.fullmove_number != right.fullmove_number
        || left.key != right.key
        || left.occupied != right.occupied
        || left.material_signature != right.material_signature
        || left.mnue_phase_units != right.mnue_phase_units) {
        return false;
    }
    for (int square = 0; square < SQ_NB; ++square) {
        if (left.board[square] != right.board[square])
            return false;
    }
    return true;
}

[[nodiscard]] bool run_mnue_v2_special_move_checks(
    memory::Memory& mem,
    std::ostream& output
) {
    struct Scenario {
        std::string_view name;
        std::string_view fen;
        std::string_view move;
    };
    constexpr std::array<Scenario, 13> Scenarios{{
        {"quiet", "7k/8/8/8/8/8/8/4K3 w - - 0 1", "e1f1"},
        {"capture", "7k/8/8/8/8/8/4p3/3QK3 w - - 0 1", "d1e2"},
        {"pawn_move", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "e2e4"},
        {"pawn_capture", "7k/8/8/8/3p4/4P3/8/4K3 w - - 0 1", "e3d4"},
        {"king_move", "7k/8/8/8/8/8/8/4K3 w - - 0 1", "e1e2"},
        {"castle_kingside", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1g1"},
        {"castle_queenside", "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1c1"},
        {"promotion", "7k/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8q"},
        {"underpromotion", "7k/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8n"},
        {"capture_promotion", "1r5k/P7/8/8/8/8/8/4K3 w - - 0 1", "a7b8q"},
        {"en_passant", "7k/8/8/3pP3/8/8/8/4K3 w - d6 0 1", "e5d6"},
        {"slider_open", "4k3/8/8/8/8/8/B7/R3K3 w - - 0 1", "a2b3"},
        {"discovered_attack", "4k3/8/8/8/8/8/4B3/4R1K1 w - - 0 1", "e2f3"}
    }};

    u64 checks = 0;
    u64 mismatches = 0;
    for (const Scenario& scenario : Scenarios) {
        Position current{};
        if (!parse_fen(current, mem, scenario.fen)) {
            output << "info string MNUEv2 special FEN failed "
                << scenario.name << '\n';
            ++mismatches;
            continue;
        }
        const Position initial = current;
        Move move = Move(0);
        if (!find_uci_move(current, mem, scenario.move, move)) {
            output << "info string MNUEv2 special move not legal "
                << scenario.name << ' ' << scenario.move << '\n';
            ++mismatches;
            continue;
        }
        mnue::v2::AccumulatorStack stack{};
        if (!mnue::v2::debug_check_incremental(
                current,
                mem,
                stack,
                output
            )) {
            ++mismatches;
        }
        ++checks;
        StateInfo state{};
        stack.push(current, mem, move);
        make_move(current, move, mem.tables, state);
        stack.after_make(current, mem);
        if (!mnue::v2::debug_check_incremental(
                current,
                mem,
                stack,
                output
            )) {
            ++mismatches;
        }
        ++checks;
        unmake_move(current, move, mem.tables, state);
        stack.pop(current, mem);
        if (!same_position_state(current, initial)
            || !mnue::v2::debug_check_incremental(
                current,
                mem,
                stack,
                output
            )) {
            ++mismatches;
        }
        ++checks;
    }
    output << "mnue v2 special moves"
        << " scenarios " << Scenarios.size()
        << " checks " << checks
        << " mismatches " << mismatches
        << " ok " << (mismatches == 0 ? 1 : 0)
        << '\n';
    return mismatches == 0;
}

[[nodiscard]] bool parse_go_command(
    const Position& pos,
    const memory::Memory& mem,
    timeman::TimeManager& time_manager,
    std::string_view command,
    search::SearchLimits& limits
) noexcept {
    // Convert the UCI go command into normalized parameters, then let the time
    // manager derive the final soft/hard budgets.
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
    bool& live,
    std::string& backend
) noexcept {
    std::istringstream iss{std::string(command)};
    std::string token;
    std::string value;

    depth = -1;
    threads = 0;
    live = false;
    backend = "auto";

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

    bool live_seen = false;
    bool backend_seen = false;
    while (iss >> value) {
        if (value == "live" && !live_seen) {
            live = true;
            live_seen = true;
        } else if (!backend_seen) {
            backend = value;
            backend_seen = true;
        } else {
            return false;
        }
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
    bool full_pv = false;
    bool singular_telemetry = false;
    bool use_msv_smp = false;
    bool msv_info = false;
    int threads = DEFAULT_UCI_THREADS;
    int contempt = DEFAULT_UCI_CONTEMPT;
    int syzygy_probe_depth = syzygy::DEFAULT_PROBE_DEPTH;
    int syzygy_probe_limit = syzygy::DEFAULT_PROBE_LIMIT;
    bool syzygy_50_move_rule = true;
    std::string eval_file = default_eval_file();
    std::string eval_file_p2{}; // empty → use embedded
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
        return use_nnue
            && (
                mnue::x1::loaded()
                || mnue::v2::loaded()
                || mnue::p2_loaded()
            );
    }

    [[nodiscard]] std::string mnue_name() const {
        if (mnue::x1::loaded())
            return mnue::x1::path();
        if (mnue::v2::loaded())
            return mnue::v2::path();
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
        out << "MagnusChess 4.5.132 by the Theodore Magnus Øen Nidhar";
        out << std::endl;
    }

    void emit_uci_id(std::ostream& out) const {
        out << "id name MagnusChess4.5.132";
        
        if(is_beta) {
            out << "-dev";
        }
        
        out << '\n';

        out << "id author Theodore Magnus Øen Nidhar\n";
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
        out << "option name FullPV type check default false\n";
        out << "option name Singular Telemetry type check default false\n";
        out << "option name UseMsvSmp type check default false\n";
        out << "option name MsvInfo type check default false\n";
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
        out << "option name MNUEfile type string default " << mnue::kEmbeddedP2Filename << "\n";
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
        // If v2 was loaded manually via mnuev2load, keep it.
        if (mnue::v2::loaded())
            return true;

        // External file explicitly set by user — load from disk.
        if (!eval_file_p2.empty()) {
            const std::string p2_resolved = resolve_file_path(eval_file_p2);

            if (mnue::p2_loaded() && mnue::p2_path() == p2_resolved)
                return true;

            if (is_mnue_v2_file(p2_resolved)) {
                if (!mnue::v2::load(p2_resolved)) {
                    if (out)
                        *out << "info string failed to load mnue v2: "
                            << mnue::v2::last_error() << '\n';
                    return false;
                }
                mnue::unload_p2();
                if (out)
                    mnue::v2::debug_dump_network(*out);
                return true;
            }

            if (mnue::load_p2(p2_resolved)) {
                mnue::v2::unload();
                if (out)
                    *out << "info string loaded mnue " << p2_resolved << '\n';
                return true;
            }

            if (out)
                *out << "info string failed to load mnue: "
                     << mnue::last_error() << '\n';
            return false;
        }

        // Default: compile-time embedded P2 network.
        if (mnue::p2_loaded() && mnue::p2_path() == mnue::kEmbeddedP2Filename)
            return true;

        if (mnue::p2_embedded_available() && mnue::load_p2_embedded()) {
            if (out)
                *out << "info string loaded embedded mnue p2\n";
            return true;
        }

        if (out)
            *out << "info string no MNUE network available\n";
        return false;
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
        else if (name == "FullPV") {
            bool parsed = false;
            if (parse_bool(value, parsed))
                full_pv = parsed;
        }
        else if (name == "Singular Telemetry") {
            bool parsed = false;
            if (parse_bool(value, parsed))
                singular_telemetry = parsed;
        }
        else if (name == "UseMsvSmp") {
            bool parsed = false;
            if (parse_bool(value, parsed))
                use_msv_smp = parsed;
        }
        else if (name == "MsvInfo") {
            bool parsed = false;
            if (parse_bool(value, parsed))
                msv_info = parsed;
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
            // "<embedded>" or empty → revert to built-in network.
            if (value.empty() || value == "<embedded>") {
                eval_file_p2.clear();
                mnue::v2::unload();
                mnue::unload_p2();
                memory::memory_clear_hash(mem);
                if (use_nnue && !ensure_eval_loaded(&out))
                    out << "info string eval unavailable, search will fall back to hce\n";
                return;
            }
            const std::string resolved = resolve_file_path(value);
            std::error_code ec;
            if (!std::filesystem::exists(resolved, ec) || ec) {
                out << "info string MNUEfile not found: " << value << '\n';
                return;
            }
            eval_file_p2 = resolved;
            memory::memory_clear_hash(mem);

            if (use_nnue && !ensure_eval_loaded(&out))
                out << "info string eval unavailable, search will fall back to hce\n";
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

        if (mnue::v2::loaded()) {
            const int raw_stm = mnue::v2::evaluate_reference(pos, mem);
            const int search_stm = mnue::search_score(raw_stm, pos);
            const int search_cp_stm =
                mnue::search_score_to_cp(search_stm, pos);
            const mnue::WdlTriplet wdl_white = white_pov_wdl(
                pos,
                mnue::search_score_to_wdl(search_stm, pos)
            );
            out << "info string mnue v2 path "
                << mnue::v2::path() << '\n';
            out << "info string mnue v2 material "
                << mnue::v2::material_units(pos) << '\n';
            out << "info string mnue v2 bucket "
                << mnue::v2::material_bucket(pos) << '\n';
            out << "info string mnue v2 output "
                << mnue::v2::evaluate_reference_output(pos, mem) << '\n';
            out << "info string mnue v2 raw "
                << white_pov_score(pos, raw_stm) << '\n';
            out << "info string mnue v2 search "
                << white_pov_score(pos, search_stm) << '\n';
            out << "info string mnue v2 searchcp "
                << white_pov_score(pos, search_cp_stm) << '\n';
            out << "info string mnue v2 wdl "
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
            mnue_active()
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
            limits.use_msv_smp = use_msv_smp;
            limits.msv_info = msv_info;
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

        if (mnue_active()) {
            if (mnue::v2::loaded()) {
                mnue::v2::debug_dump_network(std::cout);
            } else {
                std::cout << "info string MNUE evaluation using "
                    << mnue_name()
                    << " (20.1 MB, (1,32,16,1024,10240), "
                    << mnue::eval_simd_name() << ")\n";
            }
        }

        limits.contempt = contempt;
        limits.full_pv = full_pv;
        limits.singular_telemetry = singular_telemetry;
        limits.use_msv_smp = use_msv_smp;
        limits.msv_info = msv_info;
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
            const search::SearchResult result =
                search::iterative_deepening(root, mem, limits, &tracked_out);
            tracked_out.flush();

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
        std::string backend;
        if (!parse_divide_command(
                line,
                depth,
                perft_threads,
                live,
                backend
            )
            || !attack_select_backend(backend)) {
            out << "info string usage: " << perft_usage_hint() << '\n';
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

            const std::string resolved = resolve_file_path(path);
            if (mnue::load_p2(resolved)) {
                mnue::v2::unload();
                memory::memory_clear_hash(mem);
                out << "info string loaded mnue p2 " << resolved << '\n';
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
            if (mnue::v2::loaded())
                mnue::v2::debug_dump_position(pos, mem, out);
            else
                mnue::debug_dump_p2_features(pos, out);
            return true;
        }

        if (line == "mnuecheck") {
            if (use_nnue)
                (void)ensure_eval_loaded(&out);
            if (mnue::v2::loaded()) {
                mnue::v2::AccumulatorStack stack{};
                (void)mnue::v2::debug_check_incremental(
                    pos,
                    mem,
                    stack,
                    out
                );
            } else {
                (void)mnue::debug_check_p2_incremental(pos, out);
            }
            return true;
        }

        if (command_starts_with(line, "mnuev2load")) {
            const std::string path{
                arrow_arguments(command_arguments(line, "mnuev2load"))
            };
            if (path.empty()) {
                out << "info string usage: mnuev2load <path-to-v2.mnue>\n";
                return true;
            }
            const std::string resolved = resolve_file_path(path);
            if (mnue::v2::load(resolved)) {
                mnue::unload_p2();
                eval_file_p2 = resolved;
                memory::memory_clear_hash(mem);
                mnue::v2::debug_dump_network(out);
            } else {
                out << "info string failed to load mnue v2: "
                    << mnue::v2::last_error() << '\n';
            }
            return true;
        }

        if (line == "mnuev2info") {
            mnue::v2::debug_dump_network(out);
            return true;
        }

        if (line == "mnuev2debug") {
            mnue::v2::debug_dump_position(pos, mem, out);
            return true;
        }

        if (line == "mnuev2eval") {
            if (!mnue::v2::loaded()) {
                out << "mnue v2 loaded 0\n";
            } else {
                mnue::v2::AccumulatorStack stack{};
                out << "mnue v2 loaded 1"
                    << " bucket " << mnue::v2::material_bucket(pos)
                    << " output "
                    << mnue::v2::evaluate_reference_output(pos, mem)
                    << " reference "
                    << mnue::v2::evaluate_reference(pos, mem)
                    << " incremental "
                    << mnue::v2::evaluate_incremental(pos, mem, stack)
                    << '\n';
            }
            return true;
        }

        if (command_starts_with(line, "mnuev2scalar")) {
            const std::string value{
                arrow_arguments(
                    command_arguments(line, "mnuev2scalar")
                )
            };
            if (value == "on" || value == "true" || value == "1")
                mnue::v2::set_force_scalar(true);
            else if (value == "off" || value == "false" || value == "0")
                mnue::v2::set_force_scalar(false);
            else if (!value.empty()) {
                out << "info string usage: mnuev2scalar [on|off]\n";
                return true;
            }
            out << "mnue v2 force_scalar "
                << (mnue::v2::force_scalar() ? 1 : 0)
                << " backend " << mnue::v2::backend_name() << '\n';
            return true;
        }

        if (command_starts_with(line, "mnuev2check")) {
            if (!mnue::v2::loaded()) {
                out << "mnue v2 loaded 0\n";
                return true;
            }

            int sequences = 10;
            int plies = 32;
            {
                std::istringstream input{std::string(line)};
                std::string command;
                input >> command;
                if (input >> sequences) {
                    if (!(input >> plies)) {
                        out << "info string usage: mnuev2check [sequences plies]\n";
                        return true;
                    }
                }
            }
            sequences = std::clamp(sequences, 1, 10000);
            plies = std::clamp(plies, 1, 128);

            u64 checks = 0;
            u64 mismatches = 0;
            for (int sequence = 0; sequence < sequences; ++sequence) {
                Position current{};
                position_copy_without_accumulators(current, pos);
                mnue::v2::AccumulatorStack stack{};
                std::array<Move, 128> moves{};
                std::array<StateInfo, 128> states{};
                int depth = 0;
                u32 rng = static_cast<u32>(
                    current.key ^ (current.key >> 32)
                    ^ (0x9E3779B9u * static_cast<u32>(sequence + 1))
                );

                if (!mnue::v2::debug_check_incremental(
                        current,
                        mem,
                        stack,
                        out
                    )) {
                    ++mismatches;
                }
                ++checks;

                for (int ply = 0; ply < plies; ++ply) {
                    MoveList legal{};
                    generate_legal(current, mem, legal);
                    if (legal.size == 0)
                        break;
                    rng = rng * 1664525u + 1013904223u;
                    const Move move = legal.moves[
                        rng % static_cast<u32>(legal.size)
                    ];
                    moves[depth] = move;
                    stack.push(current, mem, move);
                    make_move(
                        current,
                        move,
                        mem.tables,
                        states[depth]
                    );
                    stack.after_make(current, mem);
                    ++depth;
                    if (!mnue::v2::debug_check_incremental(
                            current,
                            mem,
                            stack,
                            out
                        )) {
                        out << "info string MNUEv2 random mismatch make"
                            << " sequence " << sequence
                            << " ply " << ply
                            << " move "
                            << search::move_to_uci(move)
                            << " fen " << display_fen(current)
                            << '\n';
                        ++mismatches;
                    }
                    ++checks;
                }

                while (depth > 0) {
                    --depth;
                    unmake_move(
                        current,
                        moves[depth],
                        mem.tables,
                        states[depth]
                    );
                    stack.pop(current, mem);
                    if (!mnue::v2::debug_check_incremental(
                            current,
                            mem,
                            stack,
                            out
                        )) {
                        ++mismatches;
                    }
                    ++checks;
                }
                if (stack.size() != 1)
                    ++mismatches;
            }

            out << "mnue v2 random make/unmake"
                << " sequences " << sequences
                << " plies " << plies
                << " checks " << checks
                << " mismatches " << mismatches
                << " ok " << (mismatches == 0 ? 1 : 0)
                << '\n';
            return true;
        }

        if (line == "mnuev2bucketcheck") {
            (void)mnue::v2::debug_bucket_selftest(out);
            return true;
        }

        if (line == "mnuev2specialcheck") {
            if (!mnue::v2::loaded()) {
                out << "mnue v2 loaded 0\n";
                return true;
            }
            (void)run_mnue_v2_special_move_checks(mem, out);
            return true;
        }

        if (command_starts_with(line, "mnuev2telemetry")) {
            if (!mnue::v2::loaded()) {
                out << "mnue v2 loaded 0\n";
                return true;
            }
            int plies = 100;
            {
                std::istringstream input{std::string(line)};
                std::string command;
                input >> command;
                if (!(input >> plies))
                    plies = 100;
            }
            plies = std::clamp(plies, 1, 128);
            Position current{};
            position_copy_without_accumulators(current, pos);
            mnue::v2::AccumulatorStack stack{};
            std::array<Move, 128> moves{};
            std::array<StateInfo, 128> states{};
            int depth = 0;
            u32 rng = 0x5EED1234u;
            (void)mnue::v2::evaluate_incremental(
                current,
                mem,
                stack
            );
            for (int ply = 0; ply < plies; ++ply) {
                MoveList legal{};
                generate_legal(current, mem, legal);
                if (legal.size == 0)
                    break;
                rng = rng * 1664525u + 1013904223u;
                const Move move =
                    legal.moves[rng % static_cast<u32>(legal.size)];
                moves[depth] = move;
                stack.push(current, mem, move);
                make_move(current, move, mem.tables, states[depth]);
                stack.after_make(current, mem);
                ++depth;
                (void)mnue::v2::evaluate_incremental(
                    current,
                    mem,
                    stack
                );
            }
            while (depth > 0) {
                --depth;
                unmake_move(
                    current,
                    moves[depth],
                    mem.tables,
                    states[depth]
                );
                stack.pop(current, mem);
                (void)mnue::v2::evaluate_incremental(
                    current,
                    mem,
                    stack
                );
            }
            mnue::v2::print_telemetry(stack.telemetry(), out);
            return true;
        }

        if (command_starts_with(line, "mnuev2cycles")) {
            const std::string action{
                arrow_arguments(
                    command_arguments(line, "mnuev2cycles")
                )
            };
            if (action == "reset") {
                mnue::v2::reset_cycle_telemetry();
                out << "mnue v2 cycle telemetry reset\n";
            } else if (action.empty() || action == "report") {
                mnue::v2::print_cycle_telemetry(out);
            } else {
                out << "info string usage: mnuev2cycles [reset|report]\n";
            }
            return true;
        }

        if (command_starts_with(line, "mnuev2bench")) {
            if (!mnue::v2::loaded()) {
                out << "mnue v2 loaded 0\n";
                return true;
            }
            int iterations = 1000;
            int repeats = 5;
            {
                std::istringstream input{std::string(line)};
                std::string command;
                input >> command;
                if (input >> iterations)
                    (void)(input >> repeats);
            }
            iterations = std::clamp(iterations, 1, 1000000);
            repeats = std::clamp(repeats, 3, 15);
            const auto summarize = [&](const char* name, const auto& run) {
                std::vector<double> rates;
                rates.reserve(static_cast<std::size_t>(repeats));
                i64 checksum = 0;
                for (int repeat = 0; repeat < repeats; ++repeat) {
                    const auto start =
                        std::chrono::steady_clock::now();
                    checksum += run();
                    const auto stop =
                        std::chrono::steady_clock::now();
                    const double seconds =
                        std::chrono::duration<double>(
                            stop - start
                        ).count();
                    rates.push_back(
                        static_cast<double>(iterations) / seconds
                    );
                }
                std::sort(rates.begin(), rates.end());
                out << "mnue v2 bench " << name
                    << " iterations " << iterations
                    << " repeats " << repeats
                    << " median_per_second "
                    << static_cast<u64>(
                        rates[rates.size() / 2]
                    )
                    << " min " << static_cast<u64>(rates.front())
                    << " max " << static_cast<u64>(rates.back())
                    << " checksum " << checksum
                    << '\n';
            };

            summarize("full_refresh_scalar", [&]() {
                i64 checksum = 0;
                for (int i = 0; i < iterations; ++i)
                    checksum += mnue::v2::evaluate_reference(pos, mem);
                return checksum;
            });

            const bool previous_force = mnue::v2::force_scalar();
            mnue::v2::set_force_scalar(true);
            summarize("incremental_scalar_cached", [&]() {
                mnue::v2::AccumulatorStack stack{};
                i64 checksum = 0;
                for (int i = 0; i < iterations; ++i) {
                    checksum += mnue::v2::evaluate_incremental(
                        pos,
                        mem,
                        stack
                    );
                }
                return checksum;
            });
            mnue::v2::set_force_scalar(false);
            summarize("incremental_selected_backend_cached", [&]() {
                mnue::v2::AccumulatorStack stack{};
                i64 checksum = 0;
                for (int i = 0; i < iterations; ++i) {
                    checksum += mnue::v2::evaluate_incremental(
                        pos,
                        mem,
                        stack
                    );
                }
                return checksum;
            });

            MoveList legal{};
            generate_legal(pos, mem, legal);
            if (legal.size > 0) {
                const auto run_make_eval_unmake = [&]() {
                    Position current{};
                    position_copy_without_accumulators(current, pos);
                    mnue::v2::AccumulatorStack stack{};
                    (void)mnue::v2::evaluate_incremental(
                        current,
                        mem,
                        stack
                    );
                    i64 checksum = 0;
                    for (int i = 0; i < iterations; ++i) {
                        const Move move = legal.moves[
                            i % legal.size
                        ];
                        StateInfo state{};
                        stack.push(current, mem, move);
                        make_move(
                            current,
                            move,
                            mem.tables,
                            state
                        );
                        stack.after_make(current, mem);
                        checksum += mnue::v2::evaluate_incremental(
                            current,
                            mem,
                            stack
                        );
                        unmake_move(
                            current,
                            move,
                            mem.tables,
                            state
                        );
                        stack.pop(current, mem);
                    }
                    return checksum;
                };
                mnue::v2::set_force_scalar(true);
                summarize(
                    "make_eval_unmake_scalar",
                    run_make_eval_unmake
                );
                mnue::v2::set_force_scalar(false);
                summarize(
                    "make_eval_unmake_selected_backend",
                    run_make_eval_unmake
                );
            }
            mnue::v2::set_force_scalar(previous_force);
            return true;
        }

        if (line == "mnuev2simdcheck") {
            (void)mnue::v2::debug_attack_kernel_selftest(out);
            return true;
        }

        if (line == "mnuev2loadcheck") {
            (void)mnue::v2::debug_loader_selftest(out);
            return true;
        }

        if (command_starts_with(line, "mnuev2goldencheck")) {
            const std::string fixture{
                arrow_arguments(
                    command_arguments(line, "mnuev2goldencheck")
                )
            };
            if (fixture.empty()) {
                out << "info string usage: mnuev2goldencheck <fixture>\n";
                return true;
            }
            ensure_attack_ready();
            (void)run_mnue_v2_golden_check(
                resolve_file_path(fixture),
                mem,
                out
            );
            return true;
        }

        if (mnue::x1::loaded()) {
            mnue::x1::AccumulatorStack stack{};
            const int raw_stm =
                mnue::x1::evaluate_incremental(pos, mem, stack);
            const int search_stm = mnue::search_score(raw_stm, pos);
            const int search_cp_stm =
                mnue::search_score_to_cp(search_stm, pos);

            out << "info string mnue x1 path "
                << mnue::x1::path() << '\n';
            out << "info string mnue x1 raw "
                << white_pov_score(pos, raw_stm) << '\n';
            out << "info string mnue x1 search "
                << white_pov_score(pos, search_stm) << '\n';
            out << "info string mnue x1 searchcp "
                << white_pov_score(pos, search_cp_stm) << '\n';
        }

        if (command_starts_with(line, "mnuex1load")) {
            const std::string path{
                arrow_arguments(command_arguments(line, "mnuex1load"))
            };
            if (path.empty()) {
                out << "info string usage: mnuex1load <path-to-x1.mnue>\n";
                return true;
            }

            const std::string resolved = resolve_file_path(path);
            if (mnue::x1::load(resolved)) {
                out << "info string loaded mnue x1 " << resolved << '\n';
            } else {
                out << "info string failed to load mnue x1: "
                    << mnue::x1::last_error() << '\n';
            }
            return true;
        }

        if (line == "mnuex1unload") {
            mnue::x1::unload();
            memory::memory_clear_hash(mem);
            out << "info string unloaded mnue x1\n";
            return true;
        }

        if (line == "mnuex1debug") {
            mnue::x1::debug_dump_features(pos, mem, out);
            return true;
        }

        if (line == "mnuex1eval") {
            if (!mnue::x1::loaded()) {
                out << "mnue x1 loaded 0\n";
            } else {
                out << "mnue x1 loaded 1"
                    << " bucket " << mnue::x1::output_bucket(pos)
                    << " reference "
                    << mnue::x1::evaluate_reference(pos, mem)
                    << " fast "
                    << mnue::x1::evaluate_fast(pos, mem)
                    << " path " << mnue::x1::path()
                    << '\n';
            }
            return true;
        }

        if (line == "mnuex1bench") {
            if (!mnue::x1::loaded()) {
                out << "mnue x1 loaded 0\n";
                return true;
            }

            constexpr int Iterations = 10000;
            using EvalFn = int (*)(
                const Position&,
                const memory::Memory&
            ) noexcept;
            const auto run = [&](const char* name, EvalFn evaluator) {
                i64 checksum = 0;
                const auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < Iterations; ++i)
                    checksum += evaluator(pos, mem);
                const auto stop = std::chrono::steady_clock::now();
                const double seconds =
                    std::chrono::duration<double>(stop - start).count();
                const double evaluations_per_second =
                    static_cast<double>(Iterations) / seconds;

                out << "mnue x1 " << name << " bench"
                    << " iterations " << Iterations
                    << " seconds " << std::fixed << std::setprecision(6)
                    << seconds
                    << " evals_per_second " << std::setprecision(0)
                    << evaluations_per_second
                    << " checksum " << checksum
                    << std::defaultfloat
                    << '\n';
            };

            run("reference", &mnue::x1::evaluate_reference);
            run("fast", &mnue::x1::evaluate_fast);

            mnue::x1::AccumulatorStack stack{};
            (void)mnue::x1::evaluate_incremental(pos, mem, stack);
            i64 checksum = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < Iterations; ++i) {
                checksum += mnue::x1::evaluate_incremental(
                    pos,
                    mem,
                    stack
                );
            }
            const auto stop = std::chrono::steady_clock::now();
            const double seconds =
                std::chrono::duration<double>(stop - start).count();
            out << "mnue x1 cached bench"
                << " iterations " << Iterations
                << " seconds " << std::fixed << std::setprecision(6)
                << seconds
                << " evals_per_second " << std::setprecision(0)
                << static_cast<double>(Iterations) / seconds
                << " checksum " << checksum
                << std::defaultfloat
                << '\n';
            return true;
        }

        if (line == "mnuex1check") {
            if (!mnue::x1::loaded()) {
                out << "mnue x1 loaded 0\n";
                return true;
            }

            constexpr int CheckDepth = 2;
            mnue::x1::AccumulatorStack stack{};
            u64 checked_positions = 0;
            u64 mismatches = 0;

            const auto visit = [&](auto&& self, Position& current, int depth)
                -> void {
                const int rebuilt =
                    mnue::x1::evaluate_fast(current, mem);
                const int incremental =
                    mnue::x1::evaluate_incremental(current, mem, stack);
                ++checked_positions;
                if (rebuilt != incremental)
                    ++mismatches;

                if (depth == 0)
                    return;

                MoveList moves{};
                generate_legal(current, mem, moves);
                for (int i = 0; i < moves.size; ++i) {
                    const Move move = moves.moves[i];
                    StateInfo state{};
                    stack.push();
                    make_move(current, move, mem.tables, state);
                    self(self, current, depth - 1);
                    unmake_move(current, move, mem.tables, state);
                    stack.pop();
                }
            };

            Position check_position{};
            position_copy_without_accumulators(check_position, pos);
            visit(visit, check_position, CheckDepth);
            const auto stats = stack.stats();

            out << "mnue x1 incremental check"
                << " depth " << CheckDepth
                << " positions " << checked_positions
                << " mismatches " << mismatches
                << " feature_builds " << stats.feature_builds
                << " rebuilds " << stats.rebuilds
                << " diff_updates " << stats.diff_updates
                << " added_rows " << stats.added_rows
                << " removed_rows " << stats.removed_rows
                << " ok " << (mismatches == 0 ? 1 : 0)
                << '\n';
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
