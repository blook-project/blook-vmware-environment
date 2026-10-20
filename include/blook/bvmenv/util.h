// blook-vmware-environment: small host utilities (filesystem, strings, logging,
// locking, time).
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace blook::bvmenv {

// Structured, user facing error. main() turns this into a JSON error object and
// a non-zero exit code.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

namespace fs = std::filesystem;

std::string read_text(const fs::path& path);
void write_text(const fs::path& path, std::string_view data, bool binary = false);
void ensure_dir(const fs::path& path);
void remove_tree(const fs::path& path);
[[nodiscard]] bool file_exists(const fs::path& path);
[[nodiscard]] std::uint64_t file_size(const fs::path& path);
// Last modification time; false when the file does not exist.
bool last_write_time(const fs::path& path, fs::file_time_type* out);

std::string trim(std::string_view s);
std::string to_lower(std::string_view s);
bool starts_with_ci(std::string_view s, std::string_view prefix);
bool contains_ci(std::string_view s, std::string_view needle);
std::vector<std::string> split_ws(std::string_view s);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string replace_all(std::string s, std::string_view from, std::string_view to);
std::string shell_quote(std::string_view s);  // cmd.exe argument quoting
std::string tail_of(const std::string& s, std::size_t max_bytes);

// Encode a string for inclusion in JSON with ASCII-only output.
std::string printable_ascii(std::string_view s);

void sleep_ms(int ms);

// Host side log. Everything goes to a log file when one is configured, and
// errors always go to stderr.
class Logger {
public:
    Logger() = default;
    void configure(bool verbose, fs::path log_file);
    void info(std::string_view message) const;
    void debug(std::string_view message) const;
    void warn(std::string_view message) const;

    [[nodiscard]] bool verbose() const { return verbose_; }

private:
    void emit(std::string_view level, std::string_view message) const;
    bool verbose_ = false;
    fs::path log_file_;
};

// Cross-process lock so that concurrent bvmenv invocations do not corrupt lab
// state. Backed by a named mutex; released on scope exit.
class FileLock {
public:
    FileLock(const std::string& name, std::chrono::milliseconds timeout);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
    void* handle_ = nullptr;
};

}  // namespace blook::bvmenv
