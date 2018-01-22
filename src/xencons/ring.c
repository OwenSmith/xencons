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
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

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

static BOOLEAN
RingPoll(
    IN  PXENCONS_RING   Ring
    )
{
    UNREFERENCED_PARAMETER(Ring);
    return FALSE;
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

    // empty queue(s)

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

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
RingDestroy(
    IN  PXENCONS_RING   Ring
    )
{
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
}
