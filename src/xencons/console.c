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

#include "driver.h"
#include "console.h"
#include "stream.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

struct _XENCONS_CONSOLE {
    PXENCONS_FDO        Fdo;
    LIST_ENTRY          List;
    KSPIN_LOCK          Lock;
};

typedef struct _XENCONS_HANDLE {
    LIST_ENTRY      ListEntry;
    PFILE_OBJECT    FileObject;
    PXENCONS_STREAM Stream;
} XENCONS_HANDLE, *PXENCONS_HANDLE;

#define CONSOLE_POOL 'SNOC'

static FORCEINLINE PVOID
__ConsoleAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, CONSOLE_POOL);
}

static FORCEINLINE VOID
__ConsoleFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, CONSOLE_POOL);
}

static VOID
__ConsoleDestroyHandle(
    IN  PXENCONS_CONSOLE    Console,
    IN  PXENCONS_HANDLE     Handle
    )
{
    UNREFERENCED_PARAMETER(Console);

    Trace("%p\n", Handle->FileObject);

    RtlZeroMemory(&Handle->ListEntry, sizeof(LIST_ENTRY));

    StreamDestroy(Handle->Stream);
    Handle->Stream = NULL;

    Handle->FileObject = NULL;

    ASSERT(IsZeroMemory(Handle, sizeof(XENCONS_HANDLE)));
    __ConsoleFree(Handle);
}

static PXENCONS_HANDLE
__ConsoleFindHandle(
    IN  PXENCONS_CONSOLE    Console,
    IN  PFILE_OBJECT        FileObject
    )
{
    KIRQL                   Irql;
    PLIST_ENTRY             ListEntry;
    PXENCONS_HANDLE         Handle;
    NTSTATUS                status;

    KeAcquireSpinLock(&Console->Lock, &Irql);

    for (ListEntry = Console->List.Flink;
         ListEntry != &Console->List;
         ListEntry = ListEntry->Flink) {
        Handle = CONTAINING_RECORD(ListEntry,
                                   XENCONS_HANDLE,
                                   ListEntry);

        if (Handle->FileObject == FileObject)
            goto found;
    }

    status = STATUS_UNSUCCESSFUL;
    goto fail1;

found:
    KeReleaseSpinLock(&Console->Lock, Irql);

    return Handle;

fail1:
    Error("fail1 (%08x)\n", status);

    KeReleaseSpinLock(&Console->Lock, Irql);

    return NULL;
}

NTSTATUS
ConsoleDispatchCreate(
    IN  PXENCONS_CONSOLE    Console,
    IN  PFILE_OBJECT        FileObject
    )
{
    PXENCONS_HANDLE         Handle;
    KIRQL                   Irql;
    NTSTATUS                status;

    ASSERT(__ConsoleFindHandle(Console, FileObject) == NULL);

    Handle = __ConsoleAllocate(sizeof(XENCONS_HANDLE));

    status = STATUS_NO_MEMORY;
    if (Handle == NULL)
        goto fail1;

    status = StreamCreate(Console->Fdo, &Handle->Stream);
    if (!NT_SUCCESS(status))
        goto fail2;

    Handle->FileObject = FileObject;

    KeAcquireSpinLock(&Console->Lock, &Irql);
    InsertTailList(&Console->List, &Handle->ListEntry);
    KeReleaseSpinLock(&Console->Lock, Irql);

    Trace("%p\n", Handle->FileObject);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    ASSERT(IsZeroMemory(Handle, sizeof(XENCONS_HANDLE)));
    __ConsoleFree(Handle);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
ConsoleDispatchCleanup(
    IN  PXENCONS_CONSOLE    Console,
    IN  PFILE_OBJECT        FileObject
    )
{
    PXENCONS_HANDLE         Handle;
    KIRQL                   Irql;
    NTSTATUS                status;

    Handle = __ConsoleFindHandle(Console, FileObject);
    status = STATUS_UNSUCCESSFUL;

    if (Handle == NULL)
        goto fail1;

    KeAcquireSpinLock(&Console->Lock, &Irql);
    RemoveEntryList(&Handle->ListEntry);
    KeReleaseSpinLock(&Console->Lock, Irql);

    __ConsoleDestroyHandle(Console, Handle);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
ConsoleDispatchReadWrite(
    IN  PXENCONS_CONSOLE    Console,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    PXENCONS_HANDLE         Handle;
    NTSTATUS                status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    Handle = __ConsoleFindHandle(Console, StackLocation->FileObject);
    status = STATUS_UNSUCCESSFUL;

    if (Handle == NULL)
        goto fail1;

    // If StreamPutQueue succeeds, the IRP is queued and marked pending
    status = StreamPutQueue(Handle->Stream, Irp);
    if (!NT_SUCCESS(status))
        goto fail2;

    return STATUS_PENDING;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
ConsoleEnable(
    IN  PXENCONS_CONSOLE    Console
    )
{
    UNREFERENCED_PARAMETER(Console);
    return STATUS_SUCCESS;
}

VOID
ConsoleDisable(
    IN  PXENCONS_CONSOLE    Console
    )
{
    KIRQL                   Irql;
    LIST_ENTRY              List;
    PLIST_ENTRY             ListEntry;
    PXENCONS_HANDLE         Handle;

    InitializeListHead(&List);

    KeAcquireSpinLock(&Console->Lock, &Irql);

    ListEntry = Console->List.Flink;
    if (!IsListEmpty(&Console->List)) {
        RemoveEntryList(&Console->List);
        InitializeListHead(&Console->List);
        AppendTailList(&List, ListEntry);
    }

    KeReleaseSpinLock(&Console->Lock, Irql);

    while (!IsListEmpty(&List)) {
        ListEntry = RemoveHeadList(&List);
        ASSERT3P(ListEntry, != , &List);

        Handle = CONTAINING_RECORD(ListEntry,
                                   XENCONS_HANDLE,
                                   ListEntry);

        __ConsoleDestroyHandle(Console, Handle);
    }
}

NTSTATUS
ConsoleCreate(
    IN  PXENCONS_FDO        Fdo,
    OUT PXENCONS_CONSOLE    *Console
    )
{
    NTSTATUS                status;

    *Console = __ConsoleAllocate(sizeof(XENCONS_CONSOLE));

    status = STATUS_NO_MEMORY;
    if (*Console == NULL)
        goto fail1;

    (*Console)->Fdo = Fdo;
    InitializeListHead(&(*Console)->List);
    KeInitializeSpinLock(&(*Console)->Lock);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
ConsoleDestroy(
    IN  PXENCONS_CONSOLE    Console
    )
{
    RtlZeroMemory(&Console->Lock, sizeof(KSPIN_LOCK));

    ASSERT(IsListEmpty(&Console->List));
    RtlZeroMemory(&Console->List, sizeof(LIST_ENTRY));

    Console->Fdo = NULL;

    ASSERT(IsZeroMemory(Console, sizeof(XENCONS_CONSOLE)));
    __ConsoleFree(Console);
}
