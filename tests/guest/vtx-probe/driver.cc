// blook-vmware-environment: in-guest VT-x exposure probe.
//
// User mode cannot read IA32_FEATURE_CONTROL, and Win32_Processor/
// PF_VIRT_FIRMWARE_ENABLED are not trustworthy inside a VM, so a downstream
// hypervisor project needs ground truth. This driver reads, in ring 0:
//
//   * CPUID.1:ECX[5]          - VMX present
//   * CPUID.1:ECX[31]         - hypervisor already present
//   * IA32_FEATURE_CONTROL    - bit 0 lock, bit 2 VMXON outside SMX
//   * IA32_VMX_BASIC (0x480)  - VMCS revision id / supported memory type
//
// and stores the results under
//   HKLM\SOFTWARE\blook\vtx-probe
// so the user mode verification script (and `bvmenv base verify`) can assert
// that nested virtualisation is actually usable in the guest.
#include <ntddk.h>
#include <intrin.h>

extern "C" {
DRIVER_INITIALIZE DriverEntry;
}

namespace {

struct CpuidRegs {
    unsigned int eax, ebx, ecx, edx;
};

CpuidRegs cpuid(unsigned int leaf, unsigned int subleaf) {
    int regs[4] = {};
    __cpuidex(regs, static_cast<int>(leaf), static_cast<int>(subleaf));
    return {static_cast<unsigned int>(regs[0]), static_cast<unsigned int>(regs[1]),
            static_cast<unsigned int>(regs[2]), static_cast<unsigned int>(regs[3])};
}

void set_value(HANDLE key, PCWSTR name, ULONG value) {
    UNICODE_STRING value_name;
    RtlInitUnicodeString(&value_name, name);
    ZwSetValueKey(key, &value_name, 0, REG_DWORD, &value, sizeof(value));
}

}  // namespace

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driver_object,
                                _In_ PUNICODE_STRING registry_path) {
    UNREFERENCED_PARAMETER(driver_object);
    UNREFERENCED_PARAMETER(registry_path);

    const CpuidRegs leaf1 = cpuid(1, 0);
    const bool vmx = (leaf1.ecx & (1u << 5)) != 0;
    const bool hypervisor_present = (leaf1.ecx & (1u << 31)) != 0;

    unsigned long long feature_control = 0;
    unsigned long long vmx_basic = 0;
    bool locked = false;
    bool vmxon_outside_smx = false;
    __try {
        feature_control = __readmsr(0x3A);  // IA32_FEATURE_CONTROL
        vmx_basic = __readmsr(0x480);       // IA32_VMX_BASIC
        locked = (feature_control & 1ull) != 0;
        vmxon_outside_smx = (feature_control & (1ull << 2)) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // MSR access can fault on exotic configurations; report zeros.
    }

    // ZwCreateKey creates only the leaf, so ...\blook must exist first: without
    // this the probe ran but never wrote its results.
    UNICODE_STRING parent_path;
    RtlInitUnicodeString(&parent_path, L"\\Registry\\Machine\\Software\\blook");
    OBJECT_ATTRIBUTES parent_attributes;
    InitializeObjectAttributes(&parent_attributes, &parent_path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    HANDLE parent_key = nullptr;
    if (NT_SUCCESS(ZwCreateKey(&parent_key, KEY_WRITE, &parent_attributes, 0, nullptr,
                               REG_OPTION_NON_VOLATILE, nullptr))) {
        ZwClose(parent_key);
    }

    UNICODE_STRING key_path;
    RtlInitUnicodeString(&key_path, L"\\Registry\\Machine\\Software\\blook\\vtx-probe");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &key_path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);
    HANDLE key = nullptr;
    NTSTATUS status =
        ZwCreateKey(&key, KEY_WRITE, &attributes, 0, nullptr, REG_OPTION_NON_VOLATILE, nullptr);
    if (NT_SUCCESS(status)) {
        set_value(key, L"CpuidVmx", vmx ? 1 : 0);
        set_value(key, L"HypervisorPresent", hypervisor_present ? 1 : 0);
        set_value(key, L"FeatureControlLocked", locked ? 1 : 0);
        set_value(key, L"VmxOnOutsideSmx", vmxon_outside_smx ? 1 : 0);
        set_value(key, L"FeatureControlLow", static_cast<ULONG>(feature_control & 0xFFFFFFFFull));
        set_value(key, L"VmxBasicLow", static_cast<ULONG>(vmx_basic & 0xFFFFFFFFull));
        set_value(key, L"ProbeResult", (vmx && locked && vmxon_outside_smx) ? 1 : 0);
        ZwClose(key);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "bvmenv-vtx: vmx=%d lock=%d vmxon_outside_smx=%d feature_control=0x%llx\n", vmx,
                   locked, vmxon_outside_smx, feature_control);
    }
    return STATUS_SUCCESS;  // unloadable on purpose: no DriverUnload needed
}
