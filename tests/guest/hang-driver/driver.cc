// blook-vmware-environment: in-guest test driver that hard-hangs the system.
//
// DriverEntry returns successfully, then a system thread raises IRQL to
// HIGH_LEVEL and spins. The guest stops scheduling anything, VMware Tools stops
// answering and the framebuffer freezes - exactly the failure mode
// `bvmenv state` has to classify without guest cooperation.
//
//   sc create bvmenohang type= kernel binPath= C:\Windows\System32\drivers\bvmenohang.sys
//   sc start bvmenohang
//
// The delay before the hang can be tuned through
//   HKLM\SYSTEM\CurrentControlSet\Services\bvmenohang\Parameters\DelaySeconds
#include <ntddk.h>

extern "C" {
DRIVER_INITIALIZE DriverEntry;
}

namespace {

ULONG read_delay_seconds(PUNICODE_STRING registry_path) {
    ULONG seconds = 3;
    HANDLE key = nullptr;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, registry_path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);
    if (NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &attributes))) {
        UNICODE_STRING value_name;
        RtlInitUnicodeString(&value_name, L"DelaySeconds");
        UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)] = {};
        ULONG length = 0;
        if (NT_SUCCESS(ZwQueryValueKey(key, &value_name, KeyValuePartialInformation, buffer,
                                       sizeof(buffer), &length))) {
            auto* info = reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION*>(buffer);
            if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG)) {
                seconds = *reinterpret_cast<ULONG*>(info->Data);
            }
        }
        ZwClose(key);
    }
    if (seconds == 0)
        seconds = 1;
    return seconds;
}

VOID hang_thread(_In_ PVOID context) {
    const ULONG seconds = static_cast<ULONG>(reinterpret_cast<ULONG_PTR>(context));
    LARGE_INTEGER interval;
    interval.QuadPart = -static_cast<LONGLONG>(seconds) * 10 * 1000 * 1000;  // 100ns units
    KeDelayExecutionThread(KernelMode, FALSE, &interval);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "bvmenohang: raising IRQL to HIGH_LEVEL and spinning forever\n");
    KIRQL previous = PASSIVE_LEVEL;
    KeRaiseIrql(HIGH_LEVEL, &previous);
    for (;;) {
        KeStallExecutionProcessor(1000);
    }
}

}  // namespace

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driver_object,
                                _In_ PUNICODE_STRING registry_path) {
    driver_object->DriverUnload = nullptr;
    const ULONG seconds = read_delay_seconds(registry_path);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "bvmenohang: loaded, hanging in %lu second(s)\n", seconds);
    HANDLE thread = nullptr;
    const NTSTATUS status =
        PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, nullptr, nullptr, nullptr, hang_thread,
                             reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(seconds)));
    if (NT_SUCCESS(status)) {
        ZwClose(thread);
        return STATUS_SUCCESS;
    }
    return status;
}
