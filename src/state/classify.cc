// blook-vmware-environment: screen classification (pure heuristics).
//
// The classifier deliberately does not depend on the guest OS: it only looks at
// the framebuffer statistics (and OCR text when an engine is available), so it
// keeps working while the guest is bugchecked or hung.
#include "blook/bvmenv/classify.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

#include "blook/bvmenv/util.h"

namespace blook::bvmenv {
namespace {

const std::vector<std::string> kBugcheckMarkers = {
    "stop code", "bugcheck", "your device ran into a problem", "错误代码", "停止代码",
    "we're just collecting some error info", "收集某些错误信息",
};

const std::vector<std::string> kLoginMarkers = {
    "sign in", "sign-in", "登录", "登陆", "welcome", "欢迎", "other user", "其他用户",
    "switch user", "切换用户", "password", "密码",
};

const std::vector<std::string> kShellMarkers = {
    "recycle bin", "回收站", "start", "开始", "taskbar",
};

bool has_any(const std::string& text, const std::vector<std::string>& markers) {
    for (const auto& marker : markers) {
        if (contains_ci(text, marker)) return true;
    }
    return false;
}

}  // namespace

std::string extract_stop_code(const std::string& text) {
    // 1) "STOP CODE: DRIVER_IRQL_NOT_LESS_OR_EQUAL" (any locale using ':' or '：')
    static const std::regex stop_code_re(
        R"((?:STOP\s*CODE|stop\s*code|错误代码|停止代码)\s*[:：]\s*([A-Za-z0-9_]+))");
    // 2) bare bugcheck constant, e.g. 0x000000D1 / 0xC000021A
    static const std::regex hex_re(R"(\b0x[0-9A-Fa-f]{8}\b)");
    // 3) known symbolic bugcheck names when OCR splits the line
    static const std::regex symbol_re(
        R"(\b(?:DRIVER_IRQL_NOT_LESS_OR_EQUAL|IRQL_NOT_LESS_OR_EQUAL|PAGE_FAULT_IN_NONPAGED_AREA|SYSTEM_SERVICE_EXCEPTION|KMODE_EXCEPTION_NOT_HANDLED|ATTEMPTED_WRITE_TO_READONLY_MEMORY|UNEXPECTED_KERNEL_MODE_TRAP|BUGCODE_NDIS_DRIVER|MANUALLY_INITIATED_CRASH|CLOCK_WATCHDOG_TIMEOUT|DPC_WATCHDOG_VIOLATION|KERNEL_SECURITY_CHECK_FAILURE|BAD_POOL_CALLER|MEMORY_MANAGEMENT|NTFS_FILE_SYSTEM|CRITICAL_PROCESS_DIED)\b)");

    std::smatch match;
    if (std::regex_search(text, match, stop_code_re)) return match[1].str();
    if (std::regex_search(text, match, hex_re)) return match[0].str();
    if (std::regex_search(text, match, symbol_re)) return match[0].str();
    return {};
}

ScreenReport classify_screen(const ScreenStats& stats, const std::string& ocr_text) {
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

    // --- bugcheck ---------------------------------------------------------
    // Windows 10/11 bugcheck screens are dominated by a blue background with
    // light text. OCR is the authoritative signal when available, colour is the
    // fallback (and covers the case where no OCR engine exists at all).
    if (bugcheck_text) {
        report.classification = ScreenClass::Bsod;
        report.confidence = 0.98;
        report.reason = "bugcheck text detected in OCR";
        if (report.stop_code.empty()) report.stop_code = extract_stop_code(ocr_text);
        return report;
    }
    if (stats.blue_ratio >= 0.45 && stats.white_ratio >= 0.002) {
        report.classification = ScreenClass::Bsod;
        report.confidence = 0.75;
        report.reason = "dominant blue framebuffer with light text (bugcheck colours)";
        return report;
    }

    // --- all black --------------------------------------------------------
    if (stats.black_ratio >= 0.85) {
        if (stats.white_ratio < 0.002 && stats.colored_ratio < 0.01) {
            report.classification = ScreenClass::Black;
            report.confidence = 0.9;
            report.reason = "uniformly black framebuffer";
        } else {
            report.classification = ScreenClass::Boot;
            report.confidence = 0.7;
            report.reason = "black framebuffer with a small amount of light content (boot screen)";
        }
        return report;
    }

    // --- login ------------------------------------------------------------
    if (login_text) {
        report.classification = ScreenClass::Login;
        report.confidence = 0.85;
        report.reason = "logon UI markers in OCR";
        return report;
    }

    // --- desktop ----------------------------------------------------------
    // A desktop is colourful and busy; a logon screen is a mostly flat
    // background with a single light text block.
    if (stats.colored_ratio >= 0.12 || stats.gray_ratio + stats.white_ratio >= 0.5) {
        report.classification = shell_text ? ScreenClass::Desktop : ScreenClass::Desktop;
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

    report.classification = ScreenClass::Unknown;
    report.confidence = 0.3;
    report.reason = "no heuristic matched";
    return report;
}

}  // namespace blook::bvmenv
