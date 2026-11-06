// Port of eculib/base.py (KlineAdapter). The HondaECU code above this layer only
// sees write/read/purge/kline/wake, so FTDI and J2534 look identical to it.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

// Raised where eculib would raise USBError / FtdiError: the adapter is gone.
class AdapterError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct AdapterStats {
    unsigned retries = 0;
    unsigned checksum_errors = 0;
    unsigned unneeded_retry = 0;
};

class KlineAdapter {
public:
    // Defaults from HondaECU controlpanel.py (hondaecu.ini [DEFAULT]).
    int retries = 3;
    double timeout = 0.2;
    double kline_timeout = 0.1;
    double kline_wait = 0.002;
    int kline_testbytes = 1;
    AdapterStats stats;

    virtual ~KlineAdapter() = default;

    // Sends raw bytes. Returns false when the adapter accepted fewer bytes.
    virtual bool write(const Bytes& data) = 0;
    // Everything received so far, including the echo of what was sent.
    virtual Bytes read() = 0;
    virtual void purge_buffers() = 0;
    // True when the K-Line is alive. J2534 cannot sense this and always says true.
    virtual bool kline() = 0;
    // HondaECU.init(): K-Line low 70 ms, then idle high before talking.
    virtual void wake() = 0;
    // False for FTDI-style sensing adapters, true when OFF cannot be told from "no reply".
    virtual bool cannot_sense_kline() const { return false; }
};

struct AdapterInfo {
    enum Kind { kFtdi, kJ2534 };
    Kind kind = kFtdi;
    std::wstring label;
    // FTDI
    std::string ftdi_serial;
    int ftdi_index = -1;
    // J2534
    std::wstring j2534_library;
};

std::vector<AdapterInfo> ListFtdiAdapters();
std::vector<AdapterInfo> ListJ2534Adapters();

// Both throw AdapterError with a readable UTF-8 message when the device will not open.
std::unique_ptr<KlineAdapter> OpenFtdiAdapter(const AdapterInfo& info);
std::unique_ptr<KlineAdapter> OpenJ2534Adapter(const AdapterInfo& info);
