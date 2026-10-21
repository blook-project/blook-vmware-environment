// blook-vmware-environment: host disk safety tests.
//
// Regression: a bring-up helper once enumerated file-backed virtual disks and
// detached the user's 500 GB DevDrive (D:). These tests pin the allowlist that
// replaces it: bvmenv may only ever touch its own images by explicit path.
#include <gtest/gtest.h>

#include <filesystem>

#include "blook/bvmenv/lab.h"
#include "blook/bvmenv/util.h"

using namespace blook::bvmenv;

TEST(Safety, OwnBaseImageIsAllowed) {
    const fs::path root = L"C:\\blook-vms";
    EXPECT_TRUE(is_owned_vhd_path(root, root / L"base" / L"blook-base.vhd"));
    EXPECT_TRUE(is_owned_vhd_path(root, root / L"base" / L"answer.vhd"));
}

TEST(Safety, ForeignDisksAreRejected) {
    const fs::path root = L"C:\\blook-vms";
    // The exact incident: the user's DevDrive is a file-backed VHD on C:.
    EXPECT_FALSE(is_owned_vhd_path(root, L"C:\\DevDrive.vhdx"));
    EXPECT_FALSE(is_owned_vhd_path(root, L"C:\\DevDrive.vhdx"));
    EXPECT_FALSE(is_owned_vhd_path(root, L"D:\\something.vhd"));
    EXPECT_FALSE(is_owned_vhd_path(root, L"C:\\Windows\\system.vhd"));
}

TEST(Safety, OutsideVmRootIsRejected) {
    const fs::path root = L"C:\\blook-vms";
    EXPECT_FALSE(is_owned_vhd_path(root, L"C:\\other\\blook-base.vhd"));
    EXPECT_FALSE(is_owned_vhd_path(root, L"C:\\blook-vms2\\base\\blook-base.vhd"));
    // Path traversal must not escape the root.
    EXPECT_FALSE(is_owned_vhd_path(root, L"C:\\blook-vms\\base\\..\\..\\DevDrive.vhdx"));
}

TEST(Safety, OwnNamesOutsideBaseStillAllowedOnlyInsideRoot) {
    const fs::path root = L"C:\\blook-vms";
    // Same file name but inside the root is fine (the tool only creates it there).
    EXPECT_TRUE(is_owned_vhd_path(root, L"C:\\blook-vms\\vms\\vm-a\\blook-base.vhd"));
    // Case insensitive on Windows paths.
    EXPECT_TRUE(is_owned_vhd_path(root, L"C:\\BLOOK-VMS\\BASE\\BLOOK-BASE.VHD"));
}

TEST(Safety, DetachRefusesForeignPath) {
    Config cfg = Config::load();
    cfg.vm_root = L"C:\\blook-vms";
    // Must throw before touching diskpart.
    EXPECT_THROW(detach_own_vhd(cfg, L"C:\\DevDrive.vhdx"), Error);
    EXPECT_THROW(detach_own_vhd(cfg, L"D:\\anything.vhd"), Error);
}

TEST(Safety, DetachRefusesMissingOwnImage) {
    Config cfg = Config::load();
    cfg.vm_root = fs::temp_directory_path() / "bvmenv-safety-tests";
    ensure_dir(cfg.vm_root / "base");
    EXPECT_THROW(detach_own_vhd(cfg, cfg.vm_root / "base" / "blook-base.vhd"), Error);
}
