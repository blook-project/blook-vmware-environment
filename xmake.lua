-- blook-vmware-environment: VMware Workstation based Windows kernel-test lab.
--
-- Targets
--   bvmenv-core   static library (process, vmrun/vmcli backend, image analysis,
--                 state classification, lab orchestration)
--   bvmenv        the CLI shipped to downstream projects (bvmenv.exe)
--   bvmenv-tests  host side unit tests (gtest)
--   guest-*       x64 artifacts that run INSIDE the guest; the two drivers need
--                 the WDK and are opt-in via --guest_drivers=y
--
-- Usage:
--   xmake f -y -m releasedbg && xmake -y            # CLI + tests
--   xmake f -y --guest_drivers=y && xmake -y        # + in-guest drivers (WDK)
--   xmake run bvmenv-tests                          # unit tests

set_project("blook-vmware-environment")
set_arch("x64")
set_languages("cxx20")
set_defaultmode("releasedbg")
add_rules("mode.debug", "mode.releasedbg", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})

-- Single source of the version: version.txt (read by the release workflow).
-- Single source of the version: version.txt. The xmake project sandbox has no
-- file IO, so the release workflow (and scripts/build.ps1) export
-- BVMENV_VERSION before configuring; the fallback keeps local builds working.
local project_version = os.getenv("BVMENV_VERSION") or "0.1.0"
set_version(project_version)

add_requires("gtest 1.17.0")

option("vmware_dir")
    set_default("Y:\\Program Files (x86)\\VMware\\VMware Workstation")
    set_showmenu(true)
    set_description("VMware Workstation installation directory (vmrun/vmcli)")
option_end()

option("guest_drivers")
    set_default(false)
    set_showmenu(true)
    set_description("Build the in-guest test drivers (requires the WDK)")
option_end()

option("wdk_dir")
    set_default("Y:\\Program Files (x86)\\Windows Kits\\10")
    set_showmenu(true)
    set_description("WDK/SDK root (only used for --guest_drivers=y)")
option_end()

option("static_runtime")
    set_default(false)
    set_showmenu(true)
    set_description("Link the MSVC runtime statically (release packaging)")

-- Shared host-side settings. xmake's add_*/set_* act on the enclosing target,
-- so this must be called from inside a target scope.
function host_common(allow_static_runtime)
    set_encodings("utf-8")
    add_defines("UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN", "_CRT_SECURE_NO_WARNINGS")
    add_defines('BVMENV_VERSION="' .. project_version .. '"')
    add_includedirs("include", {public = true})
    add_cxxflags("/utf-8", "/W4", "/permissive-", "/Zc:__cplusplus", "/WX", {tools = "cl"})
    add_syslinks("advapi32", "ole32", "oleaut32", "windowscodecs", "shell32",
                 "user32", "gdi32", "version", "bcrypt", "shlwapi")
    if allow_static_runtime ~= false and has_config("static_runtime") then
        set_runtimes("MT")
    end
end

target("bvmenv-core")
    set_kind("static")
    add_files("src/core/*.cc", "src/vmware/*.cc", "src/state/*.cc", "src/lab/*.cc")
    add_headerfiles("include/blook/bvmenv/*.h")
    host_common()

target("bvmenv")
    set_kind("binary")
    add_deps("bvmenv-core")
    add_files("src/cli/*.cc")
    host_common()

target("bvmenv-tests")
    set_kind("binary")
    add_deps("bvmenv-core")
    add_packages("gtest")
    add_files("tests/unit/*.cc")
    host_common()
    -- forward slashes: the value ends up inside a C string literal, so
    -- backslashes from os.projectdir() would be parsed as escape sequences.
    add_defines('BVMENV_TEST_SOURCE_DIR="' .. os.projectdir():gsub("\\", "/") .. '"')
    add_tests("unit")

-- In-guest user mode helper: proves VT-x/VMX is exposed inside the VM, which a
-- downstream project needs for its own hypervisor.
target("guest-nested-virt-check")
    set_kind("binary")
    set_languages("cxx17")
    add_files("tests/guest/nested-virt-check/*.cc")
    set_encodings("utf-8")
    add_defines("UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN", "_CRT_SECURE_NO_WARNINGS")
    add_cxxflags("/utf-8", "/W4", {tools = "cl"})
    add_syslinks("advapi32")

-- In-guest test drivers: real BSOD / hard hang evidence. Opt-in: they need the
-- WDK and are never part of the host tool.
if has_config("guest_drivers") then
    target("guest-bsod-driver")
        add_rules("wdk.driver", "wdk.env.wdm")
        set_languages("cxx17")
        set_values("wdk.env.winver", "win10_vb")
        set_exceptions("no-cxx")
        add_files("tests/guest/bsod-driver/*.cc")
        add_defines("UNICODE", "_UNICODE", "NOMINMAX", "POOL_NX_OPTIN=1")
        add_cxxflags("/kernel", "/GR-", "/EHs-c-", "/Zc:threadSafeInit-", "/GS-", "/utf-8", {force = true})
        add_ldflags("/INTEGRITYCHECK", "/ENTRY:DriverEntry", {force = true})
        set_encodings("utf-8")

    target("guest-vtx-probe")
        add_rules("wdk.driver", "wdk.env.wdm")
        set_languages("cxx17")
        set_values("wdk.env.winver", "win10_vb")
        set_exceptions("no-cxx")
        add_files("tests/guest/vtx-probe/*.cc")
        add_defines("UNICODE", "_UNICODE", "NOMINMAX", "POOL_NX_OPTIN=1")
        add_cxxflags("/kernel", "/GR-", "/EHs-c-", "/Zc:threadSafeInit-", "/GS-", "/utf-8", {force = true})
        add_ldflags("/INTEGRITYCHECK", "/ENTRY:DriverEntry", {force = true})
        set_encodings("utf-8")

    target("guest-hang-driver")
        add_rules("wdk.driver", "wdk.env.wdm")
        set_languages("cxx17")
        set_values("wdk.env.winver", "win10_vb")
        set_exceptions("no-cxx")
        add_files("tests/guest/hang-driver/*.cc")
        add_defines("UNICODE", "_UNICODE", "NOMINMAX", "POOL_NX_OPTIN=1")
        add_cxxflags("/kernel", "/GR-", "/EHs-c-", "/Zc:threadSafeInit-", "/GS-", "/utf-8", {force = true})
        add_ldflags("/INTEGRITYCHECK", "/ENTRY:DriverEntry", {force = true})
        set_encodings("utf-8")
end
