// TunerPro-HondaECU, GNU GPL v3. Based on HondaECU by Ryan Hope, changed in November 2026.
// Port of src/threads/kline.py (KlineWorker). Method names follow the Python.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "honda_ecu.h"

// One "KlineWorker" dispatcher signal from the Python, delivered on the worker thread.
struct WorkerEvent {
    std::string info;           // same names as the Python: "state", "read.progress", ...
    int value = 0;              // state, flashcount, dtccount, password, table number
    int dl = 0;                 // "data": info[3]
    double percent = -1;        // progress, -1 means unknown
    std::string text;           // progress text or result ("good", "bad", ...)
    Bytes bytes;                // "ecmid" and "data" payload
    std::map<std::string, std::vector<std::string>> dtc;  // keys "0x74" current, "0x73" past
};

class KlineWorker {
public:
    using Sink = std::function<void(const WorkerEvent&)>;

    explicit KlineWorker(Sink sink);
    ~KlineWorker();

    // Handlers the Python registers on the dispatcher. All are safe from any thread.
    void Activate(std::unique_ptr<KlineAdapter> adapter);  // FTDIDevice "activate"
    void Deactivate();                                     // FTDIDevice "deactivate"
    void SetUpdateTables(bool on);                         // DatalogPanel data.on / data.off
    void SetUpdateErrors(bool on);                         // ErrorPanel dtc.on / dtc.off
    void ClearCodes();                                     // ErrorPanel dtc.clear
    void ReadPanel(const std::wstring& path, uint32_t offset);
    void WritePanel(const Bytes& data, uint32_t offset);
    void EepromRead(const std::wstring& path);
    void EepromWrite(const Bytes& data);
    void EepromFormat(int mode);
    void SendPassword(const Bytes& password);
    void CancelPassword();
    // Stops a pending or running read. Writes are not cancellable, as in HondaECU.
    void CancelRead();

    void SetLogSink(std::function<void(const std::string&)> log);
    AdapterStats Stats();

private:
    void run();
    void drain_requests();
    void cleanup();
    void clear_data();
    void reset_state();
    void emit(const WorkerEvent& event);
    void emit_info(const char* info, int value);
    void progress(const char* info, double percent, const std::string& text);

    int do_get_ecmid();
    int do_get_flashcount();
    int do_clear_codes();
    int do_get_dtcs();
    int do_probe_tables();
    int do_update_tables();
    int do_basic_tasks();
    int do_idle_tasks();
    void do_update_state();
    int do_password();
    int do_on_power();
    int do_secure();

    std::string read_eeprom();
    std::string read_flash();
    int write_eeprom(const Bytes& byts);
    int write_flash(const Bytes& byts, uint32_t offset);
    void do_init_write(bool recover);
    int do_erase(int wait = 11);
    int do_write();
    int do_read_eeprom();
    int do_write_eeprom();
    int do_format_eeprom(int mode);
    int do_read();
    int write_helper(bool init, bool recover = false);
    int read_helper();
    int read_eeprom_helper();
    int write_eeprom_helper();
    int format_eeprom_helper(int mode);

    Sink sink_;
    std::function<void(const std::string&)> log_;
    std::thread thread_;
    std::atomic<bool> run_{true};
    std::atomic<bool> cancel_read_{false};
    std::atomic<bool> cancel_password_{false};

    struct ReadInfo { std::wstring path; uint32_t offset; };
    struct WriteInfo { Bytes data; uint32_t offset; };
    struct EepromInfo { std::string cmd; std::wstring path; Bytes data; int mode; };

    // Requests from the UI thread, moved over by drain_requests().
    std::mutex mutex_;
    std::unique_ptr<KlineAdapter> pending_adapter_;
    bool deactivate_requested_ = false;
    std::optional<ReadInfo> req_read_;
    std::optional<WriteInfo> req_write_;
    std::optional<EepromInfo> req_eeprom_;
    std::optional<Bytes> req_password_;
    AdapterStats stats_snapshot_;

    // Everything below belongs to the worker thread.
    std::unique_ptr<HondaECU> ecu_;
    bool ready_ = false;
    bool sendpassword_ = false;
    Bytes password_;
    ECUSTATE state_ = ECUSTATE::UNKNOWN;
    std::optional<ReadInfo> readinfo_;
    std::optional<WriteInfo> writeinfo_;
    std::optional<EepromInfo> eeprominfo_;

    Bytes ecmid_;
    std::map<std::string, std::vector<std::string>> errorcodes_;
    std::atomic<bool> update_errors_{true};  // dtc.on: also the heartbeat that notices key-off
    std::atomic<bool> clear_codes_{false};
    int flashcount_ = -1;
    int dtccount_ = -1;
    std::atomic<bool> update_tables_{false};
    std::map<int, std::pair<int, Bytes>> tables_;
};
