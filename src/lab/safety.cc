// blook-vmware-environment: host disk safety.
//
// The lab attaches and detaches its own VHDs. It must never touch anything else:
// an earlier bring-up helper enumerated file-backed virtual disks and detached
// the *user's* 500 GB DevDrive (D:) by accident. Every disk operation therefore
// goes through an explicit allowlist keyed on (a) the path being inside
// BVMENV_VM_ROOT and (b) the file name being one the tool itself creates.
#include <algorithm>
#include <cctype>
#include <string>

#include "blook/bvmenv/lab.h"
#include "blook/bvmenv/process.h"
#include "blook/bvmenv/util.h"

namespace blook::bvmenv {
namespace {

std::string normalized(const fs::path& p) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(p, ec);
    std::string text = to_utf8((ec ? p : canonical).wstring());
    std::replace(text.begin(), text.end(), '/', '\\');
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!text.empty() && text.back() == '\\')
        text.pop_back();
    return text;
}

bool starts_with_path(const std::string& text, const std::string& prefix) {
    if (text == prefix)
        return true;
    if (text.size() <= prefix.size())
        return false;
    return text.compare(0, prefix.size(), prefix) == 0 && text[prefix.size()] == '\\';
}

}  // namespace

bool is_owned_vhd_path(const fs::path& vm_root, const fs::path& candidate) {
    const std::string root = normalized(vm_root);
    const std::string path = normalized(candidate);
    if (root.empty() || path.empty() || !starts_with_path(path, root))
        return false;
    std::string name = to_lower(to_utf8(candidate.filename().wstring()));
    return name == "blook-base.vhd" || name == "answer.vhd";
}

bool detach_own_vhd(const Config& cfg, const fs::path& vhd) {
    if (!is_owned_vhd_path(cfg.vm_root, vhd)) {
        throw Error(
            "refusing to detach a disk that bvmenv does not own: " + to_utf8(vhd.wstring()) +
            " (allowed: files named blook-base.vhd/answer.vhd "
            "inside " +
            to_utf8(cfg.vm_root.wstring()) + ")");
    }
    if (!file_exists(vhd)) {
        // Nothing attached under that name can be ours; never guess.
        throw Error("refusing to detach: VHD not found at " + to_utf8(vhd.wstring()));
    }
    const fs::path script = cfg.cache_dir() / "diskpart-detach.txt";
    write_text(script,
               "select vdisk file=\"" + to_utf8(vhd.wstring()) + "\"\r\ndetach vdisk noerr\r\n");
    ProcessOptions options;
    options.command_line = build_command_line({L"diskpart.exe", L"/s", script.wstring()});
    options.timeout = std::chrono::minutes(2);
    const ProcessResult result = run_process(options);
    log_command(options.command_line,
                std::chrono::duration_cast<std::chrono::milliseconds>(options.timeout).count(),
                static_cast<int>(result.exit_code), result.timed_out);
    if (result.exit_code != 0) {
        throw Error("diskpart detach failed for " + to_utf8(vhd.wstring()) + ": " +
                    tail_of(trim(result.out + result.err), 500));
    }
    return true;
}

std::vector<std::string> attached_file_backed_vhds() {
    // Read-only enumeration: reports, never touches.
    ProcessOptions options;
    options.command_line = build_command_line(
        {L"powershell.exe", L"-NoProfile", L"-NonInteractive", L"-Command",
         L"Get-Disk | Where-Object { $_.BusType -eq 'File Backed Virtual' } | "
         L"ForEach-Object { \"$($_.Number)`t$([int]($_.Size/1GB))GB`t$($_.Path)\" }"});
    options.timeout = std::chrono::seconds(30);
    const ProcessResult result = run_process(options);
    std::vector<std::string> out;
    std::string current;
    for (const char ch : result.out) {
        if (ch == '\n') {
            const std::string line = trim(current);
            if (!line.empty())
                out.push_back(line);
            current.clear();
        } else if (ch != '\r') {
            current.push_back(ch);
        }
    }
    const std::string last = trim(current);
    if (!last.empty())
        out.push_back(last);
    return out;
}

}  // namespace blook::bvmenv
