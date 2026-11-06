// TunerPro plug-in entry points and the "Honda ECU" menu.
//
// TunerPro has no public SDK. It loads every DLL in "Documents\TunerPro Files\Plugins" that
// exports TPCreatePlugin / TPReleasePlugin and talks to the object through a vtable. This
// layout was recovered by reverse engineering; only the calls this plug-in needs are named.
// The plug-in offers no data-acquisition component: it only needs to be loaded, and then
// adds its own top-level menu to TunerPro's main window.
#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ui.h"

namespace {

HINSTANCE g_instance = nullptr;
HANDLE g_watcher = nullptr;
volatile LONG g_stop = 0;
HWND g_main = nullptr;
HMENU g_popup = nullptr;
UINT g_first_command = 0;
HHOOK g_install_hook = nullptr;
HWND g_install_target = nullptr;
const UINT_PTR kSubclassId = 0x48454355;  // "HECU"
const UINT_PTR kWatchdogTimer = 0x4845;

const wchar_t kMenuTitle[] = L"Honda ECU";

FILE* g_log = nullptr;

void Log(const char* format, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_instance, path, MAX_PATH);
    wchar_t* dot = wcsrchr(path, L'.');
    if (dot) wcscpy_s(dot, path + MAX_PATH - dot, L".log");
    g_log = _wfsopen(path, L"w", _SH_DENYNO);
}

struct MenuItemDef {
    PanelCommand command;
    const wchar_t* thai;
    const wchar_t* english;
};
const MenuItemDef kItems[] = {
    {kPanelFlashRead, L"อ่านไฟล์จากกล่อง (Read)...", L"Read from ECU..."},
    {kPanelFlashWrite, L"เขียนไฟล์ลงกล่อง (Write)...", L"Write to ECU..."},
    {kPanelEeprom, L"EEPROM (อ่าน / เขียน / ดู)...", L"EEPROM (read / write / view)..."},
    {kPanelData, L"ข้อมูลสด (Data Logging)...", L"Data Logging..."},
    {kPanelTroubleCodes, L"รหัสปัญหา (Trouble Codes)...", L"Trouble Codes..."},
    {kPanelOpen, L"เปิดหน้าต่าง Honda ECU...", L"Open Honda ECU window..."},
};
const int kItemCount = sizeof(kItems) / sizeof(kItems[0]);

bool MenuHasId(HMENU menu, UINT id) {
    const int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; ++i) {
        HMENU sub = GetSubMenu(menu, i);
        if (sub) {
            if (MenuHasId(sub, id)) return true;
        } else if (GetMenuItemID(menu, i) == id) {
            return true;
        }
    }
    return false;
}

// Command ids must not collide with TunerPro's, which are not documented.
UINT FindFreeCommandBlock(HMENU menu) {
    for (UINT base = 0xBF00; base > 0x9000; base -= 0x100) {
        bool free = true;
        for (int i = 0; i < kItemCount && free; ++i) free = !MenuHasId(menu, base + i);
        if (free) return base;
    }
    return 0xBF00;
}

bool MenuContainsPopup(HMENU menu, HMENU popup) {
    const int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; ++i) {
        if (GetSubMenu(menu, i) == popup) return true;
    }
    return false;
}

void AddMenu() {
    HMENU bar = GetMenu(g_main);
    if (!bar) return;
    if (g_popup && MenuContainsPopup(bar, g_popup)) return;
    if (!g_popup) {
        g_first_command = FindFreeCommandBlock(bar);
        g_popup = CreatePopupMenu();
        for (int i = 0; i < kItemCount; ++i) {
            if (kItems[i].command == kPanelOpen) AppendMenuW(g_popup, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(g_popup, MF_STRING, g_first_command + i, TunerProIsThai() ? kItems[i].thai : kItems[i].english);
        }
    }
    // In front of the last item, which is Help on TunerPro's bar.
    const int count = GetMenuItemCount(bar);
    InsertMenuW(bar, count > 0 ? count - 1 : 0, MF_BYPOSITION | MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(g_popup), kMenuTitle);
    DrawMenuBar(g_main);
    Log("menu added, %d items on the bar, first command 0x%X", count + 1, g_first_command);
}

LRESULT CALLBACK MainSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_COMMAND: {
            const UINT id = LOWORD(wp);
            if (g_first_command && id >= g_first_command && id < g_first_command + kItemCount) {
                PanelShow(g_instance, hwnd, kItems[id - g_first_command].command);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (PanelWriteInProgress()) {
                MessageBoxW(hwnd, TunerProIsThai() ? L"กำลังเขียนข้อมูลลงกล่อง ห้ามปิด TunerPro จนกว่าจะเสร็จ" : L"Writing to the ECU. Do not close TunerPro until it finishes.", kMenuTitle, MB_OK | MB_ICONWARNING);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            KillTimer(hwnd, kWatchdogTimer);
            PanelShutdown();
            RemoveWindowSubclass(hwnd, MainSubclass, kSubclassId);
            g_main = nullptr;
            break;
        default: break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// TunerPro can rebuild its menu bar, so put the entry back if it disappears.
void CALLBACK MenuWatchdog(HWND, UINT, UINT_PTR, DWORD) {
    if (g_main) AddMenu();
}

// Runs on TunerPro's UI thread, which is where subclassing and menu changes belong.
LRESULT CALLBACK InstallHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && g_install_target && !g_main) {
        const CWPSTRUCT* cwp = reinterpret_cast<CWPSTRUCT*>(lp);
        if (cwp->hwnd == g_install_target) {
            g_main = g_install_target;
            SetWindowSubclass(g_main, MainSubclass, kSubclassId, 0);
            AddMenu();
            SetTimer(g_main, kWatchdogTimer, 1500, MenuWatchdog);
        }
    }
    return CallNextHookEx(g_install_hook, code, wp, lp);
}

BOOL CALLBACK FindMainWindowProc(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || !GetMenu(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;
    *reinterpret_cast<HWND*>(param) = hwnd;
    return FALSE;
}

DWORD WINAPI WatcherThread(LPVOID) {
    while (!g_stop && !g_main) {
        HWND found = nullptr;
        EnumWindows(FindMainWindowProc, reinterpret_cast<LPARAM>(&found));
        if (found) {
            wchar_t cls[64] = {0}, title[160] = {0};
            GetClassNameW(found, cls, 64);
            GetWindowTextW(found, title, 160);
            Log("main window %p class=%ls title=%ls", found, cls, title);
            g_install_target = found;
            const DWORD thread = GetWindowThreadProcessId(found, nullptr);
            g_install_hook = SetWindowsHookExW(WH_CALLWNDPROC, InstallHook, g_instance, thread);
            DWORD_PTR result = 0;
            SendMessageTimeoutW(found, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 3000, &result);
            if (g_install_hook) UnhookWindowsHookEx(g_install_hook);
            g_install_hook = nullptr;
            g_install_target = nullptr;
        }
        if (!g_main) Sleep(250);
    }
    return 0;
}

// The ABI TunerPro expects: MSVC thiscall vtable in exactly this slot order.
class HondaPlugin {
public:
    virtual ~HondaPlugin() {}
    virtual int __thiscall GetPluginInfo(uint8_t* info) {
        Log("GetPluginInfo(%p) cbSize=0x%lX", info, info ? *reinterpret_cast<DWORD*>(info) : 0);
        if (!info) return 0;
        *reinterpret_cast<DWORD*>(info + 0x04) = 3;
        static const GUID guid = {0x7c1a5e40, 0x93d2, 0x4b6f, {0x8a, 0x17, 0x2e, 0x60, 0xd4, 0xb9, 0x51, 0xc3}};
        memcpy(info + 0x08, &guid, sizeof(guid));
        strcpy_s(reinterpret_cast<char*>(info + 0x18), 0x32, "Honda ECU K-Line");
        strcpy_s(reinterpret_cast<char*>(info + 0x4A), 0x200, "Read and write Honda ECU flash and EEPROM over FTDI or J2534. Port of HondaECU by Ryan Hope.");
        strcpy_s(reinterpret_cast<char*>(info + 0x24A), 0x0F, "1.0.0");
        strcpy_s(reinterpret_cast<char*>(info + 0x259), 100, "AlexDev-TH");
        *reinterpret_cast<DWORD*>(info + 0x2C0) = 1;
        *reinterpret_cast<DWORD*>(info + 0x2C4) = 0;
        *reinterpret_cast<DWORD*>(info + 0x2C8) = 0x60000;
        return 1;
    }
    virtual int __thiscall Slot2(void* p) {
        Log("Slot2(%p)", p);
        return 0;
    }
    virtual void* __thiscall GetComponent(int index) {
        Log("GetComponent(%d)", index);
        return nullptr;
    }
    virtual int __thiscall Slot4(void* p) {
        Log("Slot4(%p)", p);
        return 0;
    }
    // TunerPro passes three arguments here (5000, host pointer, 0 at load). The count has to
    // match because the callee cleans the stack; one too few trips TunerPro's stack cookie.
    virtual int __thiscall Notify(int code, void* p, void* q) {
        Log("Notify(%d, %p, %p)", code, p, q);
        return 0;
    }
};

HondaPlugin* g_plugin = nullptr;

std::wstring g_last_opened;

std::string ReadSettingsFile() {
    wchar_t docs[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) return "";
    const std::wstring path = std::wstring(docs) + L"\\TunerPro Files\\TunerProSettings.json";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return "";
    std::string text;
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
    fclose(f);
    return text;
}

// TunerPro is an ANSI program, so paths in its settings file are in the system code page.
std::wstring DecodeSettingsString(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) {
        cp = CP_ACP;
        n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    }
    if (n <= 1) return L"";
    std::wstring out(n - 1, L'\0');
    MultiByteToWideChar(cp, 0, s.c_str(), -1, &out[0], n);
    return out;
}

// Reads the JSON string starting at the opening quote.
std::string ParseJsonString(const std::string& text, size_t quote, size_t* end) {
    std::string out;
    size_t i = quote + 1;
    while (i < text.size() && text[i] != '"') {
        if (text[i] == '\\' && i + 1 < text.size()) {
            ++i;
            switch (text[i]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                default: out += text[i]; break;
            }
        } else {
            out += text[i];
        }
        ++i;
    }
    if (end) *end = i + 1;
    return out;
}

std::vector<std::wstring> SettingsBinPaths() {
    std::vector<std::wstring> paths;
    const std::string text = ReadSettingsFile();
    size_t at = text.find("\"lastBinPath\"");
    if (at != std::string::npos) {
        size_t quote = text.find('"', text.find(':', at));
        if (quote != std::string::npos) paths.push_back(DecodeSettingsString(ParseJsonString(text, quote, nullptr)));
    }
    at = text.find("\"recentBins\"");
    if (at != std::string::npos) {
        size_t pos = text.find('[', at);
        const size_t close = text.find(']', pos);
        while (pos != std::string::npos && close != std::string::npos) {
            const size_t quote = text.find('"', pos);
            if (quote == std::string::npos || quote > close) break;
            paths.push_back(DecodeSettingsString(ParseJsonString(text, quote, &pos)));
        }
    }
    return paths;
}

std::wstring BaseName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

}  // namespace

// Follows TunerPro's own language: the "language" setting picks lang\<code>.txt next to the
// exe. It is Thai if the code is "th", or if that file is one the tunerpro-thai installer
// rewrote, because that project has to put the Thai text in en.txt and marks it on line one.
bool TunerProIsThai() {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    std::string code = "en";
    const std::string text = ReadSettingsFile();
    size_t at = text.find("\"language\"");
    if (at != std::string::npos) {
        const size_t quote = text.find('"', text.find(':', at));
        if (quote != std::string::npos) code = ParseJsonString(text, quote, nullptr);
    }
    if (code == "th") {
        cached = 1;
        return true;
    }
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) *slash = 0;
    const std::wstring lang = std::wstring(exe) + L"\\lang\\" + DecodeSettingsString(code) + L".txt";
    if (FILE* f = _wfopen(lang.c_str(), L"rb")) {
        char head[64] = {0};
        fread(head, 1, sizeof(head) - 1, f);
        fclose(f);
        if (strstr(head, "# TunerPro-Thai")) cached = 1;
    }
    return cached != 0;
}

// TunerPro's title only carries the file name ("TunerPro RT - name.bin | XDF File - ..."), so
// the folder comes from the paths it records in its settings. The caller shows the full path
// to the user before any write.
std::wstring TunerProCurrentBinPath() {
    if (!g_main) return L"";
    wchar_t title[1024] = {0};
    GetWindowTextW(g_main, title, 1024);
    std::wstring name = title;
    const std::wstring prefix = L"TunerPro RT - ";
    if (name.compare(0, prefix.size(), prefix) != 0) return L"";
    name = name.substr(prefix.size());
    const size_t bar = name.find(L" | ");
    if (bar != std::wstring::npos) name = name.substr(0, bar);
    if (name.empty()) return L"";
    if (!g_last_opened.empty() && _wcsicmp(BaseName(g_last_opened).c_str(), name.c_str()) == 0) return g_last_opened;
    for (const std::wstring& path : SettingsBinPaths()) {
        if (_wcsicmp(BaseName(path).c_str(), name.c_str()) == 0 && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return L"";
}

bool TunerProOpenBin(const std::wstring& path) {
    if (!g_main) return false;
    g_last_opened = path;
    // The same message Explorer sends when a file is dragged onto the window.
    const size_t chars = path.size() + 2;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    if (!mem) return false;
    auto* drop = static_cast<DROPFILES*>(GlobalLock(mem));
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    memcpy(reinterpret_cast<uint8_t*>(drop) + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(mem);
    if (!PostMessageW(g_main, WM_DROPFILES, reinterpret_cast<WPARAM>(mem), 0)) {
        GlobalFree(mem);
        return false;
    }
    return true;
}

extern "C" {

__declspec(dllexport) void* TPCreatePlugin() {
    if (!g_plugin) {
        // TunerPro may unload a plug-in that offers no component, but the menu lives on.
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&TPCreatePlugin), &self);
        OpenLog();
        Log("TPCreatePlugin on thread %lu", GetCurrentThreadId());
        g_plugin = new HondaPlugin();
        g_watcher = CreateThread(nullptr, 0, WatcherThread, nullptr, 0, nullptr);
    }
    return g_plugin;
}

__declspec(dllexport) void TPReleasePlugin() {
    Log("TPReleasePlugin");
    delete g_plugin;
    g_plugin = nullptr;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
