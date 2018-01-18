/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 *
 * Redistribution and use in source 1and binary forms,
 * with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * *   Redistributions of source code must retain the above
 *     copyright notice, this list of conditions and the23
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

#define INITGUID 1
#define UNICODE

#include <windows.h>
#include <tchar.h>
#include <stdlib.h>
#include <strsafe.h>
#include <wtsapi32.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <setupapi.h>
#include <malloc.h>
#include <assert.h>
#include <winioctl.h>

#include <xencons_device.h>
#include <version.h>

#include "messages.h"

#define MONITOR_NAME        __MODULE__
#define MONITOR_DISPLAYNAME MONITOR_NAME

typedef struct _MONITOR_HANDLE {
    PTCHAR                  DevicePath;
    PTCHAR                  Executable;
    PCHAR                   Name;
    HDEVNOTIFY              DeviceNotification;
    HANDLE                  Device;
    HANDLE                  MonitorEvent;
    HANDLE                  MonitorThread;
    HANDLE                  DeviceEvent;
    HANDLE                  DeviceThread;
    HANDLE                  ServerEvent;
    HANDLE                  ServerThread;
    CRITICAL_SECTION        CriticalSection;
    LIST_ENTRY              ListHead;
    DWORD                   ListCount;
    LIST_ENTRY              ListEntry;
} MONITOR_HANDLE, *PMONITOR_HANDLE;

typedef struct _MONITOR_PIPE {
    PMONITOR_HANDLE         Handle;
    HANDLE                  Pipe;
    HANDLE                  Event;
    HANDLE                  Thread;
    LIST_ENTRY              ListEntry;
} MONITOR_PIPE, *PMONITOR_PIPE;

typedef struct _MONITOR_CONTEXT {
    SERVICE_STATUS          Status;
    SERVICE_STATUS_HANDLE   Service;
    HKEY                    ParametersKey;
    HANDLE                  EventLog;
    HANDLE                  StopEvent;
    HDEVNOTIFY              InterfaceNotification;
    CRITICAL_SECTION        CriticalSection;
    LIST_ENTRY              ListHead;
    DWORD                   ListCount;
} MONITOR_CONTEXT, *PMONITOR_CONTEXT;

MONITOR_CONTEXT MonitorContext;

#define PIPE_NAME TEXT("\\\\.\\pipe\\xencons")

#define MAXIMUM_BUFFER_SIZE 1024

#define SERVICES_KEY "SYSTEM\\CurrentControlSet\\Services"

#define SERVICE_KEY(_Service) \
        SERVICES_KEY ## "\\" ## _Service

#define PARAMETERS_KEY(_Service) \
        SERVICE_KEY(_Service) ## "\\Parameters"

static VOID
#pragma prefast(suppress:6262) // Function uses '1036' bytes of stack: exceeds /analyze:stacksize'1024'
__Log(
    IN  const TCHAR      *Format,
    IN  ...
    )
{
#if DBG
    PMONITOR_CONTEXT    Context = &MonitorContext;
    const TCHAR         *Strings[1];
#endif
    TCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    va_list             Arguments;
    size_t              Length;
    HRESULT             Result;

    va_start(Arguments, Format);
    Result = StringCchVPrintf(Buffer,
                              MAXIMUM_BUFFER_SIZE,
                              Format,
                              Arguments);
    va_end(Arguments);

    if (Result != S_OK && Result != STRSAFE_E_INSUFFICIENT_BUFFER)
        return;

    Result = StringCchLength(Buffer, MAXIMUM_BUFFER_SIZE, &Length);
    if (Result != S_OK)
        return;

    Length = __min(MAXIMUM_BUFFER_SIZE - 1, Length + 2);

    __analysis_assume(Length < MAXIMUM_BUFFER_SIZE);
    __analysis_assume(Length >= 2);
    Buffer[Length] = '\0';
    Buffer[Length - 1] = '\n';
    Buffer[Length - 2] = '\r';

    OutputDebugString(Buffer);

#if DBG
    Strings[0] = Buffer;

    if (Context->EventLog != NULL)
        ReportEvent(Context->EventLog,
                    EVENTLOG_INFORMATION_TYPE,
                    0,
                    MONITOR_LOG,
                    NULL,
                    ARRAYSIZE(Strings),
                    0,
                    Strings,
                    NULL);
#endif
}

#define Log(_Format, ...) \
    __Log(TEXT(__MODULE__ "|" __FUNCTION__ ": " _Format), __VA_ARGS__)

static PTCHAR
GetErrorMessage(
    IN  HRESULT Error
    )
{
    PTCHAR      Message;
    ULONG       Index;

    if (!FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                       FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL,
                       Error,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       (LPTSTR)&Message,
                       0,
                       NULL))
        return NULL;

    for (Index = 0; Message[Index] != '\0'; Index++) {
        if (Message[Index] == '\r' || Message[Index] == '\n') {
            Message[Index] = '\0';
            break;
        }
    }

    return Message;
}

static const CHAR *
ServiceStateName(
    IN  DWORD   State
    )
{
#define _STATE_NAME(_State) \
    case SERVICE_ ## _State: \
        return #_State

    switch (State) {
    _STATE_NAME(START_PENDING);
    _STATE_NAME(RUNNING);
    _STATE_NAME(STOP_PENDING);
    _STATE_NAME(STOPPED);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _STATE_NAME
}

static VOID
ReportStatus(
    IN  DWORD           CurrentState,
    IN  DWORD           Win32ExitCode,
    IN  DWORD           WaitHint)
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    static DWORD        CheckPoint = 1;
    BOOL                Success;
    HRESULT             Error;

    Log("====> (%hs)", ServiceStateName(CurrentState));

    Context->Status.dwCurrentState = CurrentState;
    Context->Status.dwWin32ExitCode = Win32ExitCode;
    Context->Status.dwWaitHint = WaitHint;

    if (CurrentState == SERVICE_START_PENDING)
        Context->Status.dwControlsAccepted = 0;
    else
        Context->Status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                             SERVICE_ACCEPT_SHUTDOWN |
                                             SERVICE_ACCEPT_SESSIONCHANGE;

    if (CurrentState == SERVICE_RUNNING ||
        CurrentState == SERVICE_STOPPED )
        Context->Status.dwCheckPoint = 0;
    else
        Context->Status.dwCheckPoint = CheckPoint++;

    Success = SetServiceStatus(Context->Service, &Context->Status);

    if (!Success)
        goto fail1;

    Log("<====");

    return;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static FORCEINLINE VOID
__InitializeListHead(
    IN  PLIST_ENTRY ListEntry
    )
{
    ListEntry->Flink = ListEntry;
    ListEntry->Blink = ListEntry;
}

static FORCEINLINE VOID
__InsertTailList(
    IN  PLIST_ENTRY ListHead,
    IN  PLIST_ENTRY ListEntry
    )
{
    ListEntry->Blink = ListHead->Blink;
    ListEntry->Flink = ListHead;
    ListHead->Blink->Flink = ListEntry;
    ListHead->Blink = ListEntry;
}

static FORCEINLINE VOID
__RemoveEntryList(
    IN  PLIST_ENTRY ListEntry
    )
{
    PLIST_ENTRY     Flink;
    PLIST_ENTRY     Blink;

    Flink = ListEntry->Flink;
    Blink = ListEntry->Blink;
    Flink->Blink = Blink;
    Blink->Flink = Flink;

    ListEntry->Flink = ListEntry;
    ListEntry->Blink = ListEntry;
}

static VOID
PutString(
    IN  HANDLE      Handle,
    IN  PUCHAR      Buffer,
    IN  DWORD       Length
    )
{
    DWORD           Offset;

    Offset = 0;
    while (Offset < Length) {
        DWORD   Written;
        BOOL    Success;

        Success = WriteFile(Handle,
                            &Buffer[Offset],
                            Length - Offset,
                            &Written,
                            NULL);
        if (!Success)
            break;

        Offset += Written;
    }
}

DWORD WINAPI
PipeThread(
    IN  LPVOID          Argument
    )
{
    PMONITOR_PIPE       Pipe = (PMONITOR_PIPE)Argument;
    PMONITOR_HANDLE     Handle = Pipe->Handle;
    UCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    OVERLAPPED          Overlapped;
    HANDLE              Handles[2];
    DWORD               Length;
    DWORD               Object;
    HRESULT             Error;

    Log("====> %hs", Handle->Name);

    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);
    if (Overlapped.hEvent == NULL)
        goto fail1;

    Handles[0] = Pipe->Event;
    Handles[1] = Overlapped.hEvent;

    EnterCriticalSection(&Handle->CriticalSection);
    __InsertTailList(&Handle->ListHead, &Pipe->ListEntry);
    ++Handle->ListCount;
    LeaveCriticalSection(&Handle->CriticalSection);

    for (;;) {
        (VOID) ReadFile(Pipe->Pipe,
                        Buffer,
                        sizeof(Buffer),
                        NULL,
                        &Overlapped);

        Object = WaitForMultipleObjects(ARRAYSIZE(Handles),
                                        Handles,
                                        FALSE,
                                        INFINITE);
        if (Object == WAIT_OBJECT_0)
            break;

        if (!GetOverlappedResult(Pipe->Pipe,
                                 &Overlapped,
                                 &Length,
                                 FALSE))
            break;

        ResetEvent(Overlapped.hEvent);

        PutString(Handle->Device,
                  Buffer,
                  Length);
    }

    EnterCriticalSection(&Handle->CriticalSection);
    __RemoveEntryList(&Pipe->ListEntry);
    --Handle->ListCount;
    LeaveCriticalSection(&Handle->CriticalSection);

    CloseHandle(Overlapped.hEvent);

    FlushFileBuffers(Pipe->Pipe);
    DisconnectNamedPipe(Pipe->Pipe);
    CloseHandle(Pipe->Pipe);
    CloseHandle(Pipe->Thread);
    free(Pipe);

    Log("<==== %hs", Handle->Name);

    return 0;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

DWORD WINAPI
ServerThread(
    IN  LPVOID          Argument
    )
{
    PMONITOR_HANDLE     Handle = (PMONITOR_HANDLE)Argument;
    OVERLAPPED          Overlapped;
    HANDLE              Handles[2];
    HANDLE              Pipe;
    DWORD               Object;
    TCHAR               PipeName[MAX_PATH];
    PMONITOR_PIPE       Instance;
    HRESULT             Error;

    Log("====> %hs", Handle->Name);

    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);
    if (Overlapped.hEvent == NULL)
        goto fail1;

    Handles[0] = Handle->ServerEvent;
    Handles[1] = Overlapped.hEvent;

    Error = StringCbPrintf(PipeName,
                           sizeof(PipeName),
                           TEXT("%s\\%hs"),
                           PIPE_NAME,
                           Handle->Name);

    if (Error != S_OK && Error != STRSAFE_E_INSUFFICIENT_BUFFER)
        goto fail2;

    Log("PipeName = %s", PipeName);

    for (;;) {
        Pipe = CreateNamedPipe(PipeName,
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                               PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE,
                               PIPE_UNLIMITED_INSTANCES,
                               MAXIMUM_BUFFER_SIZE,
                               MAXIMUM_BUFFER_SIZE,
                               0,
                               NULL);
        if (Pipe == INVALID_HANDLE_VALUE)
            goto fail3;

        (VOID)ConnectNamedPipe(Pipe,
                               &Overlapped);

        Object = WaitForMultipleObjects(ARRAYSIZE(Handles),
                                        Handles,
                                        FALSE,
                                        INFINITE);
        if (Object == WAIT_OBJECT_0) {
            CloseHandle(Pipe);
            break;
        }

        ResetEvent(Overlapped.hEvent);

        Instance = (PMONITOR_PIPE)malloc(sizeof(MONITOR_PIPE));
        if (Instance == NULL)
            goto fail4;

        __InitializeListHead(&Instance->ListEntry);
        Instance->Handle = Handle;
        Instance->Pipe = Pipe;
        Instance->Event = Handle->ServerEvent;
        Instance->Thread = CreateThread(NULL,
                                        0,
                                        PipeThread,
                                        Instance,
                                        0,
                                        NULL);
        if (Instance->Thread == INVALID_HANDLE_VALUE)
            goto fail5;
    }

    CloseHandle(Overlapped.hEvent);

    Log("<==== %hs", Handle->Name);

    return 0;

fail5:
    Log("fail5");

    free(Instance);

fail4:
    Log("fail4");

    CloseHandle(Pipe);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    CloseHandle(Overlapped.hEvent);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

DWORD WINAPI
MonitorThread(
    IN  LPVOID          Argument
    )
{
    PMONITOR_HANDLE     Handle = (PMONITOR_HANDLE)Argument;
    PROCESS_INFORMATION ProcessInfo;
    STARTUPINFO         StartupInfo;
    BOOL                Success;
    HANDLE              Handles[2];
    DWORD               Object;
    HRESULT             Error;

    Log("====> %hs", Handle->Name);

    // If there is no executable, this thread can finish now.
    if (Handle->Executable == NULL)
        goto done;

again:
    ZeroMemory(&ProcessInfo, sizeof (ProcessInfo));
    ZeroMemory(&StartupInfo, sizeof (StartupInfo));
    StartupInfo.cb = sizeof (StartupInfo);

    Log("Executing: %s", Handle->Executable);

#pragma warning(suppress:6053) // CommandLine might not be NUL-terminated
    Success = CreateProcess(NULL,
                            Handle->Executable,
                            NULL,
                            NULL,
                            FALSE,
                            CREATE_NO_WINDOW |
                            CREATE_NEW_PROCESS_GROUP,
                            NULL,
                            NULL,
                            &StartupInfo,
                            &ProcessInfo);
    if (!Success)
        goto fail1;

    Handles[0] = Handle->MonitorEvent;
    Handles[1] = ProcessInfo.hProcess;

    Object = WaitForMultipleObjects(ARRAYSIZE(Handles),
                                   Handles,
                                   FALSE,
                                   INFINITE);

#define WAIT_OBJECT_1 (WAIT_OBJECT_0 + 1)

    switch (Object) {
    case WAIT_OBJECT_0:
        ResetEvent(Handle->MonitorEvent);

        TerminateProcess(ProcessInfo.hProcess, 1);
        CloseHandle(ProcessInfo.hProcess);
        CloseHandle(ProcessInfo.hThread);
        break;

    case WAIT_OBJECT_1:
        CloseHandle(ProcessInfo.hProcess);
        CloseHandle(ProcessInfo.hThread);
        goto again;

    default:
        break;
    }

//#undef WAIT_OBJECT_1

done:
    Log("<==== %hs", Handle->Name);

    return 0;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

DWORD WINAPI
DeviceThread(
    IN  LPVOID          Argument
    )
{
    PMONITOR_HANDLE     Handle = (PMONITOR_HANDLE)Argument;
    OVERLAPPED          Overlapped;
    HANDLE              Device;
    UCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    DWORD               Length;
    DWORD               Wait;
    HANDLE              Handles[2];
    DWORD               Error;

    Log("====> %hs", Handle->Name);

    ZeroMemory(&Overlapped, sizeof(OVERLAPPED));
    Overlapped.hEvent = CreateEvent(NULL,
                                    TRUE,
                                    FALSE,
                                    NULL);
    if (Overlapped.hEvent == NULL)
        goto fail1;

    Handles[0] = Handle->DeviceEvent;
    Handles[1] = Overlapped.hEvent;

    Device = CreateFile(Handle->DevicePath,
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL,
                        OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED,
                        NULL);
    if (Device == INVALID_HANDLE_VALUE)
        goto fail2;

    for (;;) {
        PLIST_ENTRY     ListEntry;

        (VOID) ReadFile(Device,
                        Buffer,
                        sizeof(Buffer),
                        NULL,
                        &Overlapped);

        Wait = WaitForMultipleObjects(ARRAYSIZE(Handles),
                                      Handles,
                                      FALSE,
                                      INFINITE);
        if (Wait == WAIT_OBJECT_0)
            break;

        if (!GetOverlappedResult(Device,
                                 &Overlapped,
                                 &Length,
                                 FALSE))
            break;

        ResetEvent(Overlapped.hEvent);

        EnterCriticalSection(&Handle->CriticalSection);

        for (ListEntry = Handle->ListHead.Flink;
             ListEntry != &Handle->ListHead;
             ListEntry = ListEntry->Flink) {
            PMONITOR_PIPE   Instance;

            Instance = CONTAINING_RECORD(ListEntry, MONITOR_PIPE, ListEntry);

            PutString(Instance->Pipe,
                      Buffer,
                      Length);
        }
        LeaveCriticalSection(&Handle->CriticalSection);
    }

    CloseHandle(Device);

    CloseHandle(Overlapped.hEvent);

    Log("<==== %hs", Handle->Name);

    return 0;

fail2:
    Log("fail2\n");

    CloseHandle(Overlapped.hEvent);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return 1;
}

#define ECHO(_Handle, _Buffer) \
    PutString((_Handle), (PUCHAR)TEXT(_Buffer), (DWORD)_tcslen((_Buffer)) * sizeof(TCHAR))

static VOID
MonitorWaitForPipeThreads(
    IN  PMONITOR_HANDLE Handle
    )
{
    HANDLE              *Handles;
    DWORD               Index;
    PLIST_ENTRY         ListEntry;

    EnterCriticalSection(&Handle->CriticalSection);

    if (Handle->ListCount == 0)
        goto fail1;

    Handles = (HANDLE*)malloc(sizeof(HANDLE) * Handle->ListCount);
    if (Handles == NULL)
        goto fail2;

    Index = 0;
    for (ListEntry = Handle->ListHead.Flink;
         ListEntry != &Handle->ListHead && Index < Handle->ListCount;
         ListEntry = ListEntry->Flink) {
        PMONITOR_PIPE Pipe = CONTAINING_RECORD(ListEntry, MONITOR_PIPE, ListEntry);
        Handles[Index++] = Pipe->Thread;
    }

    Handle->ListCount = 0;

    LeaveCriticalSection(&Handle->CriticalSection);

#pragma warning(suppress:6385) // Reading invalid data from 'Handles'...
    WaitForMultipleObjects(Index,
                           Handles,
                           TRUE,
                           INFINITE);
    free(Handles);
    return;

fail2:
    Log("fail2");

fail1:
    Log("fail1");

    LeaveCriticalSection(&Handle->CriticalSection);

    return;
}

static BOOL
GetExecutable(
    IN  PCHAR           Name,
    OUT PTCHAR          *Executable
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    DWORD               MaxValueLength;
    DWORD               ExecutableLength;
    HKEY                Key;
    DWORD               Type;
    HRESULT             Error;


    Error = RegOpenKeyExA(Context->ParametersKey,
                         Name,
                         0,
                         KEY_READ,
                         &Key);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    Error = RegQueryInfoKey(Key,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            &MaxValueLength,
                            NULL,
                            NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    ExecutableLength = MaxValueLength + sizeof (TCHAR);

    *Executable = calloc(1, ExecutableLength);
    if (Executable == NULL)
        goto fail3;

    Error = RegQueryValueEx(Key,
                            TEXT("Executable"),
                            NULL,
                            &Type,
                            (LPBYTE)(*Executable),
                            &ExecutableLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail4;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail5;
    }

    RegCloseKey(Key);

    return TRUE;

fail5:
    Log("fail5");

fail4:
    Log("fail4");

    free(*Executable);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    RegCloseKey(Key);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
MonitorCreateHandle(
    IN  PTCHAR              DevicePath,
    OUT PMONITOR_HANDLE     *Handle
    )
{
    PMONITOR_CONTEXT        Context = &MonitorContext;
    DEV_BROADCAST_HANDLE    Notification;
    CHAR                    Name[MAX_PATH];
    DWORD                   Bytes;
    HRESULT                 Error;
    BOOL                    Success;

    Log("====> %s", DevicePath);

    *Handle = malloc(sizeof(MONITOR_HANDLE));
    if (*Handle == NULL)
        goto fail1;

    (*Handle)->DevicePath = _wcsdup(DevicePath);
    if ((*Handle)->DevicePath == NULL)
        goto fail2;

    (*Handle)->Device = CreateFile(DevicePath,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                NULL);

    if ((*Handle)->Device == INVALID_HANDLE_VALUE)
        goto fail3;

    ECHO((*Handle)->Device, "\r\n[ATTACHED]\r\n");

    ZeroMemory(&Notification, sizeof(Notification));
    Notification.dbch_size = sizeof(Notification);
    Notification.dbch_devicetype = DBT_DEVTYP_HANDLE;
    Notification.dbch_handle = (*Handle)->Device;

    (*Handle)->DeviceNotification =
        RegisterDeviceNotification(Context->Service,
                                   &Notification,
                                   DEVICE_NOTIFY_SERVICE_HANDLE);
    if ((*Handle)->DeviceNotification == NULL)
        goto fail4;

    memset(Name, 0, sizeof(Name));
    Success = DeviceIoControl((*Handle)->Device,
                              IOCTL_XENCONS_GET_NAME,
                              NULL,
                              0,
                              Name,
                              sizeof(Name),
                              &Bytes,
                              NULL);
    if (!Success)
        goto fail5;

    Log("Name = %hs", Name);

    (*Handle)->Name = _strdup(Name);
    if ((*Handle)->Name == NULL)
        goto fail6;

    Success = GetExecutable((*Handle)->Name, &(*Handle)->Executable);
    if (!Success)
        (*Handle)->Executable = NULL;

    Log("Executable = %s", (*Handle)->Executable);

    __InitializeListHead(&(*Handle)->ListHead);
    InitializeCriticalSection(&(*Handle)->CriticalSection);

    (*Handle)->DeviceEvent = CreateEvent(NULL,
                                      TRUE,
                                      FALSE,
                                      NULL);

    if ((*Handle)->DeviceEvent == NULL)
        goto fail7;

    (*Handle)->DeviceThread = CreateThread(NULL,
                                        0,
                                        DeviceThread,
                                        *Handle,
                                        0,
                                        NULL);

    if ((*Handle)->DeviceThread == NULL)
        goto fail8;

    (*Handle)->ServerEvent = CreateEvent(NULL,
                                      TRUE,
                                      FALSE,
                                      NULL);
    if ((*Handle)->ServerEvent == NULL)
        goto fail9;

    (*Handle)->ServerThread = CreateThread(NULL,
                                        0,
                                        ServerThread,
                                        *Handle,
                                        0,
                                        NULL);
    if ((*Handle)->ServerThread == NULL)
        goto fail10;

    (*Handle)->MonitorEvent = CreateEvent(NULL,
                                          TRUE,
                                          FALSE,
                                          NULL);

    if ((*Handle)->MonitorEvent == NULL)
        goto fail11;

    (*Handle)->MonitorThread = CreateThread(NULL,
                                            0,
                                            MonitorThread,
                                            *Handle,
                                            0,
                                            NULL);

    if ((*Handle)->MonitorThread == NULL)
        goto fail12;

    Log("<==== 0x%p", (PVOID)(*Handle)->Device);

    return TRUE;

fail12:
    Log("fail12");

    CloseHandle((*Handle)->MonitorEvent);
    (*Handle)->MonitorEvent = NULL;

fail11:
    Log("fail11");

    SetEvent((*Handle)->ServerEvent);
    WaitForSingleObject((*Handle)->ServerThread, INFINITE);

fail10:
    Log("fail10");

    CloseHandle((*Handle)->ServerEvent);
    (*Handle)->ServerEvent = NULL;

fail9:
    Log("fail9");

    SetEvent((*Handle)->DeviceEvent);
    WaitForSingleObject((*Handle)->DeviceThread, INFINITE);

fail8:
    Log("fail8\n");

    CloseHandle((*Handle)->DeviceEvent);
    (*Handle)->DeviceEvent = NULL;

fail7:
    Log("fail7");

    DeleteCriticalSection(&(*Handle)->CriticalSection);
    ZeroMemory(&(*Handle)->ListHead, sizeof(LIST_ENTRY));

    free((*Handle)->Executable);
    (*Handle)->Executable = NULL;

    free((*Handle)->Name);
    (*Handle)->Name = NULL;

fail6:
    Log("fail6");

fail5:
    Log("fail5");

    UnregisterDeviceNotification((*Handle)->DeviceNotification);
    (*Handle)->DeviceNotification = NULL;

    ECHO((*Handle)->Device, "\r\n[DETACHED]\r\n");

fail4:
    Log("fail4");

    CloseHandle((*Handle)->Device);
    (*Handle)->Device = INVALID_HANDLE_VALUE;

fail3:
    Log("fail3");

    free((*Handle)->DevicePath);
    (*Handle)->DevicePath = NULL;

fail2:
    Log("fail2");

    free(*Handle);
    *Handle = NULL;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static VOID
MonitorDeleteHandle(
    IN  PMONITOR_HANDLE Handle
    )
{
    Log("====> %s", Handle->DevicePath);

    SetEvent(Handle->MonitorEvent);
    WaitForSingleObject(Handle->MonitorThread, INFINITE);

    CloseHandle(Handle->MonitorEvent);
    Handle->MonitorEvent = NULL;

    SetEvent(Handle->ServerEvent);
    MonitorWaitForPipeThreads(Handle);
    WaitForSingleObject(Handle->ServerThread, INFINITE);

    CloseHandle(Handle->ServerEvent);
    Handle->ServerEvent = NULL;

    SetEvent(Handle->DeviceEvent);
    WaitForSingleObject(Handle->DeviceThread, INFINITE);

    CloseHandle(Handle->DeviceEvent);
    Handle->DeviceEvent = NULL;

    DeleteCriticalSection(&Handle->CriticalSection);
    ZeroMemory(&Handle->ListHead, sizeof(LIST_ENTRY));

    free(Handle->Executable);
    Handle->Executable = NULL;

    free(Handle->Name);
    Handle->Name = NULL;

    free(Handle->DevicePath);
    Handle->DevicePath = NULL;

    UnregisterDeviceNotification(Handle->DeviceNotification);
    Handle->DeviceNotification = NULL;

    ECHO(Handle->Device, "\r\n[DETACHED]\r\n");

    CloseHandle(Handle->Device);
    Handle->Device = INVALID_HANDLE_VALUE;

    free(Handle);

    Log("<====");
}

static VOID
MonitorAdd(
    IN  PTCHAR              DevicePath
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    PMONITOR_HANDLE     Handle;
    BOOL                Success;

    Log("====> %s", DevicePath);

    Success = MonitorCreateHandle(DevicePath, &Handle);
    if (!Success)
        goto fail1;

    EnterCriticalSection(&Context->CriticalSection);
    __InsertTailList(&Context->ListHead, &Handle->ListEntry);
    ++Context->ListCount;
    LeaveCriticalSection(&Context->CriticalSection);

    Log("<====");

    return;

fail1:
    Log("fail1");

    return;
}

static VOID
MonitorRemove(
    IN  HANDLE          Device
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    PMONITOR_HANDLE     Handle;
    PLIST_ENTRY         ListEntry;

    Log("====> 0x%p", (PVOID)Device);

    EnterCriticalSection(&Context->CriticalSection);
    for (ListEntry = Context->ListHead.Flink;
         ListEntry != &Context->ListHead;
         ListEntry = ListEntry->Flink) {
        Handle = CONTAINING_RECORD(ListEntry,
                                   MONITOR_HANDLE,
                                   ListEntry);

        if (Handle->Device != Device)
            continue;

        LeaveCriticalSection(&Context->CriticalSection);

        MonitorDeleteHandle(Handle);

        Log("<====");
        return;
    }
    LeaveCriticalSection(&Context->CriticalSection);

    Log("<====");
}

static BOOL
MonitorEnumerate(
    VOID
    )
{
    HDEVINFO                            DeviceInfoSet;
    SP_DEVICE_INTERFACE_DATA            DeviceInterfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA    DeviceInterfaceDetail;
    DWORD                               Size;
    DWORD                               Index;
    HRESULT                             Error;
    BOOL                                Success;

    Log("====>");

    DeviceInfoSet = SetupDiGetClassDevs(&GUID_XENCONS_DEVICE,
                                        NULL,
                                        NULL,
                                        DIGCF_PRESENT |
                                        DIGCF_DEVICEINTERFACE);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        goto fail1;

    for (Index = 0; TRUE; ++Index) {
        DeviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        Success = SetupDiEnumDeviceInterfaces(DeviceInfoSet,
                                              NULL,
                                              &GUID_XENCONS_DEVICE,
                                              Index,
                                              &DeviceInterfaceData);
        if (!Success)
            break;

        Success = SetupDiGetDeviceInterfaceDetail(DeviceInfoSet,
                                                  &DeviceInterfaceData,
                                                  NULL,
                                                  0,
                                                  &Size,
                                                  NULL);
        if (!Success && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            goto fail2;

        DeviceInterfaceDetail = calloc(1, Size);
        if (DeviceInterfaceDetail == NULL)
            goto fail3;

        DeviceInterfaceDetail->cbSize =
            sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        Success = SetupDiGetDeviceInterfaceDetail(DeviceInfoSet,
                                                  &DeviceInterfaceData,
                                                  DeviceInterfaceDetail,
                                                  Size,
                                                  NULL,
                                                  NULL);
        if (!Success)
            goto fail4;

        MonitorAdd(DeviceInterfaceDetail->DevicePath);

        free(DeviceInterfaceDetail);
        continue;

fail4:
        Log("fail4");

        free(DeviceInterfaceDetail);

fail3:
        Log("fail3");

fail2:
        Log("fail2");

        SetupDiDestroyDeviceInfoList(DeviceInfoSet);

        goto fail1;
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    Log("<====");

    return TRUE;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static VOID
MonitorRemoveAll(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    PMONITOR_HANDLE     Handle;
    PLIST_ENTRY         ListEntry;

    Log("====>");

    EnterCriticalSection(&Context->CriticalSection);
    for (;;) {
        ListEntry = Context->ListHead.Flink;
        if (ListEntry == &Context->ListHead)
            break;

        __RemoveEntryList(ListEntry);

        LeaveCriticalSection(&Context->CriticalSection);

        Handle = CONTAINING_RECORD(ListEntry,
                                   MONITOR_HANDLE,
                                   ListEntry);


        MonitorDeleteHandle(Handle);

        EnterCriticalSection(&Context->CriticalSection);
    }
    LeaveCriticalSection(&Context->CriticalSection);

    Log("<====");
}

DWORD WINAPI
MonitorCtrlHandlerEx(
    IN  DWORD           Ctrl,
    IN  DWORD           EventType,
    IN  LPVOID          EventData,
    IN  LPVOID          Argument
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;

    UNREFERENCED_PARAMETER(Argument);

    switch (Ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
        SetEvent(Context->StopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
        return NO_ERROR;

    case SERVICE_CONTROL_DEVICEEVENT: {
        PDEV_BROADCAST_HDR  Header = EventData;

        switch (EventType) {
        case DBT_DEVICEARRIVAL:
            if (Header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                PDEV_BROADCAST_DEVICEINTERFACE  Interface = EventData;

                if (IsEqualGUID(&Interface->dbcc_classguid,
                                &GUID_XENCONS_DEVICE))
                    MonitorAdd(Interface->dbcc_name);
            }
            break;

        case DBT_DEVICEQUERYREMOVE:
        case DBT_DEVICEREMOVEPENDING:
        case DBT_DEVICEREMOVECOMPLETE:
            if (Header->dbch_devicetype == DBT_DEVTYP_HANDLE) {
                PDEV_BROADCAST_HANDLE Device = EventData;

                MonitorRemove(Device->dbch_handle);
            }
            break;
        }

        return NO_ERROR;
    }
    default:
        break;
    }

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

VOID WINAPI
MonitorMain(
    _In_    DWORD                   argc,
    _In_    LPTSTR                  *argv
    )
{
    PMONITOR_CONTEXT                Context = &MonitorContext;
    DEV_BROADCAST_DEVICEINTERFACE   Interface;
    HRESULT                         Error;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    Log("====>");

    __InitializeListHead(&Context->ListHead);
    InitializeCriticalSection(&Context->CriticalSection);

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         TEXT(PARAMETERS_KEY(__MODULE__)),
                         0,
                         KEY_READ,
                         &Context->ParametersKey);
    if (Error != ERROR_SUCCESS)
        goto fail1;

    Context->Service = RegisterServiceCtrlHandlerEx(TEXT(MONITOR_NAME),
                                                    MonitorCtrlHandlerEx,
                                                    NULL);
    if (Context->Service == NULL)
        goto fail2;

    Context->EventLog = RegisterEventSource(NULL,
                                            TEXT(MONITOR_NAME));
    if (Context->EventLog == NULL)
        goto fail3;

    Context->Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    Context->Status.dwServiceSpecificExitCode = 0;

    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    Context->StopEvent = CreateEvent(NULL,
                                     TRUE,
                                     FALSE,
                                     NULL);

    if (Context->StopEvent == NULL)
        goto fail4;

    ZeroMemory(&Interface, sizeof (Interface));
    Interface.dbcc_size = sizeof (Interface);
    Interface.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    Interface.dbcc_classguid = GUID_XENCONS_DEVICE;

    Context->InterfaceNotification =
        RegisterDeviceNotification(Context->Service,
                                   &Interface,
                                   DEVICE_NOTIFY_SERVICE_HANDLE);
    if (Context->InterfaceNotification == NULL)
        goto fail5;

    // The device may already by present
    MonitorEnumerate();

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    // wait until service is shut down
    WaitForSingleObject(Context->StopEvent,
                        INFINITE);

    MonitorRemoveAll();

    UnregisterDeviceNotification(Context->InterfaceNotification);

    CloseHandle(Context->StopEvent);

    ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);

    (VOID) DeregisterEventSource(Context->EventLog);

    CloseHandle(Context->ParametersKey);

    DeleteCriticalSection(&Context->CriticalSection);
    ZeroMemory(&Context->ListHead, sizeof(LIST_ENTRY));

    Log("<====");

    return;

fail5:
    Log("fail5");

    CloseHandle(Context->StopEvent);

fail4:
    Log("fail4");

    ReportStatus(SERVICE_STOPPED, GetLastError(), 0);

    (VOID) DeregisterEventSource(Context->EventLog);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    CloseHandle(Context->ParametersKey);

fail1:
    Error = GetLastError();

    DeleteCriticalSection(&Context->CriticalSection);
    ZeroMemory(&Context->ListHead, sizeof(LIST_ENTRY));

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static BOOL
MonitorCreate(
    VOID
    )
{
    SC_HANDLE   SCManager;
    SC_HANDLE   Service;
    TCHAR       Path[MAX_PATH];
    HRESULT     Error;

    Log("====>");

    if(!GetModuleFileName(NULL, Path, MAX_PATH))
        goto fail1;

    SCManager = OpenSCManager(NULL,
                              NULL,
                              SC_MANAGER_ALL_ACCESS);

    if (SCManager == NULL)
        goto fail2;

    Service = CreateService(SCManager,
                            TEXT(MONITOR_NAME),
                            TEXT(MONITOR_DISPLAYNAME),
                            SERVICE_ALL_ACCESS,
                            SERVICE_WIN32_OWN_PROCESS,
                            SERVICE_AUTO_START,
                            SERVICE_ERROR_NORMAL,
                            Path,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL);

    if (Service == NULL)
        goto fail3;

    CloseServiceHandle(Service);
    CloseServiceHandle(SCManager);

    Log("<====");

    return TRUE;

fail3:
    Log("fail3");

    CloseServiceHandle(SCManager);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
MonitorDelete(
    VOID
    )
{
    SC_HANDLE           SCManager;
    SC_HANDLE           Service;
    BOOL                Success;
    SERVICE_STATUS      Status;
    HRESULT             Error;

    Log("====>");

    SCManager = OpenSCManager(NULL,
                              NULL,
                              SC_MANAGER_ALL_ACCESS);

    if (SCManager == NULL)
        goto fail1;

    Service = OpenService(SCManager,
                          TEXT(MONITOR_NAME),
                          SERVICE_ALL_ACCESS);

    if (Service == NULL)
        goto fail2;

    Success = ControlService(Service,
                             SERVICE_CONTROL_STOP,
                             &Status);

    if (!Success)
        goto fail3;

    Success = DeleteService(Service);

    if (!Success)
        goto fail4;

    CloseServiceHandle(Service);
    CloseServiceHandle(SCManager);

    Log("<====");

    return TRUE;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

    CloseServiceHandle(Service);

fail2:
    Log("fail2");

    CloseServiceHandle(SCManager);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
MonitorEntry(
    VOID
    )
{
    SERVICE_TABLE_ENTRY Table[] = {
        { TEXT(MONITOR_NAME), MonitorMain },
        { NULL, NULL }
    };
    HRESULT             Error;

    Log("%s (%s) ====>",
        TEXT(MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR),
        TEXT(DAY_STR "/" MONTH_STR "/" YEAR_STR));

    if (!StartServiceCtrlDispatcher(Table))
        goto fail1;

    Log("%s (%s) <====",
        TEXT(MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR),
        TEXT(DAY_STR "/" MONTH_STR "/" YEAR_STR));

    return TRUE;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

int CALLBACK
_tWinMain(
    _In_        HINSTANCE   Current,
    _In_opt_    HINSTANCE   Previous,
    _In_        LPSTR       CmdLine,
    _In_        int         CmdShow
    )
{
    BOOL                    Success;

    UNREFERENCED_PARAMETER(Current);
    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CmdShow);

    if (_tcslen(CmdLine) != 0) {
         if (_stricmp(CmdLine, "create") == 0)
             Success = MonitorCreate();
         else if (_stricmp(CmdLine, "delete") == 0)
             Success = MonitorDelete();
         else
             Success = FALSE;
    } else
        Success = MonitorEntry();

    return Success ? 0 : 1;
}
