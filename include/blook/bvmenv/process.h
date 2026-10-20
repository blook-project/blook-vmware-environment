// blook-vmware-environment: process execution with output capture and timeout.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blook::bvmenv {

struct ProcessOptions {
    std::wstring command_line;                 // passed verbatim to CreateProcessW
    std::optional<std::wstring> working_dir;
    std::chrono::milliseconds timeout{0};      // 0 = no timeout
    std::map<std::wstring, std::wstring> env;  // extra environment variables
    bool inherit_environment = true;
    std::string stdin_data;                    // written to the child's stdin
    bool close_stdin = true;
};

struct ProcessResult {
    bool started = false;
    bool timed_out = false;
    std::uint32_t exit_code = 0;
    std::string out;
    std::string err;
    std::string error;  // start failure / internal error description
};

// Runs a process with stdout/stderr capture. Kills the whole process tree on
// timeout or when the caller shuts down, so no orphaned vmrun.exe processes are
// left behind.
ProcessResult run_process(const ProcessOptions& options);

// Convenience: build a command line from an argv vector with correct Windows
// quoting rules.
std::wstring build_command_line(const std::vector<std::wstring>& argv);

// True when the current process has an elevated (administrator) token.
bool is_process_elevated();

// Locate an executable on PATH (returns empty when not found).
std::wstring find_executable(std::wstring_view name);

// UTF-8 / UTF-16 helpers.
std::string to_utf8(std::wstring_view w);
std::wstring to_wide(std::string_view s);

}  // namespace blook::bvmenv
