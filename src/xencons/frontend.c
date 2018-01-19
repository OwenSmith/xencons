/*   Copyright (c) Citrix Systems Inc.
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
#include <suspend_interface.h>

#include "driver.h"
#include "frontend.h"
#include "thread.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define FRONTEND_POOL   'TNRF'
#define DOMID_INVALID   (0x7FFFU)

struct _XENCONS_FRONTEND {
    PXENCONS_PDO                Pdo;
    PCHAR                       Path;
    BOOLEAN                     Online;
    FRONTEND_STATE              State;
    KSPIN_LOCK                  Lock;
    PXENCONS_THREAD             EjectThread;
    KEVENT                      EjectEvent;

    PCHAR                       BackendPath;
    USHORT                      BackendDomain;

    PCHAR                       Name;
    PCHAR                       Protocol;

    XENBUS_DEBUG_INTERFACE      DebugInterface;
    XENBUS_SUSPEND_INTERFACE    SuspendInterface;
    XENBUS_STORE_INTERFACE      StoreInterface;

    PXENBUS_SUSPEND_CALLBACK    SuspendCallback;
    PXENBUS_DEBUG_CALLBACK      DebugCallback;
    PXENBUS_STORE_WATCH         Watch;
};

static FORCEINLINE PVOID
__FrontendAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, FRONTEND_POOL);
}

static FORCEINLINE VOID
__FrontendFree(
    IN  PVOID   Buffer
    )
{
    if (Buffer)
        __FreePoolWithTag(Buffer, FRONTEND_POOL);
}

static FORCEINLINE const PCHAR
XenbusStateName(
    IN  XenbusState   State
    )
{
#define _STATE_NAME(_State)     \
    case  XenbusState ## _State:  \
        return #_State;

    switch (State) {
        _STATE_NAME(Unknown);
        _STATE_NAME(Initialising);
        _STATE_NAME(InitWait);
        _STATE_NAME(Initialised);
        _STATE_NAME(Connected);
        _STATE_NAME(Closing);
        _STATE_NAME(Closed);
        _STATE_NAME(Reconfiguring);
        _STATE_NAME(Reconfigured);
    default:
        break;
    }

    return "INVALID";

#undef  _STATE_NAME
}

static const PCHAR
FrontendStateName(
    IN  FRONTEND_STATE  State
    )
{
#define _STATE_NAME(_State)     \
    case  FRONTEND_ ## _State:  \
        return #_State;

    switch (State) {
        _STATE_NAME(UNKNOWN);
        _STATE_NAME(CLOSED);
        _STATE_NAME(PREPARED);
        _STATE_NAME(CONNECTED);
        _STATE_NAME(ENABLED);
    default:
        break;
    }

    return "INVALID";

#undef  _STATE_NAME
}

static FORCEINLINE PXENCONS_PDO
__FrontendGetPdo(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    return Frontend->Pdo;
}

PXENCONS_PDO
FrontendGetPdo(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    return __FrontendGetPdo(Frontend);
}

static FORCEINLINE PCHAR
__FrontendGetPath(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    return Frontend->Path;
}

PCHAR
FrontendGetPath(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    return __FrontendGetPath(Frontend);
}

static FORCEINLINE PCHAR
__FrontendGetBackendPath(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    return Frontend->BackendPath;
}

PCHAR
FrontendGetBackendPath(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    return __FrontendGetBackendPath(Frontend);
}

static FORCEINLINE USHORT
__FrontendGetBackendDomain(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    return Frontend->BackendDomain;
}

USHORT
FrontendGetBackendDomain(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    return __FrontendGetBackendDomain(Frontend);
}

PCHAR
FrontendGetName(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    return Frontend->Name;
}

PCHAR
FrontendGetProtocol(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    return Frontend->Protocol;
}

NTSTATUS
FrontendDispatchCreate(
    IN  PXENCONS_FRONTEND   Frontend,
    IN  PFILE_OBJECT        FileObject
    )
{
    UNREFERENCED_PARAMETER(Frontend);
    UNREFERENCED_PARAMETER(FileObject);
    return STATUS_SUCCESS;
}

NTSTATUS
FrontendDispatchCleanup(
    IN  PXENCONS_FRONTEND   Frontend,
    IN  PFILE_OBJECT        FileObject
    )
{
    UNREFERENCED_PARAMETER(Frontend);
    UNREFERENCED_PARAMETER(FileObject);
    return STATUS_SUCCESS;
}

NTSTATUS
FrontendDispatchReadWrite(
    IN  PXENCONS_FRONTEND   Frontend,
    IN  PIRP                Irp
    )
{
    UNREFERENCED_PARAMETER(Frontend);
    UNREFERENCED_PARAMETER(Irp);
    return STATUS_DEVICE_NOT_READY;
}

static VOID
FrontendDebugCallback(
    IN  PVOID               Argument,
    IN  BOOLEAN             Crashing
    )
{
    PXENCONS_FRONTEND       Frontend = Argument;
    
    UNREFERENCED_PARAMETER(Crashing);

    XENBUS_DEBUG(Printf,
                 &Frontend->DebugInterface,
                 "PATH: %s\n",
                 __FrontendGetPath(Frontend));
    XENBUS_DEBUG(Printf,
                 &Frontend->DebugInterface,
                 "NAME: %s\n",
                 FrontendGetName(Frontend));
    XENBUS_DEBUG(Printf,
                 &Frontend->DebugInterface,
                 "PROTOCOL: %s\n",
                 FrontendGetProtocol(Frontend));
}

static VOID
FrontendSetOnline(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    Trace("====>\n");

    Frontend->Online = TRUE;

    Trace("<====\n");
}

static VOID
FrontendSetOffline(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    Trace("====>\n");

    Frontend->Online = FALSE;
    PdoRequestEject(__FrontendGetPdo(Frontend));

    Trace("<====\n");
}

static BOOLEAN
FrontendIsOnline(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    return Frontend->Online;
}

static BOOLEAN
FrontendIsBackendOnline(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    PCHAR                   Buffer;
    BOOLEAN                 Online;
    NTSTATUS                status;

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetBackendPath(Frontend),
                          "online",
                          &Buffer);
    if (!NT_SUCCESS(status)) {
        Online = FALSE;
    } else {
        Online = (BOOLEAN)strtol(Buffer, NULL, 2);

        XENBUS_STORE(Free,
                     &Frontend->StoreInterface,
                     Buffer);
    }

    return Online;
}

static NTSTATUS
FrontendAcquireBackend(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    PCHAR                   Buffer;
    NTSTATUS                status;

    Trace("=====>\n");

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetPath(Frontend),
                          "backend",
                          &Buffer);
    if (!NT_SUCCESS(status))
        goto fail1;

    Frontend->BackendPath = Buffer;

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetPath(Frontend),
                          "backend-id",
                          &Buffer);
    if (!NT_SUCCESS(status)) {
        Frontend->BackendDomain = 0;
    } else {
        Frontend->BackendDomain = (USHORT)strtol(Buffer, NULL, 10);

        XENBUS_STORE(Free,
                     &Frontend->StoreInterface,
                     Buffer);
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    Trace("<====\n");
    return status;
}

static VOID
FrontendReleaseBackend(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    Trace("=====>\n");

    ASSERT(Frontend->BackendDomain != DOMID_INVALID);
    ASSERT(Frontend->BackendPath != NULL);

    Frontend->BackendDomain = DOMID_INVALID;

    XENBUS_STORE(Free,
                 &Frontend->StoreInterface,
                 Frontend->BackendPath);
    Frontend->BackendPath = NULL;

    Trace("<=====\n");
}

static FORCEINLINE NTSTATUS
__FrontendReadParameter(
    IN  PXENCONS_FRONTEND   Frontend,
    IN  PCHAR               Name,
    OUT PCHAR               *Result
    )
{
    PCHAR                   Buffer;
    ULONG                   Length;
    NTSTATUS                status;

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetBackendPath(Frontend),
                          Name,
                          &Buffer);
    if (!NT_SUCCESS(status))
        goto fail1;

    Length = (ULONG)strlen(Buffer);

    if (*Result)
        __FrontendFree(*Result);

    *Result = __FrontendAllocate(Length + 1);
    if (*Result == NULL)
        goto fail2;

    RtlCopyMemory(*Result, Buffer, Length);

    XENBUS_STORE(Free,
                 &Frontend->StoreInterface,
                 Buffer);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    *Result = NULL;

    XENBUS_STORE(Free,
                 &Frontend->StoreInterface,
                 Buffer);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__FrontendSetState(
    IN  PXENCONS_FRONTEND   Frontend,
    IN  XenbusState         State
    )
{
    (VOID)XENBUS_STORE(Printf,
                       &Frontend->StoreInterface,
                       NULL,
                       __FrontendGetPath(Frontend),
                       "state",
                       "%u",
                       State);
}

static FORCEINLINE XenbusState
__FrontendWaitState(
    IN  PXENCONS_FRONTEND   Frontend,
    IN  XenbusState         OldState
    )
{
    KEVENT                      Event;
    PXENBUS_STORE_WATCH         Watch;
    LARGE_INTEGER               Start;
    ULONGLONG                   TimeDelta;
    LARGE_INTEGER               Timeout;
    XenbusState                 State = OldState;
    NTSTATUS                    status;

    Trace("%s: ====> %s\n",
          __FrontendGetBackendPath(Frontend),
          XenbusStateName(State));

    ASSERT(FrontendIsOnline(Frontend));

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    status = XENBUS_STORE(WatchAdd,
                          &Frontend->StoreInterface,
                          __FrontendGetBackendPath(Frontend),
                          "state",
                          &Event,
                          &Watch);
    if (!NT_SUCCESS(status))
        Watch = NULL;

    KeQuerySystemTime(&Start);
    TimeDelta = 0;

    Timeout.QuadPart = 0;

    while (State == OldState && TimeDelta < 120000) {
        PCHAR           Buffer;
        LARGE_INTEGER   Now;

        if (Watch != NULL) {
            ULONG   Attempt = 0;

            while (++Attempt < 1000) {
                status = KeWaitForSingleObject(&Event,
                                               Executive,
                                               KernelMode,
                                               FALSE,
                                               &Timeout);
                if (status != STATUS_TIMEOUT)
                    break;

                // We are waiting for a watch event at DISPATCH_LEVEL so
                // it is our responsibility to poll the store ring.
                XENBUS_STORE(Poll,
                             &Frontend->StoreInterface);

                KeStallExecutionProcessor(1000);   // 1ms
            }

            KeClearEvent(&Event);
        }

        status = XENBUS_STORE(Read,
                              &Frontend->StoreInterface,
                              NULL,
                              __FrontendGetBackendPath(Frontend),
                              "state",
                              &Buffer);
        if (!NT_SUCCESS(status)) {
            State = XenbusStateUnknown;
        } else {
            State = (XenbusState)strtol(Buffer, NULL, 10);

            XENBUS_STORE(Free,
                         &Frontend->StoreInterface,
                         Buffer);
        }

        KeQuerySystemTime(&Now);

        TimeDelta = (Now.QuadPart - Start.QuadPart) / 10000ull;
    }

    if (Watch != NULL)
        (VOID) XENBUS_STORE(WatchRemove,
                            &Frontend->StoreInterface,
                            Watch);

    Trace("%s: <==== (%s)\n",
          __FrontendGetBackendPath(Frontend),
          XenbusStateName(State));

    return State;
}

static VOID
FrontendClose(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    XenbusState             State;

    Trace("====>\n");

    __FrontendSetState(Frontend, XenbusStateClosing);

    State = XenbusStateUnknown;
    while (State != XenbusStateClosed) {
        State = __FrontendWaitState(Frontend, State);
        switch (State) {
        case XenbusStateClosing:
            __FrontendSetState(Frontend, XenbusStateClosed);
            break;

        case XenbusStateClosed:
            break;

        default:
            __FrontendSetState(Frontend, XenbusStateClosing);
            break;
        }
    }

    FrontendReleaseBackend(Frontend);

    Trace("<====\n");
}

static NTSTATUS
FrontendPrepare(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    XenbusState             State;
    NTSTATUS                status;

    Trace("====>\n");

    status = FrontendAcquireBackend(Frontend);
    if (!NT_SUCCESS(status))
        goto fail1;

    FrontendSetOnline(Frontend);

    __FrontendSetState(Frontend, XenbusStateInitialising);

    State = XenbusStateUnknown;
    while (State != XenbusStateInitWait) {
        State = __FrontendWaitState(Frontend, State);
        switch (State) {
        case XenbusStateInitWait:
            break;

        default:
            break;
        }
    }

    Trace("<====\n");
    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
FrontendConnect(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    XenbusState             State;
    ULONG                   Attempt;
    NTSTATUS                status;

    Trace("====>\n");

    status = XENBUS_DEBUG(Acquire, &Frontend->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_DEBUG(Register,
                          &Frontend->DebugInterface,
                          __MODULE__ "|FRONTEND",
                          FrontendDebugCallback,
                          Frontend,
                          &Frontend->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail2;

    //status = RingConnect(Frontend->Ring);
    //if (!NT_SUCCESS(status))
    //    goto fail3;

    Attempt = 0;
    do {
        PXENBUS_STORE_TRANSACTION   Transaction;

        status = XENBUS_STORE(TransactionStart,
                              &Frontend->StoreInterface,
                              &Transaction);
        if (!NT_SUCCESS(status))
            break;

        //status = RingStoreWrite(Frontend->Ring, Transaction);
        //if (!NT_SUCCESS(status))
        //    goto abort;

        status = XENBUS_STORE(TransactionEnd,
                              &Frontend->StoreInterface,
                              Transaction,
                              TRUE);
        if (status != STATUS_RETRY || ++Attempt > 10)
            break;

        continue;

    //abort:
    //    (VOID)XENBUS_STORE(TransactionEnd,
    //                       &Frontend->StoreInterface,
    //                       Transaction,
    //                       FALSE);
    //    break;
    } while (status == STATUS_RETRY);

    if (!NT_SUCCESS(status))
        goto fail4;

    status = __FrontendReadParameter(Frontend,
                                     "name",
                                     &Frontend->Name);
    if (!NT_SUCCESS(status))
        goto fail5;

    (VOID)__FrontendReadParameter(Frontend, "protocol", &Frontend->Protocol);

    __FrontendSetState(Frontend, XenbusStateConnected);

    State = XenbusStateUnknown;
    while (State != XenbusStateConnected) {
        State = __FrontendWaitState(Frontend, State);
        switch (State) {
        case XenbusStateConnected:
            break;

        case XenbusStateClosed:
        case XenbusStateUnknown:
            status = STATUS_UNSUCCESSFUL;
            goto fail6;

        default:
            break;
        }
    }

    Trace("<====\n");
    return STATUS_SUCCESS;

fail6:
    Error("fail6\n");

    __FrontendFree(Frontend->Protocol);
    Frontend->Protocol = NULL;

    __FrontendFree(Frontend->Name);
    Frontend->Name = NULL;

fail5:
    Error("fail5\n");

fail4:
    Error("fail4\n");

//fail3:
//    Error("fail3\n");

    XENBUS_DEBUG(Deregister,
                 &Frontend->DebugInterface,
                 Frontend->DebugCallback);
    Frontend->DebugCallback = NULL;

fail2:
    Error("fail2\n");

    XENBUS_DEBUG(Release, &Frontend->DebugInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    Trace("<====\n");
    return status;
}

static VOID
FrontendDisconnect(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    Trace("====>\n");

    __FrontendFree(Frontend->Protocol);
    Frontend->Protocol = NULL;

    __FrontendFree(Frontend->Name);
    Frontend->Name = NULL;

    //RingDisconnect(Frontend->Ring);

    XENBUS_DEBUG(Deregister,
                 &Frontend->DebugInterface,
                 Frontend->DebugCallback);
    Frontend->DebugCallback = NULL;

    XENBUS_DEBUG(Release, &Frontend->DebugInterface);

    Trace("<====\n");
}

static NTSTATUS
FrontendEnable(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    Trace("====>\n");

    UNREFERENCED_PARAMETER(Frontend);
    //RingEnable(Frontend->Ring);

    Trace("<====\n");
    return STATUS_SUCCESS;
}

static VOID
FrontendDisable(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    Trace("====>\n");

    UNREFERENCED_PARAMETER(Frontend);
    //RingDisable(Frontend->Ring);

    Trace("<====\n");
}

NTSTATUS
FrontendSetState(
    IN  PXENCONS_FRONTEND   Frontend,
    IN  FRONTEND_STATE      State
    )
{
    BOOLEAN                     Failed;
    KIRQL                       Irql;

    KeAcquireSpinLock(&Frontend->Lock, &Irql);

    Info("%s: ====> '%s' -> '%s'\n",
         __FrontendGetPath(Frontend),
         FrontendStateName(Frontend->State),
         FrontendStateName(State));

    Failed = FALSE;
    while (Frontend->State != State && !Failed) {
        NTSTATUS    status;

        switch (Frontend->State) {
        case FRONTEND_UNKNOWN:
            switch (State) {
            case FRONTEND_CLOSED:
            case FRONTEND_PREPARED:
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendPrepare(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_PREPARED;
                } else {
                    Failed = TRUE;
                }
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_CLOSED:
            switch (State) {
            case FRONTEND_PREPARED:
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendPrepare(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_PREPARED;
                } else {
                    Failed = TRUE;
                }
                break;

            case FRONTEND_UNKNOWN:
                Frontend->State = FRONTEND_UNKNOWN;
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_PREPARED:
            switch (State) {
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendConnect(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_CONNECTED;
                } else {
                    FrontendClose(Frontend);
                    Frontend->State = FRONTEND_CLOSED;

                    Failed = TRUE;
                }
                break;

            case FRONTEND_CLOSED:
            case FRONTEND_UNKNOWN:
                FrontendClose(Frontend);
                Frontend->State = FRONTEND_CLOSED;
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_CONNECTED:
            switch (State) {
            case FRONTEND_ENABLED:
                status = FrontendEnable(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_ENABLED;
                } else {
                    FrontendClose(Frontend);
                    Frontend->State = FRONTEND_CLOSED;

                    FrontendDisconnect(Frontend);
                    Failed = TRUE;
                }
                break;

            case FRONTEND_PREPARED:
            case FRONTEND_CLOSED:
            case FRONTEND_UNKNOWN:
                FrontendClose(Frontend);
                Frontend->State = FRONTEND_CLOSED;

                FrontendDisconnect(Frontend);
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_ENABLED:
            switch (State) {
            case FRONTEND_CONNECTED:
            case FRONTEND_PREPARED:
            case FRONTEND_CLOSED:
            case FRONTEND_UNKNOWN:
                FrontendDisable(Frontend);
                Frontend->State = FRONTEND_CONNECTED;
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        default:
            ASSERT(FALSE);
            break;
        }

        Info("%s in state '%s'\n",
             __FrontendGetPath(Frontend),
             FrontendStateName(Frontend->State));
    }

    KeReleaseSpinLock(&Frontend->Lock, Irql);

    Info("%s: <=====\n", __FrontendGetPath(Frontend));

    return (!Failed) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static DECLSPEC_NOINLINE NTSTATUS
FrontendEject(
    IN  PXENCONS_THREAD Self,
    IN  PVOID           Context
    )
{
    PXENCONS_FRONTEND   Frontend = Context;
    PKEVENT             Event;

    Trace("%s: ====>\n", __FrontendGetPath(Frontend));

    Event = ThreadGetEvent(Self);

    for (;;) {
        KIRQL       Irql;

        KeWaitForSingleObject(Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        KeClearEvent(Event);

        if (ThreadIsAlerted(Self))
            break;

        KeAcquireSpinLock(&Frontend->Lock, &Irql);

        // It is not safe to use interfaces before this point
        if (Frontend->State == FRONTEND_UNKNOWN ||
            Frontend->State == FRONTEND_CLOSED)
            goto loop;

        if (!FrontendIsOnline(Frontend))
            goto loop;

        if (!FrontendIsBackendOnline(Frontend))
            PdoRequestEject(__FrontendGetPdo(Frontend));

    loop:
        KeReleaseSpinLock(&Frontend->Lock, Irql);

        KeSetEvent(&Frontend->EjectEvent, IO_NO_INCREMENT, FALSE);
    }

    KeSetEvent(&Frontend->EjectEvent, IO_NO_INCREMENT, FALSE);

    Trace("%s: <====\n", __FrontendGetPath(Frontend));

    return STATUS_SUCCESS;
}

static FORCEINLINE VOID
__FrontendResume(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    ASSERT3U(KeGetCurrentIrql(), == , DISPATCH_LEVEL);

    ASSERT3U(Frontend->State, == , FRONTEND_UNKNOWN);
    // backends can be problematic if closed
    //(VOID)FrontendSetState(Frontend, FRONTEND_CLOSED);
}

static FORCEINLINE VOID
__FrontendSuspend(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    ASSERT3U(KeGetCurrentIrql(), == , DISPATCH_LEVEL);

    (VOID)FrontendSetState(Frontend, FRONTEND_UNKNOWN);
}

static DECLSPEC_NOINLINE VOID
FrontendSuspendCallback(
    IN  PVOID           Argument
    )
{
    PXENCONS_FRONTEND   Frontend = Argument;

    __FrontendSuspend(Frontend);
    __FrontendResume(Frontend);
}

NTSTATUS
FrontendResume(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    KIRQL                   Irql;
    NTSTATUS                status;

    Trace("====>\n");

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    status = XENBUS_SUSPEND(Acquire, &Frontend->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FrontendResume(Frontend);

    status = XENBUS_SUSPEND(Register,
                            &Frontend->SuspendInterface,
                            SUSPEND_CALLBACK_LATE,
                            FrontendSuspendCallback,
                            Frontend,
                            &Frontend->SuspendCallback);
    if (!NT_SUCCESS(status))
        goto fail2;

    KeLowerIrql(Irql);

    KeClearEvent(&Frontend->EjectEvent);
    ThreadWake(Frontend->EjectThread);

    Trace("waiting for eject thread\n");

    (VOID)KeWaitForSingleObject(&Frontend->EjectEvent,
                                Executive,
                                KernelMode,
                                FALSE,
                                NULL);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    __FrontendSuspend(Frontend);

    XENBUS_SUSPEND(Release, &Frontend->SuspendInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return status;
}

VOID
FrontendSuspend(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    KIRQL                   Irql;

    Trace("====>\n");

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    XENBUS_SUSPEND(Deregister,
                   &Frontend->SuspendInterface,
                   Frontend->SuspendCallback);
    Frontend->SuspendCallback = NULL;

    __FrontendSuspend(Frontend);

    XENBUS_SUSPEND(Release, &Frontend->SuspendInterface);

    KeLowerIrql(Irql);

    KeClearEvent(&Frontend->EjectEvent);
    ThreadWake(Frontend->EjectThread);

    Trace("waiting for eject thread\n");

    (VOID)KeWaitForSingleObject(&Frontend->EjectEvent,
                                Executive,
                                KernelMode,
                                FALSE,
                                NULL);

    Trace("<====\n");
}

NTSTATUS
FrontendCreate(
    IN  PXENCONS_PDO        Pdo,
    OUT PXENCONS_FRONTEND   *Frontend
    )
{
    PCHAR                   Name;
    ULONG                   Length;
    PCHAR                   Path;
    NTSTATUS                status;

    Name = PdoGetName(Pdo);

    Length = sizeof("devices/console/") + (ULONG)strlen(Name);
    Path = __FrontendAllocate(Length);

    status = STATUS_NO_MEMORY;
    if (Path == NULL)
        goto fail1;

    status = RtlStringCbPrintfA(Path,
                                Length,
                                "device/console/%s",
                                Name);
    if (!NT_SUCCESS(status))
        goto fail2;

    *Frontend = __FrontendAllocate(sizeof(XENCONS_FRONTEND));

    status = STATUS_NO_MEMORY;
    if (*Frontend == NULL)
        goto fail3;

    (*Frontend)->Pdo = Pdo;
    (*Frontend)->Path = Path;
    (*Frontend)->BackendDomain = DOMID_INVALID;

    KeInitializeSpinLock(&(*Frontend)->Lock);

    (*Frontend)->Online = TRUE;

    FdoGetDebugInterface(PdoGetFdo(Pdo), &(*Frontend)->DebugInterface);
    FdoGetSuspendInterface(PdoGetFdo(Pdo), &(*Frontend)->SuspendInterface);
    FdoGetStoreInterface(PdoGetFdo(Pdo), &(*Frontend)->StoreInterface);

    KeInitializeEvent(&(*Frontend)->EjectEvent, NotificationEvent, FALSE);

    status = ThreadCreate(FrontendEject, *Frontend, &(*Frontend)->EjectThread);
    if (!NT_SUCCESS(status))
        goto fail4;

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    RtlZeroMemory(&(*Frontend)->EjectEvent, sizeof(KEVENT));

    RtlZeroMemory(&(*Frontend)->StoreInterface,
                  sizeof(XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&(*Frontend)->SuspendInterface,
                  sizeof(XENBUS_SUSPEND_INTERFACE));

    RtlZeroMemory(&(*Frontend)->DebugInterface,
                  sizeof(XENBUS_DEBUG_INTERFACE));

    (*Frontend)->Online = FALSE;

    RtlZeroMemory(&(*Frontend)->Lock, sizeof(KSPIN_LOCK));

    (*Frontend)->BackendDomain = 0;
    (*Frontend)->Path = NULL;
    (*Frontend)->Pdo = NULL;

    ASSERT(IsZeroMemory(*Frontend, sizeof(XENCONS_FRONTEND)));

    __FrontendFree(*Frontend);
    *Frontend = NULL;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    __FrontendFree(Path);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
FrontendDestroy(
    IN  PXENCONS_FRONTEND   Frontend
    )
{
    ASSERT3U(KeGetCurrentIrql(), == , PASSIVE_LEVEL);

    ASSERT(Frontend->State == FRONTEND_UNKNOWN);

    ThreadAlert(Frontend->EjectThread);
    ThreadJoin(Frontend->EjectThread);
    Frontend->EjectThread = NULL;

    RtlZeroMemory(&Frontend->EjectEvent, sizeof(KEVENT));

    RtlZeroMemory(&Frontend->StoreInterface,
                  sizeof(XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&Frontend->SuspendInterface,
                  sizeof(XENBUS_SUSPEND_INTERFACE));

    RtlZeroMemory(&Frontend->DebugInterface,
                  sizeof(XENBUS_DEBUG_INTERFACE));

    RtlZeroMemory(&Frontend->Lock, sizeof(KSPIN_LOCK));

    Frontend->BackendDomain = 0;

    __FrontendFree(Frontend->Path);
    Frontend->Path = NULL;

    Frontend->Pdo = NULL;

    ASSERT(IsZeroMemory(Frontend, sizeof(XENCONS_FRONTEND)));
    __FrontendFree(Frontend);
}
