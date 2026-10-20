// blook-vmware-environment: in-guest test driver that bugchecks on load.
//
// Used by the lab to prove that host-side state classification detects a real
// BSOD (and that `bvmenv reset` recovers from it). Load it with:
//   sc create bvmenobsod type= kernel binPath= C:\Windows\System32\drivers\bvmenobsod.sys
//   sc start bvmenobsod
//
// The default bugcheck is 0xD1 DRIVER_IRQL_NOT_LESS_OR_EQUAL. A different code
// can be selected through the service image path's registry parameters:
//   HKLM\SYSTEM\CurrentControlSet\Services\bvmenobsod\Parameters\BugCheckCode
#include <ntddk.h>

#define BVMENO_TAG 'dsob'

extern "C" {
DRIVER_INITIALIZE DriverEntry;
}

namespace {

ULONG read_bugcheck_code(PUNICODE_STRING registry_path) {
    // Default: DRIVER_IRQL_NOT_LESS_OR_EQUAL
    ULONG code = 0xD1;
    HANDLE key = nullptr;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, registry_path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);
    if (NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &attributes))) {
        UNICODE_STRING value_name;
        RtlInitUnicodeString(&value_name, L"BugCheckCode");
        UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)] = {};
        ULONG length = 0;
        if (NT_SUCCESS(ZwQueryValueKey(key, &value_name, KeyValuePartialInformation, buffer,
                                       sizeof(buffer), &length))) {
            auto* info = reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION*>(buffer);
            if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG)) {
                code = *reinterpret_cast<ULONG*>(info->Data);
            }
        }
        ZwClose(key);
    }
    return code;
}

}  // namespace

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driver_object,
                                _In_ PUNICODE_STRING registry_path) {
    driver_object->DriverUnload = nullptr;
    const ULONG code = read_bugcheck_code(registry_path);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "bvmenobsod: triggering bugcheck 0x%08X from DriverEntry\n", code);
    // Parameters 2..5 are the classic "driver data" values; parameter 1 is a
    // magic marker so the crash is recognisable in the minidump / event log.
    KeBugCheckEx(code, 0xB10CB10C, reinterpret_cast<ULONG_PTR>(driver_object), 0, 0);
    return STATUS_SUCCESS;  // not reached
}
