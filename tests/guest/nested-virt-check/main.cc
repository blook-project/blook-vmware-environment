// blook-vmware-environment: verify that the guest exposes VT-x/VMX.
//
// One downstream project runs its own hypervisor inside the VM, so the base
// image must expose VT-x (vhv.enable = "TRUE"). This helper is a user mode
// check that needs no driver:
//   * CPUID.1:ECX.VMX[5]      - the CPU advertises VMX
//   * CPUID.0x40000000         - hypervisor vendor (VMwareVMware) / cpu flag
//   * IsProcessorFeaturePresent(PF_VIRT_FIRMWARE_ENABLED)
//     - firmware has VT-x enabled (not disabled by BIOS/hypervisor)
// Exit code 0 when VT-x looks usable, 1 otherwise. Machine readable on stdout.
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

struct CpuidResult {
    unsigned int eax;
    unsigned int ebx;
    unsigned int ecx;
    unsigned int edx;
};

CpuidResult cpuid(unsigned int leaf, unsigned int subleaf = 0) {
    CpuidResult result{};
#if defined(_MSC_VER)
    int regs[4];
    __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
    result.eax = static_cast<unsigned int>(regs[0]);
    result.ebx = static_cast<unsigned int>(regs[1]);
    result.ecx = static_cast<unsigned int>(regs[2]);
    result.edx = static_cast<unsigned int>(regs[3]);
#else
    __asm__ volatile("cpuid"
                     : "=a"(result.eax), "=b"(result.ebx), "=c"(result.ecx), "=d"(result.edx)
                     : "a"(leaf), "c"(subleaf));
#endif
    return result;
}

// Reads a 4-byte vendor string out of a CPUID leaf.
void vendor_string(unsigned int leaf, char out[13]) {
    const CpuidResult r = cpuid(leaf);
    std::memcpy(out + 0, &r.ebx, 4);
    std::memcpy(out + 4, &r.edx, 4);
    std::memcpy(out + 8, &r.ecx, 4);
    out[12] = '\0';
}

}  // namespace

int main() {
    const CpuidResult leaf1 = cpuid(1);
    const bool vmx = (leaf1.ecx & (1u << 5)) != 0;
    const bool hypervisor_present = (leaf1.ecx & (1u << 31)) != 0;

    char hypervisor_vendor[13] = {0};
    if (hypervisor_present)
        vendor_string(0x40000000, hypervisor_vendor);

    const BOOL firmware_virt = IsProcessorFeaturePresent(20 /* PF_VIRT_FIRMWARE_ENABLED */);

    std::printf("VMX=%d\n", vmx ? 1 : 0);
    std::printf("HYPERVISOR_PRESENT=%d\n", hypervisor_present ? 1 : 0);
    std::printf("HYPERVISOR_VENDOR=%s\n", hypervisor_present ? hypervisor_vendor : "none");
    std::printf("FIRMWARE_VIRTUALIZATION_ENABLED=%d\n", firmware_virt ? 1 : 0);
    std::printf("CPUID_LEAF=%08x\n", leaf1.eax);

    const bool ok = vmx && firmware_virt;
    std::printf("RESULT=%s\n", ok ? "ok" : "fail");
    return ok ? 0 : 1;
}
