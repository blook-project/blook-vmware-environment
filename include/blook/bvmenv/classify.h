// blook-vmware-environment: host side screen analysis and classification.
//
// Everything here works without guest cooperation: the frame comes from the
// hypervisor (vmcli MKS captureScreenshot), so it still works when the guest OS
// is bugchecked, hung or mid-boot.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "blook/bvmenv/types.h"

namespace blook::bvmenv {

struct ScreenStats;

// Extra host-side context used to avoid false positives: a blue frame with
// light text is the Windows logon UI when the guest is alive, and only a
// bugcheck when the guest has stopped answering.
struct ScreenContext {
    bool tools_running = false;    // VMware Tools heartbeat
    bool guest_queryable = false;  // guest operations answered
    int unchanged_samples = 0;     // identical consecutive frames observed
};

// Decodes a PNG (or BMP) into statistics. Returns nullopt when the file cannot
// be read; `error` gets a human readable reason.
std::optional<ScreenStats> analyze_image(const std::filesystem::path& png, std::string* error);

// Pure classification of statistics + optional OCR text. Exposed separately so
// unit tests can drive it with synthetic statistics.
ScreenReport classify_screen(const ScreenStats& stats, const std::string& ocr_text,
                             const ScreenContext& context = {});

// OCR the frame with an external engine. Tries (in order): the configured
// command, `tesseract` on PATH, and the Windows.Media.Ocr engine. Returns an
// empty string when no engine is usable.
std::string ocr_image(const std::filesystem::path& png,
                      const std::filesystem::path& configured_command, std::string* engine);

// Extracts a bugcheck code from arbitrary OCR text, e.g.
//   "STOP CODE: DRIVER_IRQL_NOT_LESS_OR_EQUAL" -> "DRIVER_IRQL_NOT_LESS_OR_EQUAL"
//   "错误代码: 0x000000D1"                      -> "0x000000D1"
std::string extract_stop_code(const std::string& text);

// Parses the tail of a VMware vmware.log for errors / panics.
std::vector<LogIssue> parse_vmware_log(const std::filesystem::path& log_path,
                                       std::size_t max_issues = 10);

// Parses a guest serial log (kernel debug output) for bugcheck information.
std::vector<LogIssue> parse_serial_log(const std::filesystem::path& log_path,
                                       std::size_t max_issues = 10);

}  // namespace blook::bvmenv
