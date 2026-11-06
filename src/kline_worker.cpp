// TunerPro-HondaECU, GNU GPL v3. Based on HondaECU by Ryan Hope, changed in November 2026.
// Port of src/threads/kline.py. One deliberate change from the Python is marked in
// write_eeprom. The rest differs only where C++ needs it (locks, cancel of a read, and an
// "op.finished" event so the UI can re-enable its buttons on every exit path).
#include "kline_worker.h"

#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string KbText(double bytes, double rate) {
    char buf[64];
    if (rate > 0) {
        std::snprintf(buf, sizeof(buf), "%.02fKB @ %.02fB/s", bytes / 1024.0, rate);
    } else {
        std::snprintf(buf, sizeof(buf), "%.02fKB @ ---", bytes / 1024.0);
    }
    return buf;
}

}  // namespace

KlineWorker::KlineWorker(Sink sink) : sink_(std::move(sink)) {
    thread_ = std::thread([this] { run(); });
}

KlineWorker::~KlineWorker() {
    run_ = false;
    if (thread_.joinable()) thread_.join();
    cleanup();
}

void KlineWorker::SetLogSink(std::function<void(const std::string&)> log) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_ = std::move(log);
}

AdapterStats KlineWorker::Stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_snapshot_;
}

void KlineWorker::Activate(std::unique_ptr<KlineAdapter> adapter) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_adapter_ = std::move(adapter);
}

void KlineWorker::Deactivate() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_adapter_.reset();
    deactivate_requested_ = true;
}

void KlineWorker::SetUpdateTables(bool on) { update_tables_ = on; }
void KlineWorker::SetUpdateErrors(bool on) { update_errors_ = on; }
void KlineWorker::ClearCodes() { clear_codes_ = true; }

void KlineWorker::ReadPanel(const std::wstring& path, uint32_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    cancel_read_ = false;
    req_read_ = ReadInfo{path, offset};
}

void KlineWorker::WritePanel(const Bytes& data, uint32_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    req_write_ = WriteInfo{data, offset};
}

void KlineWorker::EepromRead(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    cancel_read_ = false;
    req_eeprom_ = EepromInfo{"read", path, {}, 0};
}

void KlineWorker::EepromWrite(const Bytes& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    req_eeprom_ = EepromInfo{"write", L"", data, 0};
}

void KlineWorker::EepromFormat(int mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    req_eeprom_ = EepromInfo{"format", L"", {}, mode};
}

void KlineWorker::SendPassword(const Bytes& password) {
    std::lock_guard<std::mutex> lock(mutex_);
    req_password_ = password;
}

void KlineWorker::CancelPassword() {
    std::lock_guard<std::mutex> lock(mutex_);
    req_password_.reset();
    cancel_password_ = true;
}

void KlineWorker::CancelRead() { cancel_read_ = true; }

// Runs between steps on the worker thread, so the adapter never vanishes mid-frame and a
// long read or write never holds a lock the TunerPro UI thread could be waiting on.
void KlineWorker::drain_requests() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deactivate_requested_) {
        deactivate_requested_ = false;
        cleanup();
    }
    if (pending_adapter_) {
        cleanup();
        ecu_ = std::make_unique<HondaECU>(std::move(pending_adapter_));
        ecu_->debug = log_;
        ready_ = true;
    }
    if (req_read_) {
        readinfo_ = std::move(req_read_);
        req_read_.reset();
    }
    if (req_write_) {
        writeinfo_ = std::move(req_write_);
        req_write_.reset();
    }
    if (req_eeprom_) {
        eeprominfo_ = std::move(req_eeprom_);
        req_eeprom_.reset();
    }
    if (cancel_password_.exchange(false)) sendpassword_ = false;
    if (req_password_) {
        // KlineWorker.PasswordHandler
        if (state_ != ECUSTATE::SECURE) {
            sendpassword_ = true;
            password_ = *req_password_;
        }
        req_password_.reset();
    }
    if (cancel_read_.exchange(false)) {
        // A cancel that arrives while the request is still waiting for its state.
        if (readinfo_) {
            readinfo_.reset();
            progress("read.progress", 0, "interrupted");
            emit_info("op.finished", 0);
        }
        if (eeprominfo_ && eeprominfo_->cmd == "read") {
            eeprominfo_.reset();
            progress("read_eeprom.progress", 0, "interrupted");
            emit_info("op.finished", 0);
        }
    }
    if (ecu_) stats_snapshot_ = ecu_->dev->stats;
}

void KlineWorker::emit(const WorkerEvent& event) {
    if (sink_) sink_(event);
}

void KlineWorker::emit_info(const char* info, int value) {
    WorkerEvent e;
    e.info = info;
    e.value = value;
    emit(e);
}

void KlineWorker::progress(const char* info, double percent, const std::string& text) {
    WorkerEvent e;
    e.info = info;
    e.percent = percent;
    e.text = text;
    emit(e);
}

void KlineWorker::cleanup() {
    ecu_.reset();
    clear_data();
}

void KlineWorker::clear_data() {
    ready_ = false;
    sendpassword_ = false;
    password_.clear();
    readinfo_.reset();
    writeinfo_.reset();
    eeprominfo_.reset();
    state_ = ECUSTATE::UNKNOWN;
    reset_state();
}

void KlineWorker::reset_state() {
    ecmid_.clear();
    errorcodes_.clear();
    clear_codes_ = false;
    flashcount_ = -1;
    dtccount_ = -1;
    tables_.clear();
    WorkerEvent ecmid;
    ecmid.info = "ecmid";
    emit(ecmid);
    emit_info("flashcount", flashcount_);
    emit_info("state", static_cast<int>(state_));
}

std::string KlineWorker::read_eeprom() {
    std::string ret = "good";
    const std::wstring binfile = eeprominfo_->path;
    Bytes rom;
    int offset = 0;
    while (offset <= 0xFF) {
        if (!run_ || cancel_read_) return "interrupted";
        Bytes data;
        if (ecu_->pgmfi_read_eeprom_word(static_cast<uint8_t>(offset), &data) && data.size() >= 2) {
            rom.push_back(data[0]);
            rom.push_back(data[1]);
            offset++;
            progress("read_eeprom.progress", offset / 256.0 * 100.0, "");
        } else {
            ret = "bad";
            break;
        }
    }
    if (ecu_->dev->kline()) {
        progress("read_eeprom.progress", offset / 256.0 * 100.0, "");
    } else {
        return "interrupted";
    }
    if (ret == "good") {
        size_t end = 512;
        if (std::equal(rom.begin(), rom.begin() + 256, rom.begin() + 256)) end = 256;
        FILE* f = _wfopen(binfile.c_str(), L"wb");
        if (!f) return "file error";
        fwrite(rom.data(), 1, end, f);
        fclose(f);
    }
    return ret;
}

std::string KlineWorker::read_flash() {
    int readsize = 12;
    const uint32_t offset = readinfo_->offset;
    uint32_t location = offset;
    const std::wstring binfile = readinfo_->path;
    FILE* fbin = _wfopen(binfile.c_str(), L"wb");
    if (!fbin) return "file error";
    double t = Now();
    uint32_t size = location;
    double rate = 0;
    for (;;) {
        if (!run_ || cancel_read_) {
            fclose(fbin);
            return "interrupted";
        }
        Bytes data;
        if (ecu_->pgmfi_read_flash_bytes(location, readsize, &data)) {
            fwrite(data.data(), 1, data.size(), fbin);
            fflush(fbin);
            location += readsize;
            const double n = Now();
            progress("read.progress", -1, KbText(location - offset, rate));
            if (n - t > 1) {
                rate = (location - size) / (n - t);
                t = n;
                size = location;
            }
        } else {
            readsize -= 1;
            if (readsize < 1) break;
        }
    }
    fclose(fbin);
    if (ecu_->dev->kline()) {
        progress("read.progress", -1, KbText(location - offset, rate));
    } else {
        return "interrupted";
    }
    fbin = _wfopen(binfile.c_str(), L"rb");
    std::string status = "bad";
    if (fbin) {
        Bytes byts;
        uint8_t buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), fbin)) > 0) byts.insert(byts.end(), buf, buf + got);
        fclose(fbin);
        if (!byts.empty()) status = ValidationName(do_validation(byts));
    }
    return status;
}

int KlineWorker::write_eeprom(const Bytes& byts) {
    const int maxi = static_cast<int>(byts.size() / 2);
    int i = 0;
    while (i < maxi) {
        // Changed from the Python, which passes byts[i:(i + 2)]. read_eeprom stores two bytes
        // per word, so word i lives at i*2; the Python slice would shift the whole image.
        const bool status = ecu_->pgmfi_write_eeprom_word(static_cast<uint8_t>(i), byts[i * 2], byts[i * 2 + 1]);
        progress("write_eeprom.progress", static_cast<double>(i) / maxi * 100.0, "");
        if (status) {
            i += 1;
        } else {
            progress("write_eeprom.progress", 0, "interrupted");
            return 1;
        }
    }
    return 0;
}

int KlineWorker::write_flash(const Bytes& byts, uint32_t offset) {
    const size_t ossize = byts.size();
    const int offseti = static_cast<int>(offset / 16);
    int writesize = 128;
    int z = writesize / 16;
    int maxi = static_cast<int>(ossize / writesize);
    int i = 0;
    size_t w = 0;
    double t = Now();
    double rate = 0;
    size_t size = 0;
    char text[96];
    while (i < maxi) {
        const int start = offseti + z * i;
        const int end = (i + 1 == maxi) ? 0 : offseti + z * (i + 1);
        Bytes x;
        x.push_back(static_cast<uint8_t>(start >> 8));
        x.push_back(static_cast<uint8_t>(start));
        x.insert(x.end(), byts.begin() + i * writesize, byts.begin() + (i + 1) * writesize);
        x.push_back(static_cast<uint8_t>(end >> 8));
        x.push_back(static_cast<uint8_t>(end));
        const uint8_t c1 = checksum8bit(x);
        const uint8_t c2 = checksum8bitHonda(x);
        Bytes frame = {0x01, 0x06};
        frame.insert(frame.end(), x.begin(), x.end());
        frame.push_back(c1);
        frame.push_back(c2);
        auto info = ecu_->send_command({0x7E}, frame);
        if (info) {
            const int ilen = info->rml;
            if (ilen == 7) {
                const Bytes& d = info->rdata;
                const int returned = d.size() >= 4 ? (d[2] << 8 | d[3]) : -1;
                if (returned != offseti + z * (i + 1)) {
                    progress("write.progress", 0, "unexpected return");
                    return 1;
                }
                ecu_->dev->stats.unneeded_retry++;
            }
        } else if (i == 0) {
            if (writesize == 128) {
                writesize = 64;
                z = writesize / 16;
                maxi = static_cast<int>(ossize / writesize);
                continue;
            }
            progress("write.progress", 0, "failed");
            return 2;
        } else {
            progress("write.progress", 0, "max retries");
            return 3;
        }
        const double n = Now();
        if (rate > 0) {
            std::snprintf(text, sizeof(text), "%.02fKB of %.02fKB @ %.02fB/s", w / 1024.0, ossize / 1024.0, rate);
        } else {
            std::snprintf(text, sizeof(text), "%.02fKB of %.02fKB @ ---", w / 1024.0, ossize / 1024.0);
        }
        progress("write.progress", static_cast<double>(i) / maxi * 100.0, text);
        if (n - t > 1) {
            rate = (w - size) / (n - t);
            t = n;
            size = w;
        }
        i += 1;
        if (i % 2 == 0 && writesize == 64) {
            ecu_->send_command({0x7E}, {0x01, 0x07});
            Sleep(200);
        }
        w = static_cast<size_t>(i) * writesize;
    }
    return 0;
}

void KlineWorker::do_init_write(bool recover) {
    do_update_state();
    if (recover) {
        ecu_->do_init_recover();
        ecu_->send_command({0x72}, {0x00, 0xF1});
        Sleep(1000);
        ecu_->send_command({0x27}, {0x00, 0x01, 0x00});
    } else {
        ecu_->do_init_write();
    }
    do_update_state();
    Sleep(100);
}

int KlineWorker::do_erase(int wait) {
    do_update_state();
    int ret = 1;
    emit_info("erase", 0);
    ecu_->get_write_status();
    int w = wait;
    char text[64];
    for (int i = 0; i < wait; ++i) {
        if (!run_) return 1;
        w = wait - i;
        std::snprintf(text, sizeof(text), "waiting for %d seconds", w);
        progress("write.progress", static_cast<double>(w) / wait * 100.0, text);
        Sleep(1000);
    }
    std::snprintf(text, sizeof(text), "waiting for %d seconds", w);
    progress("write.progress", 0, text);
    if (ecu_->do_erase()) {
        Sleep(2000);
        int e = 0;
        for (;;) {
            double pct = e / 35.0 * 100.0;
            progress("write.progress", pct > 100.0 ? 100.0 : pct, "erasing ecu");
            Sleep(100);
            e += 1;
            auto info = ecu_->send_command({0x7E}, {0x01, 0x05});
            if (info && info->rdata.size() > 1) {
                if (info->rdata[1] == 0x00) {
                    ecu_->get_write_status();
                    ret = 0;
                    break;
                }
                if (info->rdata[1] == 0xFA) {
                    progress("write.progress", 0, "erase block error");
                    ret = 2;
                    break;
                }
            } else {
                break;
            }
        }
    } else {
        progress("write.progress", 0, "erase failed");
    }
    do_update_state();
    return ret;
}

int KlineWorker::do_write() {
    do_update_state();
    int ret = 1;
    emit_info("write", 0);
    if (write_flash(writeinfo_->data, writeinfo_->offset) == 0) {
        WorkerEvent e;
        e.info = "write.result";
        e.text = ecu_->do_post_write() ? "good" : "bad";
        emit(e);
        ret = 0;
    }
    do_update_state();
    return ret;
}

int KlineWorker::do_read_eeprom() {
    do_update_state();
    int ret = 1;
    emit_info("read_eeprom", 0);
    const std::string result = read_eeprom();
    if (result == "interrupted") {
        progress("read_eeprom.progress", 0, "interrupted");
    } else {
        WorkerEvent e;
        e.info = "read_eeprom.result";
        e.text = result;
        emit(e);
        ret = 0;
    }
    do_update_state();
    return ret;
}

int KlineWorker::do_write_eeprom() {
    do_update_state();
    int ret = 1;
    emit_info("write_eeprom", 0);
    const int result = write_eeprom(eeprominfo_->data);
    WorkerEvent e;
    e.info = "write_eeprom.result";
    e.text = result == 0 ? "good" : "bad";
    emit(e);
    ret = 0;
    do_update_state();
    return ret;
}

int KlineWorker::do_format_eeprom(int mode) {
    do_update_state();
    emit_info("format_eeprom", 0);
    const bool status = mode == 1 ? ecu_->pgmfi_format_eeprom_FF() : ecu_->pgmfi_format_eeprom_00();
    emit_info("format_eeprom.result", status ? 1 : 0);
    do_update_state();
    return status ? 0 : 1;
}

int KlineWorker::do_read() {
    do_update_state();
    int ret = 1;
    emit_info("read", 0);
    const std::string result = read_flash();
    if (result == "interrupted") {
        progress("read.progress", 0, "interrupted");
    } else {
        WorkerEvent e;
        e.info = "read.result";
        e.text = result;
        emit(e);
        ret = 0;
    }
    do_update_state();
    return ret;
}

int KlineWorker::do_get_ecmid() {
    auto info = ecu_->send_command({0x72}, {0x71, 0x00});
    if (info && info->rdata.size() >= 7) {
        ecmid_.assign(info->rdata.begin() + 2, info->rdata.begin() + 7);
        WorkerEvent e;
        e.info = "ecmid";
        e.bytes = ecmid_;
        emit(e);
        return 0;
    }
    return 1;
}

int KlineWorker::do_get_flashcount() {
    auto info = ecu_->send_command({0x7D}, {0x01, 0x01, 0x03});
    if (info && info->rdata.size() > 4) {
        flashcount_ = info->rdata[4];
        emit_info("flashcount", flashcount_);
        return 0;
    }
    return 1;
}

int KlineWorker::do_clear_codes() {
    int ret = 1;
    while (clear_codes_) {
        auto info = ecu_->send_command({0x72}, {0x60, 0x03});
        if (info) {
            if (info->rdata.size() > 1 && info->rdata[1] == 0x00) {
                ret = 0;
                dtccount_ = -1;
                errorcodes_.clear();
                clear_codes_ = false;
            }
        } else {
            dtccount_ = -1;
            errorcodes_.clear();
            clear_codes_ = false;
        }
    }
    return ret;
}

int KlineWorker::do_get_dtcs() {
    std::map<std::string, std::vector<std::string>> errorcodes;
    const struct { uint8_t t; const char* key; } kinds[] = {{0x74, "0x74"}, {0x73, "0x73"}};
    for (const auto& kind : kinds) {
        auto& list = errorcodes[kind.key];
        for (uint8_t i = 1; i < 0x0C; ++i) {
            auto info = ecu_->send_command({0x72}, {kind.t, i});
            if (!info) return 1;
            const Bytes& d = info->rdata;
            for (size_t j : {3u, 5u, 7u}) {
                if (j + 1 < d.size() && d[j] != 0) {
                    char code[16];
                    std::snprintf(code, sizeof(code), "%02d-%02d", d[j], d[j + 1]);
                    list.push_back(code);
                }
            }
        }
    }
    int dtccount = 0;
    for (const auto& kv : errorcodes) dtccount += static_cast<int>(kv.second.size());
    if (dtccount_ != dtccount) {
        dtccount_ = dtccount;
        emit_info("dtccount", dtccount_);
    }
    if (errorcodes_ != errorcodes) {
        errorcodes_ = errorcodes;
        WorkerEvent e;
        e.info = "dtc";
        e.dtc = errorcodes_;
        emit(e);
    }
    return 0;
}

int KlineWorker::do_probe_tables() {
    auto tables = ecu_->probe_tables();
    if (!tables.empty()) {
        tables_ = tables;
        for (const auto& kv : tables_) {
            WorkerEvent e;
            e.info = "data";
            e.value = kv.first;
            e.dl = kv.second.first;
            e.bytes = kv.second.second;
            emit(e);
        }
        return 0;
    }
    return 1;
}

int KlineWorker::do_update_tables() {
    for (auto& kv : tables_) {
        auto info = ecu_->send_command({0x72}, {0x71, static_cast<uint8_t>(kv.first)});
        if (!info || info->rdl <= 2) return 1;
        kv.second = {info->rdl, info->rdata};
        WorkerEvent e;
        e.info = "data";
        e.value = kv.first;
        e.dl = info->rdl;
        e.bytes = info->rdata;
        emit(e);
    }
    return 0;
}

int KlineWorker::do_basic_tasks() {
    int ret = 0;
    if (ecmid_.empty()) ret += do_get_ecmid();
    return ret;
}

int KlineWorker::do_idle_tasks() {
    int ret = do_basic_tasks();
    if (flashcount_ < 0) ret += do_get_flashcount();
    if (clear_codes_) ret += do_clear_codes();
    if (update_errors_ || dtccount_ < 0) ret += do_get_dtcs();
    if (tables_.empty()) {
        ret += do_probe_tables();
    } else if (update_tables_) {
        ret += do_update_tables();
    }
    return ret;
}

void KlineWorker::do_update_state() {
    const ECUSTATE state = ecu_->detect_ecu_state();
    if (state != state_) {
        state_ = state;
        emit_info("state", static_cast<int>(state_));
        if (state_ == ECUSTATE::OFF) reset_state();
    }
}

int KlineWorker::do_password() {
    Bytes first = {0xE0};
    Bytes second = {0xE0};
    first.insert(first.end(), password_.begin(), password_.begin() + 7);
    second.insert(second.end(), password_.begin() + 7, password_.end());
    const bool p1 = ecu_->send_command({0x27}, first).has_value();
    const bool p2 = ecu_->send_command({0x27}, second).has_value();
    const bool passok = p1 && p2;
    emit_info("password", passok ? 1 : 0);
    return passok ? 0 : 1;
}

int KlineWorker::write_helper(bool init, bool recover) {
    int ret = 1;
    if (init) {
        do_init_write(recover);
        Sleep(100);
    }
    if (do_erase() == 0) {
        do_write();
        ret = 0;
    }
    writeinfo_.reset();
    emit_info("op.finished", 0);
    return ret;
}

int KlineWorker::read_helper() {
    const int ret = do_read();
    readinfo_.reset();
    emit_info("op.finished", 0);
    return ret;
}

int KlineWorker::read_eeprom_helper() {
    const int ret = do_read_eeprom();
    eeprominfo_.reset();
    emit_info("op.finished", 0);
    return ret;
}

int KlineWorker::write_eeprom_helper() {
    const int ret = do_write_eeprom();
    eeprominfo_.reset();
    emit_info("op.finished", 0);
    return ret;
}

int KlineWorker::format_eeprom_helper(int mode) {
    const int ret = do_format_eeprom(mode);
    eeprominfo_.reset();
    emit_info("op.finished", 0);
    return ret;
}

int KlineWorker::do_on_power() {
    int ret = 0;
    if (sendpassword_) {
        ret += do_password();
        sendpassword_ = false;
    }
    return ret;
}

int KlineWorker::do_secure() {
    int ret = 1;
    if (state_ == ECUSTATE::SECURE) {
        if (readinfo_) {
            ret = read_helper();
        } else if (eeprominfo_) {
            if (eeprominfo_->cmd == "read") {
                ret = read_eeprom_helper();
            } else if (eeprominfo_->cmd == "write") {
                ret = write_eeprom_helper();
            } else if (eeprominfo_->cmd == "format") {
                ret = format_eeprom_helper(eeprominfo_->mode);
            }
        }
    }
    return ret;
}

void KlineWorker::run() {
    timeBeginPeriod(1);  // the Sleep(1) polling in the adapters needs millisecond sleeps
    while (run_) {
        drain_requests();
        if (!ready_) {
            Sleep(2);
            continue;
        }
        try {
            if (state_ == ECUSTATE::UNKNOWN || state_ == ECUSTATE::OFF) {
                do_update_state();
                if (state_ == ECUSTATE::OK) {
                    do_on_power();
                    do_update_state();
                }
            } else if (state_ == ECUSTATE::SECURE) {
                do_secure();
                do_update_state();
            } else if (state_ == ECUSTATE::OK) {
                if (writeinfo_) {
                    write_helper(true);
                    do_update_state();
                } else if (do_idle_tasks() > 0) {
                    do_update_state();
                }
            } else if (state_ == ECUSTATE::RECOVER_OLD) {
                do_basic_tasks();
                if (writeinfo_) {
                    write_helper(true);
                    do_update_state();
                }
            } else if (state_ == ECUSTATE::RECOVER_NEW) {
                do_basic_tasks();
                if (writeinfo_) {
                    write_helper(true, true);
                    do_update_state();
                }
            } else if (state_ == ECUSTATE::FLASH) {
                if (writeinfo_) write_helper(false);
                do_update_state();
            }
            Sleep(2);  // the Python spins here; a plugin must not burn a CPU core
        } catch (const AdapterError& e) {
            WorkerEvent event;
            event.info = "device.lost";
            event.text = e.what();
            emit(event);
            cleanup();
        }
    }
    timeEndPeriod(1);
}
