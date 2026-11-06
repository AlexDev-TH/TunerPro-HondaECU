// A stand-in ftd2xx.dll wired to the simulated ECU. Echoes every byte while the K-Line is up,
// exactly as a half-duplex K-Line cable does, and times the 70 ms bit-bang pulse.
#include <chrono>
#include <deque>

#include "sim_ecu.h"

SIM_EXPORTS

namespace {
std::mutex g_mutex;
std::deque<uint8_t> g_rx;
bool g_open = false;
int g_bitmode = 0;
std::chrono::steady_clock::time_point g_low_start;
bool g_low = false;
bool g_fail = false;
}  // namespace

extern "C" __declspec(dllexport) void Sim_FailDevice(int fail) { g_fail = fail != 0; }

extern "C" {
#define FTAPI __declspec(dllexport) ULONG WINAPI

FTAPI FT_CreateDeviceInfoList(DWORD* count) {
    *count = 1;
    return 0;
}
FTAPI FT_GetDeviceInfoDetail(DWORD, DWORD* flags, DWORD* type, DWORD* id, DWORD* loc, char* serial, char* desc, void** h) {
    *flags = 0;
    *type = 5;
    *id = 0x04036001;
    *loc = 1;
    strcpy(serial, "SIM00001");
    strcpy(desc, "Simulated FT232R");
    *h = nullptr;
    return 0;
}
FTAPI FT_Open(int, void** h) {
    g_open = true;
    *h = reinterpret_cast<void*>(0x1234);
    return 0;
}
FTAPI FT_OpenEx(void*, DWORD, void** h) {
    g_open = true;
    *h = reinterpret_cast<void*>(0x1234);
    return 0;
}
FTAPI FT_Close(void*) {
    g_open = false;
    return 0;
}
FTAPI FT_ResetDevice(void*) { return 0; }
FTAPI FT_SetBaudRate(void*, ULONG baud) { return baud == 10400 ? 0 : 6; }
FTAPI FT_SetDataCharacteristics(void*, UCHAR bits, UCHAR stop, UCHAR parity) { return bits == 8 && stop == 0 && parity == 0 ? 0 : 6; }
FTAPI FT_SetFlowControl(void*, USHORT, UCHAR, UCHAR) { return 0; }
FTAPI FT_SetLatencyTimer(void*, UCHAR) { return 0; }
FTAPI FT_SetTimeouts(void*, ULONG, ULONG) { return 0; }
FTAPI FT_SetBitMode(void*, UCHAR mask, UCHAR mode) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bitmode = mode;
    if (mode == 0) g_low = false;
    (void)mask;
    return 0;
}
FTAPI FT_Write(void*, void* data, DWORD size, DWORD* written) {
    if (g_fail) return 4;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    *written = size;
    if (g_bitmode == 1) {
        for (DWORD i = 0; i < size; ++i) {
            if (p[i] == 0 && !g_low) {
                g_low = true;
                g_low_start = std::chrono::steady_clock::now();
            } else if (p[i] == 1 && g_low) {
                g_low = false;
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_low_start).count();
                Sim().wake_pulse(ms);
            }
        }
        return 0;
    }
    if (!Sim().powered) return 0;  // a dead K-Line gives no echo
    g_rx.insert(g_rx.end(), p, p + size);
    SimBytes frame(p, p + size);
    SimBytes reply = Sim().handle(frame);
    g_rx.insert(g_rx.end(), reply.begin(), reply.end());
    return 0;
}
FTAPI FT_Read(void*, void* out, DWORD want, DWORD* got) {
    if (g_fail) return 4;
    std::lock_guard<std::mutex> lock(g_mutex);
    DWORD n = 0;
    uint8_t* p = static_cast<uint8_t*>(out);
    while (n < want && !g_rx.empty()) {
        p[n++] = g_rx.front();
        g_rx.pop_front();
    }
    *got = n;
    return 0;
}
FTAPI FT_GetQueueStatus(void*, DWORD* queued) {
    if (g_fail) return 4;
    std::lock_guard<std::mutex> lock(g_mutex);
    *queued = static_cast<DWORD>(g_rx.size());
    return 0;
}
FTAPI FT_Purge(void*, ULONG) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_rx.clear();
    return 0;
}
}
