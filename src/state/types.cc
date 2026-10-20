// blook-vmware-environment: value types, JSON projection, time helpers.
#include "blook/bvmenv/types.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

#include "blook/bvmenv/util.h"

namespace blook::bvmenv {
namespace {

json::Value optional_number(const std::optional<long long>& value) {
    if (!value)
        return json::Value{};
    return json::Value(*value);
}

}  // namespace

std::string_view to_string(PowerState s) {
    switch (s) {
        case PowerState::Off:
            return "off";
        case PowerState::On:
            return "on";
        case PowerState::Suspended:
            return "suspended";
        case PowerState::Unknown:
            break;
    }
    return "unknown";
}

std::string_view to_string(ToolsState s) {
    switch (s) {
        case ToolsState::NotInstalled:
            return "not_installed";
        case ToolsState::Installed:
            return "installed";
        case ToolsState::Running:
            return "running";
        case ToolsState::Unknown:
            break;
    }
    return "unknown";
}

std::optional<PowerState> parse_power_state(std::string_view s) {
    if (s == "off")
        return PowerState::Off;
    if (s == "on")
        return PowerState::On;
    if (s == "suspended")
        return PowerState::Suspended;
    if (s == "unknown")
        return PowerState::Unknown;
    return std::nullopt;
}

std::optional<ToolsState> parse_tools_state(std::string_view s) {
    if (s == "not_installed")
        return ToolsState::NotInstalled;
    if (s == "installed")
        return ToolsState::Installed;
    if (s == "running")
        return ToolsState::Running;
    if (s == "unknown")
        return ToolsState::Unknown;
    return std::nullopt;
}

std::string_view to_string(ScreenClass s) {
    switch (s) {
        case ScreenClass::Black:
            return "black";
        case ScreenClass::Boot:
            return "boot";
        case ScreenClass::Login:
            return "login";
        case ScreenClass::Desktop:
            return "desktop";
        case ScreenClass::Bsod:
            return "bsod";
        case ScreenClass::Hang:
            return "hang";
        case ScreenClass::Unknown:
            break;
    }
    return "unknown";
}

std::optional<ScreenClass> parse_screen_class(std::string_view s) {
    if (s == "black")
        return ScreenClass::Black;
    if (s == "boot")
        return ScreenClass::Boot;
    if (s == "login")
        return ScreenClass::Login;
    if (s == "desktop")
        return ScreenClass::Desktop;
    if (s == "bsod")
        return ScreenClass::Bsod;
    if (s == "hang")
        return ScreenClass::Hang;
    if (s == "unknown")
        return ScreenClass::Unknown;
    return std::nullopt;
}

std::string_view to_string(E2eOutcome o) {
    switch (o) {
        case E2eOutcome::Pass:
            return "pass";
        case E2eOutcome::Fail:
            return "fail";
        case E2eOutcome::Timeout:
            return "timeout";
        case E2eOutcome::Bsod:
            return "bsod";
        case E2eOutcome::Hang:
            return "hang";
        case E2eOutcome::Error:
            break;
    }
    return "error";
}

std::optional<E2eOutcome> parse_e2e_outcome(std::string_view s) {
    if (s == "pass")
        return E2eOutcome::Pass;
    if (s == "fail")
        return E2eOutcome::Fail;
    if (s == "timeout")
        return E2eOutcome::Timeout;
    if (s == "bsod")
        return E2eOutcome::Bsod;
    if (s == "hang")
        return E2eOutcome::Hang;
    if (s == "error")
        return E2eOutcome::Error;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// JSON projections
// ---------------------------------------------------------------------------
json::Value ScreenReport::to_json() const {
    json::Value out(json::Object{});
    out["classification"] = std::string(to_string(classification));
    out["confidence"] = confidence;
    out["reason"] = reason;
    out["stop_code"] = stop_code.empty() ? json::Value{} : json::Value(stop_code);
    out["screenshot"] = screenshot;
    json::Value ocr(ocr_text);
    out["ocr_text"] = ocr;
    return out;
}

json::Value ExecResult::to_json() const {
    json::Value value(json::Object{});
    value["ok"] = ran && !timed_out && exit_code == 0;
    value["ran"] = ran;
    value["timed_out"] = timed_out;
    value["exit_code"] = exit_code;
    value["run_id"] = run_id;
    value["guest_dir"] = guest_dir;
    value["stdout_path"] = stdout_path;
    value["stderr_path"] = stderr_path;
    value["duration_ms"] = static_cast<long long>(duration.count());
    value["stdout_tail"] = out.size() > 8192 ? out.substr(out.size() - 8192) : out;
    value["stderr_tail"] = err.size() > 8192 ? err.substr(err.size() - 8192) : err;
    if (!error.empty())
        value["error"] = error;
    return value;
}

json::Value ResetReport::to_json() const {
    json::Value out(json::Object{});
    out["performed"] = performed;
    out["ok"] = ok;
    out["reason"] = reason;
    out["strategy"] = strategy;
    out["duration_ms"] = static_cast<long long>(duration.count());
    return out;
}

json::Value StateReport::to_json() const {
    json::Value value(json::Object{});
    value["schema"] = std::string("bvmenv.state.v1");
    value["vm"] = vm;
    value["vmx"] = vmx;
    value["power"] = std::string(to_string(power));
    json::Value tools_json(json::Object{});
    tools_json["state"] = std::string(to_string(tools.state));
    tools_json["version"] = tools.version;
    tools_json["running"] = tools.running;
    value["tools"] = tools_json;
    value["ip"] = ip.empty() ? json::Value{} : json::Value(ip);
    value["uptime_seconds"] = optional_number(uptime_seconds);
    json::Value hang(json::Object{});
    hang["suspected"] = hang_suspected;
    hang["samples"] = samples;
    hang["unchanged_samples"] = unchanged_samples;
    value["hang"] = hang;
    value["screen"] = screen.to_json();
    json::Value issues(json::Array{});
    for (const auto& issue : log_issues) {
        json::Value item(json::Object{});
        item["level"] = issue.level;
        item["source"] = issue.source;
        item["message"] = issue.message;
        issues.push(std::move(item));
    }
    value["log"] = json::Object{{"issues", std::move(issues)}};
    value["guest_queryable"] = guest_queryable;
    value["sampled_at"] = sampled_at;
    return value;
}

json::Value E2eResult::to_json() const {
    json::Value out(json::Object{});
    out["schema"] = schema;
    out["run_id"] = run_id;
    out["vm"] = vm;
    out["bundle"] = bundle;
    out["entry"] = entry;
    json::Value arg_list(json::Array{});
    for (const auto& a : args)
        arg_list.push(json::Value(a));
    out["args"] = std::move(arg_list);
    out["started_at"] = started_at;
    out["finished_at"] = finished_at;
    out["duration_ms"] = static_cast<long long>(duration.count());
    out["outcome"] = std::string(to_string(outcome));
    out["exit_code"] = exit_code;
    out["timed_out"] = timed_out;
    out["artifact_dir"] = artifact_dir;
    out["exec"] = exec.to_json();
    out["reset"] = reset.to_json();
    out["state_before"] = state_before.to_json();
    out["state_after"] = state_after.to_json();
    if (!error.empty())
        out["error"] = error;
    return out;
}

// ---------------------------------------------------------------------------
// time
// ---------------------------------------------------------------------------
std::string to_iso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(tp);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buffer;
}

std::string now_iso8601() {
    return to_iso8601(std::chrono::system_clock::now());
}

std::string now_stamp() {
    const std::time_t seconds = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02dT%02d%02d%02dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buffer;
}

}  // namespace blook::bvmenv
