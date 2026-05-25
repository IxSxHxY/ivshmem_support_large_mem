#include "driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, IVSHMEMQueueInitialize)
#endif

#ifdef _WIN64
// 32bit struct for when a 32bit application sends IOCTL codes
typedef struct IVSHMEM_MMAP32
{
    IVSHMEM_PEERID peerID; // our peer id
    IVSHMEM_SIZE size;     // the size of the memory region
    UINT32 ptr;            // pointer to the memory region
    UINT16 vectors;        // the number of vectors available
} IVSHMEM_MMAP32, *PIVSHMEM_MMAP32;
#endif

// Forwards
static NTSTATUS ioctl_request_peerid(const PDEVICE_CONTEXT DeviceContext,
                                     const size_t OutputBufferLength,
                                     const WDFREQUEST Request,
                                     size_t *BytesReturned);

static NTSTATUS ioctl_request_size(const PDEVICE_CONTEXT DeviceContext,
                                   const size_t OutputBufferLength,
                                   const WDFREQUEST Request,
                                   size_t *BytesReturned);

static NTSTATUS ioctl_request_mmap(const PDEVICE_CONTEXT DeviceContext,
                                   const size_t InputBufferLength,
                                   const size_t OutputBufferLength,
                                   const WDFREQUEST Request,
                                   size_t *BytesReturned,
                                   BOOLEAN ForKernel);

static NTSTATUS ioctl_release_mmap(const PDEVICE_CONTEXT DeviceContext,
                                   const WDFREQUEST Request,
                                   size_t *BytesReturned);

static NTSTATUS ioctl_ring_doorbell(const PDEVICE_CONTEXT DeviceContext,
                                    const size_t InputBufferLength,
                                    const WDFREQUEST Request,
                                    size_t *BytesReturned);

static NTSTATUS ioctl_register_event(const PDEVICE_CONTEXT DeviceContext,
                                     const size_t InputBufferLength,
                                     const WDFREQUEST Request,
                                     size_t *BytesReturned);

static NTSTATUS ioctl_map_prp_list(const PDEVICE_CONTEXT DeviceContext,
                                   const size_t InputBufferLength,
                                   const size_t OutputBufferLength,
                                   const WDFREQUEST Request,
                                   size_t *BytesReturned);

static NTSTATUS ioctl_unmap_prp_list(const PDEVICE_CONTEXT DeviceContext,
                                     const size_t InputBufferLength,
                                     const WDFREQUEST Request,
                                     size_t *BytesReturned);

NTSTATUS IVSHMEMQueueInitialize(_In_ WDFDEVICE Device)
{
    WDFQUEUE queue;
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG queueConfig;

    PAGED_CODE();

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = IVSHMEMEvtIoDeviceControl;
    queueConfig.EvtIoStop = IVSHMEMEvtIoStop;

    status = WdfIoQueueCreate(Device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    return status;
}

VOID IVSHMEMEvtIoDeviceControl(_In_ WDFQUEUE Queue,
                               _In_ WDFREQUEST Request,
                               _In_ size_t OutputBufferLength,
                               _In_ size_t InputBufferLength,
                               _In_ ULONG IoControlCode)
{
    WDFDEVICE hDevice = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT deviceContext = DeviceGetContext(hDevice);
    size_t bytesReturned = 0;
    DEBUG_INFO("Inside IVSHMEMEvtIoDeviceControl");
    // revision 0 devices have to wait until the shared memory has been provided to the vm
    if (deviceContext->devRegisters->ivProvision < 0)
    {
        DEBUG_ERROR("Device not ready yet, ivProvision = %d", deviceContext->devRegisters->ivProvision);
        WdfRequestCompleteWithInformation(Request, STATUS_DEVICE_NOT_READY, 0);
        return;
    }
    DEBUG_INFO("IoControlCode = %x", IoControlCode);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    switch (IoControlCode)
    {
        case IOCTL_IVSHMEM_REQUEST_PEERID:
            status = ioctl_request_peerid(deviceContext, OutputBufferLength, Request, &bytesReturned);
            break;

        case IOCTL_IVSHMEM_REQUEST_SIZE:
            status = ioctl_request_size(deviceContext, OutputBufferLength, Request, &bytesReturned);
            break;

        case IOCTL_IVSHMEM_REQUEST_MMAP:
            status = ioctl_request_mmap(deviceContext,
                                        InputBufferLength,
                                        OutputBufferLength,
                                        Request,
                                        &bytesReturned,
                                        FALSE);
            break;
        case IOCTL_IVSHMEM_REQUEST_KMAP:
            status = ioctl_request_mmap(deviceContext,
                                        InputBufferLength,
                                        OutputBufferLength,
                                        Request,
                                        &bytesReturned,
                                        TRUE);
            break;
        case IOCTL_IVSHMEM_RELEASE_MMAP:
            status = ioctl_release_mmap(deviceContext, Request, &bytesReturned);
            break;

        case IOCTL_IVSHMEM_RING_DOORBELL:
            status = ioctl_ring_doorbell(deviceContext, InputBufferLength, Request, &bytesReturned);
            break;

        case IOCTL_IVSHMEM_REGISTER_EVENT:
            status = ioctl_register_event(deviceContext, InputBufferLength, Request, &bytesReturned);
            break;

        case IOCTL_IVSHMEM_MAP_PRP_LIST:
            status = ioctl_map_prp_list(deviceContext, InputBufferLength, OutputBufferLength, Request, &bytesReturned);
            break;

        case IOCTL_IVSHMEM_UNMAP_PRP_LIST:
            status = ioctl_unmap_prp_list(deviceContext, InputBufferLength, Request, &bytesReturned);
            break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

VOID IVSHMEMEvtIoStop(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ ULONG ActionFlags)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(ActionFlags);
    WdfRequestStopAcknowledge(Request, TRUE);
    return;
}

// VOID IVSHMEMEvtDeviceFileCleanup(_In_ WDFFILEOBJECT FileObject)
// {
//     PDEVICE_CONTEXT deviceContext = DeviceGetContext(WdfFileObjectGetDevice(FileObject));

//     // remove queued events that belonged to the session
//     KIRQL oldIRQL;
//     KeAcquireSpinLock(&deviceContext->eventListLock, &oldIRQL);
//     PLIST_ENTRY entry = deviceContext->eventList.Flink;
//     while (entry != &deviceContext->eventList)
//     {
//         _Analysis_assume_(entry != NULL);
//         PIVSHMEMEventListEntry event = CONTAINING_RECORD(entry, IVSHMEMEventListEntry, ListEntry);
//         if (event->owner != FileObject)
//         {
//             entry = entry->Flink;
//             continue;
//         }

//         PLIST_ENTRY next = entry->Flink;
//         RemoveEntryList(entry);
//         ObDereferenceObject(event->event);
//         event->owner = NULL;
//         event->event = NULL;
//         event->vector = 0;
//         --deviceContext->eventBufferUsed;
//         entry = next;
//     }
//     KeReleaseSpinLock(&deviceContext->eventListLock, oldIRQL);

//     if (!deviceContext->shmemMap)
//     {
//         return;
//     }

//     if (deviceContext->owner != FileObject)
//     {
//         return;
//     }

//     MmUnmapLockedPages(deviceContext->shmemMap, deviceContext->shmemMDL);
//     deviceContext->shmemMap = NULL;
//     deviceContext->owner = NULL;
// }

VOID IVSHMEMEvtDeviceFileCleanup(_In_ WDFFILEOBJECT FileObject)
{
    PDEVICE_CONTEXT deviceContext = DeviceGetContext(WdfFileObjectGetDevice(FileObject));

    KIRQL oldIRQL;
    KeAcquireSpinLock(&deviceContext->eventListLock, &oldIRQL);
    PLIST_ENTRY entry = deviceContext->eventList.Flink;
    while (entry != &deviceContext->eventList)
    {
        PIVSHMEMEventListEntry event = CONTAINING_RECORD(entry, IVSHMEMEventListEntry, ListEntry);
        if (event->owner != FileObject)
        {
            entry = entry->Flink;
            continue;
        }

        PLIST_ENTRY next = entry->Flink;
        RemoveEntryList(entry);
        ObDereferenceObject(event->event);
        event->owner = NULL;
        event->event = NULL;
        event->vector = 0;
        --deviceContext->eventBufferUsed;
        entry = next;
    }
    KeReleaseSpinLock(&deviceContext->eventListLock, oldIRQL);

    if (!deviceContext->shmemMap || deviceContext->owner != FileObject)
    {
        return;
    }

    DEBUG_INFO("Cleaning up SHMEM mappings for FileObject %p", FileObject);

    for (ULONG i = 0; i < 32; i++)
    {
        if (deviceContext->mdlArray[i] != NULL)
        {
            PVOID chunkVA = (PVOID)((SIZE_T)deviceContext->shmemMap + (i * 1ULL * 1024 * 1024 * 1024));

            __try
            {
                MmUnmapLockedPages(chunkVA, deviceContext->mdlArray[i]);
                IoFreeMdl(deviceContext->mdlArray[i]);
                DEBUG_INFO("Unmapped and freed MDL chunk %u at %p", i, chunkVA);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                DEBUG_ERROR("Exception unmapping chunk %u", i);
            }

            deviceContext->mdlArray[i] = NULL;
        }
    }

    deviceContext->shmemMap = NULL;
    deviceContext->shmemMDL = NULL;
    deviceContext->owner = NULL;

    WdfWaitLockAcquire(deviceContext->PrpMapLock, NULL);

    PLIST_ENTRY head = &deviceContext->PrpMapListHead;
    PLIST_ENTRY curr = head->Flink;

    while (curr != head)
    {
        PPRP_MAP_ENTRY pEntry = CONTAINING_RECORD(curr, PRP_MAP_ENTRY, ListEntry);
        PLIST_ENTRY next = curr->Flink;
        if (pEntry->ProcessContext == PsGetCurrentProcess())
        {
            __try
            {
                if (pEntry->UserVa && pEntry->pMdl)
                {
                    MmUnmapLockedPages(pEntry->UserVa, pEntry->pMdl);
                    IoFreeMdl(pEntry->pMdl);
                    DEBUG_INFO("Auto-unmapped PRP MDL at VA %p", pEntry->UserVa);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                DEBUG_ERROR("Exception during auto-cleanup of PRP MDL");
            }

            RemoveEntryList(curr);
            ExFreePool(pEntry);
        }
        curr = next;
    }

    WdfWaitLockRelease(deviceContext->PrpMapLock);

    DEBUG_INFO("SHMEM Cleanup complete.");
}

static NTSTATUS ioctl_request_peerid(const PDEVICE_CONTEXT DeviceContext,
                                     const size_t OutputBufferLength,
                                     const WDFREQUEST Request,
                                     size_t *BytesReturned)
{
    if (OutputBufferLength != sizeof(IVSHMEM_PEERID))
    {
        DEBUG_ERROR("IOCTL_IVSHMEM_REQUEST_PEERID: Invalid size, expected %u but got %u",
                    sizeof(IVSHMEM_PEERID),
                    OutputBufferLength);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    IVSHMEM_PEERID *out = NULL;
    if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, OutputBufferLength, (PVOID *)&out, NULL)))
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_REQUEST_PEERID: Failed to retrieve the output buffer");
        return STATUS_INVALID_USER_BUFFER;
    }

    *out = (IVSHMEM_PEERID)DeviceContext->devRegisters->ivProvision;
    *BytesReturned = sizeof(IVSHMEM_PEERID);
    return STATUS_SUCCESS;
}

static NTSTATUS ioctl_request_size(const PDEVICE_CONTEXT DeviceContext,
                                   const size_t OutputBufferLength,
                                   const WDFREQUEST Request,
                                   size_t *BytesReturned)
{
    if (OutputBufferLength != sizeof(IVSHMEM_SIZE))
    {
        DEBUG_ERROR("IOCTL_IVSHMEM_REQUEST_SIZE: Invalid size, expected %u but got %u",
                    sizeof(IVSHMEM_SIZE),
                    OutputBufferLength);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    IVSHMEM_SIZE *out = NULL;
    if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, OutputBufferLength, (PVOID *)&out, NULL)))
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_REQUEST_SIZE: Failed to retrieve the output buffer");
        return STATUS_INVALID_USER_BUFFER;
    }

    *out = DeviceContext->shmemAddr.NumberOfBytes;
    *BytesReturned = sizeof(IVSHMEM_SIZE);
    return STATUS_SUCCESS;
}

static NTSTATUS ioctl_request_mmap(const PDEVICE_CONTEXT DeviceContext,
                                   const size_t InputBufferLength,
                                   const size_t OutputBufferLength,
                                   const WDFREQUEST Request,
                                   size_t *BytesReturned,
                                   BOOLEAN ForKernel)
{
    // only one mapping at a time is allowed
    if (DeviceContext->shmemMap)
    {
        return STATUS_DEVICE_ALREADY_ATTACHED;
    }

    if (InputBufferLength != sizeof(IVSHMEM_MMAP_CONFIG))
    {
        DEBUG_ERROR("IOCTL_IVSHMEM_MMAP: Invalid input size, expected %u but got %u",
                    sizeof(IVSHMEM_MMAP_CONFIG),
                    InputBufferLength);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    PIVSHMEM_MMAP_CONFIG in;
    if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, InputBufferLength, (PVOID)&in, NULL)))
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_MMAP: Failed to retrieve the input buffer");
        return STATUS_INVALID_USER_BUFFER;
    }

    MEMORY_CACHING_TYPE cacheType;
    switch (in->cacheMode)
    {
        case IVSHMEM_CACHE_NONCACHED:
            cacheType = MmNonCached;
            break;
        case IVSHMEM_CACHE_CACHED:
            cacheType = MmCached;
            break;
        case IVSHMEM_CACHE_WRITECOMBINED:
            cacheType = MmWriteCombined;
            break;
        default:
            DEBUG_ERROR("IOCTL_IVSHMEM_MMAP: Invalid cache mode: %u", in->cacheMode);
            return STATUS_INVALID_PARAMETER;
    }

#ifdef _WIN64
    PIRP irp = WdfRequestWdmGetIrp(Request);
    const BOOLEAN is32Bit = IoIs32bitProcess(irp);
    const size_t bufferLen = is32Bit ? sizeof(IVSHMEM_MMAP32) : sizeof(IVSHMEM_MMAP);
#else
    const size_t bufferLen = sizeof(IVSHMEM_MMAP);
#endif
    PVOID buffer;

    if (OutputBufferLength != bufferLen)
    {
        DEBUG_ERROR("IOCTL_IVSHMEM_REQUEST_MMAP: Invalid size, expected %u but got %u", bufferLen, OutputBufferLength);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, bufferLen, (PVOID *)&buffer, NULL)))
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_REQUEST_MMAP: Failed to retrieve the output buffer");
        return STATUS_INVALID_USER_BUFFER;
    }

    __try
    {
        if (DeviceContext->shmemMDL)
        {
            DeviceContext->shmemMap = MmMapLockedPagesSpecifyCache(DeviceContext->shmemMDL,
                                                                   ForKernel ? KernelMode : UserMode,
                                                                   cacheType,
                                                                   NULL,
                                                                   FALSE,
                                                                   NormalPagePriority | MdlMappingNoExecute);
        }
        else
        {
            if (DeviceContext->shmemMap)
            {
                return STATUS_DEVICE_ALREADY_ATTACHED;
            }

            PHYSICAL_ADDRESS pa = DeviceContext->shmemAddr.PhysicalAddress;
            SIZE_T length = DeviceContext->shmemAddr.NumberOfBytes;
            PVOID base_va = NULL;
            NTSTATUS status;
            DEBUG_INFO("IVSHMEM Debug: pa = 0x%lx, length = %lu", pa.QuadPart, (UINT64)length);

            DEBUG_INFO("IVSHMEM Debug: DeviceContext = %p, BaseVA = %p", DeviceContext, base_va);
            // --- STEP 1: RESERVE ---
            status = ZwAllocateVirtualMemory(ZwCurrentProcess(), &base_va, 0, &length, MEM_RESERVE, PAGE_NOACCESS);

            if (!NT_SUCCESS(status))
            {
                DEBUG_ERROR("ZwAllocateVirtualMemory (Reserve) failed: 0x%08x", status);
                return status;
            }

            // --- STEP 2: FREE (RELEASE) ---
            SIZE_T regionSize = 0;
            status = ZwFreeVirtualMemory(ZwCurrentProcess(), &base_va, &regionSize, MEM_RELEASE);

            if (!NT_SUCCESS(status))
            {
                DEBUG_ERROR("ZwFreeVirtualMemory failed: 0x%08x", status);
                return status;
            }

            // --- STEP 3: ALLOCATE MDLS ---
            SIZE_T CHUNK_SIZE = 1ULL * 1024 * 1024 * 1024; // 1GB
            SIZE_T offset = 0;
            PMDL headMdl = NULL;
            PMDL prevMdl = NULL;
            ULONG count = 0;

            while (offset < length)
            {
                SIZE_T chunkLen = min(CHUNK_SIZE, length - offset);
                MM_PHYSICAL_ADDRESS_LIST addrSpace;
                addrSpace.PhysicalAddress.QuadPart = pa.QuadPart + offset;
                addrSpace.NumberOfBytes = (ULONG)chunkLen;

                PMDL mdl;
                status = MmAllocateMdlForIoSpace(&addrSpace, 1, &mdl);
                if (!NT_SUCCESS(status))
                {
                    // goto ErrorCleanup;
                    DEBUG_ERROR("Not success!");
                    return 0;
                }

                if (!headMdl)
                {
                    headMdl = mdl;
                }
                if (prevMdl)
                {
                    prevMdl->Next = mdl;
                }
                prevMdl = mdl;

                offset += chunkLen;
                count++;
            }

            PVOID currentTargetVA = base_va; //
            PMDL curr = headMdl;             //
            ULONG i = 0;
            offset = 0;

            while (curr != NULL)
            {
                SIZE_T thisChunkLen = min(CHUNK_SIZE, length - offset);
                PVOID mappedChunk = MmMapLockedPagesSpecifyCache(curr,
                                                                 UserMode,
                                                                 cacheType,
                                                                 currentTargetVA, //
                                                                 FALSE,
                                                                 NormalPagePriority | MdlMappingNoExecute);

                if (mappedChunk != currentTargetVA)
                {
                    DEBUG_ERROR("FATAL: Memory discontinuity at chunk %u", i);

                    return STATUS_CONFLICTING_ADDRESSES;
                }

                DEBUG_INFO("Successfully mapped chunk %u at %p", i, mappedChunk);

                if (i < 32)
                {
                    DeviceContext->mdlArray[i] = curr;
                }

                currentTargetVA = (PVOID)((SIZE_T)currentTargetVA + thisChunkLen);
                offset += thisChunkLen;
                curr = curr->Next;
                i++;
            }

            DeviceContext->shmemMDL = headMdl; //
            DeviceContext->shmemMap = base_va; //
            DeviceContext->mdlCount = i;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_REQUEST_MMAP: Exception trying to map pages");
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    if (!DeviceContext->shmemMap)
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_REQUEST_MMAP: shmemMap is NULL");
        return STATUS_DRIVER_INTERNAL_ERROR;
    }

    DeviceContext->owner = WdfRequestGetFileObject(Request);
#ifdef _WIN64
    if (is32Bit)
    {
        PIVSHMEM_MMAP32 out = (PIVSHMEM_MMAP32)buffer;
        out->peerID = (UINT16)DeviceContext->devRegisters->ivProvision;
        out->size = (UINT64)DeviceContext->shmemAddr.NumberOfBytes;
        out->ptr = PtrToUint(DeviceContext->shmemMap);
        out->vectors = DeviceContext->interruptsUsed;
    }
    else
#endif
    {
        PIVSHMEM_MMAP out = (PIVSHMEM_MMAP)buffer;
        out->peerID = (UINT16)DeviceContext->devRegisters->ivProvision;
        out->size = (UINT64)DeviceContext->shmemAddr.NumberOfBytes;
        out->ptr = DeviceContext->shmemMap;
        out->vectors = DeviceContext->interruptsUsed;
    }

    *BytesReturned = bufferLen;
    return STATUS_SUCCESS;
}

static NTSTATUS ioctl_release_mmap(const PDEVICE_CONTEXT DeviceContext, const WDFREQUEST Request, size_t *BytesReturned)
{
    // ensure the mapping exists
    if (!DeviceContext->shmemMap)
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_RELEASE_MMAP: not mapped");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    // ensure someone else other then the owner doesn't attempt to release the mapping
    if (DeviceContext->owner != WdfRequestGetFileObject(Request))
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_RELEASE_MMAP: Invalid owner");
        return STATUS_INVALID_HANDLE;
    }

    // MmUnmapLockedPages(DeviceContext->shmemMap, DeviceContext->shmemMDL);

    for (ULONG i = 0; i < 32; i++)
    {
        if (DeviceContext->mdlArray[i] != NULL)
        {
            PVOID chunkVA = (PVOID)((SIZE_T)DeviceContext->shmemMap + (i * 1ULL * 1024 * 1024 * 1024));

            __try
            {
                MmUnmapLockedPages(chunkVA, DeviceContext->mdlArray[i]);
                IoFreeMdl(DeviceContext->mdlArray[i]);
                DEBUG_INFO("Unmapped and freed MDL chunk %u at %p", i, chunkVA);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                DEBUG_ERROR("Exception unmapping chunk %u", i);
            }

            DeviceContext->mdlArray[i] = NULL;
        }
    }
    DeviceContext->shmemMap = NULL;
    DeviceContext->owner = NULL;
    *BytesReturned = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS ioctl_ring_doorbell(const PDEVICE_CONTEXT DeviceContext,
                                    const size_t InputBufferLength,
                                    const WDFREQUEST Request,
                                    size_t *BytesReturned)
{
    // ensure someone else other then the owner doesn't attempt to trigger IRQs
    if (DeviceContext->owner != WdfRequestGetFileObject(Request))
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_RING_DOORBELL: Invalid owner");
        return STATUS_INVALID_HANDLE;
    }

    if (InputBufferLength != sizeof(IVSHMEM_RING))
    {
        DEBUG_ERROR("IOCTL_IVSHMEM_RING_DOORBELL: Invalid size, expected %u but got %u",
                    sizeof(IVSHMEM_RING),
                    InputBufferLength);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    PIVSHMEM_RING in;
    if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, InputBufferLength, (PVOID)&in, NULL)))
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_RING_DOORBELL: Failed to retrieve the input buffer");
        return STATUS_INVALID_USER_BUFFER;
    }

    WRITE_REGISTER_ULONG(&DeviceContext->devRegisters->doorbell, (ULONG)in->vector | ((ULONG)in->peerID << 16));

    *BytesReturned = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS ioctl_register_event(const PDEVICE_CONTEXT DeviceContext,
                                     const size_t InputBufferLength,
                                     const WDFREQUEST Request,
                                     size_t *BytesReturned)
{
    // ensure someone else other then the owner isn't attempting to register events
    if (DeviceContext->owner != WdfRequestGetFileObject(Request))
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_REGISTER_EVENT: Invalid owner");
        return STATUS_INVALID_HANDLE;
    }

    if (InputBufferLength != sizeof(IVSHMEM_EVENT))
    {
        DEBUG_ERROR("IOCTL_IVSHMEM_REGISTER_EVENT: Invalid size, expected %u but got %u",
                    sizeof(PIVSHMEM_EVENT),
                    InputBufferLength);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    // early non locked quick check to see if we are out of event space
    if (DeviceContext->eventBufferUsed == MAX_EVENTS)
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_REGISTER_EVENT: Event buffer full");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    PIVSHMEM_EVENT in;
    if (!NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, InputBufferLength, (PVOID)&in, NULL)))
    {
        DEBUG_ERROR("%s", "IOCTL_IVSHMEM_REGISTER_EVENT: Failed to retrieve the input buffer");
        return STATUS_INVALID_USER_BUFFER;
    }

    PRKEVENT hObject;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(in->event,
                                              SYNCHRONIZE | EVENT_MODIFY_STATE,
                                              *ExEventObjectType,
                                              UserMode,
                                              &hObject,
                                              NULL)))
    {
        DEBUG_ERROR("%s", "Unable to reference user-mode event object");
        return STATUS_INVALID_HANDLE;
    }

    // clear the event in case the caller didn't think to
    KeClearEvent(hObject);

    // lock the event list so we can push the new entry into it
    KIRQL oldIRQL;
    KeAcquireSpinLock(&DeviceContext->eventListLock, &oldIRQL);
    {
        // check again if there is space before we search as we now hold the lock
        if (DeviceContext->eventBufferUsed == MAX_EVENTS)
        {
            KeReleaseSpinLock(&DeviceContext->eventListLock, oldIRQL);

            DEBUG_ERROR("%s", "IOCTL_IVSHMEM_REGISTER_EVENT: Event buffer full");
            ObDereferenceObject(hObject);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        // look for a free slot
        BOOLEAN done = FALSE;
        for (UINT16 i = 0; i < MAX_EVENTS; ++i)
        {
            PIVSHMEMEventListEntry event = &DeviceContext->eventBuffer[i];
            if (event->event != NULL)
            {
                continue;
            }

            // found one, assign the event to it and add it to the list
            event->owner = WdfRequestGetFileObject(Request);
            event->event = hObject;
            event->vector = in->vector;
            event->singleShot = in->singleShot;
            ++DeviceContext->eventBufferUsed;
            InsertTailList(&DeviceContext->eventList, &event->ListEntry);
            done = TRUE;
            break;
        }

        // this should never occur, if it does it indicates memory corruption
        if (!done)
        {
            DEBUG_ERROR("IOCTL_IVSHMEM_REGISTER_EVENT: deviceContext->eventBufferUsed (%u) < MAX_EVENTS (%u) but no "
                        "slots found!",
                        DeviceContext->eventBufferUsed,
                        MAX_EVENTS);
            KeBugCheckEx(CRITICAL_STRUCTURE_CORRUPTION, 0, 0, 0, 0x1C);
        }
    }
    KeReleaseSpinLock(&DeviceContext->eventListLock, oldIRQL);

    *BytesReturned = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS ioctl_map_prp_list(const PDEVICE_CONTEXT DeviceContext,
                                   const size_t InputBufferLength,
                                   const size_t OutputBufferLength,
                                   const WDFREQUEST Request,
                                   size_t *BytesReturned)
{
    NTSTATUS status;
    PIVSHMEM_MAP_REQUEST pReq;
    PIVSHMEM_PRP_MAP_RESPONSE pRes;
    PMDL pMdl = NULL;
    PVOID userVa = NULL;
    MM_PHYSICAL_ADDRESS_LIST paList[MAX_PRP_ENTRIES];
    DEBUG_INFO("Inside ioctl_map_prp_list!");
    *BytesReturned = 0;
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    size_t InputSize = 0, OutputSize = 0;
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(IVSHMEM_MAP_REQUEST), (PVOID *)&pReq, &InputSize);
    DEBUG_INFO("[ioctl_map_prp_list] Input Original = %ld | Retrieved = %ld", sizeof(IVSHMEM_MAP_REQUEST), InputSize);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
#ifdef _WIN64
    PIRP irp = WdfRequestWdmGetIrp(Request);
    const BOOLEAN is32Bit = IoIs32bitProcess(irp);
    const size_t outputLen = is32Bit ? sizeof(IVSHMEM_PRP_MAP_RESPONSE32) : sizeof(IVSHMEM_PRP_MAP_RESPONSE);
#else
    const size_t outputLen = sizeof(IVSHMEM_PRP_MAP_RESPONSE);
#endif
    status = WdfRequestRetrieveOutputBuffer(Request, outputLen, (PVOID *)&pRes, &OutputSize);
    DEBUG_INFO("[ioctl_map_prp_list] Output Original = %ld | Retrieved = %ld", outputLen, OutputSize);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (pReq->PageCount == 0 || pReq->PageCount > MAX_PRP_ENTRIES)
    {
        return STATUS_INVALID_PARAMETER;
    }
    DEBUG_INFO("[ioctl_map_prp_list] Page Count = %lld", pReq->PageCount);
    for (ULONG i = 0; i < pReq->PageCount; i++)
    {
        // paList[i].PhysicalAddress.QuadPart = pReq->PhysAddrList[i];
        paList[i].PhysicalAddress.QuadPart = DeviceContext->shmemAddr.PhysicalAddress.QuadPart + pReq->PhysAddrList[i];
        paList[i].NumberOfBytes = 4096;
        DEBUG_INFO("[ioctl_map_prp_list] Addr = 0x%llx", paList[i].PhysicalAddress.QuadPart);
    }

    status = MmAllocateMdlForIoSpace(paList, pReq->PageCount, &pMdl);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    __try
    {
        userVa = MmMapLockedPagesSpecifyCache(pMdl, UserMode, MmNonCached, NULL, FALSE, NormalPagePriority);

        PPRP_MAP_ENTRY pEntry = (PPRP_MAP_ENTRY)ExAllocatePoolWithTag(NonPagedPool, sizeof(PRP_MAP_ENTRY), 'PRPE');
        if (pEntry)
        {
            pEntry->pMdl = pMdl;
            pEntry->UserVa = userVa;
            pEntry->ProcessContext = PsGetCurrentProcess();

            WdfWaitLockAcquire(DeviceContext->PrpMapLock, NULL);
            InsertTailList(&DeviceContext->PrpMapListHead, &pEntry->ListEntry);
            WdfWaitLockRelease(DeviceContext->PrpMapLock);
        }
        DEBUG_INFO("[ioctl_map_prp_list] userVa = %p", userVa);

#ifdef _WIN64
        if (is32Bit)
        {
            PIVSHMEM_PRP_MAP_RESPONSE32 pRes32 = (PIVSHMEM_PRP_MAP_RESPONSE32)pRes;
            pRes32->UserVa = PtrToUint(userVa);
        }
        else
#endif
        {
            pRes->UserVa = userVa;
        }

        *BytesReturned = outputLen;
        status = STATUS_SUCCESS;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (pMdl)
        {
            IoFreeMdl(pMdl);
        }
        status = STATUS_ACCESS_DENIED;
    }

    return status;
}

static NTSTATUS ioctl_unmap_prp_list(const PDEVICE_CONTEXT DeviceContext,
                                     const size_t InputBufferLength,
                                     const WDFREQUEST Request,
                                     size_t *BytesReturned)
{
    NTSTATUS status = STATUS_NOT_FOUND;
    PIVSHMEM_PRP_UNMAP_REQUEST pReq;
    PLIST_ENTRY curr, head;
    DEBUG_INFO("Inside ioctl_unmap_prp_list!");
    *BytesReturned = 0;
    UNREFERENCED_PARAMETER(InputBufferLength);
#ifdef _WIN64
    PIRP irp = WdfRequestWdmGetIrp(Request);
    const BOOLEAN is32Bit = IoIs32bitProcess(irp);
    const size_t inputLen = is32Bit ? sizeof(IVSHMEM_PRP_UNMAP_REQUEST32) : sizeof(IVSHMEM_PRP_UNMAP_REQUEST);
#else
    const size_t inputLen = sizeof(IVSHMEM_PRP_UNMAP_REQUEST);
#endif
    size_t InputSize = 0;
    status = WdfRequestRetrieveInputBuffer(Request, inputLen, (PVOID *)&pReq, &InputSize);
    DEBUG_INFO("[ioctl_unmap_prp_list] Input Original = %ld | Retrieved = %ld", inputLen, InputSize);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WdfWaitLockAcquire(DeviceContext->PrpMapLock, NULL);

    head = &DeviceContext->PrpMapListHead;
    curr = head->Flink;

    while (curr != head)
    {
        PPRP_MAP_ENTRY pEntry = CONTAINING_RECORD(curr, PRP_MAP_ENTRY, ListEntry);
        if (pEntry->UserVa == pReq->UserVa)
        {
            DEBUG_INFO("[ioctl_unmap_prp_list] Removing UserVA=%p | MdlContext=%p", pEntry->UserVa, pEntry->pMdl);
            MmUnmapLockedPages(pEntry->UserVa, pEntry->pMdl);
            IoFreeMdl(pEntry->pMdl);

            RemoveEntryList(curr);
            ExFreePool(pEntry);
            status = STATUS_SUCCESS;
            break;
        }
        curr = curr->Flink;
    }
    DEBUG_INFO("[ioctl_unmap_prp_list] PRP List unmapped successfully!");
    WdfWaitLockRelease(DeviceContext->PrpMapLock);
    return status;
}