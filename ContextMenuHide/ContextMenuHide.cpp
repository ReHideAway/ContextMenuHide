#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>

// ============================================================
// ContextMenuHide.exe
// ContextMenuHideHook.dll を読み込んでグローバルフックをインストールする。
// タスクトレイ常駐アプリ。右クリックメニューから終了できる。
// ============================================================

#define IDI_APP      101
#define WM_TRAYICON  (WM_APP + 1)
#define ID_EXIT      100

typedef BOOL  (*FnInstallHook)();
typedef void  (*FnUninstallHook)();
typedef void (*FnReInstallHook)();

static FnUninstallHook  g_fnUninstall = nullptr;
static HMODULE          g_hDll        = nullptr;
static NOTIFYICONDATAW  g_nid         = {};
static HWINEVENTHOOK  g_hWinEvent  = NULL;
static FnReInstallHook g_fnReInstall = nullptr;

// ------------------------------------------------------------
// トレイアイコンを削除してフック・DLL を解放する
// ------------------------------------------------------------
static void Cleanup()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_hWinEvent)   { UnhookWinEvent(g_hWinEvent); g_hWinEvent = nullptr; }
    g_fnReInstall = nullptr;  // FreeLibrary より前に NULL 化して無効ポインタ呼び出しを防ぐ
    if (g_fnUninstall) { g_fnUninstall(); g_fnUninstall = nullptr; }
    if (g_hDll)        { FreeLibrary(g_hDll); g_hDll = nullptr; }
}

// ------------------------------------------------------------
// Explorer ウィンドウが新規作成されたときに SetWinEventHook から呼ばれる。
// CabinetWClass（現行 Win10/11 File Explorer）を検知して ReInstallHook を呼ぶ。
// ExploreWClass は Vista 以降のレガシークラス名（互換のため残す）。
// WINEVENT_OUTOFCONTEXT のため EXE のメッセージループスレッド上で直列実行される。
// ------------------------------------------------------------
static void CALLBACK OnWinEvent(
    HWINEVENTHOOK /*hWinEventHook*/,
    DWORD         /*event*/,
    HWND          hwnd,
    LONG          idObject,
    LONG          idChild,
    DWORD         /*dwEventThread*/,
    DWORD         /*dwmsEventTime*/)
{
    // OBJID_WINDOW / CHILDID_SELF のみ対象（子オブジェクトや非ウィンドウは無視）
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    // hwnd が NULL の場合（非ウィンドウオブジェクト）は GetClassNameW が失敗して return
    wchar_t cls[64] = {};
    if (!GetClassNameW(hwnd, cls, _countof(cls))) return;

    // Explorer のウィンドウクラス名でフィルタ（それ以外は無視）
    if (_wcsicmp(cls, L"CabinetWClass")  != 0 &&
        _wcsicmp(cls, L"ExploreWClass")  != 0) return;

    if (g_fnReInstall) g_fnReInstall();
}

// ------------------------------------------------------------
// ウィンドウプロシージャ
// ------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP)
        {
            HMENU hMenu = CreatePopupMenu();
            if (!hMenu) return 0;
            AppendMenuW(hMenu, MF_STRING, ID_EXIT, L"Exit");

            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hWnd); // メニューが確実に前面に出るようにする
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            PostMessageW(hWnd, WM_NULL, 0, 0); // タスクスイッチ強制（Microsoft 推奨の回避策）
            DestroyMenu(hMenu);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_EXIT)
            PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // 多重起動防止：名前付き Mutex で既存インスタンスを確認
    HANDLE hMutex = CreateMutexW(NULL, FALSE, L"Local\\ContextMenuHide");
    if (!hMutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(hMutex);
        return 0; // 既に起動中 → 静かに終了
    }

    // EXE と同じフォルダにある DLL のフルパスを組み立てる
    wchar_t dllPath[MAX_PATH] = {};
    DWORD pathLen = GetModuleFileNameW(NULL, dllPath, MAX_PATH);
    if (pathLen == 0 || pathLen >= MAX_PATH)
        return 1;

    wchar_t* lastSlash = wcsrchr(dllPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    
    // ビット数に応じた DLL 名を選択（コンパイル時判定）
    #ifdef _WIN64
        const wchar_t* dllName = L"ContextMenuHideHook64.dll";
    #else
        const wchar_t* dllName = L"ContextMenuHideHook86.dll";
    #endif
    wcscat_s(dllPath, dllName);

    HMODULE hDll = LoadLibraryW(dllPath);
    if (!hDll) return 1;

    FnInstallHook   fnInstall   = (FnInstallHook)  GetProcAddress(hDll, "InstallHook");
    FnUninstallHook fnUninstall = (FnUninstallHook)GetProcAddress(hDll, "UninstallHook");

    if (!fnInstall || !fnUninstall) { FreeLibrary(hDll); return 1; }

    if (!fnInstall()) { FreeLibrary(hDll); return 1; }

    g_fnUninstall = fnUninstall;
    g_hDll        = hDll;

    // ReInstallHook 関数ポインタを取得する
    g_fnReInstall = (FnReInstallHook)GetProcAddress(hDll, "ReInstallHook");

    // Explorer ウィンドウ生成イベントを監視する
    // WINEVENT_OUTOFCONTEXT: DLL 不要、コールバックは本プロセスのメッセージループで受信
    g_hWinEvent = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
        NULL,                              // hmodWinEventProc: OUTOFCONTEXT なら NULL
        OnWinEvent,
        0, 0,                              // 全プロセス・全スレッドを監視
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // 非表示メッセージウィンドウを作成する
    HICON hAppIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    WNDCLASSW wc    = {};
    wc.lpfnWndProc  = WndProc;
    wc.hInstance    = hInstance;
    wc.hIcon        = hAppIcon;
    wc.lpszClassName = L"ContextMenuHideWnd";
    RegisterClassW(&wc);

    HWND hWnd = CreateWindowExW(0, L"ContextMenuHideWnd", NULL,
        0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    if (!hWnd) { CloseHandle(hMutex); Cleanup(); return 1; }

    // タスクトレイアイコンを追加する
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hWnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = hAppIcon;
    wcscpy_s(g_nid.szTip, L"ContextMenuHide");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // メッセージループ：フックを生かし続けるために必須
    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessageW(&msg, NULL, 0, 0)) != 0)
    {
        if (bRet == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(hWnd);
    Cleanup();
    CloseHandle(hMutex);
    return 0;
}