// A stand-in J2534 04.04 driver wired to the simulated ECU. It checks what a real driver
// would care about (ISO9141, no driver checksum, a filter before any RX, loopback off) and
// can act like the driver differences the adapter must survive.
#include <cstddef>
#include <cstdio>
#include <deque>
#include <map>

#include "sim_ecu.h"

SIM_EXPORTS

namespace {

struct PASSTHRU_MSG {
    unsigned long ProtocolID, RxStatus, TxFlags, Timestamp, DataSize, ExtraDataIndex;
    unsigned char Data[4128];
};
struct SCONFIG {
    unsigned long Parameter, Value;
};
struct SCONFIG_LIST {
    unsigned long NumOfParams;
    SCONFIG* ConfigPtr;
};

std::mutex g_mutex;
std::deque<SimBytes> g_rx;
bool g_device_open = false;
bool g_channel_open = false;
bool g_filter = false;
unsigned long g_flags = 0;
std::map<unsigned long, unsigned long> g_config;
int g_fast_init_mode = 0;  // 0 works, 1 ERR_NOT_SUPPORTED, 2 reports success but sends no pulse
bool g_reject_k_line_only = false;
bool g_split_replies = false;
bool g_fail = false;
int g_config_errors = 0;
std::string g_last_error;

long Err(long code, const char* text) {
    g_last_error = text;
    return code;
}

void QueueReply(const SimBytes& reply) {
    if (reply.empty()) return;
    if (g_split_replies && reply.size() > 3) {
        g_rx.push_back(SimBytes(reply.begin(), reply.begin() + 2));
        g_rx.push_back(SimBytes(reply.begin() + 2, reply.end()));
    } else {
        g_rx.push_back(reply);
    }
}

}  // namespace

extern "C" {
__declspec(dllexport) void Sim_SetFastInitMode(int mode) { g_fast_init_mode = mode; }
__declspec(dllexport) void Sim_SetRejectKLineOnly(int on) { g_reject_k_line_only = on != 0; }
__declspec(dllexport) void Sim_SetSplitReplies(int on) { g_split_replies = on != 0; }
__declspec(dllexport) void Sim_FailDevice(int fail) { g_fail = fail != 0; }
__declspec(dllexport) unsigned long Sim_GetConfig(unsigned long parameter) { return g_config.count(parameter) ? g_config[parameter] : 0xFFFFFFFF; }
__declspec(dllexport) unsigned long Sim_GetConnectFlags() { return g_flags; }
__declspec(dllexport) int Sim_DeviceOpen() { return g_device_open ? 1 : 0; }

__declspec(dllexport) long __stdcall PassThruOpen(void*, unsigned long* device) {
    if (g_device_open) return Err(0x0E, "in use");
    g_device_open = true;
    *device = 1;
    return 0;
}
__declspec(dllexport) long __stdcall PassThruClose(unsigned long) {
    g_device_open = false;
    return 0;
}
__declspec(dllexport) long __stdcall PassThruConnect(unsigned long, unsigned long protocol, unsigned long flags, unsigned long baud, unsigned long* channel) {
    if (protocol != 3) return Err(0x03, "wrong protocol");
    if (baud != 10400) return Err(0x10, "wrong baud");
    if (g_reject_k_line_only && (flags & 0x1000)) return Err(0x06, "invalid flags");
    g_flags = flags;
    g_channel_open = true;
    g_filter = false;
    g_config.clear();
    g_rx.clear();
    *channel = 7;
    return 0;
}
__declspec(dllexport) long __stdcall PassThruDisconnect(unsigned long) {
    g_channel_open = false;
    return 0;
}
__declspec(dllexport) long __stdcall PassThruReadMsgs(unsigned long, PASSTHRU_MSG* msg, unsigned long* count, unsigned long timeout) {
    if (g_fail) return Err(0x08, "device not connected");
    // When hosted inside TunerPro for a UI test, the key switch is a file holding 0 or 1.
    static DWORD last_poll = 0;
    if (GetTickCount() - last_poll > 100) {
        last_poll = GetTickCount();
        char path[MAX_PATH];
        if (GetEnvironmentVariableA("HONDAECU_SIM_POWER_FILE", path, MAX_PATH)) {
            if (FILE* f = fopen(path, "rb")) {
                const int c = fgetc(f);
                fclose(f);
                const bool on = c == '1';
                if ((c == '0' || c == '1') && on != Sim().powered) Sim().set_power(on);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_rx.empty() && g_filter) {
            const SimBytes data = g_rx.front();
            g_rx.pop_front();
            memset(msg, 0, offsetof(PASSTHRU_MSG, Data));
            msg->ProtocolID = 3;
            msg->DataSize = static_cast<unsigned long>(data.size());
            memcpy(msg->Data, data.data(), data.size());
            *count = 1;
            return 0;
        }
    }
    Sleep(timeout);
    *count = 0;
    return 0x10;  // ERR_BUFFER_EMPTY
}
__declspec(dllexport) long __stdcall PassThruWriteMsgs(unsigned long, PASSTHRU_MSG* msg, unsigned long* count, unsigned long) {
    if (g_fail) return Err(0x08, "device not connected");
    std::lock_guard<std::mutex> lock(g_mutex);
    SimBytes frame(msg->Data, msg->Data + msg->DataSize);
    *count = 1;
    bool all_zero = frame.size() >= 40;
    for (uint8_t b : frame) all_zero = all_zero && b == 0;
    if (all_zero) {
        // Bytes at 10400 baud: about 0.96 ms each, about 90 % low.
        Sim().wake_pulse(frame.size() * 0.96);
        return 0;
    }
    QueueReply(Sim().handle(frame));
    return 0;
}
__declspec(dllexport) long __stdcall PassThruStartMsgFilter(unsigned long, unsigned long type, PASSTHRU_MSG*, PASSTHRU_MSG*, PASSTHRU_MSG* flow, unsigned long* id) {
    if (type != 1 || flow) return Err(0x07, "bad filter");
    g_filter = true;
    *id = 1;
    return 0;
}
__declspec(dllexport) long __stdcall PassThruIoctl(unsigned long, unsigned long id, void* in, void* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    switch (id) {
        case 0x02: {
            auto* list = static_cast<SCONFIG_LIST*>(in);
            for (unsigned long i = 0; i < list->NumOfParams; ++i) g_config[list->ConfigPtr[i].Parameter] = list->ConfigPtr[i].Value;
            return 0;
        }
        case 0x05: {
            if (g_fast_init_mode == 1) return Err(0x01, "not supported");
            if (g_fast_init_mode == 2) return 0;  // says yes, does nothing
            auto* input = static_cast<PASSTHRU_MSG*>(in);
            auto* output = static_cast<PASSTHRU_MSG*>(out);
            const double low = g_config.count(0x14) ? g_config[0x14] : 25;
            Sleep(200);
            Sim().wake_pulse(low);
            SimBytes frame(input->Data, input->Data + input->DataSize);
            SimBytes reply = Sim().handle(frame);
            if (reply.empty()) return Err(0x09, "timeout");
            output->DataSize = static_cast<unsigned long>(reply.size());
            memcpy(output->Data, reply.data(), reply.size());
            return 0;
        }
        case 0x07: return 0;
        case 0x08:
            g_rx.clear();
            return 0;
        case 0x0A:
            g_filter = false;
            return 0;
        default: return Err(0x0F, "bad ioctl");
    }
}
__declspec(dllexport) long __stdcall PassThruGetLastError(char* text) {
    strcpy(text, g_last_error.c_str());
    return 0;
}
}
