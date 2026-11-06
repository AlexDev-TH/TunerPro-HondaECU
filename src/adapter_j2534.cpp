// J2534 transport (Scanmatik SM2, Tactrix OpenPort 2.0, any registered PassThru device).
//
// HondaECU itself is FTDI only, so this class is the one new piece. It presents the same
// byte stream as FTDI: send() in honda_ecu.cpp reads back its own echo first, so the sent
// bytes are queued here as a synthetic echo and the hardware echo stays off (LOOPBACK=0).
#include "kline_adapter.h"

#include <cstddef>
#include <cstring>
#include <type_traits>

namespace {

struct PASSTHRU_MSG {
    unsigned long ProtocolID;
    unsigned long RxStatus;
    unsigned long TxFlags;
    unsigned long Timestamp;
    unsigned long DataSize;
    unsigned long ExtraDataIndex;
    unsigned char Data[4128];
};
struct SCONFIG {
    unsigned long Parameter;
    unsigned long Value;
};
struct SCONFIG_LIST {
    unsigned long NumOfParams;
    SCONFIG* ConfigPtr;
};

const unsigned long ISO9141 = 3;
const unsigned long CONNECT_ISO9141_NO_CHECKSUM = 0x00000200;
const unsigned long CONNECT_ISO9141_K_LINE_ONLY = 0x00001000;
const unsigned long PASS_FILTER = 1;

const unsigned long IOCTL_SET_CONFIG = 0x02;
const unsigned long IOCTL_FAST_INIT = 0x05;
const unsigned long IOCTL_CLEAR_TX_BUFFER = 0x07;
const unsigned long IOCTL_CLEAR_RX_BUFFER = 0x08;
const unsigned long IOCTL_CLEAR_MSG_FILTERS = 0x0A;

const unsigned long PARAM_LOOPBACK = 0x03;
const unsigned long PARAM_P1_MAX = 0x07;
const unsigned long PARAM_P3_MIN = 0x0A;
const unsigned long PARAM_P4_MIN = 0x0C;
const unsigned long PARAM_TINIL = 0x14;
const unsigned long PARAM_TWUP = 0x15;
const unsigned long PARAM_PARITY = 0x16;

const long STATUS_NOERROR = 0x00;
const long ERR_NOT_SUPPORTED = 0x01;
const long ERR_INVALID_IOCTL_ID = 0x0F;
const long ERR_TIMEOUT = 0x09;
const long ERR_BUFFER_EMPTY = 0x10;

const unsigned long RX_TX_MSG_TYPE = 0x01;
const unsigned long RX_START_OF_MESSAGE = 0x02;

typedef long(__stdcall* PassThruOpen_t)(void*, unsigned long*);
typedef long(__stdcall* PassThruClose_t)(unsigned long);
typedef long(__stdcall* PassThruConnect_t)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long*);
typedef long(__stdcall* PassThruDisconnect_t)(unsigned long);
typedef long(__stdcall* PassThruReadMsgs_t)(unsigned long, PASSTHRU_MSG*, unsigned long*, unsigned long);
typedef long(__stdcall* PassThruWriteMsgs_t)(unsigned long, PASSTHRU_MSG*, unsigned long*, unsigned long);
typedef long(__stdcall* PassThruStartMsgFilter_t)(unsigned long, unsigned long, PASSTHRU_MSG*, PASSTHRU_MSG*, PASSTHRU_MSG*, unsigned long*);
typedef long(__stdcall* PassThruIoctl_t)(unsigned long, unsigned long, void*, void*);
typedef long(__stdcall* PassThruGetLastError_t)(char*);

// Vendor drivers run in-process, so a crash inside one must not take TunerPro down.
long GuardedOpen(PassThruOpen_t fn, unsigned long* device, bool* crashed) {
    __try {
        return fn(nullptr, device);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *crashed = true;
        return -1;
    }
}

long GuardedConnect(PassThruConnect_t fn, unsigned long device, unsigned long flags, unsigned long baud, unsigned long* channel, bool* crashed) {
    __try {
        return fn(device, ISO9141, flags, baud, channel);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *crashed = true;
        return -1;
    }
}

class J2534Adapter : public KlineAdapter {
public:
    explicit J2534Adapter(const AdapterInfo& info) {
        lib_ = LoadLibraryExW(info.j2534_library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!lib_) throw AdapterError("cannot load the J2534 driver DLL, error " + std::to_string(GetLastError()));
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(lib_, name));
            ok = ok && fn;
        };
        get(open_, "PassThruOpen");
        get(close_, "PassThruClose");
        get(connect_, "PassThruConnect");
        get(disconnect_, "PassThruDisconnect");
        get(read_, "PassThruReadMsgs");
        get(write_, "PassThruWriteMsgs");
        get(start_filter_, "PassThruStartMsgFilter");
        get(ioctl_, "PassThruIoctl");
        get(last_error_, "PassThruGetLastError");
        if (!ok) throw AdapterError("the J2534 driver DLL does not export the PassThru 04.04 API");

        bool crashed = false;
        long st = GuardedOpen(open_, &device_, &crashed);
        if (crashed) throw AdapterError("PassThruOpen crashed inside the driver");
        if (st != STATUS_NOERROR) throw AdapterError("PassThruOpen failed: " + error_text(st) + " (device unplugged or in use?)");
        device_open_ = true;

        // K-Line only without driver checksum, because Honda frames carry their own checksum.
        const unsigned long flag_options[] = {CONNECT_ISO9141_K_LINE_ONLY | CONNECT_ISO9141_NO_CHECKSUM, CONNECT_ISO9141_NO_CHECKSUM};
        for (unsigned long flags : flag_options) {
            st = GuardedConnect(connect_, device_, flags, 10400, &channel_, &crashed);
            if (crashed) {
                close();
                throw AdapterError("PassThruConnect crashed inside the driver");
            }
            if (st == STATUS_NOERROR) {
                channel_open_ = true;
                break;
            }
        }
        if (!channel_open_) {
            std::string text = error_text(st);
            close();
            throw AdapterError("PassThruConnect(ISO9141, 10400) failed: " + text);
        }

        // One at a time, since some drivers reject a whole list for one unsupported entry.
        // P3_MIN and P4_MIN are zero so frames leave back to back as they do over FTDI.
        // P1_MAX (0.5 ms units) sets how soon a reply is handed over; a split reply still
        // works because read() returns a byte stream.
        set_config(PARAM_LOOPBACK, 0);
        set_config(PARAM_P1_MAX, 10);
        set_config(PARAM_P3_MIN, 0);
        set_config(PARAM_P4_MIN, 0);
        set_config(PARAM_PARITY, 0);

        ioctl_(channel_, IOCTL_CLEAR_MSG_FILTERS, nullptr, nullptr);
        PASSTHRU_MSG mask = make_msg(Bytes{0x00});
        PASSTHRU_MSG pattern = make_msg(Bytes{0x00});
        unsigned long filter = 0;
        st = start_filter_(channel_, PASS_FILTER, &mask, &pattern, nullptr, &filter);
        if (st != STATUS_NOERROR) {
            std::string text = error_text(st);
            close();
            throw AdapterError("PassThruStartMsgFilter failed: " + text);
        }
        purge_buffers();
    }

    ~J2534Adapter() override { close(); }

    bool cannot_sense_kline() const override { return true; }

    bool write(const Bytes& data) override {
        // A late reply from an earlier timed-out frame must not be taken for this frame's.
        ioctl_(channel_, IOCTL_CLEAR_RX_BUFFER, nullptr, nullptr);
        rx_queue_.clear();
        PASSTHRU_MSG msg = make_msg(data);
        unsigned long count = 1;
        long st = write_(channel_, &msg, &count, 1000 + static_cast<unsigned long>(data.size()) * 2);
        tx_since_wake_ = true;
        if (st == STATUS_NOERROR && count == 1) {
            rx_queue_.insert(rx_queue_.end(), data.begin(), data.end());
            return true;
        }
        if (st == ERR_TIMEOUT) return false;
        throw AdapterError("PassThruWriteMsgs failed: " + error_text(st));
    }

    Bytes read() override {
        Bytes out;
        out.swap(rx_queue_);
        unsigned long count = 1;
        long st = read_(channel_, &rx_msg_, &count, 2);
        if (st == STATUS_NOERROR) {
            if (count > 0 && !(rx_msg_.RxStatus & (RX_TX_MSG_TYPE | RX_START_OF_MESSAGE))) {
                size_t n = rx_msg_.DataSize > sizeof(rx_msg_.Data) ? sizeof(rx_msg_.Data) : rx_msg_.DataSize;
                out.insert(out.end(), rx_msg_.Data, rx_msg_.Data + n);
                if (n > 0) rx_since_wake_ = true;
            }
        } else if (st != ERR_BUFFER_EMPTY && st != ERR_TIMEOUT) {
            throw AdapterError("PassThruReadMsgs failed: " + error_text(st));
        }
        return out;
    }

    void purge_buffers() override {
        rx_queue_.clear();
        ioctl_(channel_, IOCTL_CLEAR_RX_BUFFER, nullptr, nullptr);
        ioctl_(channel_, IOCTL_CLEAR_TX_BUFFER, nullptr, nullptr);
    }

    // There is no way to watch the wire itself: the device's own K-Line monitoring would
    // answer even with the ECU off. The ECU state code treats "no reply" as the off case.
    bool kline() override { return true; }

    // HondaECU.init() needs K low 70 ms and quiet to 200 ms. J2534 has a standard way to do
    // that (FAST_INIT with TINIL/TWUP), but not every driver offers it on an ISO9141 channel
    // or honours the timing, so when a wake got no reply the next one uses the other method.
    void wake() override {
        if (tx_since_wake_ && !rx_since_wake_) use_burst_ = !use_burst_;
        tx_since_wake_ = false;
        rx_since_wake_ = false;
        if (fast_init_unavailable_) use_burst_ = true;
        if (!use_burst_ && !wake_fast_init()) {
            fast_init_unavailable_ = true;
            use_burst_ = true;
        }
        if (use_burst_) wake_zero_burst();
        rx_queue_.clear();
        ioctl_(channel_, IOCTL_CLEAR_RX_BUFFER, nullptr, nullptr);
    }

private:
    // False only when the driver says it cannot do this. No reply to the ping payload is
    // normal because the pulse has still gone out, and HondaECU.init() expects no reply.
    bool wake_fast_init() {
        set_config(PARAM_TINIL, 70);
        set_config(PARAM_TWUP, 200);
        PASSTHRU_MSG input = make_msg(Bytes{0xFE, 0x04, 0x72, 0x8C});  // HondaECU.ping()
        PASSTHRU_MSG output = make_msg(Bytes{});
        long st = ioctl_(channel_, IOCTL_FAST_INIT, &input, &output);
        if (st == ERR_NOT_SUPPORTED || st == ERR_INVALID_IOCTL_ID) return false;
        if (st == STATUS_NOERROR && output.DataSize > 0) rx_since_wake_ = true;
        return true;
    }

    // 80 zero bytes at 10400 baud hold K low for about 77 ms, leaving only short stop-bit
    // gaps, without changing the baud rate.
    void wake_zero_burst() {
        PASSTHRU_MSG msg = make_msg(Bytes(80, 0x00));
        unsigned long count = 1;
        write_(channel_, &msg, &count, 1000);
        Sleep(77 + 130);
    }

    PASSTHRU_MSG make_msg(const Bytes& data) {
        PASSTHRU_MSG msg;
        std::memset(&msg, 0, offsetof(PASSTHRU_MSG, Data));
        msg.ProtocolID = ISO9141;
        msg.DataSize = static_cast<unsigned long>(data.size());
        if (!data.empty()) std::memcpy(msg.Data, data.data(), data.size());
        return msg;
    }

    long set_config(unsigned long parameter, unsigned long value) {
        SCONFIG cfg = {parameter, value};
        SCONFIG_LIST list = {1, &cfg};
        return ioctl_(channel_, IOCTL_SET_CONFIG, &list, nullptr);
    }

    std::string error_text(long status) {
        char text[128] = {0};
        last_error_(text);
        std::string out = "status " + std::to_string(status);
        if (text[0]) out += std::string(" (") + text + ")";
        return out;
    }

    void close() {
        if (channel_open_) disconnect_(channel_);
        if (device_open_) close_(device_);
        channel_open_ = false;
        device_open_ = false;
        // The driver DLL stays loaded: several vendors crash when unloaded and reloaded.
    }

    HMODULE lib_ = nullptr;
    PassThruOpen_t open_ = nullptr;
    PassThruClose_t close_ = nullptr;
    PassThruConnect_t connect_ = nullptr;
    PassThruDisconnect_t disconnect_ = nullptr;
    PassThruReadMsgs_t read_ = nullptr;
    PassThruWriteMsgs_t write_ = nullptr;
    PassThruStartMsgFilter_t start_filter_ = nullptr;
    PassThruIoctl_t ioctl_ = nullptr;
    PassThruGetLastError_t last_error_ = nullptr;

    unsigned long device_ = 0;
    unsigned long channel_ = 0;
    bool device_open_ = false;
    bool channel_open_ = false;

    Bytes rx_queue_;
    PASSTHRU_MSG rx_msg_ = {};

    bool use_burst_ = false;
    bool fast_init_unavailable_ = false;
    bool tx_since_wake_ = false;
    bool rx_since_wake_ = false;
};

std::wstring RegString(HKEY key, const wchar_t* name) {
    wchar_t buf[512] = {0};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) != ERROR_SUCCESS) return L"";
    if (type != REG_SZ && type != REG_EXPAND_SZ) return L"";
    return buf;
}

DWORD RegDword(HKEY key, const wchar_t* name) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS) return 0;
    return type == REG_DWORD ? value : 0;
}

}  // namespace

std::vector<AdapterInfo> ListJ2534Adapters() {
    std::vector<AdapterInfo> out;
    HKEY root = nullptr;
    // This DLL is 32-bit, so WOW64 maps the path to the 32-bit driver registrations.
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\PassThruSupport.04.04", 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t sub[256];
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY dev = nullptr;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &dev) != ERROR_SUCCESS) continue;
        std::wstring library = RegString(dev, L"FunctionLibrary");
        std::wstring name = RegString(dev, L"Name");
        std::wstring vendor = RegString(dev, L"Vendor");
        bool iso9141 = RegDword(dev, L"ISO9141") != 0;
        RegCloseKey(dev);
        if (library.empty() || !iso9141) continue;
        AdapterInfo a;
        a.kind = AdapterInfo::kJ2534;
        a.j2534_library = library;
        a.label = L"J2534: ";
        if (!vendor.empty()) a.label += vendor + L" ";
        a.label += name.empty() ? std::wstring(sub) : name;
        out.push_back(a);
    }
    RegCloseKey(root);
    return out;
}

std::unique_ptr<KlineAdapter> OpenJ2534Adapter(const AdapterInfo& info) {
    return std::make_unique<J2534Adapter>(info);
}
