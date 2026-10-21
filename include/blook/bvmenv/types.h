// blook-vmware-environment: shared value types for the lab.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "blook/bvmenv/json.h"

namespace blook::bvmenv {

// ---------------------------------------------------------------------------
// power / tools
// ---------------------------------------------------------------------------
enum class PowerState { Off, On, Suspended, Unknown };
enum class ToolsState { NotInstalled, Installed, Running, Unknown };

std::string_view to_string(PowerState s);
std::string_view to_string(ToolsState s);
std::optional<PowerState> parse_power_state(std::string_view s);
std::optional<ToolsState> parse_tools_state(std::string_view s);

// ---------------------------------------------------------------------------
// screen classification (host side, never depends on guest userland)
// ---------------------------------------------------------------------------
enum class ScreenClass {
    Unknown,  // no frame could be captured / no classification possible
    Black,    // frame is (almost) uniformly black
    Boot,     // firmware / boot loader / pre-login boot progress
    Login,    // lock screen / logon UI
    Desktop,  // interactive session with a shell
    Bsod,     // bugcheck screen (stop code extracted when OCR is available)
    Hang,     // screen unchanged while the guest is unresponsive
};

std::string_view to_string(ScreenClass s);
std::optional<ScreenClass> parse_screen_class(std::string_view s);

struct ScreenStats {
    int width = 0;
    int height = 0;
    // Ratios are fractions of the sampled pixels, 0..1.
    double black_ratio = 0.0;    // near black (all channels < 24)
    double white_ratio = 0.0;    // near white (all channels > 220)
    double blue_ratio = 0.0;     // strongly blue (classic bugcheck / setup)
    double gray_ratio = 0.0;     // low saturation mid tones
    double colored_ratio = 0.0;  // saturated, non blue
    double mean_r = 0.0;
    double mean_g = 0.0;
    double mean_b = 0.0;
    double dark_ratio = 0.0;        // all channels < 64
    std::uint64_t fingerprint = 0;  // perceptual-ish hash for hang detection
    int sampled_pixels = 0;
};

struct ScreenReport {
    ScreenClass classification = ScreenClass::Unknown;
    double confidence = 0.0;
    std::string reason;
    std::string stop_code;   // e.g. "DRIVER_IRQL_NOT_LESS_OR_EQUAL (0xD1)"
    std::string ocr_text;    // truncated OCR dump (first 4 KiB)
    std::string screenshot;  // absolute path of the captured PNG (host side)

    [[nodiscard]] json::Value to_json() const;
};

// ---------------------------------------------------------------------------
// VM metadata
// ---------------------------------------------------------------------------
struct VmInfo {
    std::string name;
    std::string vmx;  // absolute path
    std::string dir;  // absolute path of the VM directory
    bool is_base = false;
    PowerState power = PowerState::Off;
    std::string base;      // parent VM name for forks ("" for the base)
    std::string snapshot;  // base snapshot name used for the clone
    int cpus = 0;
    int memory_mb = 0;
    std::string created_at;  // ISO-8601 UTC
};

struct ToolInfo {
    ToolsState state = ToolsState::Unknown;
    std::string version;
    bool running = false;
};

// ---------------------------------------------------------------------------
// execution
// ---------------------------------------------------------------------------
struct ExecRequest {
    std::string vm;
    std::string command_line;  // executed through cmd.exe /c
    std::optional<std::string> workdir;
    std::chrono::seconds timeout{120};
    bool elevated = false;
    bool ignore_exit_code = false;
    std::string run_id;  // optional explicit run id
    // Extra guest environment variables, injected as set KEY=VALUE lines into
    // the generated job.cmd so the child process inherits them (an inline
    // set X=1 && cmd does not survive into the child on this image).
    std::vector<std::string> env;
};

struct ExecResult {
    bool ran = false;
    bool timed_out = false;
    int exit_code = -1;
    std::string run_id;
    std::string guest_dir;
    std::string stdout_path;
    std::string stderr_path;
    std::string out;  // captured stdout (truncated to a sane tail for JSON)
    std::string err;
    std::chrono::milliseconds duration{0};
    std::string error;

    [[nodiscard]] json::Value to_json() const;
};

struct ResetReport {
    bool performed = false;
    bool ok = false;
    std::string reason;
    std::string strategy;  // "snapshot" | "delete-reclone" | "none"
    std::chrono::milliseconds duration{0};

    [[nodiscard]] json::Value to_json() const;
};

// ---------------------------------------------------------------------------
// state report
// ---------------------------------------------------------------------------
struct LogIssue {
    std::string level;   // "error" | "warning"
    std::string source;  // "vmware.log" | "serial.log"
    std::string message;
};

struct StateReport {
    std::string vm;
    std::string vmx;
    PowerState power = PowerState::Unknown;
    ToolInfo tools;
    std::string ip;
    std::optional<long long> uptime_seconds;
    bool hang_suspected = false;
    int samples = 0;
    int unchanged_samples = 0;
    ScreenReport screen;
    std::vector<LogIssue> log_issues;
    std::string sampled_at;        // ISO-8601 UTC
    bool guest_queryable = false;  // guest ops answered during sampling

    [[nodiscard]] json::Value to_json() const;
};

// ---------------------------------------------------------------------------
// e2e
// ---------------------------------------------------------------------------
enum class E2eOutcome { Pass, Fail, Timeout, Bsod, Hang, Error };

std::string_view to_string(E2eOutcome o);
std::optional<E2eOutcome> parse_e2e_outcome(std::string_view s);

struct E2eRequest {
    std::string vm;
    std::string bundle;  // local directory with the test payload
    std::string entry;   // guest path or name of the payload entry point
    std::vector<std::string> args;
    std::chrono::seconds timeout{120};
    std::string out_dir;               // host artifact root
    std::string reset_on = "failure";  // failure | always | never
    int wait_ready_timeout_s = 180;
};

struct E2eResult {
    std::string schema = "bvmenv.e2e.v1";
    std::string run_id;
    std::string vm;
    std::string bundle;
    std::string entry;
    std::vector<std::string> args;
    std::string started_at;
    std::string finished_at;
    std::chrono::milliseconds duration{0};
    E2eOutcome outcome = E2eOutcome::Error;
    int exit_code = -1;
    bool timed_out = false;
    std::string artifact_dir;
    ExecResult exec;
    ResetReport reset;
    StateReport state_before;
    StateReport state_after;
    std::string error;

    [[nodiscard]] json::Value to_json() const;
};

// ISO-8601 UTC timestamp helpers.
std::string now_iso8601();
std::string to_iso8601(std::chrono::system_clock::time_point tp);
// Filesystem safe UTC stamp, e.g. 20261021T031500Z.
std::string now_stamp();

}  // namespace blook::bvmenv
