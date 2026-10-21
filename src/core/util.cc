// blook-vmware-environment: host utilities.
#include "blook/bvmenv/util.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include "blook/bvmenv/process.h"

namespace blook::bvmenv {

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw Error("cannot read " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_text(const fs::path& path, std::string_view data, bool binary) {
    ensure_dir(path.parent_path());
    std::ofstream out(path, std::ios::binary | (binary ? std::ios::trunc : std::ios::trunc));
    if (!out) {
        throw Error("cannot write " + path.string());
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out)
        throw Error("short write to " + path.string());
}

void ensure_dir(const fs::path& path) {
    if (path.empty())
        return;
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec && !fs::is_directory(path)) {
        throw Error("cannot create directory " + path.string() + ": " + ec.message());
    }
}

void remove_tree(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

bool file_exists(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::uint64_t file_size(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

bool last_write_time(const fs::path& path, fs::file_time_type* out) {
    std::error_code ec;
    if (!fs::exists(path, ec))
        return false;
    auto time = fs::last_write_time(path, ec);
    if (ec)
        return false;
    if (out)
        *out = time;
    return true;
}

std::string trim(std::string_view s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return std::string(s.substr(begin, end - begin));
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size())
        return false;
    return to_lower(s.substr(0, prefix.size())) == to_lower(prefix);
}

bool contains_ci(std::string_view s, std::string_view needle) {
    if (needle.empty())
        return true;
    return to_lower(s).find(to_lower(needle)) != std::string::npos;
}

std::vector<std::string> split_ws(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (i > start)
            out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i)
            out += sep;
        out += parts[i];
    }
    return out;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    if (from.empty())
        return s;
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            break;
        }
        out.append(s, pos, hit - pos);
        out.append(to);
        pos = hit + from.size();
    }
    return out;
}

std::string shell_quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"')
            out += "\\\"";
        else
            out.push_back(c);
    }
    out += "\"";
    return out;
}

std::string batch_quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"')
            out += "\"\"";
        else
            out.push_back(c);
    }
    out += '"';
    return out;
}

std::string tail_of(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes)
        return s;
    return s.substr(s.size() - max_bytes);
}

std::string printable_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    }
    return out;
}

namespace {
fs::path g_command_log;
}

std::string local_time_string() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buffer;
}

void set_command_log(const fs::path& log_file) {
    g_command_log = log_file;
}

void log_command(std::wstring_view command_line, long long duration_ms, int exit_code,
                 bool timed_out) {
    if (g_command_log.empty())
        return;
    const std::string line = "[" + local_time_string() + "] " + to_utf8(command_line) +
                             " -> exit=" + std::to_string(exit_code) + " " +
                             std::to_string(duration_ms) + "ms" + (timed_out ? " TIMED OUT" : "") +
                             "\n";
    std::ofstream out(g_command_log, std::ios::binary | std::ios::app);
    if (out)
        out << line;
}

void sleep_ms(int ms) {
    if (ms > 0)
        Sleep(static_cast<DWORD>(ms));
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------
void Logger::configure(bool verbose, fs::path log_file) {
    verbose_ = verbose;
    log_file_ = std::move(log_file);
}

void Logger::emit(std::string_view level, std::string_view message) const {
    if (!verbose_ && level != "error")
        return;
    const std::string line = "[" + std::string(level) + "] " + std::string(message) + "\n";
    if (!log_file_.empty()) {
        std::ofstream out(log_file_, std::ios::binary | std::ios::app);
        if (out)
            out << line;
    }
    if (level == "error") {
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    } else if (verbose_) {
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    }
}

void Logger::info(std::string_view message) const {
    emit("info", message);
}
void Logger::debug(std::string_view message) const {
    emit("debug", message);
}
void Logger::warn(std::string_view message) const {
    emit("warn", message);
}

// ---------------------------------------------------------------------------
// FileLock
// ---------------------------------------------------------------------------
namespace {
std::wstring mutex_name(const std::string& name) {
    return L"Global\\blook-bvmenv-" + to_wide(name);
}
}  // namespace

FileLock::FileLock(const std::string& name, std::chrono::milliseconds timeout) {
    HANDLE handle = CreateMutexW(nullptr, FALSE, mutex_name(name).c_str());
    if (!handle)
        throw Error("CreateMutex failed for lock " + name);
    const DWORD wait =
        WaitForSingleObject(handle, static_cast<DWORD>(timeout.count() > 0 ? timeout.count() : 1));
    if (wait == WAIT_TIMEOUT || wait == WAIT_ABANDONED) {
        CloseHandle(handle);
        if (wait == WAIT_TIMEOUT) {
            throw Error("another bvmenv invocation holds the lab lock (" + name +
                        "); retry once it finishes");
        }
        // WAIT_ABANDONED: previous owner died, we own it now.
        HANDLE replacement = CreateMutexW(nullptr, FALSE, mutex_name(name).c_str());
        if (!replacement)
            throw Error("CreateMutex failed for lock " + name);
        handle_ = replacement;
        return;
    }
    handle_ = handle;
}

FileLock::~FileLock() {
    if (handle_) {
        ReleaseMutex(static_cast<HANDLE>(handle_));
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

}  // namespace blook::bvmenv
