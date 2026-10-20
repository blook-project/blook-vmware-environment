// blook-vmware-environment: OCR bridge used to read bugcheck text.
//
// The lab must classify a BSOD without guest cooperation. Colour heuristics
// detect "a blue screen", but the stop code (and the difference between a
// recovery dialog and a bugcheck) needs text. Two optional engines are used:
//   1. an explicitly configured OCR command (BVMENV_OCR_COMMAND), and
//   2. `tesseract` from PATH.
// When neither is available the classifier degrades to colour heuristics and
// reports a lower confidence instead of failing.
#include <windows.h>

#include <string>

#include "blook/bvmenv/classify.h"
#include "blook/bvmenv/process.h"
#include "blook/bvmenv/util.h"

namespace blook::bvmenv {

std::string ocr_image(const fs::path& png, const fs::path& configured_command,
                      std::string* engine_out) {
    std::wstring exe;
    std::string engine;
    if (!configured_command.empty() && file_exists(configured_command)) {
        exe = configured_command.wstring();
        engine = "configured";
    } else if (const std::wstring found = find_executable(L"tesseract"); !found.empty()) {
        exe = found;
        engine = "tesseract";
    } else if (const std::wstring found2 = find_executable(L"tesseract.exe"); !found2.empty()) {
        exe = found2;
        engine = "tesseract";
    } else {
        if (engine_out)
            engine_out->clear();
        return {};
    }

    if (engine_out)
        *engine_out = engine;
    const std::wstring png_w = png.wstring();
    const bool is_tesseract = engine == "tesseract";
    const std::vector<std::vector<std::wstring>> attempts = is_tesseract
        ? std::vector<std::vector<std::wstring>>{
              {exe, png_w, L"stdout", L"-l", L"chi_sim+eng"},
              {exe, png_w, L"stdout", L"-l", L"eng"},
              {exe, png_w, L"stdout"},
          }
        : std::vector<std::vector<std::wstring>>{{exe, png_w}};

    for (const auto& argv : attempts) {
        ProcessOptions options;
        options.command_line = build_command_line(argv);
        options.timeout = std::chrono::seconds(45);
        const ProcessResult result = run_process(options);
        if (result.exit_code == 0 && !trim(result.out + result.err).empty()) {
            return result.out;
        }
    }
    return {};
}

}  // namespace blook::bvmenv
