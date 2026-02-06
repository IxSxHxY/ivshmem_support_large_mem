#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>

#define IVSHMEM_NONPAGED_POOL NonPagedPoolNx

#include "device.h"
#include "queue.h"

// using error levels to avoid the debug print filter
#define DEBUG_ERROR(fmt, ...)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[E:IVSHMEM] " fmt "\n", ##__VA_ARGS__));                  \
    } while (0)
#define DEBUG_INFO(fmt, ...)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[I:IVSHMEM] " fmt "\n", ##__VA_ARGS__));                  \
    } while (0)
#define DEBUG_PRINT(fmt, ...)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        DbgPrint("[P:IVSHMEM] " fmt "\n", ##__VA_ARGS__);                                                              \
    } while (0)

EXTERN_C_START

//
// WDFDRIVER Events
//
NTSYSAPI
NTSTATUS
NTAPI
ZwAllocateVirtualMemory(
    _In_ HANDLE ProcessHandle,
    _Inout_ PVOID *BaseAddress,
    _In_ ULONG_PTR ZeroBits,
    _Inout_ PSIZE_T RegionSize,
    _In_ ULONG AllocationType,
    _In_ ULONG Protect
);

NTSYSAPI
NTSTATUS
NTAPI
ZwFreeVirtualMemory(
    _In_ HANDLE ProcessHandle,
    _Inout_ PVOID *BaseAddress,
    _Inout_ PSIZE_T RegionSize,
    _In_ ULONG FreeType
);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD IVSHMEMEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP IVSHMEMEvtDriverContextCleanup;

EXTERN_C_END
