-- xmake package manifest for blook-vmware-environment.
--
-- Published as a release asset (blook-vmware-environment-<version>-windows-x64.zip)
-- and mirrored into https://github.com/blook-project/blook-xmake-repo by
-- scripts/publish-registry.ps1.
--
-- Downstream usage:
--   add_repositories("blook https://github.com/blook-project/blook-xmake-repo.git")
--   add_requires("blook-vmware-environment 0.1.0")
--   target("tests"):add_packages("blook-vmware-environment")
--
-- The package installs the `bvmenv` CLI plus the guest payload. It is a *host
-- tool*, not a linkable library, so downstream projects either
--   1. consume the executable at runtime (bvmenv.exe + guest/ payload), or
--   2. depend on the repo checkout directly during development:
--        xmake f --bvmenv_dir=D:\blook-kernel-project\blook-vmware-environment
package("blook-vmware-environment")
    set_homepage("https://github.com/blook-project/blook-vmware-environment")
    set_description("VMware Workstation lab: base image, linked clones, guest exec, host-side BSOD/hang state")
    set_license("MIT")

    set_urls("https://github.com/blook-project/blook-vmware-environment/releases/download/v$(version)/blook-vmware-environment-$(version)-windows-x64.zip")

    add_versions("0.1.0", "<sha256-placeholder-updated-by-publish-registry>")

    on_install("windows|x64", function (package)
        local bindir = path.join(package:installdir(), "bin")
        os.mkdir(bindir)
        os.cp("bin/x64/bvmenv.exe", bindir)
        -- Guest payload is required at runtime: keep it next to the binary.
        if os.isdir("guest") then
            os.cp("guest", path.join(package:installdir(), "guest"))
        end
        if os.isdir("docs") then
            os.cp("docs", path.join(package:installdir(), "docs"))
        end
        package:addenv("PATH", "bin")
    end)

    on_test(function (package)
        os.vrun("bvmenv --version")
    end)
package_end()
