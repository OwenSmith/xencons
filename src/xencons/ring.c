/* Copyright (c) Citrix Systems Inc.
* All rights reserved.
*
* Redistribution and use in source and binary forms,
* with or without modification, are permitted provided
* that the following conditions are met:
*
* *   Redistributions of source code must retain the above
*     copyright notice, this list of conditions and the
*     following disclaimer.
* *   Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the
*     following disclaimer in the documentation and/or other
*     materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
* CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
* INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
* CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGE.
*/

#include <ntddk.h>
#include <ntstrsafe.h>
#include <stdlib.h>
#include <xen.h>

#include <debug_interface.h>
#include <store_interface.h>
#include <evtchn_interface.h>
#include <gnttab_interface.h>

#include "frontend.h"
#include "ring.h"
#include "names.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

typedef struct _XENCONS_CSQ {
    IO_CSQ                      Csq;
    LIST_ENTRY                  List;
    KSPIN_LOCK                  Lock;
} XENCONS_CSQ, *PXENCONS_CSQ;

struct _XENCONS_RING {
    PXENCONS_FRONTEND           Frontend;
    KSPIN_LOCK                  Lock;
    PMDL                        Mdl;
    struct xencons_interface    *Shared;
    PXENBUS_GNTTAB_ENTRY        Entry;
    BOOLEAN                     Connected;
    BOOLEAN                     Enabled;
    KDPC                        Dpc;
    ULONG                       Dpcs;
    PXENBUS_EVTCHN_CHANNEL      Channel;
    ULONG                       Events;
    XENBUS_STORE_INTERFACE      StoreInterface;
    XENBUS_GNTTAB_INTERFACE     GnttabInterface;
    XENBUS_EVTCHN_INTERFACE     EvtchnInterface;
    XENBUS_DEBUG_INTERFACE      DebugInterface;
    PXENBUS_DEBUG_CALLBACK      DebugCallback;
    PXENBUS_GNTTAB_CACHE        GnttabCache;
    XENCONS_CSQ                 Read;
    XENCONS_CSQ                 Write;
};

#define RING_TAG  'GNIR'
#define MAXNAMELEN  128

static FORCEINLINE PVOID
__RingAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, RING_TAG);
}

static FORCEINLINE VOID
__RingFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, RING_TAG);
}

__drv_requiresIRQL(DISPATCH_LEVEL)
RingAcquireLock(
    IN  PXENCONS_RING   Ring
    )
{
    ASSERT3U(KeGetCurrentIrql(), == , DISPATCH_LEVEL);

    KeAcquireSpinLockAtDpcLevel(&Ring->Lock);
}

__drv_requiresIRQL(DISPATCH_LEVEL)
RingReleaseLock(
    IN  PXENCONS_RING   Ring
    )
{
    ASSERT3U(KeGetCurrentIrql(), == , DISPATCH_LEVEL);

    KeReleaseSpinLockFromDpcLevel(&Ring->Lock);
}

IO_CSQ_INSERT_IRP_EX RingCsqInsertIrpEx;

NTSTATUS
RingCsqInsertIrpEx(
    IN  PIO_CSQ         Csq,
    IN  PIRP            Irp,
    IN  PVOID           InsertContext OPTIONAL
    )
{
    BOOLEAN             ReInsert = (BOOLEAN)(ULONG_PTR)InsertContext;
    PXENCONS_CSQ        Queue;

    Queue = CONTAINING_RECORD(Csq, XENCONS_CSQ, Csq);

    if (ReInsert) {
        // This only occurs if the worker thread de-queued the IRP but
        // then found the console to be blocked.
        InsertHeadList(&Queue->List, &Irp->Tail.Overlay.ListEntry);
    } else {
        InsertTailList(&Queue->List, &Irp->Tail.Overlay.ListEntry);
    }

    return STATUS_SUCCESS;
}

IO_CSQ_REMOVE_IRP RingCsqRemoveIrp;

VOID
RingCsqRemoveIrp(
    IN  PIO_CSQ     Csq,
    IN  PIRP        Irp
    )
{
    UNREFERENCED_PARAMETER(Csq);

    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

IO_CSQ_PEEK_NEXT_IRP RingCsqPeekNextIrp;

PIRP
RingCsqPeekNextIrp(
    IN  PIO_CSQ     Csq,
    IN  PIRP        Irp,
    IN  PVOID       PeekContext OPTIONAL
    )
{
    PXENCONS_CSQ    Queue;
    PLIST_ENTRY     ListEntry;
    PIRP            NextIrp;

    Queue = CONTAINING_RECORD(Csq, XENCONS_CSQ, Csq);

    ListEntry = (Irp == NULL) ?
            Queue->List.Flink :
            Irp->Tail.Overlay.ListEntry.Flink;

    if (ListEntry == &Queue->List)
        return NULL;

    NextIrp = CONTAINING_RECORD(ListEntry, IRP, Tail.Overlay.ListEntry);
    if (PeekContext == NULL)
        return NextIrp;

    for (;;) {
        PIO_STACK_LOCATION  StackLocation;

        if (ListEntry == &Queue->List)
            return NULL;

        StackLocation = IoGetCurrentIrpStackLocation(NextIrp);

        if (StackLocation->FileObject == PeekContext)
            return NextIrp;

        ListEntry = ListEntry->Flink;
        NextIrp = CONTAINING_RECORD(ListEntry, IRP, Tail.Overlay.ListEntry);
    }
    // unreachable
}

#pragma warning(push)
#pragma warning(disable:28167) // function changes IRQL

IO_CSQ_ACQUIRE_LOCK RingCsqAcquireLock;

VOID
RingCsqAcquireLock(
    IN  PIO_CSQ     Csq,
    OUT PKIRQL      Irql
    )
{
    PXENCONS_CSQ    Queue;

    Queue = CONTAINING_RECORD(Csq, XENCONS_CSQ, Csq);

    KeAcquireSpinLock(&Queue->Lock, Irql);
}

IO_CSQ_RELEASE_LOCK RingCsqReleaseLock;

VOID
RingCsqReleaseLock(
    IN  PIO_CSQ     Csq,
    IN  KIRQL       Irql
    )
{
    PXENCONS_CSQ    Queue;

    Queue = CONTAINING_RECORD(Csq, XENCONS_CSQ, Csq);

    KeReleaseSpinLock(&Queue->Lock, Irql);
}

#pragma warning(pop)

IO_CSQ_COMPLETE_CANCELED_IRP RingCsqCompleteCanceledIrp;

VOID
RingCsqCompleteCanceledIrp(
    IN  PIO_CSQ         Csq,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MajorFunction;

    UNREFERENCED_PARAMETER(Csq);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MajorFunction = StackLocation->MajorFunction;

    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = STATUS_CANCELLED;

    Trace("CANCELLED (%02x:%s)\n",
          MajorFunction,
          MajorFunctionName(MajorFunction));

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static FORCEINLINE NTSTATUS
__RingCsqCreate(
    IN  PXENCONS_CSQ    Csq
    )
{
    NTSTATUS            status;

    KeInitializeSpinLock(&Csq->Lock);
    InitializeListHead(&Csq->List);

    status = IoCsqInitializeEx(&Csq->Csq,
                                RingCsqInsertIrpEx,
                                RingCsqRemoveIrp,
                                RingCsqPeekNextIrp,
                                RingCsqAcquireLock,
                                RingCsqReleaseLock,
                                RingCsqCompleteCanceledIrp);
    if (!NT_SUCCESS(status))
        goto fail1;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__RingCsqDestroy(
    IN  PXENCONS_CSQ    Csq
    )
{
    ASSERT(IsListEmpty(&Csq->List));

    RtlZeroMemory(&Csq->Csq, sizeof(IO_CSQ));
    RtlZeroMemory(&Csq->List, sizeof(LIST_ENTRY));
    RtlZeroMemory(&Csq->Lock, sizeof(KSPIN_LOCK));
}

static FORCEINLINE ULONG
__RingCopyFromIn(
    IN  PXENCONS_RING   Ring,
    IN  PCHAR           Data,
    IN  ULONG           Length
    )
{
    struct xencons_interface    *Shared;
    XENCONS_RING_IDX            cons;
    XENCONS_RING_IDX            prod;
    ULONG                       Offset;

    Shared = Ring->Shared;

    KeMemoryBarrier();

    cons = Shared->in_cons;
    prod = Shared->in_prod;

    KeMemoryBarrier();

    // is there anything on in ring?
    if (prod - cons == 0)
        return 0;

    Offset = 0;
    while (Length != 0) {
        ULONG   Available;
        ULONG   Index;
        ULONG   CopyLength;

        Available = prod - cons;

        if (Available == 0)
            break;

        Index = MASK_XENCONS_IDX(cons, Shared->in);

        CopyLength = __min(Length, Available);
        CopyLength = __min(CopyLength, sizeof(Shared->in) - Index);

        RtlCopyMemory(Data + Offset, &Shared->in[Index], CopyLength);

        Offset += CopyLength;
        Length -= CopyLength;
        cons += CopyLength;
    }

    KeMemoryBarrier();

    Shared->in_cons = cons;

    KeMemoryBarrier();

    return Offset;
}

static FORCEINLINE ULONG
 __RingCopyToOut(
    IN  PXENCONS_RING   Ring,
    IN  PCHAR           Data,
    IN  ULONG           Length
    )
{
    struct xencons_interface    *Shared;
    XENCONS_RING_IDX            cons;
    XENCONS_RING_IDX            prod;
    ULONG                       Offset;

    Shared = Ring->Shared;

    KeMemoryBarrier();

    prod = Shared->out_prod;
    cons = Shared->out_cons;

    KeMemoryBarrier();

    // is there any space on out ring?
    if ((cons + sizeof(Shared->out) - prod) == 0)
        return 0;

    Offset = 0;
    while (Length != 0) {
        ULONG   Available;
        ULONG   Index;
        ULONG   CopyLength;

        Available = cons + sizeof(Shared->out) - prod;

        if (Available == 0)
            break;

        Index = MASK_XENCONS_IDX(prod, Shared->out);

        CopyLength = __min(Length, Available);
        CopyLength = __min(CopyLength, sizeof(Shared->out) - Index);

        RtlCopyMemory(&Shared->out[Index], Data + Offset, CopyLength);

        Offset += CopyLength;
        Length -= CopyLength;
        prod += CopyLength;
    }

    KeMemoryBarrier();

    Shared->out_prod = prod;

    KeMemoryBarrier();

    return Offset;
}

static BOOLEAN
RingPoll(
    IN  PXENCONS_RING   Ring
    )
{
    PIRP                Irp;
    PIO_STACK_LOCATION  StackLocation;
    ULONG               Bytes;
    ULONG               Read;
    ULONG               Written;
    NTSTATUS            status;

    Read = 0;
    Written = 0;

    for (;;) {
        Irp = IoCsqRemoveNextIrp(&Ring->Read.Csq, NULL);
        if (Irp == NULL)
            break;

        StackLocation = IoGetCurrentIrpStackLocation(Irp);
        ASSERT(StackLocation->MajorFunction == IRP_MJ_READ);

        Bytes = __RingCopyFromIn(Ring,
                                 Irp->AssociatedIrp.SystemBuffer,
                                 StackLocation->Parameters.Read.Length);
        Read += Bytes;
        if (Bytes) {
            Irp->IoStatus.Information = Bytes;
            Irp->IoStatus.Status = STATUS_SUCCESS;

            Trace("COMPLETED (%02x:%s) (%u)\n",
                  IRP_MJ_READ,
                  MajorFunctionName(IRP_MJ_READ),
                  Bytes);

            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            continue;
        }

        // no data on read ring
        status = IoCsqInsertIrpEx(&Ring->Read.Csq, Irp, NULL, (PVOID)TRUE);
        ASSERT(NT_SUCCESS(status));
        break;
    }

    for (;;) {
        Irp = IoCsqRemoveNextIrp(&Ring->Write.Csq, NULL);
        if (Irp == NULL)
            break;

        StackLocation = IoGetCurrentIrpStackLocation(Irp);
        ASSERT(StackLocation->MajorFunction == IRP_MJ_WRITE);

        Bytes = __RingCopyToOut(Ring,
                                Irp->AssociatedIrp.SystemBuffer,
                                StackLocation->Parameters.Write.Length);
        Written += Bytes;
        if (Bytes) {
            Irp->IoStatus.Information = Bytes;
            Irp->IoStatus.Status = STATUS_SUCCESS;

            Trace("COMPLETED (%02x:%s) (%u)\n",
                  IRP_MJ_WRITE,
                  MajorFunctionName(IRP_MJ_WRITE),
                  Bytes);

            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            continue;
        }

        // no space on write ring
        status = IoCsqInsertIrpEx(&Ring->Write.Csq, Irp, NULL, (PVOID)TRUE);
        ASSERT(NT_SUCCESS(status));
        break;
    }

    if (Read || Written)
        XENBUS_EVTCHN(Send,
                      &Ring->EvtchnInterface,
                      Ring->Channel);

    return FALSE;
}

static FORCEINLINE VOID
__RingCancelIrps(
    IN  PXENCONS_RING   Ring,
    IN  PFILE_OBJECT    FileObject
    )
{
    for (;;) {
        PIRP    Irp;

        Irp = IoCsqRemoveNextIrp(&Ring->Read.Csq, FileObject);
        if (Irp == NULL)
            break;

        RingCsqCompleteCanceledIrp(&Ring->Read.Csq, Irp);
    }
    for (;;) {
        PIRP    Irp;

        Irp = IoCsqRemoveNextIrp(&Ring->Write.Csq, FileObject);
        if (Irp == NULL)
            break;

        RingCsqCompleteCanceledIrp(&Ring->Write.Csq, Irp);
    }
}

NTSTATUS
RingDispatchCreate(
    IN  PXENCONS_RING   Ring,
    IN  PFILE_OBJECT    FileObject
    )
{
    UNREFERENCED_PARAMETER(Ring);
    UNREFERENCED_PARAMETER(FileObject);

    // nothing special for Create
    return STATUS_SUCCESS;
}

NTSTATUS
RingDispatchCleanup(
    IN  PXENCONS_RING   Ring,
    IN  PFILE_OBJECT    FileObject
    )
{
    // Only cancel IRPs for this FileObject
    __RingCancelIrps(Ring, FileObject);
    return STATUS_SUCCESS;
}

NTSTATUS
RingDispatchReadWrite(
    IN  PXENCONS_RING   Ring,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    switch (StackLocation->MajorFunction) {
    case IRP_MJ_READ:
        status = STATUS_INVALID_PARAMETER;
        if (StackLocation->Parameters.Read.Length == 0)
            break;
        status = IoCsqInsertIrpEx(&Ring->Read.Csq, Irp, NULL, (PVOID)FALSE);
        break;

    case IRP_MJ_WRITE:
        status = STATUS_INVALID_PARAMETER;
        if (StackLocation->Parameters.Write.Length == 0)
            break;
        status = IoCsqInsertIrpEx(&Ring->Write.Csq, Irp, NULL, (PVOID)FALSE);
        break;

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    if (NT_SUCCESS(status))
        KeInsertQueueDpc(&Ring->Dpc, NULL, NULL);

    return status;
}

__drv_functionClass(KDEFERRED_ROUTINE)
__drv_maxIRQL(DISPATCH_LEVEL)
__drv_minIRQL(DISPATCH_LEVEL)
__drv_requiresIRQL(DISPATCH_LEVEL)
__drv_sameIRQL
static VOID
RingDpc(
    IN  PKDPC       Dpc,
    IN  PVOID       Context,
    IN  PVOID       Argument1,
    IN  PVOID       Argument2
    )
{
    PXENCONS_RING   Ring = Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    ASSERT(Ring != NULL);

    for (;;) {
        if (!RingPoll(Ring))
            break;
    }

    (VOID) XENBUS_EVTCHN(Unmask,
                         &Ring->EvtchnInterface,
                         Ring->Channel,
                         FALSE,
                         FALSE);
}

KSERVICE_ROUTINE    RingEvtchnCallback;

BOOLEAN
RingEvtchnCallback(
    IN  PKINTERRUPT InterruptObject,
    IN  PVOID       Argument
    )
{
    PXENCONS_RING   Ring = Argument;

    UNREFERENCED_PARAMETER(InterruptObject);

    ASSERT(Ring != NULL);

    Ring->Events++;

    if (KeInsertQueueDpc(&Ring->Dpc, NULL, NULL))
        Ring->Dpcs++;

    return TRUE;
}

static VOID
RingDebugCallback(
    IN  PVOID       Argument,
    IN  BOOLEAN     Crashing
    )
{
    PXENCONS_RING   Ring = Argument;

    UNREFERENCED_PARAMETER(Crashing);

    XENBUS_DEBUG(Printf,
                 &Ring->DebugInterface,
                 "0x%p [%s]\n",
                 Ring,
                  (Ring->Enabled) ? "ENABLED" : "DISABLED");

    XENBUS_DEBUG(Printf,
                 &Ring->DebugInterface,
                 "Events = %lu, Dpcs = %lu\n",
                 Ring->Events,
                 Ring->Dpcs);

    XENBUS_DEBUG(Printf,
                 &Ring->DebugInterface,
                 "SHARED: in_cons = %u in_prod = %u out_cons = %u out_prod = %u\n",
                 Ring->Shared->in_cons,
                 Ring->Shared->in_prod,
                 Ring->Shared->out_cons,
                 Ring->Shared->out_prod);

    // Raw Dump of in/out buffers?
}

NTSTATUS
RingConnect(
    IN  PXENCONS_RING   Ring
    )
{
    CHAR                Name[MAXNAMELEN];
    PFN_NUMBER          Pfn;
    NTSTATUS            status;

    Trace("=====>\n");

    status = XENBUS_DEBUG(Acquire, &Ring->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_STORE(Acquire, &Ring->StoreInterface);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_EVTCHN(Acquire, &Ring->EvtchnInterface);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = XENBUS_GNTTAB(Acquire, &Ring->GnttabInterface);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = RtlStringCbPrintfA(Name,
                                 sizeof(Name),
                                 "xencons_%s_gnttab",
                                 PdoGetName(FrontendGetPdo(Ring->Frontend)));
    if (!NT_SUCCESS(status))
        goto fail5;

    status = XENBUS_GNTTAB(CreateCache,
                            &Ring->GnttabInterface,
                            Name,
                            0,
                            RingAcquireLock,
                            RingReleaseLock,
                            Ring,
                            &Ring->GnttabCache);
    if (!NT_SUCCESS(status))
        goto fail6;

    Ring->Mdl = __AllocatePage();

    status = STATUS_NO_MEMORY;
    if (Ring->Mdl == NULL)
        goto fail7;

    ASSERT(Ring->Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA);
    Ring->Shared = Ring->Mdl->MappedSystemVa;
    ASSERT(Ring->Shared != NULL);

    Pfn = MmGetMdlPfnArray(Ring->Mdl)[0];

    status = XENBUS_GNTTAB(PermitForeignAccess,
                           &Ring->GnttabInterface,
                           Ring->GnttabCache,
                           TRUE,
                           FrontendGetBackendDomain(Ring->Frontend),
                           Pfn,
                           FALSE,
                           &Ring->Entry);
    if (!NT_SUCCESS(status))
        goto fail8;

    Ring->Channel = XENBUS_EVTCHN(Open,
                                  &Ring->EvtchnInterface,
                                  XENBUS_EVTCHN_TYPE_UNBOUND,
                                  RingEvtchnCallback,
                                  Ring,
                                  FrontendGetBackendDomain(Ring->Frontend),
                                  TRUE);

    status = STATUS_UNSUCCESSFUL;
    if (Ring->Channel == NULL)
        goto fail9;

    (VOID)XENBUS_EVTCHN(Unmask,
                        &Ring->EvtchnInterface,
                        Ring->Channel,
                        FALSE,
                        TRUE);

    ASSERT(!Ring->Connected);
    Ring->Connected = TRUE;

    status = XENBUS_DEBUG(Register,
                          &Ring->DebugInterface,
                          __MODULE__ "|RING",
                          RingDebugCallback,
                          Ring,
                          &Ring->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail10;

    Trace("<=====\n");
    return STATUS_SUCCESS;

fail10:
    Error("fail10\n");

    Ring->Connected = FALSE;

    XENBUS_EVTCHN(Close,
                  &Ring->EvtchnInterface,
                  Ring->Channel);
    Ring->Channel = NULL;

fail9:
    Error("fail9\n");

    (VOID)XENBUS_GNTTAB(RevokeForeignAccess,
                        &Ring->GnttabInterface,
                        Ring->GnttabCache,
                        TRUE,
                        Ring->Entry);
    Ring->Entry = NULL;

fail8:
    Error("fail8\n");

    RtlZeroMemory(Ring->Shared, PAGE_SIZE);

    Ring->Shared = NULL;
    __FreePage(Ring->Mdl);
    Ring->Mdl = NULL;


fail7:
    Error("fail7\n");

    XENBUS_GNTTAB(DestroyCache,
                  &Ring->GnttabInterface,
                  Ring->GnttabCache);
    Ring->GnttabCache = NULL;

fail6:
    Error("fail6\n");

fail5:
    Error("fail5\n");

    XENBUS_GNTTAB(Release, &Ring->GnttabInterface);

fail4:
    Error("fail4\n");

    XENBUS_EVTCHN(Release, &Ring->EvtchnInterface);

fail3:
    Error("fail3\n");

    XENBUS_STORE(Release, &Ring->StoreInterface);

fail2:
    Error("fail2\n");

    XENBUS_DEBUG(Release, &Ring->DebugInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
RingStoreWrite(
    IN  PXENCONS_RING   Ring,
    IN  PVOID           Transaction
    )
{
    ULONG               GrantRef;
    ULONG               Port;
    NTSTATUS            status;

    Port = XENBUS_EVTCHN(GetPort,
                         &Ring->EvtchnInterface,
                         Ring->Channel);

    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "port",
                          "%u",
                          Port);
    if (!NT_SUCCESS(status))
        goto fail1;

    GrantRef = XENBUS_GNTTAB(GetReference,
                             &Ring->GnttabInterface,
                             Ring->Entry);

    status = XENBUS_STORE(Printf,
                          &Ring->StoreInterface,
                          Transaction,
                          FrontendGetPath(Ring->Frontend),
                          "ring-ref",
                          "%u",
                          GrantRef);
    if (!NT_SUCCESS(status))
        goto fail2;

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
RingEnable(
    IN  PXENCONS_RING   Ring
    )
{
    Trace("=====>\n");

    ASSERT3U(KeGetCurrentIrql(), == , DISPATCH_LEVEL);

    KeAcquireSpinLockAtDpcLevel(&Ring->Lock);
    Ring->Enabled = TRUE;
    KeReleaseSpinLockFromDpcLevel(&Ring->Lock);

    (VOID)KeInsertQueueDpc(&Ring->Dpc, NULL, NULL);

    Trace("<=====\n");
    return STATUS_SUCCESS;
}

VOID
RingDisable(
    IN  PXENCONS_RING   Ring
    )
{
    Trace("=====>\n");

    // cancel all IRPs, regardless of FileObject
    __RingCancelIrps(Ring, NULL);

    ASSERT3U(KeGetCurrentIrql(), == , DISPATCH_LEVEL);

    KeAcquireSpinLockAtDpcLevel(&Ring->Lock);
    Ring->Enabled = FALSE;
    KeReleaseSpinLockFromDpcLevel(&Ring->Lock);

    Trace("<=====\n");
}

VOID
RingDisconnect(
    IN  PXENCONS_RING   Ring
    )
{
    Trace("=====>\n");

    XENBUS_DEBUG(Deregister,
                 &Ring->DebugInterface,
                 Ring->DebugCallback);
    Ring->DebugCallback = NULL;

    ASSERT(Ring->Connected);
    Ring->Connected = FALSE;

    ASSERT3U(KeGetCurrentIrql(), == , DISPATCH_LEVEL);

    Ring->Dpcs = 0;

    Ring->Events = 0;

    XENBUS_EVTCHN(Close,
                  &Ring->EvtchnInterface,
                  Ring->Channel);
    Ring->Channel = NULL;

    (VOID)XENBUS_GNTTAB(RevokeForeignAccess,
                        &Ring->GnttabInterface,
                        Ring->GnttabCache,
                        TRUE,
                        Ring->Entry);
    Ring->Entry = NULL;

    RtlZeroMemory(Ring->Shared, PAGE_SIZE);

    Ring->Shared = NULL;
    __FreePage(Ring->Mdl);
    Ring->Mdl = NULL;

    XENBUS_GNTTAB(DestroyCache,
                  &Ring->GnttabInterface,
                  Ring->GnttabCache);
    Ring->GnttabCache = NULL;

    XENBUS_GNTTAB(Release, &Ring->GnttabInterface);

    XENBUS_EVTCHN(Release, &Ring->EvtchnInterface);

    XENBUS_STORE(Release, &Ring->StoreInterface);

    XENBUS_DEBUG(Release, &Ring->DebugInterface);

    Trace("<=====\n");
}

NTSTATUS
RingCreate(
    IN  PXENCONS_FRONTEND   Frontend,
    OUT PXENCONS_RING       *Ring
    )
{
    NTSTATUS                status;

    Trace("=====>\n");

    *Ring = __RingAllocate(sizeof(XENCONS_RING));

    status = STATUS_NO_MEMORY;
    if (*Ring == NULL)
        goto fail1;

    (*Ring)->Frontend = Frontend;

    FdoGetGnttabInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                          &(*Ring)->GnttabInterface);

    FdoGetEvtchnInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                          &(*Ring)->EvtchnInterface);

    FdoGetStoreInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                         &(*Ring)->StoreInterface);

    FdoGetDebugInterface(PdoGetFdo(FrontendGetPdo(Frontend)),
                         &(*Ring)->DebugInterface);

    KeInitializeSpinLock(&(*Ring)->Lock);

    KeInitializeDpc(&(*Ring)->Dpc, RingDpc, *Ring);

    status = __RingCsqCreate(&(*Ring)->Read);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = __RingCsqCreate(&(*Ring)->Write);
    if (!NT_SUCCESS(status))
        goto fail3;

    Trace("<=====\n");

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    __RingCsqDestroy(&(*Ring)->Read);

fail2:
    Error("fail2\n");

    RtlZeroMemory(&(*Ring)->Dpc, sizeof(KDPC));

    RtlZeroMemory(&(*Ring)->Lock, sizeof(KSPIN_LOCK));

    RtlZeroMemory(&(*Ring)->GnttabInterface,
                    sizeof(XENBUS_GNTTAB_INTERFACE));

    RtlZeroMemory(&(*Ring)->EvtchnInterface,
                    sizeof(XENBUS_EVTCHN_INTERFACE));

    RtlZeroMemory(&(*Ring)->StoreInterface,
                    sizeof(XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&(*Ring)->DebugInterface,
                    sizeof(XENBUS_DEBUG_INTERFACE));

    (*Ring)->Frontend = NULL;

    ASSERT(IsZeroMemory(*Ring, sizeof(XENCONS_RING)));
    __RingFree(*Ring);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
RingDestroy(
    IN  PXENCONS_RING   Ring
    )
{
    Trace("=====>\n");

    __RingCsqDestroy(&Ring->Write);
    __RingCsqDestroy(&Ring->Read);

    RtlZeroMemory(&Ring->Dpc, sizeof(KDPC));

    RtlZeroMemory(&Ring->Lock, sizeof(KSPIN_LOCK));

    RtlZeroMemory(&Ring->GnttabInterface,
                  sizeof(XENBUS_GNTTAB_INTERFACE));

    RtlZeroMemory(&Ring->EvtchnInterface,
                  sizeof(XENBUS_EVTCHN_INTERFACE));

    RtlZeroMemory(&Ring->StoreInterface,
                  sizeof(XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&Ring->DebugInterface,
                  sizeof(XENBUS_DEBUG_INTERFACE));

    Ring->Frontend = NULL;

    ASSERT(IsZeroMemory(Ring, sizeof(XENCONS_RING)));
    __RingFree(Ring);

    Trace("<=====\n");
}
