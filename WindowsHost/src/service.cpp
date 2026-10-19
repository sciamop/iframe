#include "host.h"  // first: winsock2.h has to come before windows.h

#include <stdio.h>
#include <wchar.h>


// The service runs iframe-windows-hostw.exe as SYSTEM in the console session (the one on the
// physical screen, whether someone is signed in or not), and starts it again when that session
// changes (signing out, switching users) or when it exits.

#define SERVICE_NAME L"iFrameWindowsHost"
// Set to ask the host to quit (it restores the monitor's resolution first). Only SYSTEM and
// administrators can create or set a Global\ event.
#define QUIT_EVENT L"Global\\iFrameWindowsHostQuit"

static SERVICE_STATUS_HANDLE status_handle;
static SERVICE_STATUS status;
static HANDLE stop_event, quit_event;
static wchar_t child_cmd[4096];

static void set_state(DWORD state) {
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWaitHint = state == SERVICE_STOP_PENDING ? 10000 : 0;
    SetServiceStatus(status_handle, &status);
}

static DWORD WINAPI on_control(DWORD code, DWORD type, void *data, void *ctx) {
    (void)type, (void)data, (void)ctx;
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        set_state(SERVICE_STOP_PENDING);
        SetEvent(stop_event);
    }
    return NO_ERROR;
}

static HANDLE launch(DWORD session) {
    HANDLE self = NULL, token = NULL;
    PROCESS_INFORMATION pi = {};
    bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &self) &&
              DuplicateTokenEx(self, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &token) &&
              SetTokenInformation(token, TokenSessionId, &session, sizeof session);
    if (ok) {
        STARTUPINFOW si = { sizeof si };
        si.lpDesktop = (LPWSTR)L"winsta0\\default";
        wchar_t cmd[sizeof child_cmd / sizeof *child_cmd];
        wcscpy(cmd, child_cmd);  // CreateProcess may write to it
        ok = CreateProcessAsUserW(token, NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    }
    if (!ok) host_log("service: couldn't start the host in session %lu (error %lu)", session, GetLastError());
    if (token) CloseHandle(token);
    if (self) CloseHandle(self);
    if (!ok) return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void stop_child(HANDLE child) {
    SetEvent(quit_event);
    if (WaitForSingleObject(child, 5000) == WAIT_TIMEOUT) {
        host_log("service: the host didn't quit; ending it");
        TerminateProcess(child, 1);
        WaitForSingleObject(child, 2000);
    }
    ResetEvent(quit_event);
    CloseHandle(child);
}

static void WINAPI service_main(DWORD argc, wchar_t **argv) {
    (void)argc, (void)argv;
    status_handle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, on_control, NULL);
    if (!status_handle) return;
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    quit_event = CreateEventW(NULL, TRUE, FALSE, QUIT_EVENT);
    set_state(SERVICE_RUNNING);
    host_log("service: started");

    HANDLE child = NULL;
    DWORD child_session = 0;
    ULONGLONG next_launch = 0;
    for (;;) {
        HANDLE waits[2] = { stop_event, child };
        DWORD r = WaitForMultipleObjects(child ? 2 : 1, waits, FALSE, 1000);
        if (r == WAIT_OBJECT_0) break;
        DWORD session = WTSGetActiveConsoleSessionId();  // 0xFFFFFFFF while sessions are switching
        if (child && r == WAIT_OBJECT_0 + 1) {
            DWORD code = 0;
            GetExitCodeProcess(child, &code);
            host_log("service: the host exited (%lu); starting it again", code);
            CloseHandle(child);
            child = NULL;
        } else if (child && session != child_session && session != 0xFFFFFFFF) {
            host_log("service: the console moved to session %lu", session);
            stop_child(child);
            child = NULL;
        }
        if (!child && session != 0xFFFFFFFF && GetTickCount64() >= next_launch) {
            next_launch = GetTickCount64() + 5000;  // no faster than this if it keeps exiting
            child = launch(session);
            child_session = session;
        }
    }
    if (child) stop_child(child);
    host_log("service: stopped");
    set_state(SERVICE_STOPPED);
}

int service_run(const wchar_t *child_args) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    swprintf(child_cmd, sizeof child_cmd / sizeof *child_cmd, L"\"%lsiframe-windows-hostw.exe\" --quit-event %ls%ls",
             exe, QUIT_EVENT, child_args);
    SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)SERVICE_NAME, service_main }, { NULL, NULL } };
    if (!StartServiceCtrlDispatcherW(table)) {
        host_log("--service is for the Windows service that install.ps1 sets up (error %lu)", GetLastError());
        return 1;
    }
    return 0;
}

void desktop_follow_input(void) {
    static thread_local HDESK ours;  // the desktop this thread switched to, if it has
    HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!input) return;
    wchar_t want[64] = L"", have[64] = L"";
    GetUserObjectInformationW(input, UOI_NAME, want, sizeof want, NULL);
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, have, sizeof have, NULL);
    if (!wcscmp(want, have) || !SetThreadDesktop(input)) {
        CloseDesktop(input);
        return;
    }
    if (ours) CloseDesktop(ours);
    ours = input;
}
