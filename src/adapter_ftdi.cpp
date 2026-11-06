// FTDI transport. Same line handling as eculib KlineAdapter / HondaECU.init(), with
// pyftdi replaced by the D2XX driver because TunerPro is a 32-bit Windows process.
#include "kline_adapter.h"

#include <chrono>
#include <cstring>
#include <type_traits>

namespace {

typedef void* FT_HANDLE;
typedef ULONG FT_STATUS;
const FT_STATUS FT_OK = 0;
const ULONG FT_PURGE_RX = 1;
const ULONG FT_PURGE_TX = 2;
const DWORD FT_OPEN_BY_SERIAL_NUMBER = 1;

struct D2xx {
    HMODULE lib = nullptr;
    FT_STATUS(WINAPI* CreateDeviceInfoList)(LPDWORD) = nullptr;
    FT_STATUS(WINAPI* GetDeviceInfoDetail)(DWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPVOID, LPVOID, FT_HANDLE*) = nullptr;
    FT_STATUS(WINAPI* Open)(int, FT_HANDLE*) = nullptr;
    FT_STATUS(WINAPI* OpenEx)(PVOID, DWORD, FT_HANDLE*) = nullptr;
    FT_STATUS(WINAPI* Close)(FT_HANDLE) = nullptr;
    FT_STATUS(WINAPI* ResetDevice)(FT_HANDLE) = nullptr;
    FT_STATUS(WINAPI* SetBaudRate)(FT_HANDLE, ULONG) = nullptr;
    FT_STATUS(WINAPI* SetDataCharacteristics)(FT_HANDLE, UCHAR, UCHAR, UCHAR) = nullptr;
    FT_STATUS(WINAPI* SetFlowControl)(FT_HANDLE, USHORT, UCHAR, UCHAR) = nullptr;
    FT_STATUS(WINAPI* SetLatencyTimer)(FT_HANDLE, UCHAR) = nullptr;
    FT_STATUS(WINAPI* SetTimeouts)(FT_HANDLE, ULONG, ULONG) = nullptr;
    FT_STATUS(WINAPI* SetBitMode)(FT_HANDLE, UCHAR, UCHAR) = nullptr;
    FT_STATUS(WINAPI* Write)(FT_HANDLE, LPVOID, DWORD, LPDWORD) = nullptr;
    FT_STATUS(WINAPI* Read)(FT_HANDLE, LPVOID, DWORD, LPDWORD) = nullptr;
    FT_STATUS(WINAPI* GetQueueStatus)(FT_HANDLE, DWORD*) = nullptr;
    FT_STATUS(WINAPI* Purge)(FT_HANDLE, ULONG) = nullptr;

    bool load() {
        if (lib) return true;
        HMODULE h = LoadLibraryW(L"ftd2xx.dll");
        if (!h) return false;
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(h, name));
            ok = ok && fn;
        };
        get(CreateDeviceInfoList, "FT_CreateDeviceInfoList");
        get(GetDeviceInfoDetail, "FT_GetDeviceInfoDetail");
        get(Open, "FT_Open");
        get(OpenEx, "FT_OpenEx");
        get(Close, "FT_Close");
        get(ResetDevice, "FT_ResetDevice");
        get(SetBaudRate, "FT_SetBaudRate");
        get(SetDataCharacteristics, "FT_SetDataCharacteristics");
        get(SetFlowControl, "FT_SetFlowControl");
        get(SetLatencyTimer, "FT_SetLatencyTimer");
        get(SetTimeouts, "FT_SetTimeouts");
        get(SetBitMode, "FT_SetBitMode");
        get(Write, "FT_Write");
        get(Read, "FT_Read");
        get(GetQueueStatus, "FT_GetQueueStatus");
        get(Purge, "FT_Purge");
        if (!ok) {
            FreeLibrary(h);
            return false;
        }
        lib = h;
        return true;
    }
};

D2xx& Api() {
    static D2xx api;
    return api;
}

std::wstring Widen(const char* s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, &out[0], n);
    return out;
}

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

class FtdiAdapter : public KlineAdapter {
public:
    explicit FtdiAdapter(const AdapterInfo& info) {
        D2xx& ft = Api();
        if (!ft.load()) throw AdapterError("ftd2xx.dll not found (install the FTDI CDM driver)");
        FT_STATUS st;
        if (!info.ftdi_serial.empty()) {
            st = ft.OpenEx(const_cast<char*>(info.ftdi_serial.c_str()), FT_OPEN_BY_SERIAL_NUMBER, &h_);
        } else {
            st = ft.Open(info.ftdi_index, &h_);
        }
        if (st != FT_OK) {
            h_ = nullptr;
            throw AdapterError("FT_Open failed, status " + std::to_string(st) + " (device in use by another program?)");
        }
        // KlineAdapter.__init__: open_from_device, set_baudrate(10400), set_line_property(8, 1, 'N')
        ft.ResetDevice(h_);
        // A cable pulled during wake() can come back still in bit-bang mode.
        check(ft.SetBitMode(h_, 0, 0), "FT_SetBitMode");
        check(ft.SetBaudRate(h_, 10400), "FT_SetBaudRate");
        check(ft.SetDataCharacteristics(h_, 8, 0, 0), "FT_SetDataCharacteristics");
        check(ft.SetFlowControl(h_, 0, 0, 0), "FT_SetFlowControl");
        // D2XX holds short packets for the latency timer, so keep it short.
        check(ft.SetLatencyTimer(h_, 2), "FT_SetLatencyTimer");
        check(ft.SetTimeouts(h_, 100, 1000), "FT_SetTimeouts");
        ft.Purge(h_, FT_PURGE_RX | FT_PURGE_TX);
    }

    ~FtdiAdapter() override {
        if (h_) Api().Close(h_);
    }

    bool write(const Bytes& data) override {
        DWORD written = 0;
        check(Api().Write(h_, const_cast<uint8_t*>(data.data()), static_cast<DWORD>(data.size()), &written), "FT_Write");
        return written == data.size();
    }

    Bytes read() override {
        DWORD queued = 0;
        check(Api().GetQueueStatus(h_, &queued), "FT_GetQueueStatus");
        if (queued == 0) {
            Sleep(1);
            return {};
        }
        Bytes out(queued);
        DWORD got = 0;
        check(Api().Read(h_, out.data(), queued, &got), "FT_Read");
        out.resize(got);
        return out;
    }

    void purge_buffers() override {
        check(Api().Purge(h_, FT_PURGE_RX | FT_PURGE_TX), "FT_Purge");
    }

    // KlineAdapter.kline_loopback_ping. pyftdi counted 2 modem status bytes in each read,
    // D2XX strips them, so the expected length is just the test bytes.
    bool kline() override {
        bool ret = false;
        const double start = Now();
        const Bytes msg(kline_testbytes, 0xFF);
        // kline_wait is 2 ms upstream, but D2XX cannot deliver the echo before the byte time
        // plus the latency timer, so give the echo a little longer to arrive.
        const double wait = kline_wait > 0.008 ? kline_wait : 0.008;
        for (;;) {
            purge_buffers();
            if (write(msg)) {
                const double t0 = Now();
                DWORD queued = 0;
                while (Now() - t0 < wait) {
                    check(Api().GetQueueStatus(h_, &queued), "FT_GetQueueStatus");
                    if (queued >= static_cast<DWORD>(kline_testbytes)) break;
                    Sleep(1);
                }
                if (read().size() == static_cast<size_t>(kline_testbytes)) {
                    ret = true;
                    break;
                }
            }
            if (Now() - start > kline_timeout) break;
        }
        purge_buffers();
        return ret;
    }

    // HondaECU.init()
    void wake() override {
        D2xx& ft = Api();
        check(ft.SetBitMode(h_, 1, 1), "FT_SetBitMode");
        write(Bytes{0x00});
        Sleep(70);
        write(Bytes{0x01});
        check(ft.SetBitMode(h_, 0, 0), "FT_SetBitMode");
        Sleep(200);
        read();
    }

private:
    void check(FT_STATUS st, const char* what) {
        if (st != FT_OK) throw AdapterError(std::string(what) + " failed, status " + std::to_string(st));
    }

    FT_HANDLE h_ = nullptr;
};

}  // namespace

std::vector<AdapterInfo> ListFtdiAdapters() {
    std::vector<AdapterInfo> out;
    D2xx& ft = Api();
    if (!ft.load()) return out;
    DWORD count = 0;
    if (ft.CreateDeviceInfoList(&count) != FT_OK) return out;
    for (DWORD i = 0; i < count; ++i) {
        DWORD flags = 0, type = 0, id = 0, loc = 0;
        char serial[64] = {0};
        char desc[128] = {0};
        FT_HANDLE h = nullptr;
        if (ft.GetDeviceInfoDetail(i, &flags, &type, &id, &loc, serial, desc, &h) != FT_OK) continue;
        AdapterInfo a;
        a.kind = AdapterInfo::kFtdi;
        a.ftdi_serial = serial;
        a.ftdi_index = static_cast<int>(i);
        a.label = L"FTDI: " + Widen(desc[0] ? desc : "USB Serial");
        if (serial[0]) a.label += L" (" + Widen(serial) + L")";
        out.push_back(a);
    }
    return out;
}

std::unique_ptr<KlineAdapter> OpenFtdiAdapter(const AdapterInfo& info) {
    return std::make_unique<FtdiAdapter>(info);
}
