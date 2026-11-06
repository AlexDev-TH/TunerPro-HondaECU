// TunerPro-HondaECU, GNU GPL v3. Based on HondaECU by Ryan Hope, changed in November 2026.
// The Honda ECU window: a Win32 port of HondaECU's controlpanel.py and frames/*.py.
//
// Every window here is a real dialog on TunerPro's UI thread, which is what hooks it into
// TunerPro's theme: TunerPro watches WM_INITDIALOG on that thread and themes the dialog and
// its children by itself. So controls must exist before WM_INITDIALOG returns, and nothing
// here may answer WM_CTLCOLOR* or paint its own colours, or it would fight that theme.
#include "ui.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "kline_worker.h"

namespace {

struct EcmEntry {
    uint8_t id[5];
    const char* model;
    const char* year;
    const char* pn;
    long checksum;
    long offset;
    long ecmidaddr;
    long keihinaddr;
};

const EcmEntry kEcmIds[] = {
#include "ecmids.inc"
};

const UINT WM_WORKER_EVENT = WM_APP + 41;
const UINT_PTR kTimerLog = 1;

enum ControlId {
    ID_ADAPTER = 100,
    ID_REFRESH,
    ID_CONNECT,
    ID_SECURE,
    ID_TAB,
    ID_STATUS,
    ID_FLASH_READ,
    ID_FLASH_WRITE,
    ID_FLASH_PATH,
    ID_FLASH_BROWSE,
    ID_FLASH_CURRENT,
    ID_FLASH_OFFSET,
    ID_FLASH_CHECKSUM,
    ID_FLASH_FIX,
    ID_FLASH_GO,
    ID_FLASH_CANCEL,
    ID_EE_READ,
    ID_EE_WRITE,
    ID_EE_FORMAT,
    ID_EE_FILL00,
    ID_EE_FILLFF,
    ID_EE_PATH,
    ID_EE_BROWSE,
    ID_EE_VIEW,
    ID_EE_GO,
    ID_EE_CANCEL,
    ID_DTC_CLEAR,
    ID_LOG_SAVE,
    ID_PW_OK,
    ID_PW_CANCEL,
    ID_PW_EDIT,
};

enum TabIndex { kTabFlash, kTabEeprom, kTabData, kTabDtc, kTabLog, kTabCount };

// The one language decision for every string in this file.
const wchar_t* T(const wchar_t* thai, const wchar_t* english) {
    return TunerProIsThai() ? thai : english;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

std::wstring Fmt(const wchar_t* format, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, format, args);
    va_end(args);
    return buf;
}

std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n > 0) GetWindowTextW(h, &s[0], n + 1);
    return s;
}

bool ParseHex(const std::wstring& text, long* value) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    long v = wcstol(text.c_str(), &end, 16);
    if (!end || *end != 0 || v < 0) return false;
    *value = v;
    return true;
}

bool LoadFile(const std::wstring& path, Bytes* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    Bytes data;
    uint8_t buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + got);
    fclose(f);
    *out = std::move(data);
    return true;
}

std::wstring HexDump(const Bytes& data) {
    std::wstring out;
    out.reserve(data.size() * 5);
    wchar_t line[32];
    for (size_t i = 0; i < data.size(); i += 16) {
        swprintf_s(line, L"%04X:", static_cast<unsigned>(i));
        out += line;
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < data.size()) {
                swprintf_s(line, L" %02X", data[i + j]);
                out += line;
            } else {
                out += L"   ";
            }
        }
        out += L"  ";
        for (size_t j = 0; j < 16 && i + j < data.size(); ++j) {
            const uint8_t c = data[i + j];
            out += (c >= 0x20 && c < 0x7F) ? static_cast<wchar_t>(c) : L'.';
        }
        out += L"\r\n";
    }
    return out;
}

// An in-memory dialog template with no controls. The controls are created in WM_INITDIALOG.
std::vector<WORD> DialogTemplate(DWORD style, DWORD ex_style, const wchar_t* title) {
    std::vector<WORD> t(sizeof(DLGTEMPLATE) / sizeof(WORD), 0);
    DLGTEMPLATE* d = reinterpret_cast<DLGTEMPLATE*>(t.data());
    d->style = style | DS_SETFONT;
    d->dwExtendedStyle = ex_style;
    t.push_back(0);  // no menu
    t.push_back(0);  // default dialog class
    for (const wchar_t* p = title; *p; ++p) t.push_back(*p);
    t.push_back(0);
    t.push_back(9);  // point size
    for (const wchar_t* p = L"Leelawadee UI"; *p; ++p) t.push_back(*p);
    t.push_back(0);
    return t;
}

const struct SensorRow {
    const wchar_t* name;
    const wchar_t* unit;
} kSensors[] = {
    {L"Engine speed", L"rpm"},         // 0
    {L"TPS sensor", L"%"},             // 1
    {L"ECT sensor", L"°C"},       // 2
    {L"IAT sensor", L"°C"},       // 3
    {L"MAP sensor", L"mBar"},          // 4
    {L"Battery voltage", L"V"},        // 5
    {L"Vehicle speed", L"Km/h"},       // 6
    {L"Injector duration", L"ms"},     // 7
    {L"Ignition advance", L"°"},  // 8
    {L"IACV pulse count", L""},        // 9
    {L"IACV command", L""},            // 10
    {L"O2 sensor #1", L"V"},           // 11
    {L"STFT #1", L""},                 // 12
    {L"O2 heater #1", L"V"},           // 13
    {L"O2 sensor #2", L"V"},           // 14
    {L"STFT #2", L""},                 // 15
    {L"O2 heater #2", L"V"},           // 16
    {L"EGCV current", L"V"},           // 17
    {L"EGCV target", L"V"},            // 18
    {L"EGCV load", L"%"},              // 19
    {L"HESD current", L"A"},           // 20
    {L"HESD target", L"A"},            // 21
    {L"HESD load", L"%"},              // 22
};

class Panel {
public:
    static Panel* Get(HINSTANCE instance, HWND owner) {
        if (!instance_) instance_ = new Panel(instance, owner);
        return instance_;
    }
    static Panel* Current() { return instance_; }
    static void Destroy() {
        delete instance_;
        instance_ = nullptr;
    }

    void Show(PanelCommand command);
    bool WriteInProgress() const { return busy_ && busy_is_write_; }

private:
    struct PageInit {
        Panel* self;
        int index;
    };

    Panel(HINSTANCE instance, HWND owner);
    ~Panel();

    static INT_PTR CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK PageProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK PasswordProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK DialogKeysHook(int, WPARAM, LPARAM);

    INT_PTR OnMessage(UINT, WPARAM, LPARAM);
    INT_PTR OnPasswordMessage(UINT, WPARAM, LPARAM);
    void OnCommand(int id, int code);
    void OnWorkerEvent(const WorkerEvent& e);
    void OnData(int table, int dl, const Bytes& raw);
    void BuildControls();
    void BuildPage(HWND page, int index);
    void BuildFlashPage(HWND page);
    void BuildEepromPage(HWND page);
    void BuildDataPage(HWND page);
    void BuildDtcPage(HWND page);
    void BuildLogPage(HWND page);
    void SelectTab(int index);
    void RefreshAdapters();
    void ToggleConnect();
    void SetState(ECUSTATE state);
    void SetEcmId(const Bytes& id);
    void ClearStatusFields();
    void DrawStatus(const DRAWITEMSTRUCT* d);
    void ValidateFlash();
    void ValidateEeprom();
    void UpdateEnables();
    void FlashGo();
    void EepromGo();
    void SetBusy(bool busy, bool is_write);
    void ShowPasswordDialog();
    void ClosePasswordDialog();
    void ShowHex(const Bytes& data, const std::wstring& title);
    void SetSensor(int row, double value, int decimals);
    void ResetSensors();
    void FlushLog();
    bool Ask(const std::wstring& text, UINT icon = MB_ICONWARNING);
    void Tell(const std::wstring& text, UINT icon);
    bool ConfirmOverwrite(const std::wstring& path);

    HWND Make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0, DWORD ex = 0);
    int S(int v) const { return MulDiv(v, dpi_, 96); }

    static Panel* instance_;
    static HHOOK hook_;

    HINSTANCE inst_;
    HWND owner_;
    HWND hwnd_ = nullptr;
    int dpi_ = 96;
    HFONT font_ = nullptr;
    HFONT font_big_ = nullptr;
    HFONT font_mid_ = nullptr;
    HFONT font_mono_ = nullptr;

    HWND adapter_ = nullptr, refresh_ = nullptr, connect_ = nullptr, secure_ = nullptr;
    HWND model_ = nullptr, pn_ = nullptr, tab_ = nullptr;
    HWND status_state_ = nullptr, status_id_ = nullptr, status_flash_ = nullptr, status_dtc_ = nullptr;
    HWND pages_[kTabCount] = {};

    HWND fl_read_ = nullptr, fl_write_ = nullptr, fl_path_ = nullptr, fl_browse_ = nullptr, fl_current_ = nullptr;
    HWND fl_offset_ = nullptr, fl_cks_label_ = nullptr, fl_checksum_ = nullptr, fl_fix_ = nullptr;
    HWND fl_cks_state_ = nullptr, fl_progress_ = nullptr, fl_text_ = nullptr, fl_hint_ = nullptr, fl_go_ = nullptr, fl_cancel_ = nullptr;

    HWND ee_read_ = nullptr, ee_write_ = nullptr, ee_format_ = nullptr, ee_fill00_ = nullptr, ee_fillff_ = nullptr;
    HWND ee_path_ = nullptr, ee_browse_ = nullptr, ee_view_ = nullptr, ee_progress_ = nullptr, ee_text_ = nullptr;
    HWND ee_hint_ = nullptr, ee_go_ = nullptr, ee_cancel_ = nullptr, ee_hex_ = nullptr, ee_hex_title_ = nullptr;

    HWND data_list_ = nullptr, dtc_list_ = nullptr, dtc_clear_ = nullptr, log_edit_ = nullptr, log_stats_ = nullptr;

    HWND pw_ = nullptr, pw_edit_ = nullptr, pw_msg_ = nullptr, pw_ok_ = nullptr, pw_cancel_ = nullptr, pw_info_ = nullptr;
    bool pw_waiting_ = false;

    std::unique_ptr<KlineWorker> worker_;
    std::vector<AdapterInfo> adapters_;
    bool connected_ = false;
    bool busy_ = false;
    bool busy_is_write_ = false;
    int active_tab_ = kTabFlash;
    ECUSTATE state_ = ECUSTATE::UNKNOWN;
    Bytes ecmid_;
    const EcmEntry* ecm_ = nullptr;
    std::wstring offset_default_ = L"0x0";
    std::wstring checksum_default_;
    bool offset_edited_ = false;
    bool checksum_edited_ = false;
    int maintable_ = -1;

    std::wstring ee_pending_view_;
    std::wstring last_read_path_;

    std::mutex log_mutex_;
    std::deque<std::string> log_lines_;
    bool log_dirty_ = false;

    std::wstring status_text_;
};

Panel* Panel::instance_ = nullptr;
HHOOK Panel::hook_ = nullptr;

// TunerPro's message loop knows nothing about these dialogs, so route Tab and arrows here.
LRESULT CALLBACK Panel::DialogKeysHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && wparam == PM_REMOVE && instance_) {
        MSG* msg = reinterpret_cast<MSG*>(lparam);
        if (msg->message >= WM_KEYFIRST && msg->message <= WM_KEYLAST) {
            HWND target = instance_->pw_ && IsWindowVisible(instance_->pw_) ? instance_->pw_ : instance_->hwnd_;
            if (target && IsWindowVisible(target) && (msg->hwnd == target || IsChild(target, msg->hwnd))) {
                if (IsDialogMessageW(target, msg)) {
                    msg->message = WM_NULL;
                    msg->wParam = 0;
                    msg->lParam = 0;
                }
            }
        }
    }
    return CallNextHookEx(hook_, code, wparam, lparam);
}

Panel::Panel(HINSTANCE instance, HWND owner) : inst_(instance), owner_(owner) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    HDC dc = GetDC(nullptr);
    dpi_ = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);

    auto make_font = [&](int pt, int weight, const wchar_t* face) {
        return CreateFontW(-MulDiv(pt, dpi_, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    };
    font_ = make_font(9, FW_NORMAL, L"Leelawadee UI");
    font_mid_ = make_font(10, FW_SEMIBOLD, L"Leelawadee UI");
    font_big_ = make_font(15, FW_BOLD, L"Leelawadee UI");
    font_mono_ = make_font(9, FW_NORMAL, L"Consolas");

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    std::vector<WORD> tmpl = DialogTemplate(style, WS_EX_CONTROLPARENT, L"Honda ECU");
    CreateDialogIndirectParamW(inst_, reinterpret_cast<DLGTEMPLATE*>(tmpl.data()), owner_, MainProc, reinterpret_cast<LPARAM>(this));
    if (hwnd_) {
        RefreshAdapters();
        SetTimer(hwnd_, kTimerLog, 300, nullptr);
    }
    if (!hook_) hook_ = SetWindowsHookExW(WH_GETMESSAGE, DialogKeysHook, nullptr, GetCurrentThreadId());
}

Panel::~Panel() {
    worker_.reset();
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (pw_) DestroyWindow(pw_);
    if (hwnd_) DestroyWindow(hwnd_);
    for (HFONT f : {font_, font_big_, font_mid_, font_mono_}) DeleteObject(f);
}

HWND Panel::Make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return c;
}

void Panel::BuildControls() {
    // Client size, then centre over TunerPro.
    RECT rc = {0, 0, S(780), S(604)};
    AdjustWindowRectEx(&rc, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
    RECT ow;
    GetWindowRect(owner_, &ow);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    SetWindowPos(hwnd_, nullptr, ow.left + std::max<long>(0, (ow.right - ow.left - w) / 2),
                 ow.top + std::max<long>(0, (ow.bottom - ow.top - h) / 2), w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, GetClassLongPtrW(owner_, GCLP_HICONSM));
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, GetClassLongPtrW(owner_, GCLP_HICON));

    Make(hwnd_, L"STATIC", T(L"อุปกรณ์:", L"Adapter:"), SS_RIGHT, 8, 13, 52, 20);
    adapter_ = Make(hwnd_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 64, 10, 366, 240, ID_ADAPTER);
    refresh_ = Make(hwnd_, L"BUTTON", T(L"รีเฟรช", L"Refresh"), WS_TABSTOP, 436, 9, 66, 26, ID_REFRESH);
    connect_ = Make(hwnd_, L"BUTTON", T(L"เชื่อมต่อ", L"Connect"), WS_TABSTOP, 508, 9, 90, 26, ID_CONNECT);
    secure_ = Make(hwnd_, L"BUTTON", L"Security Access", WS_TABSTOP | WS_DISABLED, 604, 9, 168, 26, ID_SECURE);

    model_ = Make(hwnd_, L"STATIC", L"", SS_CENTER, 8, 42, 764, 30);
    SendMessageW(model_, WM_SETFONT, reinterpret_cast<WPARAM>(font_big_), TRUE);
    pn_ = Make(hwnd_, L"STATIC", L"", SS_CENTER, 8, 72, 764, 22);
    SendMessageW(pn_, WM_SETFONT, reinterpret_cast<WPARAM>(font_mid_), TRUE);

    tab_ = Make(hwnd_, WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, 8, 98, 764, 474, ID_TAB);
    const wchar_t* names[kTabCount] = {L"Flash", L"EEPROM", L"Data Logging", L"Trouble Codes", L"Log"};
    for (int i = 0; i < kTabCount; ++i) {
        TCITEMW item = {TCIF_TEXT};
        item.pszText = const_cast<wchar_t*>(names[i]);
        TabCtrl_InsertItem(tab_, i, &item);
    }

    // Plain statics instead of a status bar, so the theme colours them like everything else.
    status_state_ = Make(hwnd_, L"STATIC", L"", SS_OWNERDRAW, 12, 578, 220, 20, ID_STATUS);
    status_id_ = Make(hwnd_, L"STATIC", L"", SS_LEFT, 240, 580, 220, 20);
    status_flash_ = Make(hwnd_, L"STATIC", L"", SS_LEFT, 470, 580, 140, 20);
    status_dtc_ = Make(hwnd_, L"STATIC", L"", SS_LEFT, 620, 580, 140, 20);
    status_text_ = T(L"ยังไม่เชื่อมต่อ", L"Not connected");

    RECT tr;
    GetClientRect(tab_, &tr);
    TabCtrl_AdjustRect(tab_, FALSE, &tr);
    MapWindowPoints(tab_, hwnd_, reinterpret_cast<POINT*>(&tr), 2);
    std::vector<WORD> tmpl = DialogTemplate(WS_CHILD | DS_CONTROL | WS_CLIPCHILDREN, WS_EX_CONTROLPARENT, L"");
    for (int i = 0; i < kTabCount; ++i) {
        PageInit init = {this, i};
        HWND page = CreateDialogIndirectParamW(inst_, reinterpret_cast<DLGTEMPLATE*>(tmpl.data()), hwnd_, PageProc, reinterpret_cast<LPARAM>(&init));
        SetWindowPos(page, HWND_TOP, tr.left, tr.top, tr.right - tr.left, tr.bottom - tr.top, SWP_NOACTIVATE);
    }
    SelectTab(kTabFlash);
    ValidateFlash();
    ValidateEeprom();
    UpdateEnables();
}

void Panel::BuildPage(HWND page, int index) {
    pages_[index] = page;
    switch (index) {
        case kTabFlash: BuildFlashPage(page); break;
        case kTabEeprom: BuildEepromPage(page); break;
        case kTabData: BuildDataPage(page); break;
        case kTabDtc: BuildDtcPage(page); break;
        case kTabLog: BuildLogPage(page); break;
    }
}

void Panel::BuildFlashPage(HWND p) {
    Make(p, L"STATIC", T(L"โหมด:", L"Mode:"), SS_RIGHT, 8, 16, 92, 20);
    fl_read_ = Make(p, L"BUTTON", T(L"อ่านจากกล่อง (Read)", L"Read from ECU"), BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 108, 14, 190, 22, ID_FLASH_READ);
    fl_write_ = Make(p, L"BUTTON", T(L"เขียนลงกล่อง (Write)", L"Write to ECU"), BS_AUTORADIOBUTTON, 304, 14, 190, 22, ID_FLASH_WRITE);
    SendMessageW(fl_read_, BM_SETCHECK, BST_CHECKED, 0);

    Make(p, L"STATIC", T(L"ไฟล์:", L"File:"), SS_RIGHT, 8, 50, 92, 20);
    fl_path_ = Make(p, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP, 108, 47, 420, 25, ID_FLASH_PATH);
    fl_browse_ = Make(p, L"BUTTON", T(L"เลือก...", L"Browse..."), WS_TABSTOP, 534, 46, 70, 27, ID_FLASH_BROWSE);
    fl_current_ = Make(p, L"BUTTON", T(L"ไฟล์ที่เปิดใน TunerPro", L"Open TunerPro bin"), WS_TABSTOP, 610, 46, 138, 27, ID_FLASH_CURRENT);

    Make(p, L"STATIC", L"Start Offset:", SS_RIGHT, 8, 88, 92, 20);
    fl_offset_ = Make(p, L"EDIT", L"0x0", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 108, 85, 100, 25, ID_FLASH_OFFSET);
    fl_cks_label_ = Make(p, L"STATIC", L"Checksum Location:", SS_RIGHT, 224, 88, 134, 20);
    fl_checksum_ = Make(p, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 364, 85, 100, 25, ID_FLASH_CHECKSUM);
    fl_fix_ = Make(p, L"BUTTON", T(L"Fix (แก้ checksum ตอนเขียน)", L"Fix checksum when writing"), BS_AUTOCHECKBOX | WS_TABSTOP, 476, 87, 240, 22, ID_FLASH_FIX);

    fl_cks_state_ = Make(p, L"STATIC", L"", 0, 108, 118, 640, 20);

    fl_progress_ = Make(p, PROGRESS_CLASSW, L"", PBS_SMOOTH, 16, 160, 732, 22);
    SendMessageW(fl_progress_, PBM_SETRANGE32, 0, 1000);
    fl_text_ = Make(p, L"STATIC", L"", SS_CENTER, 16, 188, 732, 20);

    fl_hint_ = Make(p, L"STATIC", L"", 0, 16, 228, 500, 44);
    fl_cancel_ = Make(p, L"BUTTON", T(L"ยกเลิก", L"Cancel"), WS_TABSTOP | WS_DISABLED, 544, 232, 96, 34, ID_FLASH_CANCEL);
    fl_go_ = Make(p, L"BUTTON", L"Read", WS_TABSTOP | WS_DISABLED, 652, 232, 96, 34, ID_FLASH_GO);
}

void Panel::BuildEepromPage(HWND p) {
    Make(p, L"STATIC", T(L"โหมด:", L"Mode:"), SS_RIGHT, 8, 16, 60, 20);
    ee_read_ = Make(p, L"BUTTON", T(L"อ่าน (Read)", L"Read"), BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 76, 14, 116, 22, ID_EE_READ);
    ee_write_ = Make(p, L"BUTTON", T(L"เขียน (Write)", L"Write"), BS_AUTORADIOBUTTON, 196, 14, 116, 22, ID_EE_WRITE);
    ee_format_ = Make(p, L"BUTTON", T(L"ล้าง (Format)", L"Format"), BS_AUTORADIOBUTTON, 316, 14, 116, 22, ID_EE_FORMAT);
    SendMessageW(ee_read_, BM_SETCHECK, BST_CHECKED, 0);
    ee_fill00_ = Make(p, L"BUTTON", T(L"เติม 0x00", L"Fill 0x00"), BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 450, 14, 90, 22, ID_EE_FILL00);
    ee_fillff_ = Make(p, L"BUTTON", T(L"เติม 0xFF", L"Fill 0xFF"), BS_AUTORADIOBUTTON, 544, 14, 90, 22, ID_EE_FILLFF);
    SendMessageW(ee_fill00_, BM_SETCHECK, BST_CHECKED, 0);

    Make(p, L"STATIC", T(L"ไฟล์:", L"File:"), SS_RIGHT, 8, 48, 60, 20);
    ee_path_ = Make(p, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP, 76, 45, 484, 25, ID_EE_PATH);
    ee_browse_ = Make(p, L"BUTTON", T(L"เลือก...", L"Browse..."), WS_TABSTOP, 566, 44, 70, 27, ID_EE_BROWSE);
    ee_view_ = Make(p, L"BUTTON", T(L"เปิดดูไฟล์...", L"View file..."), WS_TABSTOP, 642, 44, 106, 27, ID_EE_VIEW);

    ee_progress_ = Make(p, PROGRESS_CLASSW, L"", PBS_SMOOTH, 16, 82, 732, 16);
    SendMessageW(ee_progress_, PBM_SETRANGE32, 0, 1000);
    ee_text_ = Make(p, L"STATIC", L"", 0, 16, 102, 300, 20);

    ee_hint_ = Make(p, L"STATIC", L"", 0, 16, 124, 500, 40);
    ee_cancel_ = Make(p, L"BUTTON", T(L"ยกเลิก", L"Cancel"), WS_TABSTOP | WS_DISABLED, 544, 124, 96, 32, ID_EE_CANCEL);
    ee_go_ = Make(p, L"BUTTON", L"Read", WS_TABSTOP | WS_DISABLED, 652, 124, 96, 32, ID_EE_GO);

    ee_hex_title_ = Make(p, L"STATIC", T(L"ข้อมูล EEPROM", L"EEPROM data"), 0, 16, 166, 600, 20);
    ee_hex_ = Make(p, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP | WS_GROUP, 16, 188, 732, 240);
    SendMessageW(ee_hex_, WM_SETFONT, reinterpret_cast<WPARAM>(font_mono_), TRUE);
}

void Panel::BuildDataPage(HWND p) {
    data_list_ = Make(p, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | WS_BORDER | WS_TABSTOP, 16, 12, 732, 416);
    ListView_SetExtendedListViewStyle(data_list_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    const struct { const wchar_t* name; int width; int fmt; } cols[] = {
        {T(L"ค่า", L"Sensor"), 240, LVCFMT_LEFT}, {L"Value", 140, LVCFMT_RIGHT}, {L"Unit", 100, LVCFMT_LEFT}};
    int c = 0;
    for (const auto& col : cols) {
        LVCOLUMNW lc = {LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
        lc.fmt = col.fmt;
        lc.cx = S(col.width);
        lc.pszText = const_cast<wchar_t*>(col.name);
        ListView_InsertColumn(data_list_, c++, &lc);
    }
    int row = 0;
    for (const auto& s : kSensors) {
        LVITEMW item = {LVIF_TEXT};
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(s.name);
        ListView_InsertItem(data_list_, &item);
        ListView_SetItemText(data_list_, row, 1, const_cast<wchar_t*>(L"---"));
        ListView_SetItemText(data_list_, row, 2, const_cast<wchar_t*>(s.unit));
        ++row;
    }
}

void Panel::BuildDtcPage(HWND p) {
    dtc_list_ = Make(p, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | WS_BORDER | WS_TABSTOP, 16, 12, 732, 370);
    ListView_SetExtendedListViewStyle(dtc_list_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    const struct { const wchar_t* name; int width; int fmt; } cols[] = {
        {L"DTC", 80, LVCFMT_CENTER}, {L"Description", 440, LVCFMT_LEFT}, {L"Current", 90, LVCFMT_CENTER}, {L"Past", 90, LVCFMT_CENTER}};
    int c = 0;
    for (const auto& col : cols) {
        LVCOLUMNW lc = {LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
        lc.fmt = col.fmt;
        lc.cx = S(col.width);
        lc.pszText = const_cast<wchar_t*>(col.name);
        ListView_InsertColumn(dtc_list_, c++, &lc);
    }
    dtc_clear_ = Make(p, L"BUTTON", L"Clear Codes", WS_TABSTOP | WS_DISABLED, 628, 392, 120, 34, ID_DTC_CLEAR);
}

void Panel::BuildLogPage(HWND p) {
    log_edit_ = Make(p, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | WS_BORDER | WS_TABSTOP, 16, 12, 732, 370);
    SendMessageW(log_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_mono_), TRUE);
    log_stats_ = Make(p, L"STATIC", L"", 0, 16, 396, 590, 22);
    Make(p, L"BUTTON", T(L"บันทึก Log...", L"Save log..."), WS_TABSTOP, 628, 392, 120, 34, ID_LOG_SAVE);
}

bool Panel::Ask(const std::wstring& text, UINT icon) {
    // The default answer is always No, because these questions come before a destructive step.
    return MessageBoxW(hwnd_, text.c_str(), L"Honda ECU", MB_YESNO | MB_DEFBUTTON2 | icon) == IDYES;
}

void Panel::Tell(const std::wstring& text, UINT icon) {
    MessageBoxW(hwnd_, text.c_str(), L"Honda ECU", MB_OK | icon);
}

// The path box can still hold the file chosen for a write, and a read replaces that file.
bool Panel::ConfirmOverwrite(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    return Ask(T(L"มีไฟล์นี้อยู่แล้ว ต้องการเขียนทับด้วยข้อมูลที่อ่านจากกล่องไหม?\n\n", L"This file already exists. Replace it with the data read from the ECU?\n\n") + path);
}

void Panel::Show(PanelCommand command) {
    if (!hwnd_) return;
    switch (command) {
        case kPanelFlashRead:
            SendMessageW(fl_read_, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(fl_write_, BM_SETCHECK, BST_UNCHECKED, 0);
            SetWindowTextW(fl_path_, L"");
            SelectTab(kTabFlash);
            break;
        case kPanelFlashWrite: {
            SendMessageW(fl_write_, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(fl_read_, BM_SETCHECK, BST_UNCHECKED, 0);
            std::wstring current = TunerProCurrentBinPath();
            if (!current.empty()) SetWindowTextW(fl_path_, current.c_str());
            SelectTab(kTabFlash);
            break;
        }
        case kPanelEeprom: SelectTab(kTabEeprom); break;
        case kPanelData: SelectTab(kTabData); break;
        case kPanelTroubleCodes: SelectTab(kTabDtc); break;
        default: break;
    }
    ValidateFlash();
    ValidateEeprom();
    UpdateEnables();
    ShowWindow(hwnd_, SW_SHOW);
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

void Panel::SelectTab(int index) {
    active_tab_ = index;
    TabCtrl_SetCurSel(tab_, index);
    for (int i = 0; i < kTabCount; ++i) ShowWindow(pages_[i], i == index ? SW_SHOW : SW_HIDE);
    if (worker_) worker_->SetUpdateTables(index == kTabData);
    if (index == kTabLog) log_dirty_ = true;
}

void Panel::RefreshAdapters() {
    std::wstring selected = GetText(adapter_);
    adapters_ = ListFtdiAdapters();
    for (auto& a : ListJ2534Adapters()) adapters_.push_back(a);
#ifdef HONDAECU_TESTBUILD
    wchar_t fake[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"HONDAECU_TEST_J2534", fake, MAX_PATH)) {
        AdapterInfo a;
        a.kind = AdapterInfo::kJ2534;
        a.label = L"J2534: Simulator";
        a.j2534_library = fake;
        adapters_.insert(adapters_.begin(), a);
    }
#endif
    SendMessageW(adapter_, CB_RESETCONTENT, 0, 0);
    int select = 0;
    for (size_t i = 0; i < adapters_.size(); ++i) {
        SendMessageW(adapter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(adapters_[i].label.c_str()));
        if (adapters_[i].label == selected) select = static_cast<int>(i);
    }
    if (adapters_.empty()) {
        SendMessageW(adapter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(L"ไม่พบอุปกรณ์ (FTDI / J2534)", L"No adapter found (FTDI / J2534)")));
    }
    SendMessageW(adapter_, CB_SETCURSEL, select, 0);
    UpdateEnables();
}

void Panel::ClearStatusFields() {
    SetWindowTextW(status_id_, L"");
    SetWindowTextW(status_flash_, L"");
    SetWindowTextW(status_dtc_, L"");
}

void Panel::ToggleConnect() {
    if (connected_) {
        if (worker_) worker_->Deactivate();
        connected_ = false;
        SetWindowTextW(connect_, T(L"เชื่อมต่อ", L"Connect"));
        state_ = ECUSTATE::UNKNOWN;
        status_text_ = T(L"ยังไม่เชื่อมต่อ", L"Not connected");
        SetEcmId({});
        SetWindowTextW(model_, L"");
        SetWindowTextW(pn_, L"");
        ResetSensors();
        ListView_DeleteAllItems(dtc_list_);
        ClearStatusFields();
        InvalidateRect(status_state_, nullptr, TRUE);
        ValidateFlash();
        ValidateEeprom();
        UpdateEnables();
        return;
    }
    const int sel = static_cast<int>(SendMessageW(adapter_, CB_GETCURSEL, 0, 0));
    if (sel < 0 || static_cast<size_t>(sel) >= adapters_.size()) return;
    HWND self = hwnd_;
    if (!worker_) {
        worker_ = std::make_unique<KlineWorker>([self](const WorkerEvent& e) {
            auto* copy = new WorkerEvent(e);
            if (!PostMessageW(self, WM_WORKER_EVENT, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
        });
        worker_->SetLogSink([this](const std::string& line) {
            std::lock_guard<std::mutex> lock(log_mutex_);
            log_lines_.push_back(line);
            if (log_lines_.size() > 3000) log_lines_.pop_front();
            log_dirty_ = true;
        });
    }
    worker_->SetUpdateTables(active_tab_ == kTabData);
    try {
        const AdapterInfo& info = adapters_[sel];
        std::unique_ptr<KlineAdapter> adapter =
            info.kind == AdapterInfo::kFtdi ? OpenFtdiAdapter(info) : OpenJ2534Adapter(info);
        worker_->Activate(std::move(adapter));
        connected_ = true;
        SetWindowTextW(connect_, T(L"ตัดการเชื่อมต่อ", L"Disconnect"));
    } catch (const AdapterError& e) {
        Tell(std::wstring(T(L"เปิดอุปกรณ์ไม่สำเร็จ\n\n", L"Could not open the adapter.\n\n")) + Widen(e.what()), MB_ICONERROR);
    }
    UpdateEnables();
}

void Panel::SetState(ECUSTATE state) {
    state_ = state;
    status_text_ = L"State: " + Widen(EcuStateName(state));
    InvalidateRect(status_state_, nullptr, TRUE);
    if (state == ECUSTATE::OFF || state == ECUSTATE::UNKNOWN) {
        SetWindowTextW(model_, L"");
        SetWindowTextW(pn_, L"");
        ClearStatusFields();
        ResetSensors();
    } else if (state == ECUSTATE::SECURE) {
        SetWindowTextW(model_, L"Unknown Model");
        SetWindowTextW(pn_, L"~ Security Access Mode ~");
    }
    if (pw_waiting_) {
        if (state == ECUSTATE::SECURE) {
            ClosePasswordDialog();
        } else if (state == ECUSTATE::OFF || state == ECUSTATE::UNKNOWN) {
            SetWindowTextW(pw_msg_, T(L"เปิดกุญแจ (Turn On ECU)", L"Turn On ECU"));
        }
    }
    ValidateFlash();
    ValidateEeprom();
    UpdateEnables();
}

void Panel::SetEcmId(const Bytes& id) {
    ecmid_ = id;
    ecm_ = nullptr;
    if (id.size() != 5) return;
    std::wstring text = L"ECM ID:";
    for (uint8_t b : id) text += Fmt(L" %02x", b);
    SetWindowTextW(status_id_, text.c_str());

    std::wstring model = L"Unknown Model";
    std::wstring pn = L"-";
    for (const auto& e : kEcmIds) {
        if (std::equal(id.begin(), id.end(), e.id)) {
            ecm_ = &e;
            break;
        }
    }
    if (ecm_) {
        model = Widen(ecm_->model) + L" (" + Widen(ecm_->year) + L")";
        pn = Widen(ecm_->pn);
    } else {
        for (const auto& e : kEcmIds) {
            if (std::equal(id.begin(), id.begin() + 3, e.id)) {
                model = Widen(e.model) + L" (" + Widen(e.year) + L")";
                break;
            }
        }
    }
    SetWindowTextW(model_, model.c_str());
    SetWindowTextW(pn_, pn.c_str());

    // Fill defaults from HondaECU's table unless the user typed their own value.
    if (ecm_) {
        if (!offset_edited_ && ecm_->offset >= 0) {
            offset_default_ = Fmt(L"0x%lX", ecm_->offset);
            SetWindowTextW(fl_offset_, offset_default_.c_str());
        }
        if (!checksum_edited_ && ecm_->checksum >= 0) {
            checksum_default_ = Fmt(L"0x%lX", ecm_->checksum);
            SetWindowTextW(fl_checksum_, checksum_default_.c_str());
        }
    }
    ValidateFlash();
}

// frames/flash.py OnValidateMode + OnValidateModeBin
void Panel::ValidateFlash() {
    const bool write = SendMessageW(fl_write_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const std::wstring path = GetText(fl_path_);
    SetWindowTextW(fl_go_, write ? L"Write" : L"Read");
    for (HWND h : {fl_cks_label_, fl_checksum_, fl_fix_, fl_current_}) ShowWindow(h, write ? SW_SHOW : SW_HIDE);
    EnableWindow(fl_checksum_, SendMessageW(fl_fix_, BM_GETCHECK, 0, 0) == BST_CHECKED);

    long offset = 0;
    const bool offset_ok = ParseHex(GetText(fl_offset_), &offset);
    bool enable = false;
    std::wstring hint;
    std::wstring cks;

    if (write && !path.empty()) {
        Bytes byts;
        if (!LoadFile(path, &byts) || byts.empty()) {
            cks = T(L"เปิดไฟล์ไม่ได้", L"Cannot open the file");
        } else {
            long cksum = -1;
            bool cks_bad_input = false;
            if (SendMessageW(fl_fix_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                if (!ParseHex(GetText(fl_checksum_), &cksum) || cksum >= static_cast<long>(byts.size())) cks_bad_input = true;
            }
            const size_t size = byts.size();
            Validation v = Validation::kBad;
            if (!cks_bad_input) v = do_validation(byts, cksum);
            cks = Fmt(T(L"ขนาด %u ไบต์ (%.2f KB) · Checksum: %hs", L"Size %u bytes (%.2f KB) · Checksum: %hs"), static_cast<unsigned>(size), size / 1024.0, ValidationName(v));
            if (cks_bad_input) {
                hint = T(L"ตำแหน่ง Checksum ไม่ถูกต้อง", L"The checksum location is not valid");
            } else if (v == Validation::kBad) {
                hint = T(L"Checksum ของไฟล์ไม่ผ่าน ติ๊ก Fix แล้วใส่ตำแหน่ง Checksum ก่อน", L"The file checksum is bad. Tick Fix and enter the checksum location");
            } else if (size % 64 != 0) {
                hint = T(L"ขนาดไฟล์ต้องหารด้วย 64 ลงตัว", L"The file size must be a multiple of 64");
            } else {
                enable = true;
            }
        }
    }

    if (!connected_) {
        hint = T(L"เลือกอุปกรณ์แล้วกดเชื่อมต่อก่อน", L"Choose an adapter and press Connect");
        enable = false;
    } else if (busy_) {
        hint = L"";
        enable = false;
    } else if (!write) {
        if (state_ != ECUSTATE::SECURE) {
            hint = T(L"การอ่าน ROM ต้องเข้า Security Access ก่อน: เปิดกุญแจไม่ต้องสตาร์ท กดปุ่ม Security Access ด้านบนแล้วทำตามขั้นตอน",
                     L"Reading the ROM needs Security Access: key on, engine off, then press Security Access above and follow the steps");
        } else if (!offset_ok) {
            hint = T(L"Start Offset ต้องเป็นเลขฐาน 16", L"Start Offset must be hexadecimal");
        } else {
            enable = true;
            hint = T(L"พร้อมอ่าน", L"Ready to read");
        }
    } else {
        if (state_ != ECUSTATE::OK && state_ != ECUSTATE::RECOVER_NEW && state_ != ECUSTATE::RECOVER_OLD && state_ != ECUSTATE::FLASH) {
            hint = T(L"การเขียนต้องอยู่ในสถานะ OK, RECOVER หรือ FLASH: เปิดกุญแจแล้วรอให้กล่องตอบ", L"Writing needs state OK, RECOVER or FLASH: key on and wait for the ECU to answer");
            enable = false;
        } else if (path.empty()) {
            hint = T(L"เลือกไฟล์ที่จะเขียน", L"Choose the file to write");
            enable = false;
        } else if (!offset_ok) {
            hint = T(L"Start Offset ต้องเป็นเลขฐาน 16", L"Start Offset must be hexadecimal");
            enable = false;
        } else if (enable) {
            hint = T(L"พร้อมเขียน", L"Ready to write");
        }
    }
    SetWindowTextW(fl_cks_state_, cks.c_str());
    SetWindowTextW(fl_hint_, hint.c_str());
    EnableWindow(fl_go_, enable);
    EnableWindow(fl_cancel_, busy_ && !busy_is_write_);
}

// frames/eeprom.py OnValidateMode
void Panel::ValidateEeprom() {
    const bool read = SendMessageW(ee_read_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool write = SendMessageW(ee_write_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool format = SendMessageW(ee_format_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    SetWindowTextW(ee_go_, read ? L"Read" : (write ? L"Write" : L"Format"));
    ShowWindow(ee_fill00_, format ? SW_SHOW : SW_HIDE);
    ShowWindow(ee_fillff_, format ? SW_SHOW : SW_HIDE);
    EnableWindow(ee_path_, !format);
    EnableWindow(ee_browse_, !format);

    const std::wstring path = GetText(ee_path_);
    bool enable = false;
    std::wstring hint;
    if (!connected_) {
        hint = T(L"เลือกอุปกรณ์แล้วกดเชื่อมต่อก่อน", L"Choose an adapter and press Connect");
    } else if (busy_) {
        hint = L"";
    } else if (state_ != ECUSTATE::SECURE) {
        hint = T(L"งาน EEPROM ต้องเข้า Security Access ก่อน: เปิดกุญแจไม่ต้องสตาร์ท กดปุ่ม Security Access ด้านบนแล้วทำตามขั้นตอน",
                 L"EEPROM work needs Security Access: key on, engine off, then press Security Access above and follow the steps");
    } else if (read) {
        enable = true;
        hint = T(L"พร้อมอ่าน", L"Ready to read");
    } else if (write) {
        Bytes byts;
        if (path.empty()) {
            hint = T(L"เลือกไฟล์ EEPROM ที่จะเขียน", L"Choose the EEPROM file to write");
        } else if (!LoadFile(path, &byts) || (byts.size() != 256 && byts.size() != 512)) {
            hint = T(L"ไฟล์ EEPROM ต้องมีขนาด 256 หรือ 512 ไบต์", L"An EEPROM file must be 256 or 512 bytes");
        } else {
            enable = true;
            hint = T(L"พร้อมเขียน", L"Ready to write");
        }
    } else {
        enable = true;
        hint = T(L"พร้อมล้าง EEPROM", L"Ready to format");
    }
    SetWindowTextW(ee_hint_, hint.c_str());
    EnableWindow(ee_go_, enable);
    EnableWindow(ee_cancel_, busy_ && !busy_is_write_);
}

void Panel::UpdateEnables() {
    const bool has_adapter = !adapters_.empty();
    EnableWindow(adapter_, !connected_ && !busy_ && has_adapter);
    EnableWindow(refresh_, !connected_ && !busy_);
    EnableWindow(connect_, !busy_ && has_adapter);
    EnableWindow(secure_, connected_ && !busy_ && state_ == ECUSTATE::OK);
    EnableWindow(dtc_clear_, connected_ && !busy_ && state_ == ECUSTATE::OK && ListView_GetItemCount(dtc_list_) > 0);
    for (HWND h : {fl_read_, fl_write_, fl_path_, fl_browse_, fl_current_, fl_offset_, fl_fix_}) EnableWindow(h, !busy_);
    for (HWND h : {ee_read_, ee_write_, ee_format_, ee_fill00_, ee_fillff_, ee_view_}) EnableWindow(h, !busy_);
}

void Panel::SetBusy(bool busy, bool is_write) {
    busy_ = busy;
    busy_is_write_ = busy && is_write;
    if (!busy) {
        SendMessageW(fl_progress_, PBM_SETMARQUEE, FALSE, 0);
        LONG_PTR style = GetWindowLongPtrW(fl_progress_, GWL_STYLE);
        SetWindowLongPtrW(fl_progress_, GWL_STYLE, style & ~PBS_MARQUEE);
    }
    ValidateFlash();
    ValidateEeprom();
    UpdateEnables();
}

void Panel::FlashGo() {
    if (!worker_ || !IsWindowEnabled(fl_go_)) return;
    const bool write = SendMessageW(fl_write_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    long offset = 0;
    if (!ParseHex(GetText(fl_offset_), &offset)) return;
    if (!write) {
        std::wstring path = GetText(fl_path_);
        if (path.empty()) {
            wchar_t file[MAX_PATH] = {0};
            if (ecm_) {
                MultiByteToWideChar(CP_ACP, 0, ecm_->pn, -1, file, MAX_PATH - 4);
                wcscat_s(file, L".bin");
            } else {
                wcscpy_s(file, L"ecu_dump.bin");
            }
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFilter = L"ECU dump (*.bin)\0*.bin\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"bin";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
            if (!GetSaveFileNameW(&ofn)) return;
            path = file;
            SetWindowTextW(fl_path_, path.c_str());
        } else if (!ConfirmOverwrite(path)) {
            return;
        }
        last_read_path_ = path;
        SetWindowLongPtrW(fl_progress_, GWL_STYLE, GetWindowLongPtrW(fl_progress_, GWL_STYLE) | PBS_MARQUEE);
        SendMessageW(fl_progress_, PBM_SETMARQUEE, TRUE, 40);
        SetWindowTextW(fl_text_, T(L"กำลังอ่าน...", L"Reading..."));
        SetBusy(true, false);
        worker_->ReadPanel(path, static_cast<uint32_t>(offset));
        return;
    }

    const std::wstring path = GetText(fl_path_);
    Bytes byts;
    if (!LoadFile(path, &byts) || byts.empty()) {
        Tell(T(L"เปิดไฟล์ไม่ได้", L"Cannot open the file"), MB_ICONERROR);
        return;
    }
    long cksum = -1;
    if (SendMessageW(fl_fix_, BM_GETCHECK, 0, 0) == BST_CHECKED) ParseHex(GetText(fl_checksum_), &cksum);
    if (cksum >= static_cast<long>(byts.size())) return;
    const Validation v = do_validation(byts, cksum);
    if (v == Validation::kBad) {
        ValidateFlash();
        return;
    }
    std::wstring ecu = ecm_ ? Widen(ecm_->model) + L" " + Widen(ecm_->pn) : L"Unknown Model";
    std::wstring text = T(L"เขียนไฟล์ลงกล่อง?\n\n", L"Write this file to the ECU?\n\n");
    text += T(L"ไฟล์: ", L"File: ") + path + L"\n";
    text += Fmt(T(L"ขนาด: %u ไบต์ · Start Offset: 0x%lX · Checksum: %hs\n", L"Size: %u bytes · Start Offset: 0x%lX · Checksum: %hs\n"), static_cast<unsigned>(byts.size()), offset, ValidationName(v));
    text += T(L"กล่อง: ", L"ECU: ") + ecu + L"\n\n";
    text += T(L"ไฟล์ที่เขียนคือไฟล์บนดิสก์ ถ้าแก้ใน TunerPro ให้กด Save Bin ก่อน\n", L"This writes the file on disk. Save Bin first if you changed it in TunerPro.\n");
    text += T(L"ระหว่างเขียน ห้ามดับกุญแจ ห้ามถอดสาย และแบตต้องเต็ม\nถ้าขาดกลางทางกล่องจะสตาร์ทไม่ติดจนกว่าจะเขียนใหม่สำเร็จ",
              L"Do not turn the key off or unplug the cable while writing, and use a full battery.\nAn interrupted write leaves the bike unable to start until a rewrite succeeds.");
    if (!Ask(text)) return;
    SetWindowTextW(fl_text_, T(L"เริ่มเขียน...", L"Starting the write..."));
    SetBusy(true, true);
    worker_->WritePanel(byts, static_cast<uint32_t>(offset));
}

void Panel::EepromGo() {
    if (!worker_ || !IsWindowEnabled(ee_go_)) return;
    const std::wstring path = GetText(ee_path_);
    if (SendMessageW(ee_read_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        std::wstring target = path;
        if (target.empty()) {
            wchar_t file[MAX_PATH] = L"eeprom.bin";
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFilter = L"EEPROM dump (*.bin)\0*.bin\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"bin";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
            if (!GetSaveFileNameW(&ofn)) return;
            target = file;
            SetWindowTextW(ee_path_, target.c_str());
        } else if (!ConfirmOverwrite(target)) {
            return;
        }
        ee_pending_view_ = target;
        SetBusy(true, false);
        worker_->EepromRead(target);
    } else if (SendMessageW(ee_write_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        Bytes byts;
        if (!LoadFile(path, &byts) || (byts.size() != 256 && byts.size() != 512)) {
            ValidateEeprom();
            return;
        }
        std::wstring text = T(L"เขียนไฟล์นี้ลง EEPROM ของกล่อง?\n\n", L"Write this file to the ECU's EEPROM?\n\n");
        text += path + Fmt(T(L"\nขนาด %u ไบต์", L"\nSize %u bytes"), static_cast<unsigned>(byts.size()));
        if (!Ask(text)) return;
        ShowHex(byts, T(L"ไฟล์ที่กำลังเขียน", L"File being written"));
        SetBusy(true, true);
        worker_->EepromWrite(byts);
    } else {
        const int mode = SendMessageW(ee_fillff_, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
        if (!Ask(Fmt(T(L"ล้าง EEPROM ทั้งหมดเป็น 0x%02X?\n\nข้อมูลเดิมในกล่องจะหาย ถ้ายังไม่มีไฟล์สำรองให้อ่านเก็บไว้ก่อน",
                       L"Format the whole EEPROM to 0x%02X?\n\nThe data in the ECU will be lost. Read a backup first if you do not have one."),
                     mode == 1 ? 0xFF : 0x00))) {
            return;
        }
        SetBusy(true, true);
        worker_->EepromFormat(mode);
    }
}

void Panel::ShowHex(const Bytes& data, const std::wstring& title) {
    SetWindowTextW(ee_hex_, HexDump(data).c_str());
    SetWindowTextW(ee_hex_title_, (title + Fmt(T(L" (%u ไบต์)", L" (%u bytes)"), static_cast<unsigned>(data.size()))).c_str());
}

void Panel::SetSensor(int row, double value, int decimals) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.*f", decimals, value);
    ListView_SetItemText(data_list_, row, 1, buf);
}

void Panel::ResetSensors() {
    maintable_ = -1;
    for (int i = 0; i < static_cast<int>(_countof(kSensors)); ++i) {
        ListView_SetItemText(data_list_, i, 1, const_cast<wchar_t*>(L"---"));
    }
}

// frames/data.py KlineWorkerHandler + prepare_data1
void Panel::OnData(int t, int dl, const Bytes& raw) {
    if (raw.size() < 2) return;
    const Bytes d(raw.begin() + 2, raw.end());
    if (t == 0x10 || t == 0x11 || t == 0x13 || t == 0x17) {
        if (d.size() < 14) return;
        std::vector<double> data;
        data.push_back((d[0] << 8) | d[1]);
        for (size_t i = 2; i < 14; ++i) data.push_back(d[i]);
        size_t pos = 14;
        auto u8 = [&]() { if (pos < d.size()) data.push_back(d[pos++]); };
        auto u16 = [&]() { if (pos + 1 < d.size()) { data.push_back((d[pos] << 8) | d[pos + 1]); pos += 2; } };
        if (t != 0x13) {
            u16();
            u8();
            if (t == 0x11) {
                u8();
                u16();
            } else if (t == 0x17) {
                u8();
                u8();
            }
        }
        if (t == 0x13 || t == 0x17) {
            data.insert(data.begin() + 9, 2, 0xFF);
        }
        if (data.size() < 15) return;
        data[1] = data[1] / 0xFF * 5.0;
        data[2] = data[2] / 1.6;
        data[3] = data[3] / 0xFF * 5.0;
        data[4] = -40 + data[4];
        data[5] = data[5] / 0xFF * 5.0;
        data[6] = -40 + data[6];
        data[7] = data[7] / 0xFF * 5.0;
        data[8] = 10 * data[8];
        data[11] = data[11] / 10;
        data[13] = data[13] / 0xFFFF * 265.5;
        data[14] = -64 + data[14] / 0xFF * 127.5;
        if (t == 0x11 && data.size() > 16) data[16] = data[16] / 0xFFFF * 8.0;
        const struct { int row; size_t index; int decimals; } map[] = {
            {0, 0, 0}, {1, 2, 2}, {2, 4, 0}, {3, 6, 0}, {4, 8, 0}, {5, 11, 2}, {6, 12, 0}, {7, 13, 2}, {8, 14, 2}, {9, 15, 0}, {10, 16, 4}};
        if (maintable_ < 0) maintable_ = t;
        for (const auto& m : map) {
            if ((m.row == 9 || m.row == 10) && maintable_ != 0x11) continue;
            if (m.index < data.size()) SetSensor(m.row, data[m.index], m.decimals);
        }
    } else if (t == 0x20 || t == 0x21) {
        if (d.size() < 3) return;
        const int base = t == 0x20 ? 11 : 14;
        SetSensor(base, d[0] / 255.0 * 5, 2);
        SetSensor(base + 1, d[1] / 255.0 * 2, 4);
        SetSensor(base + 2, d[2] / 255.0 * 5, 2);
    } else if (t == 0xD0) {
        if (d.size() < 11) return;
        SetSensor(17, d[5] / 255.0 * 5, 3);
        SetSensor(18, d[6] / 255.0 * 5, 3);
        SetSensor(19, static_cast<int8_t>(d[7]), 0);
        SetSensor(20, d[8] / 255.0 * 5, 3);
        SetSensor(21, d[9] / 255.0 * 5, 3);
        SetSensor(22, d[10], 0);
    }
    (void)dl;
}

void Panel::OnWorkerEvent(const WorkerEvent& e) {
    const std::string& info = e.info;
    if (info == "state") {
        SetState(static_cast<ECUSTATE>(e.value));
    } else if (info == "ecmid") {
        if (e.bytes.size() == 5) SetEcmId(e.bytes);
    } else if (info == "flashcount") {
        if (e.value >= 0) SetWindowTextW(status_flash_, Fmt(L"Flash Count: %d", e.value).c_str());
    } else if (info == "dtccount") {
        if (e.value >= 0) SetWindowTextW(status_dtc_, Fmt(L"DTC Count: %d", e.value).c_str());
        if (e.value <= 0) ListView_DeleteAllItems(dtc_list_);
        UpdateEnables();
    } else if (info == "dtc") {
        // frames/error.py
        ListView_DeleteAllItems(dtc_list_);
        std::map<std::string, std::pair<bool, bool>> codes;
        auto cur = e.dtc.find("0x74");
        auto past = e.dtc.find("0x73");
        if (cur != e.dtc.end()) for (const auto& c : cur->second) codes[c].first = true;
        if (past != e.dtc.end()) for (const auto& c : past->second) codes[c].second = true;
        int row = 0;
        for (const auto& kv : codes) {
            const std::wstring code = Widen(kv.first);
            const char* desc = DtcDescription(kv.first);
            const std::wstring text = Widen(desc ? desc : "UNKNOWN");
            LVITEMW item = {LVIF_TEXT};
            item.iItem = row;
            item.pszText = const_cast<wchar_t*>(code.c_str());
            ListView_InsertItem(dtc_list_, &item);
            ListView_SetItemText(dtc_list_, row, 1, const_cast<wchar_t*>(text.c_str()));
            ListView_SetItemText(dtc_list_, row, 2, const_cast<wchar_t*>(kv.second.first ? L"X" : L""));
            ListView_SetItemText(dtc_list_, row, 3, const_cast<wchar_t*>(kv.second.second ? L"X" : L""));
            ++row;
        }
        UpdateEnables();
    } else if (info == "data") {
        OnData(e.value, e.dl, e.bytes);
    } else if (info == "password") {
        if (pw_waiting_ && !e.value) SetWindowTextW(pw_info_, T(L"กล่องไม่รับรหัสผ่าน", L"The ECU refused the password"));
    } else if (info == "read.progress") {
        if (e.text == "interrupted") {
            SetWindowTextW(fl_text_, T(L"หยุดอ่านแล้ว", L"Read stopped"));
        } else {
            SetWindowTextW(fl_text_, (L"Read: " + Widen(e.text)).c_str());
        }
    } else if (info == "read.result") {
        const bool good = e.text == "good";
        SetWindowTextW(fl_text_, (L"Read: complete (result=" + Widen(e.text) + L")").c_str());
        std::wstring msg = good ? T(L"อ่านไฟล์จากกล่องสำเร็จ (Checksum ผ่าน)\n\n", L"The ECU was read successfully (checksum good).\n\n")
                                : std::wstring(T(L"อ่านจบแล้ว แต่ผลคือ ", L"The read finished, but the result is ")) + Widen(e.text) + T(L" (ผลรวม Checksum ของไฟล์ไม่เป็น 0)\n\n", L" (the file's checksum sum is not 0).\n\n");
        msg += last_read_path_ + T(L"\n\nเปิดไฟล์นี้ใน TunerPro เลยไหม?", L"\n\nOpen this file in TunerPro now?");
        if (Ask(msg, good ? MB_ICONINFORMATION : MB_ICONWARNING)) {
            if (!TunerProOpenBin(last_read_path_)) {
                Tell(T(L"สั่งเปิดไฟล์ไม่สำเร็จ เปิดจากเมนู Open Bin ของ TunerPro แทน", L"Could not open the file. Use Open Bin in TunerPro instead."), MB_ICONWARNING);
            }
        }
    } else if (info == "erase") {
        SetWindowTextW(fl_text_, T(L"เตรียมลบ...", L"Preparing to erase..."));
    } else if (info == "write") {
        SetWindowTextW(fl_text_, T(L"กำลังเขียน...", L"Writing..."));
    } else if (info == "write.progress") {
        if (e.percent >= 0) SendMessageW(fl_progress_, PBM_SETPOS, static_cast<WPARAM>(e.percent * 10), 0);
        SetWindowTextW(fl_text_, (L"Write: " + Widen(e.text)).c_str());
        if (e.text == "erase failed" || e.text == "erase block error" || e.text == "failed" ||
            e.text == "max retries" || e.text == "unexpected return") {
            Tell(std::wstring(T(L"เขียนไม่สำเร็จ: ", L"The write failed: ")) + Widen(e.text) +
                     T(L"\n\nถ้ากล่องยังไม่ถูกลบ ให้ปิดกุญแจรอสักครู่ เปิดใหม่แล้วลองอีกครั้งทันที\nถ้าเขียนค้างกลางทาง ให้กดเขียนซ้ำจนสำเร็จ",
                       L"\n\nIf nothing was erased yet: key off, wait a moment, key on and try again straight away.\nIf it stopped part way, write again until it succeeds."),
                 MB_ICONERROR);
        }
    } else if (info == "write.result") {
        SendMessageW(fl_progress_, PBM_SETPOS, 0, 0);
        SetWindowTextW(fl_text_, (L"Write: complete (result=" + Widen(e.text) + L")").c_str());
        if (e.text == "good") {
            Tell(T(L"เขียนสำเร็จ ปิดกุญแจรอสักครู่แล้วเปิดใหม่", L"Write complete. Key off, wait a moment, then key on."), MB_ICONINFORMATION);
        } else {
            Tell(T(L"เขียนจบแล้ว แต่กล่องตอบผลตรวจไม่ผ่าน (result=bad)", L"The write finished, but the ECU's check did not pass (result=bad)."), MB_ICONWARNING);
        }
    } else if (info == "read_eeprom.progress" || info == "write_eeprom.progress") {
        if (e.percent >= 0) SendMessageW(ee_progress_, PBM_SETPOS, static_cast<WPARAM>(e.percent * 10), 0);
        if (info == "read_eeprom.progress") {
            SetWindowTextW(ee_text_, T(L"กำลังอ่าน EEPROM", L"Reading EEPROM"));
        } else {
            SetWindowTextW(ee_text_, T(L"กำลังเขียน EEPROM", L"Writing EEPROM"));
        }
        if (e.text == "interrupted") SetWindowTextW(ee_text_, T(L"หยุดกลางคัน", L"Interrupted"));
    } else if (info == "read_eeprom.result") {
        SendMessageW(ee_progress_, PBM_SETPOS, 0, 0);
        SetWindowTextW(ee_text_, (L"Read EEPROM: " + Widen(e.text)).c_str());
        Bytes data;
        if (e.text == "good" && LoadFile(ee_pending_view_, &data)) {
            ShowHex(data, T(L"EEPROM ที่อ่านได้", L"EEPROM read from the ECU"));
        } else if (e.text != "good") {
            Tell(std::wstring(T(L"อ่าน EEPROM ไม่สำเร็จ (", L"The EEPROM read failed (")) + Widen(e.text) + L")", MB_ICONWARNING);
        }
    } else if (info == "write_eeprom.result") {
        SendMessageW(ee_progress_, PBM_SETPOS, 0, 0);
        SetWindowTextW(ee_text_, (L"Write EEPROM: " + Widen(e.text)).c_str());
        if (e.text == "good") {
            Tell(T(L"เขียน EEPROM สำเร็จ", L"EEPROM write complete."), MB_ICONINFORMATION);
        } else {
            Tell(T(L"เขียน EEPROM ไม่สำเร็จ", L"The EEPROM write failed."), MB_ICONERROR);
        }
    } else if (info == "format_eeprom.result") {
        SetWindowTextW(ee_text_, e.value ? L"Format EEPROM: good" : L"Format EEPROM: bad");
        if (e.value) {
            Tell(T(L"ล้าง EEPROM สำเร็จ", L"EEPROM format complete."), MB_ICONINFORMATION);
        } else {
            Tell(T(L"ล้าง EEPROM ไม่สำเร็จ", L"The EEPROM format failed."), MB_ICONERROR);
        }
    } else if (info == "op.finished") {
        SetBusy(false, false);
    } else if (info == "device.lost") {
        SetBusy(false, false);
        connected_ = true;
        ToggleConnect();
        Tell(std::wstring(T(L"การเชื่อมต่ออุปกรณ์หลุด\n\n", L"The adapter connection was lost.\n\n")) + Widen(e.text), MB_ICONERROR);
    }
}

void Panel::ShowPasswordDialog() {
    if (!pw_) {
        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
        std::vector<WORD> tmpl = DialogTemplate(style, WS_EX_CONTROLPARENT, L"Security Access");
        CreateDialogIndirectParamW(inst_, reinterpret_cast<DLGTEMPLATE*>(tmpl.data()), hwnd_, PasswordProc, reinterpret_cast<LPARAM>(this));
    }
    pw_waiting_ = false;
    SetWindowTextW(pw_msg_, L"");
    SetWindowTextW(pw_info_, L"HelloHowAreYou");
    ShowWindow(pw_edit_, SW_SHOW);
    ShowWindow(pw_ok_, SW_SHOW);
    EnableWindow(hwnd_, FALSE);
    ShowWindow(pw_, SW_SHOW);
    SetForegroundWindow(pw_);
    SetFocus(pw_ok_);
}

void Panel::ClosePasswordDialog() {
    pw_waiting_ = false;
    EnableWindow(hwnd_, TRUE);
    if (pw_) ShowWindow(pw_, SW_HIDE);
    SetForegroundWindow(hwnd_);
}

void Panel::FlushLog() {
    std::string joined;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (!log_dirty_) return;
        log_dirty_ = false;
        for (const auto& line : log_lines_) {
            joined += line;
            joined += "\r\n";
        }
    }
    SetWindowTextA(log_edit_, joined.c_str());
    SendMessageW(log_edit_, EM_SETSEL, static_cast<WPARAM>(joined.size()), static_cast<LPARAM>(joined.size()));
    SendMessageW(log_edit_, EM_SCROLLCARET, 0, 0);
    if (worker_) {
        const AdapterStats s = worker_->Stats();
        SetWindowTextW(log_stats_, Fmt(L"Adapter stats: retries=%u checksum_errors=%u unneeded_retry=%u", s.retries, s.checksum_errors, s.unneeded_retry).c_str());
    }
}

// Paints the status bullet with the colours TunerPro's theme hands out for a static control.
void Panel::DrawStatus(const DRAWITEMSTRUCT* d) {
    HBRUSH background = reinterpret_cast<HBRUSH>(SendMessageW(hwnd_, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(d->hDC), reinterpret_cast<LPARAM>(d->hwndItem)));
    RECT r = d->rcItem;
    FillRect(d->hDC, &r, background ? background : GetSysColorBrush(COLOR_BTNFACE));

    // HondaECU's status bullet colours: black, yellow, green, blue, purple.
    COLORREF color = RGB(110, 110, 110);
    if (connected_) {
        switch (state_) {
            case ECUSTATE::RECOVER_NEW:
            case ECUSTATE::RECOVER_OLD: color = RGB(230, 180, 0); break;
            case ECUSTATE::OK: color = RGB(40, 190, 80); break;
            case ECUSTATE::FLASH: color = RGB(60, 140, 255); break;
            case ECUSTATE::SECURE: color = RGB(170, 90, 230); break;
            default: break;
        }
    }
    const int size = S(10);
    const int top = r.top + (r.bottom - r.top - size) / 2;
    HBRUSH b = CreateSolidBrush(color);
    HGDIOBJ old_b = SelectObject(d->hDC, b);
    HGDIOBJ old_p = SelectObject(d->hDC, GetStockObject(NULL_PEN));
    Ellipse(d->hDC, r.left, top, r.left + size, top + size);
    SelectObject(d->hDC, old_p);
    SelectObject(d->hDC, old_b);
    DeleteObject(b);
    r.left += S(16);
    SetBkMode(d->hDC, TRANSPARENT);
    HGDIOBJ old_f = SelectObject(d->hDC, font_);
    DrawTextW(d->hDC, status_text_.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(d->hDC, old_f);
}

void Panel::OnCommand(int id, int code) {
    switch (id) {
        case ID_REFRESH: RefreshAdapters(); break;
        case ID_CONNECT: ToggleConnect(); break;
        case ID_SECURE: ShowPasswordDialog(); break;
        case ID_FLASH_READ:
        case ID_FLASH_WRITE:
        case ID_FLASH_FIX:
            ValidateFlash();
            break;
        case ID_FLASH_PATH:
            if (code == EN_CHANGE && fl_go_) ValidateFlash();
            break;
        case ID_FLASH_OFFSET:
            // An edit control also reports EN_CHANGE while it is being created.
            if (code == EN_CHANGE && fl_offset_) {
                if (GetText(fl_offset_) != offset_default_) offset_edited_ = true;
                ValidateFlash();
            }
            break;
        case ID_FLASH_CHECKSUM:
            if (code == EN_CHANGE && fl_checksum_) {
                if (GetText(fl_checksum_) != checksum_default_) checksum_edited_ = true;
                ValidateFlash();
            }
            break;
        case ID_FLASH_BROWSE: {
            const bool write = SendMessageW(fl_write_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            wchar_t file[MAX_PATH] = {0};
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFilter = L"ECU dump (*.bin)\0*.bin\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"bin";
            ofn.Flags = write ? (OFN_FILEMUSTEXIST | OFN_HIDEREADONLY) : (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY);
            if (write ? GetOpenFileNameW(&ofn) : GetSaveFileNameW(&ofn)) SetWindowTextW(fl_path_, file);
            break;
        }
        case ID_FLASH_CURRENT: {
            const std::wstring current = TunerProCurrentBinPath();
            if (current.empty()) {
                Tell(T(L"ไม่พบไฟล์ Bin ที่เปิดอยู่ใน TunerPro", L"No bin is open in TunerPro."), MB_ICONINFORMATION);
            } else {
                SetWindowTextW(fl_path_, current.c_str());
            }
            break;
        }
        case ID_FLASH_GO: FlashGo(); break;
        case ID_FLASH_CANCEL:
        case ID_EE_CANCEL:
            if (worker_) worker_->CancelRead();
            break;
        case ID_EE_READ:
        case ID_EE_WRITE:
        case ID_EE_FORMAT:
        case ID_EE_FILL00:
        case ID_EE_FILLFF:
            ValidateEeprom();
            break;
        case ID_EE_PATH:
            if (code == EN_CHANGE && ee_go_) ValidateEeprom();
            break;
        case ID_EE_BROWSE: {
            const bool write = SendMessageW(ee_write_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            wchar_t file[MAX_PATH] = {0};
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFilter = L"EEPROM dump (*.bin)\0*.bin\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"bin";
            ofn.Flags = write ? (OFN_FILEMUSTEXIST | OFN_HIDEREADONLY) : (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY);
            if (write ? GetOpenFileNameW(&ofn) : GetSaveFileNameW(&ofn)) {
                SetWindowTextW(ee_path_, file);
                Bytes data;
                if (write && LoadFile(file, &data)) ShowHex(data, T(L"ไฟล์ที่เลือก", L"Selected file"));
            }
            break;
        }
        case ID_EE_VIEW: {
            wchar_t file[MAX_PATH] = {0};
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFilter = L"EEPROM dump (*.bin)\0*.bin\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            if (GetOpenFileNameW(&ofn)) {
                Bytes data;
                if (LoadFile(file, &data)) ShowHex(data, file);
            }
            break;
        }
        case ID_EE_GO: EepromGo(); break;
        case ID_DTC_CLEAR:
            if (worker_ && Ask(T(L"ล้างรหัสปัญหาทั้งหมดในกล่อง?", L"Clear all trouble codes in the ECU?"))) worker_->ClearCodes();
            break;
        case ID_LOG_SAVE: {
            wchar_t file[MAX_PATH] = L"hondaecu-log.txt";
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFilter = L"Debug log files (*.txt)\0*.txt\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"txt";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
            if (GetSaveFileNameW(&ofn)) {
                std::lock_guard<std::mutex> lock(log_mutex_);
                FILE* f = _wfopen(file, L"wb");
                if (f) {
                    for (const auto& line : log_lines_) {
                        fwrite(line.data(), 1, line.size(), f);
                        fwrite("\r\n", 1, 2, f);
                    }
                    fclose(f);
                }
            }
            break;
        }
        case IDCANCEL:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        default: break;
    }
}

// Returns TRUE for a message handled here. Everything else, including WM_CTLCOLOR*, is left
// to the dialog manager and TunerPro's theme.
INT_PTR Panel::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp));
            return TRUE;
        case WM_NOTIFY: {
            const NMHDR* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->hwndFrom == tab_ && hdr->code == TCN_SELCHANGE) {
                SelectTab(TabCtrl_GetCurSel(tab_));
                return TRUE;
            }
            return FALSE;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* d = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (d->hwndItem == status_state_) {
                DrawStatus(d);
                return TRUE;
            }
            return FALSE;
        }
        case WM_WORKER_EVENT: {
            std::unique_ptr<WorkerEvent> e(reinterpret_cast<WorkerEvent*>(lp));
            OnWorkerEvent(*e);
            return TRUE;
        }
        case WM_TIMER:
            if (wp == kTimerLog && active_tab_ == kTabLog) FlushLog();
            return TRUE;
        case WM_CLOSE:
            if (WriteInProgress()) {
                Tell(T(L"กำลังเขียนข้อมูลลงกล่อง ห้ามปิดจนกว่าจะเสร็จ", L"Writing to the ECU. Do not close until it finishes."), MB_ICONWARNING);
                return TRUE;
            }
            if (busy_ && worker_) worker_->CancelRead();
            if (connected_) ToggleConnect();
            ShowWindow(hwnd_, SW_HIDE);
            return TRUE;
        case WM_DESTROY:
            hwnd_ = nullptr;
            return TRUE;
        default: return FALSE;
    }
}

INT_PTR CALLBACK Panel::MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        Panel* self = reinterpret_cast<Panel*>(lp);
        SetWindowLongPtrW(h, DWLP_USER, lp);
        self->hwnd_ = h;
        self->BuildControls();
        return TRUE;
    }
    Panel* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(h, DWLP_USER));
    return self ? self->OnMessage(msg, wp, lp) : FALSE;
}

// A tab page builds its controls here so TunerPro's theme sees them, then only passes
// notifications up to the main dialog.
INT_PTR CALLBACK Panel::PageProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        const PageInit* init = reinterpret_cast<PageInit*>(lp);
        SetWindowLongPtrW(h, DWLP_USER, reinterpret_cast<LONG_PTR>(init->self));
        init->self->BuildPage(h, init->index);
        return FALSE;
    }
    Panel* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(h, DWLP_USER));
    if (self && self->hwnd_ && (msg == WM_COMMAND || msg == WM_NOTIFY)) {
        return self->OnMessage(msg, wp, lp);
    }
    return FALSE;
}

INT_PTR Panel::OnPasswordMessage(UINT msg, WPARAM wp, LPARAM) {
    if (msg == WM_COMMAND) {
        const int id = LOWORD(wp);
        if (id == ID_PW_EDIT && HIWORD(wp) == EN_CHANGE && pw_info_) {
            // Live ASCII preview, as PasswordDialog.OnPassByte does per byte.
            std::wstring text = GetText(pw_edit_), ascii;
            const wchar_t* p = text.c_str();
            for (;;) {
                wchar_t* end = nullptr;
                long v = wcstol(p, &end, 16);
                if (end == p) break;
                ascii += (v >= 0x20 && v < 0x7F) ? static_cast<wchar_t>(v) : L'.';
                p = end;
            }
            SetWindowTextW(pw_info_, ascii.c_str());
        } else if (id == ID_PW_OK || id == IDOK) {
            if (pw_waiting_) return TRUE;
            Bytes password;
            std::wstring text = GetText(pw_edit_);
            const wchar_t* p = text.c_str();
            bool valid = true;
            for (;;) {
                wchar_t* end = nullptr;
                long v = wcstol(p, &end, 16);
                if (end == p) break;
                if (v < 0 || v > 0xFF) valid = false;
                password.push_back(static_cast<uint8_t>(v));
                p = end;
            }
            if (!valid || password.size() != 14) {
                SetWindowTextW(pw_info_, T(L"ต้องเป็นเลขฐาน 16 จำนวน 14 ไบต์", L"Enter 14 hexadecimal bytes"));
            } else {
                pw_waiting_ = true;
                ShowWindow(pw_edit_, SW_HIDE);
                ShowWindow(pw_ok_, SW_HIDE);
                SetWindowTextW(pw_info_, L"");
                SetWindowTextW(pw_msg_, T(L"ปิดกุญแจ (Turn Off ECU)", L"Turn Off ECU"));
                worker_->SendPassword(password);
            }
        } else if (id == ID_PW_CANCEL || id == IDCANCEL) {
            if (pw_waiting_ && worker_) worker_->CancelPassword();
            ClosePasswordDialog();
        }
        return TRUE;
    }
    if (msg == WM_CLOSE) {
        if (pw_waiting_ && worker_) worker_->CancelPassword();
        ClosePasswordDialog();
        return TRUE;
    }
    if (msg == WM_DESTROY) {
        pw_ = nullptr;
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK Panel::PasswordProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        Panel* self = reinterpret_cast<Panel*>(lp);
        SetWindowLongPtrW(h, DWLP_USER, lp);
        self->pw_ = h;
        RECT rc = {0, 0, self->S(470), self->S(190)};
        AdjustWindowRectEx(&rc, static_cast<DWORD>(GetWindowLongPtrW(h, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtrW(h, GWL_EXSTYLE)));
        RECT mw;
        GetWindowRect(self->hwnd_, &mw);
        const int w = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        SetWindowPos(h, nullptr, mw.left + (mw.right - mw.left - w) / 2, mw.top + (mw.bottom - mw.top - height) / 2, w, height, SWP_NOZORDER | SWP_NOACTIVATE);
        self->Make(h, L"STATIC", T(L"Password (ฐาน 16, 14 ไบต์):", L"Password (hex, 14 bytes):"), 0, 14, 12, 440, 20);
        self->pw_edit_ = self->Make(h, L"EDIT", L"48 65 6C 6C 6F 48 6F 77 41 72 65 59 6F 75", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 14, 34, 442, 25, ID_PW_EDIT);
        self->pw_info_ = self->Make(h, L"STATIC", L"", 0, 14, 62, 442, 20);
        self->pw_msg_ = self->Make(h, L"STATIC", L"", SS_CENTER, 14, 84, 442, 40);
        SendMessageW(self->pw_msg_, WM_SETFONT, reinterpret_cast<WPARAM>(self->font_big_), TRUE);
        self->pw_cancel_ = self->Make(h, L"BUTTON", T(L"ยกเลิก", L"Cancel"), WS_TABSTOP, 262, 142, 94, 32, ID_PW_CANCEL);
        self->pw_ok_ = self->Make(h, L"BUTTON", T(L"ตกลง", L"OK"), WS_TABSTOP | BS_DEFPUSHBUTTON, 362, 142, 94, 32, ID_PW_OK);
        return FALSE;
    }
    Panel* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(h, DWLP_USER));
    return self ? self->OnPasswordMessage(msg, wp, lp) : FALSE;
}

}  // namespace

void PanelShow(HINSTANCE instance, HWND owner, PanelCommand command) {
    Panel::Get(instance, owner)->Show(command);
}

bool PanelWriteInProgress() {
    return Panel::Current() && Panel::Current()->WriteInProgress();
}

void PanelShutdown() {
    Panel::Destroy();
}
