#define UNICODE      // Unicode 文字セットを有効化（wchar_t ベースの Windows API を使う）
#define _UNICODE     // CRT の Unicode 対応を有効化（_wfopen 等のワイド文字版関数を使う）
#include <windows.h> // Windows API 全般（SetWindowsHookEx, HMENU, MENUITEMINFOW 等）
#include <string>    // std::wstring / std::string
#include <vector>    // std::vector
#include <unordered_map>  // std::unordered_map（追加）
#include <unordered_set>  // std::unordered_set（ReInstallHook の liveThreads に使用）
#include <tlhelp32.h> // CreateToolhelp32Snapshot, PROCESSENTRY32W, THREADENTRY32
#include "json.hpp"  // nlohmann/json（単一ヘッダ版）

// ============================================================
// ContextMenuHideHook.dll
// SetWindowsHookEx によって全プロセスに注入される DLL。
// WM_INITMENUPOPUP（ポップアップメニューが描画される直前）を傍受し、
// blocklist.json に登録されたキーワードを含むメニュー項目を削除する。
// ============================================================

HINSTANCE g_hInstance = NULL;        // この DLL 自身のインスタンスハンドル（DllMain で初期化）
bool g_isShiftOnly = false;        // Shift のみ押下（Ctrl と Alt は押されていない）
bool g_isCtrlOnly = false;         // Ctrl のみ押下（Shift と Alt は押されていない）
bool g_isAltOnly = false;          // Alt のみ押下（Shift と Ctrl は押されていない）

struct BlockItem {
    std::wstring text;     // メニュー項目テキストのキーワード
    bool         hideAll;  // Shift 時も非表示にするか（true = Shift 時も非表示）
};
std::vector<BlockItem> g_blockList;          // ブロックリスト（text / hideAll のペア）
static INIT_ONCE       g_blockListOnce = INIT_ONCE_STATIC_INIT;

// キー: スレッドID、値: HHOOK ハンドル
static std::unordered_map<DWORD, HHOOK> g_hooks;

// ------------------------------------------------------------
// UTF-8（BOM なし）の std::string を UTF-16 の std::wstring に変換する。
// blocklist.json の文字列を Windows API で扱えるワイド文字列にするために使う。
// ------------------------------------------------------------
static std::wstring Utf8ToWide(const std::string& src) // 引数: UTF-8 エンコードの std::string
{
    if (src.empty()) return L"";                                               // 空文字列はそのまま空の wstring を返す
    int len = MultiByteToWideChar(CP_UTF8, 0, src.c_str(), -1, NULL, 0);      // 変換後に必要なバッファサイズ（文字数）を算出
    std::wstring dst(len, L'\0');                                              // 必要サイズの wstring バッファを確保
    MultiByteToWideChar(CP_UTF8, 0, src.c_str(), -1, &dst[0], len);           // 実際に UTF-8 → UTF-16 変換を実行
    if (!dst.empty() && dst.back() == L'\0') dst.pop_back();                  // 末尾に余分な NULL 文字が付く場合があるので除去
    return dst;                                                                // 変換済み wstring を返す
}


// ------------------------------------------------------------
// DLL と同じフォルダにある blocklist.json を読み込み、
// グローバル変数 g_blockList を初期化する。
// InitOnceExecuteOnce により OS レベルでスレッドセーフな一回初期化が保証される。
// ------------------------------------------------------------
static BOOL CALLBACK DoLoadBlockList(PINIT_ONCE, PVOID, PVOID*)
{
    wchar_t dllPath[MAX_PATH] = {};                          // DLL のフルパスを格納するバッファ
    DWORD pathLen = GetModuleFileNameW(g_hInstance, dllPath, MAX_PATH); // この DLL 自身のフルパスを取得（g_hInstance を指定）
    if (pathLen == 0 || pathLen >= MAX_PATH) return TRUE;

    wchar_t* lastSlash = wcsrchr(dllPath, L'\\');            // 最後の '\\' を検索（ファイル名部分の先頭を特定）
    if (lastSlash) *(lastSlash + 1) = L'\0';                 // '\\' の次を NULL にしてファイル名を切り落とす
    wcscat_s(dllPath, L"blocklist.json");                    // ディレクトリパスに "blocklist.json" を連結

    FILE* f = NULL;
    _wfopen_s(&f, dllPath, L"rb");   // ファイルをバイナリ読み取りモードで開く（BOM の検出のため "rb" を使用）
    if (!f) return TRUE;             // ファイルが開けなければロードを諦めてリターン

    fseek(f, 0, SEEK_END);           // ファイルポインタをファイル末尾に移動
    long size = ftell(f);            // 末尾のオフセット = ファイルサイズ（バイト）を取得
    if (size < 0) { fclose(f); return TRUE; } // ftell 失敗（-1L）→ クラッシュ防止のため早期リターン
    rewind(f);                       // ファイルポインタを先頭に戻す

    std::string buf(static_cast<size_t>(size), '\0'); // ファイル全体を読み込むバッファを確保（size バイト）
    size_t readBytes = fread(&buf[0], 1, static_cast<size_t>(size), f); // ファイル内容をバッファに一括読み込み
    fclose(f);                       // ファイルを閉じる（以降は buf を使う）
    if (readBytes < static_cast<size_t>(size)) {
        buf.resize(readBytes);       // 実際に読めたバイト数にバッファを縮小
    }

    // UTF-8 BOM（0xEF 0xBB 0xBF）が付いていれば取り除く
    if (buf.size() >= 3 &&                     // BOM は 3 バイトなのでサイズを確認
        (unsigned char)buf[0] == 0xEF &&       // UTF-8 BOM の第 1 バイト
        (unsigned char)buf[1] == 0xBB &&       // UTF-8 BOM の第 2 バイト
        (unsigned char)buf[2] == 0xBF)         // UTF-8 BOM の第 3 バイト
    {
        buf.erase(0, 3);  // 先頭 3 バイト（BOM）を取り除く
    }

    // nlohmann/json でパースし BlockItem リストを構築する
    try {
        auto j = nlohmann::json::parse(buf, nullptr, true, true);
        for (const auto& item : j.at("blocklist")) {
            BlockItem bi;
            bi.text    = Utf8ToWide(item.value("text", std::string{}));
            bi.hideAll = item.value("hide_all", false);
            g_blockList.push_back(std::move(bi));
        }
    } catch (...) {
        // パース失敗時は空リストのまま継続
    }
    return TRUE;
}

static void LoadBlockList()
{
    InitOnceExecuteOnce(&g_blockListOnce, DoLoadBlockList, nullptr, nullptr);
}

// ------------------------------------------------------------
// メニュー項目のテキストがブロックリストのいずれかのキーワードを
// 部分一致で含むかどうかを判定する。
// ------------------------------------------------------------
// isShiftOnly: true のとき hideAll:false の項目をスキップ（Shift のみ時は hide_all:true のみ削除対象）
static bool ShouldBlock(const wchar_t* text, bool isShiftOnly)
{
    for (const auto& item : g_blockList) {
        if (isShiftOnly && !item.hideAll) continue;             // Shift のみ時は hide_all:false をスキップ
        if (wcsstr(text, item.text.c_str())) return true;  // テキストにキーワードが部分一致すれば true を返す
    }
    return false;  // どのキーワードにも一致しなければ false を返す
}

// ------------------------------------------------------------
// 指定インデックスのメニュー項目がセパレータかどうかを判定する。
// ------------------------------------------------------------
static bool IsSeparator(HMENU hMenu, int index) // 引数: メニューハンドル、アイテムインデックス
{
    MENUITEMINFOW mii = {};                 // メニューアイテム情報を格納する構造体
    mii.cbSize = sizeof(mii);              // 構造体のサイズを設定
    mii.fMask  = MIIM_FTYPE;               // fType フィールドのみ取得（軽量化）
    if (!GetMenuItemInfoW(hMenu, index, TRUE, &mii)) return false; // 取得失敗時は false
    return (mii.fType & MFT_SEPARATOR) != 0;                        // セパレータフラグをチェック
}

// ------------------------------------------------------------
// 連続する2個のセパレータを1個に統一する。
// 末尾から先頭に向かって逆順処理することで、インデックスズレを防ぐ。
// ------------------------------------------------------------
static void CleanupSeparators(HMENU hMenu) // 引数: 処理対象のメニューハンドル
{
    if (!hMenu) return;  // ハンドルが NULL の場合は何もしない

    int count = GetMenuItemCount(hMenu);        // メニュー内のアイテム数を取得
    for (int i = count - 1; i > 0; i--) {      // 末尾から先頭に向かってループ（i > 0 で先頭は対象外）
        if (IsSeparator(hMenu, i) && IsSeparator(hMenu, i-1)) { // 現在と直前がともにセパレータならば
            DeleteMenu(hMenu, i, MF_BYPOSITION);                // 現在のセパレータを削除
        }
    }
}

// ------------------------------------------------------------
// 指定されたメニューからブロック対象のアイテムを削除する。
// WM_INITMENUPOPUP（描画直前）のタイミングで呼ばれる。
// サブメニューも再帰的に処理する。
// Ctrl のみが押されている場合（isCtrlOnly = true）は、ブロック対象をスキップしてすべて表示する。
// ------------------------------------------------------------
static void FilterMenu(HMENU hMenu) // 引数: 処理対象のメニューハンドル
{
    if (!hMenu) return;  // ハンドルが NULL の場合は何もしない

    int count = GetMenuItemCount(hMenu);        // メニュー内のアイテム数を取得
    for (int i = count - 1; i >= 0; i--) {     // 末尾から先頭に向かってループ（アイテム削除時のインデックスズレを防ぐため逆順）
        MENUITEMINFOW mii = {};                 // メニューアイテム情報を格納する構造体（ゼロ初期化）
        mii.cbSize = sizeof(mii);              // 構造体のサイズを設定（GetMenuItemInfoW の必須要件）
        mii.fMask  = MIIM_STRING | MIIM_FTYPE | MIIM_SUBMENU; // 取得するフィールド：テキスト文字列・アイテム種類・サブメニュー
        wchar_t buf[512] = {};          // メニュー項目のテキストを受け取るバッファ（512 文字）
        mii.dwTypeData = buf;           // テキストバッファのポインタをセット
        mii.cch        = _countof(buf); // バッファのサイズ（文字数）をセット

        if (!GetMenuItemInfoW(hMenu, i, TRUE, &mii)) continue; // アイテム情報の取得に失敗したらスキップ（TRUE = インデックス指定）
        if (mii.fType & MFT_SEPARATOR) continue;               // セパレーター（区切り線）はブロック対象外なのでスキップ

        // Ctrl のみが押されている場合は、ブロック対象のチェックをスキップして全項目表示
        if (!g_isCtrlOnly && ShouldBlock(buf, g_isShiftOnly)) {  // テキストがブロックリストのキーワードに部分一致する場合
            DeleteMenu(hMenu, i, MF_BYPOSITION);        // インデックス位置を指定してメニュー項目を削除
            continue;                                   // 削除したのでサブメニューのチェックは不要
        }

        // サブメニューがある場合は再帰的にフィルタリングする
        if (mii.hSubMenu)
            FilterMenu(mii.hSubMenu);  // サブメニューのハンドルを渡して再帰呼び出し
    }
    CleanupSeparators(hMenu);  // 連続するセパレータをクリーンアップ
}

// ------------------------------------------------------------
// フックプロシージャ：SetWindowsHookEx(WH_CALLWNDPROC) で登録され、
// フックされた全プロセスのすべてのウィンドウメッセージに対して呼ばれる。
// ------------------------------------------------------------
LRESULT CALLBACK CallWndProc(int nCode, WPARAM wParam, LPARAM lParam)
// nCode  : HC_ACTION のとき処理すべきメッセージ、負の値のときは即 CallNextHookEx に渡す
// wParam : メッセージの送信元が現在のスレッドかどうか（0 = 別スレッドから送信）
// lParam : CWPSTRUCT 構造体へのポインタ（message / wParam / lParam / hwnd を含む）
{
    if (nCode == HC_ACTION) {                                          // 処理が必要なコードのとき
        CWPSTRUCT* cwp = reinterpret_cast<CWPSTRUCT*>(lParam);         // lParam を CWPSTRUCT* にキャスト
        if (cwp->message == WM_INITMENUPOPUP) {                        // ポップアップメニューが表示される直前のメッセージ
            bool isShiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            bool isCtrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool isAltPressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            // 3つのキーから1つだけが押されている場合のみを判定（組み合わせは除外）
            g_isShiftOnly = isShiftPressed && !isCtrlPressed && !isAltPressed;
            g_isCtrlOnly = isCtrlPressed && !isShiftPressed && !isAltPressed;
            g_isAltOnly = isAltPressed && !isShiftPressed && !isCtrlPressed;
            LoadBlockList();                                           // ブロックリストを（未ロードなら）ここで読み込む
            HMENU hMenu = reinterpret_cast<HMENU>(cwp->wParam);        // wParam がポップアップメニューのハンドル
            FilterMenu(hMenu);                                         // ブロック対象項目をメニューから削除
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam); // チェーン内の次のフックに処理を渡す（省略すると他フックが動作しなくなる）
}

// ------------------------------------------------------------
// 全 Explorer プロセスの PID を収集する。
// 戻り値: true  = スナップショット成功（pids が空 = Explorer 未起動）
//         false = スナップショット失敗（結果を信用してはいけない）
// ------------------------------------------------------------
static bool GetExplorerPids(std::unordered_set<DWORD>& pids)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0)
                pids.insert(pe.th32ProcessID);  // break しない → 全件収集
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return true;
}

// ------------------------------------------------------------
// 全 Explorer スレッドへの Hook 登録・不要 Hook 解放を一括で行う。
// InstallHook・ReInstallHook の共通実装。
// ------------------------------------------------------------
static void SyncHooks()
{
    // ① 全 Explorer PID を収集（複数プロセス対応）
    std::unordered_set<DWORD> explorerPids;
    if (!GetExplorerPids(explorerPids)) return;  // スナップショット失敗 → 現状維持

    // ② 全 Explorer の生存スレッド ID セットを構築
    std::unordered_set<DWORD> liveThreads;
    HANDLE hTSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hTSnap == INVALID_HANDLE_VALUE) return;  // 失敗 → 現状維持
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    if (Thread32First(hTSnap, &te)) {
        do {
            if (explorerPids.count(te.th32OwnerProcessID))
                liveThreads.insert(te.th32ThreadID);
        } while (Thread32Next(hTSnap, &te));
    }
    CloseHandle(hTSnap);

    // ③ 死んだスレッドの Hook を解放（explorerPids が空なら全解除）
    for (auto it = g_hooks.begin(); it != g_hooks.end(); ) {
        if (liveThreads.count(it->first) == 0) {
            UnhookWindowsHookEx(it->second);
            it = g_hooks.erase(it);
        } else {
            ++it;
        }
    }

    // ④ 未 Hook のスレッドに新規登録（二重登録は g_hooks.count() で防止）
    for (DWORD tid : liveThreads) {
        if (g_hooks.count(tid) == 0) {
            HHOOK h = SetWindowsHookEx(WH_CALLWNDPROC, CallWndProc, g_hInstance, tid);
            if (h) g_hooks[tid] = h;
        }
    }
}

// ------------------------------------------------------------
// ContextMenuHide.exe から呼び出されるエクスポート関数
// ------------------------------------------------------------
extern "C" __declspec(dllexport) // C リンケージで DLL からエクスポート（名前マングリングを防ぐ）
BOOL InstallHook()               // 初回起動時に呼ぶ。Hook 登録に成功すれば TRUE を返す
{
    LoadBlockList();
    SyncHooks();
    return g_hooks.empty() ? FALSE : TRUE;
}

extern "C" __declspec(dllexport) // C リンケージで DLL からエクスポート
void UninstallHook()             // インストールした全フックを解除する
{
    for (auto& kv : g_hooks)
        UnhookWindowsHookEx(kv.second);
    g_hooks.clear();
}

// ------------------------------------------------------------
// Explorer 新規ウィンドウ検知時に EXE 側から呼ばれる。SyncHooks に委譲。
// ------------------------------------------------------------
extern "C" __declspec(dllexport)
void ReInstallHook()
{
    SyncHooks();
}

// ------------------------------------------------------------
// DLL エントリーポイント：プロセスへのアタッチ/デタッチ時に OS から呼ばれる
// ------------------------------------------------------------
BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD reason, LPVOID)
// hInstance : この DLL のインスタンスハンドル
// reason    : 呼び出し理由（DLL_PROCESS_ATTACH / DLL_PROCESS_DETACH 等）
// LPVOID    : 予約済み引数（通常は使用しない）
{
    if (reason == DLL_PROCESS_ATTACH) {              // プロセスにこの DLL が初めてロードされたとき
        g_hInstance = hInstance;                     // DLL のインスタンスハンドルをグローバル変数に保存（LoadBlockList 等で使用）
        DisableThreadLibraryCalls(hInstance);        // スレッドのアタッチ/デタッチ時に DllMain が呼ばれないようにする（不要な呼び出しを抑制しパフォーマンス向上）
    }
    return TRUE;  // FALSE を返すと DLL のロードが失敗扱いになる
}