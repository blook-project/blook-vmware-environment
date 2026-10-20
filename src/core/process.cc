// blook-vmware-environment: process execution.
#include "blook/bvmenv/process.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "blook/bvmenv/util.h"

namespace blook::bvmenv {
namespace {

std::string read_all(HANDLE pipe) {
    std::string out;
    char buffer[8192];
    DWORD read = 0;
    while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        out.append(buffer, read);
    }
    return out;
}

struct JobGuard {
    HANDLE job = nullptr;
    ~JobGuard() {
        if (job) CloseHandle(job);
    }
};

HANDLE create_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    return job;
}

HMODULE module_handle() {
    static HMODULE module = GetModuleHandleW(nullptr);
    return module;
}
}  // namespace

std::wstring build_command_line(const std::vector<std::wstring>& argv) {
    std::wstring out;
    for (const auto& arg : argv) {
        if (!out.empty()) out.push_back(L' ');
        const bool needs_quotes = arg.empty() || arg.find_first_of(L" \t\"") != std::wstring::npos;
        if (!needs_quotes) {
            out += arg;
            continue;
        }
        out.push_back(L'"');
        std::size_t backslashes = 0;
        for (const wchar_t c : arg) {
            if (c == L'\\') {
                ++backslashes;
                continue;
            }
            if (c == L'"') {
                out.append(backslashes * 2 + 1, L'\\');
                out.push_back(L'"');
            } else {
                out.append(backslashes, L'\\');
                out.push_back(c);
            }
            backslashes = 0;
        }
        out.append(backslashes * 2, L'\\');
        out.push_back(L'"');
    }
    return out;
}

ProcessResult run_process(const ProcessOptions& options) {
    ProcessResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_read = nullptr, out_write = nullptr;
    HANDLE err_read = nullptr, err_write = nullptr;
    HANDLE in_read = nullptr, in_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &sa, 0) || !CreatePipe(&err_read, &err_write, &sa, 0) ||
        !CreatePipe(&in_read, &in_write, &sa, 0)) {
        result.error = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = out_write;
    si.hStdError = err_write;
    si.hStdInput = in_read;
    PROCESS_INFORMATION pi{};

    // Extra environment block when requested.
    std::wstring environment;
    LPVOID env_ptr = nullptr;
    if (!options.env.empty() || !options.inherit_environment) {
        if (options.inherit_environment) {
            environment = GetEnvironmentStringsW();
            if (!environment.empty() && environment.back() == L'\0') {
                environment.pop_back();  // drop the block terminator, re-added below
            }
        }
        for (const auto& [key, value] : options.env) {
            environment += key + L"=" + value;
            environment.push_back(L'\0');
        }
        environment.push_back(L'\0');
        if (environment.size() > 1) env_ptr = environment.data();
    }

    std::vector<wchar_t> cmd_line(options.command_line.begin(), options.command_line.end());
    cmd_line.push_back(L'\0');

    const wchar_t* workdir = options.working_dir ? options.working_dir->c_str() : nullptr;
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    if (!CreateProcessW(nullptr, cmd_line.data(), nullptr, nullptr, TRUE, flags, env_ptr, workdir,
                        &si, &pi)) {
        result.error = "CreateProcess failed: " + std::to_string(GetLastError());
        CloseHandle(out_read);
        CloseHandle(out_write);
        CloseHandle(err_read);
        CloseHandle(err_write);
        CloseHandle(in_read);
        CloseHandle(in_write);
        return result;
    }

    result.started = true;
    JobGuard job;
    job.job = create_job();
    if (job.job) {
        AssignProcessToJobObject(job.job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(out_write);
    CloseHandle(err_write);
    CloseHandle(in_read);

    // Feed stdin (small inputs only; this CLI never streams large data).
    if (!options.stdin_data.empty()) {
        DWORD written = 0;
        WriteFile(in_write, options.stdin_data.data(),
                  static_cast<DWORD>(options.stdin_data.size()), &written, nullptr);
    }
    if (options.close_stdin) CloseHandle(in_write);

    std::string stdout_bytes;
    std::string stderr_bytes;
    std::thread out_reader([&] { stdout_bytes = read_all(out_read); });
    std::thread err_reader([&] { stderr_bytes = read_all(err_read); });

    DWORD wait_ms = INFINITE;
    if (options.timeout.count() > 0) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(options.timeout);
        wait_ms = static_cast<DWORD>(ms.count());
    }
    DWORD wait_result = WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait_result == WAIT_TIMEOUT) {
        result.timed_out = true;
        if (job.job) {
            TerminateJobObject(job.job, 1);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, 5000);
    }
    CloseHandle(out_read);
    CloseHandle(err_read);
    out_reader.join();
    err_reader.join();

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = exit_code;
    result.out = std::move(stdout_bytes);
    result.err = std::move(stderr_bytes);
    CloseHandle(pi.hProcess);
    return result;
}

bool is_process_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &elevation, size, &size)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return elevated;
}

std::wstring find_executable(std::wstring_view name) {
    wchar_t buffer[MAX_PATH * 4];
    if (SearchPathW(nullptr, std::wstring(name).c_str(), L".exe", static_cast<DWORD>(std::size(buffer)),
                    buffer, nullptr) > 0) {
        return buffer;
    }
    return {};
}

std::string to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), size);
    return out;
}

}  // namespace blook::bvmenv
