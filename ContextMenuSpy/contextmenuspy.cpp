#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <io.h>      // _setmode
#include <fcntl.h>   // _O_U16TEXT
#include <string>
#include <vector>

// ============================================================
// Context Menu Spy
// Dumps all context menu items when a right-click menu appears
// ============================================================

HWINEVENTHOOK g_hEventHook = NULL;
FILE* g_logFile = NULL;
bool g_isShiftPressed = false;

static BOOL WINAPI CtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT     ||
        ctrlType == CTRL_BREAK_EVENT ||
        ctrlType == CTRL_CLOSE_EVENT)
    {
        if (g_hEventHook) { UnhookWinEvent(g_hEventHook); g_hEventHook = NULL; }
        if (g_logFile)    { fclose(g_logFile); g_logFile = NULL; }
        ExitProcess(0);
    }
    return FALSE;
}

// Recursively dump menu items (supports submenus)
void DumpMenu(HMENU hMenu, int depth = 0)
{
    if (!hMenu) return;

    int count = GetMenuItemCount(hMenu);
    if (count <= 0) return;

    std::wstring indent(depth * 2, L' ');

    for (int i = 0; i < count; i++) {
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask  = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;

        wchar_t buf[512] = {};
        mii.dwTypeData = buf;
        mii.cch        = _countof(buf);

        if (!GetMenuItemInfoW(hMenu, i, TRUE, &mii)) continue;

        // Separator
        if (mii.fType & MFT_SEPARATOR) {
            wprintf(L"%ls[%02d] -------- (separator)\n", indent.c_str(), i);
            if (g_logFile)
                fwprintf(g_logFile, L"%ls[%02d] -------- (separator)\n", indent.c_str(), i);
            continue;
        }

        // Build state string
        std::wstring stateStr;
        if (mii.fState & MFS_GRAYED)   stateStr += L"GRAYED ";
        if (mii.fState & MFS_CHECKED)  stateStr += L"CHECKED ";
        if (mii.fState & MFS_DEFAULT)  stateStr += L"DEFAULT ";

        wprintf(L"%ls[%02d] ID=%-6u \"%ls\" %ls%ls\n",
            indent.c_str(),
            i,
            mii.wID,
            buf,
            stateStr.empty() ? L"" : L"[",
            stateStr.empty() ? L"" : (stateStr + L"]").c_str()
        );

        if (g_logFile)
            fwprintf(g_logFile, L"%ls[%02d] ID=%-6u \"%ls\" %ls%ls\n",
                indent.c_str(), i, mii.wID, buf,
                stateStr.empty() ? L"" : L"[",
                stateStr.empty() ? L"" : (stateStr + L"]").c_str()
            );

        // Recurse into submenu
        if (mii.hSubMenu) {
            wprintf(L"%ls  >> SubMenu:\n", indent.c_str());
            if (g_logFile)
                fwprintf(g_logFile, L"%ls  >> SubMenu:\n", indent.c_str());
            DumpMenu(mii.hSubMenu, depth + 1);
        }
    }
}

// WinEvent hook callback - fires when any popup menu opens
void CALLBACK WinEventProc(
    HWINEVENTHOOK hWinEventHook,
    DWORD         event,
    HWND          hwnd,
    LONG          idObject,
    LONG          idChild,
    DWORD         dwEventThread,
    DWORD         dwmsEventTime)
{
    if (event != EVENT_SYSTEM_MENUPOPUPSTART) return;

    // Check if Shift is pressed
    g_isShiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    // Get HMENU from the menu window
    HMENU hMenu = (HMENU)SendMessageW(hwnd, MN_GETHMENU, 0, 0);
    if (!hMenu) return;

    // Get process info
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    wchar_t winClass[256] = {};
    GetClassNameW(hwnd, winClass, _countof(winClass));

    wchar_t winTitle[256] = {};
    GetWindowTextW(hwnd, winTitle, _countof(winTitle));

    wprintf(L"\n");
    wprintf(L"========================================\n");
    wprintf(L"[Context Menu Detected]\n");
    wprintf(L"  HWND      : 0x%p\n", (void*)hwnd);
    wprintf(L"  PID       : %lu\n", pid);
    wprintf(L"  WinClass  : %ls\n", winClass);
    wprintf(L"  WinTitle  : %ls\n", winTitle);
    wprintf(L"  HMENU     : 0x%p\n", (void*)hMenu);
    wprintf(L"  ItemCount : %d\n", GetMenuItemCount(hMenu));
    wprintf(L"  Shift     : %ls\n", g_isShiftPressed ? L"YES" : L"NO");
    wprintf(L"----------------------------------------\n");

    if (g_logFile) {
        fwprintf(g_logFile, L"\n========================================\n");
        fwprintf(g_logFile, L"[Context Menu Detected]\n");
        fwprintf(g_logFile, L"  HWND      : 0x%p\n", (void*)hwnd);
        fwprintf(g_logFile, L"  PID       : %lu\n", pid);
        fwprintf(g_logFile, L"  WinClass  : %ls\n", winClass);
        fwprintf(g_logFile, L"  WinTitle  : %ls\n", winTitle);
        fwprintf(g_logFile, L"  HMENU     : 0x%p\n", (void*)hMenu);
        fwprintf(g_logFile, L"  ItemCount : %d\n", GetMenuItemCount(hMenu));
        fwprintf(g_logFile, L"  Shift     : %ls\n", g_isShiftPressed ? L"YES" : L"NO");
        fwprintf(g_logFile, L"----------------------------------------\n");
        fflush(g_logFile);
    }

    DumpMenu(hMenu);

    wprintf(L"========================================\n");
    if (g_logFile) {
        fwprintf(g_logFile, L"========================================\n");
        fflush(g_logFile);
    }
}

int wmain(int argc, wchar_t* argv[])
{
    // Enable UTF-16 console output
    _setmode(_fileno(stdout), _O_U16TEXT);

    wprintf(L"=== Context Menu Spy ===\n");
    wprintf(L"Right-click anywhere to inspect the menu.\n");
    wprintf(L"Exit: Ctrl+C\n\n");

    // Open log file (UTF-16LE)
    _wfopen_s(&g_logFile, L"context_menu_spy.log", L"w, ccs=UTF-16LE");
    if (g_logFile)
        wprintf(L"Logging to: context_menu_spy.log\n\n");

    // Install system-wide WinEvent hook
    g_hEventHook = SetWinEventHook(
        EVENT_SYSTEM_MENUPOPUPSTART,
        EVENT_SYSTEM_MENUPOPUPSTART,
        NULL,           // no DLL needed (out-of-context)
        WinEventProc,
        0,              // all processes
        0,              // all threads
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    if (!g_hEventHook) {
        wprintf(L"ERROR: SetWinEventHook failed (%lu)\n", GetLastError());
        return 1;
    }

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // Message loop (required for hook to fire)
    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessageW(&msg, NULL, 0, 0)) != 0) {
        if (bRet == -1) break;  // エラー時の無限ループを防ぐ
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWinEvent(g_hEventHook);

    if (g_logFile)
        fclose(g_logFile);

    return 0;
}