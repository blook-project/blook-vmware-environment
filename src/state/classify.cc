// blook-vmware-environment: screen classification (pure heuristics).
//
// The classifier deliberately does not depend on the guest OS: it only looks at
// the framebuffer statistics (and OCR text when an engine is available), so it
// keeps working while the guest is bugchecked or hung.
//
// The one thing colour alone cannot distinguish is "blue screen": the Windows
// logon UI is also dark blue with light text. Host-side context (tools
// heartbeat, whether guest operations answered, whether the frame has been
// changing) is therefore part of the decision: a blue frame with a *live* guest
// is a logon screen, a blue frame with a dead guest is a bugcheck.
#include "blook/bvmenv/classify.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

#include "blook/bvmenv/util.h"

namespace blook::bvmenv {
namespace {

const std::vector<std::string> kBugcheckMarkers = {
    "your device ran into a problem",
    "we're just collecting some error info",
    "we'll restart for you",
    "stop code",
    "bugcheck",
    "bug check",
    "你的设备遇到问题",
    "收集某些错误信息",
    "终止代码",
    "停止代码",
    "错误代码",
    " :( ",
};

const std::vector<std::string> kLoginMarkers = {
    "sign in",  "sign-in",     "登录",     "登陆",     "welcome", "欢迎", "other user",
    "其他用户", "switch user", "切换用户", "password", "密码",    "锁定",
};

const std::vector<std::string> kShellMarkers = {
    "recycle bin", "回收站", "start", "开始", "taskbar", "任务栏",
};

bool has_any(const std::string& text, const std::vector<std::string>& markers) {
    for (const auto& marker : markers) {
        if (contains_ci(text, marker))
            return true;
    }
    return false;
}

}  // namespace

std::string extract_stop_code(const std::string& text) {
    // 1) "STOP CODE: DRIVER_IRQL_NOT_LESS_OR_EQUAL" (any locale using ':' or '：')
    static const std::regex stop_code_re(
        R"((?:STOP\s*CODE|stop\s*code|错误代码|停止代码|终止代码)\s*[:：]\s*([A-Za-z0-9_]+))");
    // 2) bare bugcheck constant, e.g. 0x000000D1 / 0xC000021A
    static const std::regex hex_re(R"(\b0x[0-9A-Fa-f]{8}\b)");
    // 3) known symbolic bugcheck names when OCR splits the line
    static const std::regex symbol_re(
        R"(\b(?:DRIVER_IRQL_NOT_LESS_OR_EQUAL|IRQL_NOT_LESS_OR_EQUAL|PAGE_FAULT_IN_NONPAGED_AREA|SYSTEM_SERVICE_EXCEPTION|KMODE_EXCEPTION_NOT_HANDLED|ATTEMPTED_WRITE_TO_READONLY_MEMORY|UNEXPECTED_KERNEL_MODE_TRAP|BUGCODE_NDIS_DRIVER|MANUALLY_INITIATED_CRASH|CLOCK_WATCHDOG_TIMEOUT|DPC_WATCHDOG_VIOLATION|KERNEL_SECURITY_CHECK_FAILURE|BAD_POOL_CALLER|MEMORY_MANAGEMENT|NTFS_FILE_SYSTEM|CRITICAL_PROCESS_DIED)\b)");

    std::smatch match;
    if (std::regex_search(text, match, stop_code_re))
        return match[1].str();
    if (std::regex_search(text, match, hex_re))
        return match[0].str();
    if (std::regex_search(text, match, symbol_re))
        return match[0].str();
    return {};
}

ScreenReport classify_screen(const ScreenStats& stats, const std::string& ocr_text,
                             const ScreenContext& context) {
    ScreenReport report;
    report.ocr_text = ocr_text.size() > 4096 ? ocr_text.substr(0, 4096) : ocr_text;
    report.stop_code = extract_stop_code(ocr_text);

    if (stats.width <= 0 || stats.height <= 0 || stats.sampled_pixels <= 0) {
        report.classification = ScreenClass::Unknown;
        report.confidence = 0.0;
        report.reason = "no frame captured";
        return report;
    }

    const bool bugcheck_text = has_any(ocr_text, kBugcheckMarkers);
    const bool login_text = has_any(ocr_text, kLoginMarkers);
    const bool shell_text = has_any(ocr_text, kShellMarkers);
    const bool guest_alive = context.tools_running || context.guest_queryable;

    // --- bugcheck, text confirmed ----------------------------------------
    if (bugcheck_text) {
        report.classification = ScreenClass::Bsod;
        report.confidence = 0.98;
        report.reason = "bugcheck text detected in OCR";
        return report;
    }

    // --- logon UI, text confirmed ----------------------------------------
    if (login_text) {
        report.classification = ScreenClass::Login;
        report.confidence = 0.9;
        report.reason = "logon UI text detected in OCR";
        return report;
    }

    // --- black ------------------------------------------------------------
    // Boot screens are black with only a logo/progress hint, so the "uniform
    // black" test is deliberately generous; a tiny amount of light content
    // means we are looking at firmware/boot rather than a dark desktop.
    if (stats.black_ratio >= 0.85) {
        if (stats.white_ratio < 0.002 && stats.colored_ratio < 0.01) {
            report.classification = ScreenClass::Black;
            report.confidence = guest_alive ? 0.7 : 0.9;
            report.reason = "uniformly black framebuffer";
        } else {
            report.classification = ScreenClass::Boot;
            report.confidence = 0.7;
            report.reason = "black framebuffer with a small amount of light content (boot screen)";
        }
        return report;
    }

    // --- blue frame: bugcheck or logon UI? -------------------------------
    if (stats.blue_ratio >= 0.45 && stats.white_ratio >= 0.002) {
        if (!guest_alive && context.unchanged_samples >= 1) {
            report.classification = ScreenClass::Bsod;
            report.confidence = 0.85;
            report.reason =
                "dominant blue framebuffer with light text, guest unresponsive and frame unchanged";
        } else if (!guest_alive) {
            report.classification = ScreenClass::Bsod;
            report.confidence = 0.6;
            report.reason = "dominant blue framebuffer with light text and no guest heartbeat";
        } else {
            // Windows 10/11 logon UI is dark blue with the account name.
            report.classification = ScreenClass::Login;
            report.confidence = 0.75;
            report.reason = "dominant blue framebuffer with light text but a live guest (logon UI)";
        }
        return report;
    }

    // --- desktop ----------------------------------------------------------
    if (stats.colored_ratio >= 0.12 || stats.gray_ratio + stats.white_ratio >= 0.5) {
        report.classification = ScreenClass::Desktop;
        report.confidence = shell_text ? 0.85 : 0.65;
        report.reason = shell_text ? "shell markers in OCR" : "busy/colourful framebuffer";
        return report;
    }

    if (stats.dark_ratio > 0.7 && stats.white_ratio < 0.01) {
        report.classification = ScreenClass::Boot;
        report.confidence = 0.5;
        report.reason = "mostly dark framebuffer, no UI markers";
        return report;
    }

    if (guest_alive) {
        report.classification = ScreenClass::Desktop;
        report.confidence = 0.6;
        report.reason = "guest operations answered; framebuffer heuristics inconclusive";
        return report;
    }

    report.classification = ScreenClass::Unknown;
    report.confidence = 0.3;
    report.reason = "no heuristic matched";
    return report;
}

}  // namespace blook::bvmenv
