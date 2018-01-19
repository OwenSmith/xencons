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

#ifndef _XENCONS_FRONTEND_H
#define _XENCONS_FRONTEND_H

#include <ntddk.h>

#include "driver.h"

typedef struct _XENCONS_FRONTEND XENCONS_FRONTEND, *PXENCONS_FRONTEND;

typedef enum _FRONTEND_STATE {
    FRONTEND_UNKNOWN,
    FRONTEND_CLOSED,
    FRONTEND_PREPARED,
    FRONTEND_CONNECTED,
    FRONTEND_ENABLED
} FRONTEND_STATE, *PFRONTEND_STATE;

extern NTSTATUS
FrontendCreate(
    IN  PXENCONS_PDO        Pdo,
    OUT PXENCONS_FRONTEND   *Frontend
    );

extern VOID
FrontendDestroy(
    IN  PXENCONS_FRONTEND   Frontend
    );

extern NTSTATUS
FrontendResume(
    IN  PXENCONS_FRONTEND   Frontend
    );

extern VOID
FrontendSuspend(
    IN  PXENCONS_FRONTEND   Frontend
    );

extern NTSTATUS
FrontendSetState(
    IN  PXENCONS_FRONTEND   Frontend,
    IN  FRONTEND_STATE      State
    );

extern PXENCONS_PDO
FrontendGetPdo(
    IN  PXENCONS_FRONTEND   Frontend
    );

extern PCHAR
FrontendGetPath(
    IN  PXENCONS_FRONTEND   Frontend
    );

extern PCHAR
FrontendGetBackendPath(
    IN  PXENCONS_FRONTEND   Frontend
    );

extern USHORT
FrontendGetBackendDomain(
    IN  PXENCONS_FRONTEND   Frontend
    );

extern PCHAR
FrontendGetName(
    IN  PXENCONS_FRONTEND   Frontend
    );

extern PCHAR
FrontendGetProtocol(
    IN  PXENCONS_FRONTEND   Frontend
    );

extern NTSTATUS
FrontendDispatchCreate(
    IN  PXENCONS_FRONTEND   Frontend,
    IN  PFILE_OBJECT        FileObject
    );

extern NTSTATUS
FrontendDispatchCleanup(
    IN  PXENCONS_FRONTEND   Frontend,
    IN  PFILE_OBJECT        FileObject
    );

extern NTSTATUS
FrontendDispatchReadWrite(
    IN  PXENCONS_FRONTEND   Frontend,
    IN  PIRP                Irp
    );

#endif  // _XENCONS_FRONTEND_H
