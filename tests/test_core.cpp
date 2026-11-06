// End-to-end test of the port: KlineWorker -> HondaECU -> real adapter classes -> fake driver
// DLL -> simulated ECU. Run through build_tests.bat.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../src/kline_worker.h"

struct GoldenFrame {
    std::vector<uint8_t> mtype, data, frame;
};
struct GoldenRead {
    uint32_t location;
    std::vector<uint8_t> bytes;
};
struct GoldenChecksum {
    std::vector<uint8_t> data;
    uint8_t honda, bit8;
};
struct GoldenValidation {
    std::vector<uint8_t> image;
    long cksum;
    const char* status;
    std::vector<uint8_t> fixed;
};
#include "golden.inc"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("  FAIL  %s\n", what.c_str());
    }
}

std::string Hex(const std::vector<uint8_t>& d) {
    std::string out;
    char buf[4];
    for (uint8_t b : d) {
        snprintf(buf, sizeof(buf), "%02X ", b);
        out += buf;
    }
    return out;
}

struct Driver {
    HMODULE lib = nullptr;
    void (*SetPower)(int) = nullptr;
    void (*Reset)() = nullptr;
    void (*SetOnly64)(int) = nullptr;
    void (*SetIncomplete)(int, int) = nullptr;
    int (*GetFlash)(uint8_t*, int) = nullptr;
    int (*GetEeprom)(uint8_t*, int) = nullptr;
    void (*SetEeprom)(const uint8_t*, int) = nullptr;
    int (*FrameCount)() = nullptr;
    int (*GetFrame)(int, uint8_t*, int) = nullptr;
    void (*ClearFrames)() = nullptr;
    int (*Pulses)() = nullptr;
    int (*BlocksRejected)() = nullptr;
    int (*BadBlockChecksums)() = nullptr;
    void (*FailDevice)(int) = nullptr;
    // J2534 only
    void (*SetFastInitMode)(int) = nullptr;
    void (*SetRejectKLineOnly)(int) = nullptr;
    void (*SetSplitReplies)(int) = nullptr;
    unsigned long (*GetConfig)(unsigned long) = nullptr;
    unsigned long (*GetConnectFlags)() = nullptr;
    int (*DeviceOpen)() = nullptr;

    void load(const wchar_t* name) {
        lib = LoadLibraryW(name);
        if (!lib) {
            wprintf(L"cannot load %ls\n", name);
            exit(2);
        }
        auto get = [&](auto& fn, const char* n) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(lib, n)); };
        get(SetPower, "Sim_SetPower");
        get(Reset, "Sim_Reset");
        get(SetOnly64, "Sim_SetOnly64");
        get(SetIncomplete, "Sim_SetIncomplete");
        get(GetFlash, "Sim_GetFlash");
        get(GetEeprom, "Sim_GetEeprom");
        get(SetEeprom, "Sim_SetEeprom");
        get(FrameCount, "Sim_FrameCount");
        get(GetFrame, "Sim_GetFrame");
        get(ClearFrames, "Sim_ClearFrames");
        get(Pulses, "Sim_Pulses");
        get(BlocksRejected, "Sim_BlocksRejected");
        get(BadBlockChecksums, "Sim_BadBlockChecksums");
        get(FailDevice, "Sim_FailDevice");
        get(SetFastInitMode, "Sim_SetFastInitMode");
        get(SetRejectKLineOnly, "Sim_SetRejectKLineOnly");
        get(SetSplitReplies, "Sim_SetSplitReplies");
        get(GetConfig, "Sim_GetConfig");
        get(GetConnectFlags, "Sim_GetConnectFlags");
        get(DeviceOpen, "Sim_DeviceOpen");
    }

    std::vector<uint8_t> flash() {
        std::vector<uint8_t> out(GetFlash(nullptr, 0));
        GetFlash(out.data(), static_cast<int>(out.size()));
        return out;
    }
    std::vector<uint8_t> eeprom() {
        std::vector<uint8_t> out(GetEeprom(nullptr, 0));
        GetEeprom(out.data(), static_cast<int>(out.size()));
        return out;
    }
};

std::set<std::vector<uint8_t>> g_all_frames;

void CollectFrames(Driver& d) {
    const int n = d.FrameCount();
    uint8_t buf[512];
    for (int i = 0; i < n; ++i) {
        const int size = d.GetFrame(i, buf, sizeof(buf));
        if (size > 0 && size <= static_cast<int>(sizeof(buf))) g_all_frames.insert(std::vector<uint8_t>(buf, buf + size));
    }
}

class Harness {
public:
    Harness()
        : worker_([this](const WorkerEvent& e) {
              std::lock_guard<std::mutex> lock(mutex_);
              events_.push_back(e);
              if (e.info == "state") state_ = static_cast<ECUSTATE>(e.value);
          }) {}

    KlineWorker& worker() { return worker_; }

    template <typename Pred>
    bool WaitFor(Pred pred, double seconds) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < end) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pred()) return true;
            }
            Sleep(10);
        }
        return false;
    }

    bool WaitState(ECUSTATE s, double seconds) {
        return WaitFor([&] { return state_ == s; }, seconds);
    }

    // Waits for the next event of this name since the last Mark().
    bool WaitEvent(const char* info, double seconds, WorkerEvent* out = nullptr) {
        size_t found = 0;
        const bool ok = WaitFor([&] {
            for (size_t i = mark_; i < events_.size(); ++i) {
                if (events_[i].info == info) {
                    found = i;
                    return true;
                }
            }
            return false;
        }, seconds);
        if (ok && out) {
            std::lock_guard<std::mutex> lock(mutex_);
            *out = events_[found];
        }
        return ok;
    }

    void Mark() {
        std::lock_guard<std::mutex> lock(mutex_);
        mark_ = events_.size();
    }

    ECUSTATE state() {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    std::vector<WorkerEvent> Events() {
        std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

private:
    std::mutex mutex_;
    std::vector<WorkerEvent> events_;
    size_t mark_ = 0;
    ECUSTATE state_ = ECUSTATE::UNKNOWN;
    KlineWorker worker_;
};

std::wstring TempFile(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"hondaecu_test_" + name;
}

std::vector<uint8_t> ReadAll(const std::wstring& path) {
    std::vector<uint8_t> out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
}

std::vector<uint8_t> WriteImage(size_t size, int seed) {
    std::vector<uint8_t> image(size);
    for (size_t i = 0; i < size; ++i) image[i] = static_cast<uint8_t>(i * 13 + seed + (i >> 5));
    unsigned sum = 0;
    for (size_t i = 1; i < size; ++i) sum += image[i];
    image[0] = static_cast<uint8_t>(((sum ^ 0xFF) + 1) & 0xFF);
    return image;
}

void TestGolden() {
    printf("golden values from the HondaECU Python source\n");
    for (const auto& g : kGoldenFrames) {
        Check(format_message(g.mtype, g.data) == g.frame, "format_message " + Hex(g.frame));
    }
    for (const auto& g : kGoldenReads) {
        Check(format_read(g.location) == g.bytes, "format_read " + std::to_string(g.location));
    }
    for (const auto& g : kGoldenChecksums) {
        Check(checksum8bitHonda(g.data) == g.honda, "checksum8bitHonda");
        Check(checksum8bit(g.data) == g.bit8, "checksum8bit");
    }
    for (const auto& g : kGoldenValidations) {
        Bytes image = g.image;
        const Validation v = do_validation(image, g.cksum);
        Check(std::string(ValidationName(v)) == g.status, "do_validation status cksum=" + std::to_string(g.cksum));
        Check(image == g.fixed, "do_validation bytes cksum=" + std::to_string(g.cksum));
    }
}

// Every constant command the C++ sent must be one HondaECU's source sends.
void TestFramesAgainstSource() {
    printf("frames on the wire vs literal send_command() calls in the Python\n");
    std::set<std::vector<uint8_t>> known;
    for (const auto& g : kGoldenFrames) known.insert(g.frame);
    int constant = 0;
    int dynamic = 0;
    for (const auto& f : g_all_frames) {
        const bool triple = f.size() > 4 && f[0] == 0x82 && f[1] == 0x82;
        bool is_dynamic = false;
        if (triple && (f[2] == 0x00 || f[2] == 0x10 || f[2] == 0x14)) is_dynamic = true;   // address + data
        if (f[0] == 0x7E && f.size() > 3 && f[2] == 0x01 && f[3] == 0x06) is_dynamic = true;  // flash block
        if (f[0] == 0x72 && f.size() > 3 && (f[2] == 0x71 || f[2] == 0x74 || f[2] == 0x73)) is_dynamic = true;  // table / page
        if (f[0] == 0x27 && f.size() > 2 && f[2] == 0xE0) is_dynamic = true;               // password
        if (is_dynamic) {
            ++dynamic;
            continue;
        }
        ++constant;
        Check(known.count(f) > 0, "frame not in HondaECU source: " + Hex(f));
    }
    printf("  %d constant frames matched, %d address/data frames\n", constant, dynamic);
    for (const auto& g : kGoldenFrames) {
        if (!g_all_frames.count(g.frame)) printf("  note: never exercised: %s\n", Hex(g.frame).c_str());
    }
}

std::unique_ptr<KlineAdapter> OpenFtdi() {
    auto list = ListFtdiAdapters();
    if (list.empty()) return nullptr;
    return OpenFtdiAdapter(list[0]);
}

std::unique_ptr<KlineAdapter> OpenJ2534() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    path = path.substr(0, path.find_last_of(L'\\') + 1) + L"fake_j2534.dll";
    AdapterInfo info;
    info.kind = AdapterInfo::kJ2534;
    info.j2534_library = path;
    return OpenJ2534Adapter(info);
}

void Security(Harness& h, Driver& d) {
    h.Mark();
    h.worker().SendPassword({0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x48, 0x6F, 0x77, 0x41, 0x72, 0x65, 0x59, 0x6F, 0x75});
    d.SetPower(0);
    Check(h.WaitState(ECUSTATE::OFF, 12), "ECU off is seen after key off");
    d.SetPower(1);
    Check(h.WaitState(ECUSTATE::SECURE, 15), "state SECURE after key on + password");
    WorkerEvent e;
    Check(h.WaitEvent("password", 2, &e) && e.value == 1, "password accepted");
}

void PowerCycleToOk(Harness& h, Driver& d) {
    d.SetPower(0);
    Check(h.WaitState(ECUSTATE::OFF, 12), "off after power cycle");
    d.SetPower(1);
    Check(h.WaitState(ECUSTATE::OK, 15), "OK after power cycle");
}

void RunIdle(Harness& h, Driver& d) {
    (void)d;
    h.worker().SetUpdateTables(true);
    WorkerEvent e;
    bool got_id = false;
    for (int i = 0; i < 300 && !got_id; ++i) {
        for (const auto& ev : h.Events()) {
            if (ev.info == "ecmid" && ev.bytes.size() == 5) got_id = true;
        }
        Sleep(20);
    }
    Check(got_id, "ECM ID reported");
    bool id_ok = false;
    for (const auto& ev : h.Events()) {
        if (ev.info == "ecmid" && ev.bytes == Bytes{0x01, 0x02, 0x13, 0x05, 0x01}) id_ok = true;
    }
    Check(id_ok, "ECM ID bytes are 01 02 13 05 01");
    Check(h.WaitEvent("flashcount", 3, &e), "flash count event");
    bool count_ok = false;
    for (int i = 0; i < 300 && !count_ok; ++i) {
        for (const auto& ev : h.Events()) {
            if (ev.info == "flashcount" && ev.value == 7) count_ok = true;
        }
        Sleep(20);
    }
    Check(count_ok, "flash count is 7");

    // DTC: current 07-01 12-01, past 07-01 54-02
    bool dtc_ok = false;
    for (int i = 0; i < 300 && !dtc_ok; ++i) {
        for (const auto& ev : h.Events()) {
            if (ev.info == "dtc") {
                auto cur = ev.dtc.find("0x74");
                auto past = ev.dtc.find("0x73");
                dtc_ok = cur != ev.dtc.end() && past != ev.dtc.end() &&
                         cur->second == std::vector<std::string>{"07-01", "12-01"} &&
                         past->second == std::vector<std::string>{"07-01", "54-02"};
            }
        }
        Sleep(20);
    }
    Check(dtc_ok, "DTC lists match");
    Check(std::string(DtcDescription("54-02")) == "Bank angle sensor circuit high voltage", "DTC text");

    bool table11 = false, table20 = false;
    for (int i = 0; i < 300 && !(table11 && table20); ++i) {
        for (const auto& ev : h.Events()) {
            if (ev.info == "data" && ev.value == 0x11 && ev.bytes.size() == 22) table11 = true;
            if (ev.info == "data" && ev.value == 0x20 && ev.bytes.size() == 5) table20 = true;
        }
        Sleep(20);
    }
    Check(table11, "data table 0x11");
    Check(table20, "data table 0x20");
    h.worker().SetUpdateTables(false);

    h.Mark();
    h.worker().ClearCodes();
    bool cleared = false;
    for (int i = 0; i < 400 && !cleared; ++i) {
        for (const auto& ev : h.Events()) {
            if (ev.info == "dtccount" && ev.value == 0) cleared = true;
        }
        Sleep(20);
    }
    Check(cleared, "clear codes brings the count to 0");
}

void RunSecure(Harness& h, Driver& d) {
    Security(h, d);

    const std::wstring flash_path = TempFile(L"flash.bin");
    h.Mark();
    h.worker().ReadPanel(flash_path, 0);
    WorkerEvent e;
    Check(h.WaitEvent("read.result", 60, &e), "read flash finishes");
    Check(e.text == "good", "read result is good, got " + e.text);
    Check(ReadAll(flash_path) == d.flash(), "read file equals ECU flash, including the short tail");

    const std::wstring ee_path = TempFile(L"eeprom.bin");
    h.Mark();
    h.worker().EepromRead(ee_path);
    Check(h.WaitEvent("read_eeprom.result", 30, &e), "read EEPROM finishes");
    Check(e.text == "good", "read EEPROM good, got " + e.text);
    Check(ReadAll(ee_path) == d.eeprom(), "EEPROM file equals ECU (512 bytes)");

    std::vector<uint8_t> mirror(512);
    for (size_t i = 0; i < 512; ++i) mirror[i] = static_cast<uint8_t>((i % 256) * 5 + 9);
    d.SetEeprom(mirror.data(), 512);
    h.Mark();
    h.worker().EepromRead(ee_path);
    Check(h.WaitEvent("read_eeprom.result", 30, &e) && e.text == "good", "read mirrored EEPROM");
    Check(ReadAll(ee_path) == std::vector<uint8_t>(mirror.begin(), mirror.begin() + 256), "mirrored EEPROM is cut to 256 bytes");

    // The byts[i*2] correction: every word must land at its own address.
    std::vector<uint8_t> pattern(512);
    for (size_t i = 0; i < 512; ++i) pattern[i] = static_cast<uint8_t>(i ^ (i >> 3) ^ 0x5A);
    h.Mark();
    h.worker().EepromWrite(pattern);
    Check(h.WaitEvent("write_eeprom.result", 30, &e) && e.text == "good", "write EEPROM");
    Check(d.eeprom() == pattern, "written EEPROM equals the file byte for byte");

    h.Mark();
    h.worker().EepromFormat(1);
    Check(h.WaitEvent("format_eeprom.result", 10, &e) && e.value == 1, "format FF");
    Check(d.eeprom() == std::vector<uint8_t>(512, 0xFF), "EEPROM all FF");
    h.Mark();
    h.worker().EepromFormat(0);
    Check(h.WaitEvent("format_eeprom.result", 10, &e) && e.value == 1, "format 00");
    Check(d.eeprom() == std::vector<uint8_t>(512, 0x00), "EEPROM all 00");

    // Cancel a read part way. Wait out the previous operation first, or its finish event
    // would be mistaken for this one.
    Check(h.WaitEvent("op.finished", 10), "format finishes");
    Sleep(1500);
    h.Mark();
    h.worker().ReadPanel(flash_path, 0);
    Sleep(1500);
    h.worker().CancelRead();
    bool interrupted = false;
    for (int i = 0; i < 500 && !interrupted; ++i) {
        for (const auto& ev : h.Events()) {
            if (ev.info == "read.progress" && ev.text == "interrupted") interrupted = true;
        }
        Sleep(20);
    }
    Check(interrupted, "cancelled read reports interrupted");
    Sleep(1500);
}

void RunWrite(Harness& h, Driver& d, size_t size, int seed) {
    const std::vector<uint8_t> image = WriteImage(size, seed);
    h.Mark();
    h.worker().WritePanel(image, 0);
    WorkerEvent e;
    Check(h.WaitEvent("write.result", 120, &e), "write flash finishes");
    Check(e.text == "good", "write result good, got " + e.text);
    Check(h.WaitEvent("op.finished", 20), "write op.finished");
    const std::vector<uint8_t> flash = d.flash();
    Check(flash.size() >= image.size() && std::equal(image.begin(), image.end(), flash.begin()), "ECU flash equals the written image");
    Check(d.BadBlockChecksums() == 0, "every block's c1/c2 accepted");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const auto t0 = std::chrono::steady_clock::now();
    TestGolden();

    Driver ftdi, j2534;
    ftdi.load(L"ftd2xx.dll");
    j2534.load(L"fake_j2534.dll");

    printf("FTDI: idle, security, read, EEPROM, write\n");
    {
        ftdi.Reset();
        Harness h;
        h.worker().Activate(OpenFtdi());
        Check(h.WaitState(ECUSTATE::OK, 10), "FTDI reaches OK");
        RunIdle(h, ftdi);
        RunSecure(h, ftdi);
        PowerCycleToOk(h, ftdi);
        RunWrite(h, ftdi, 8192, 1);
        Check(h.state() == ECUSTATE::FLASH, "FLASH state right after the write");
        PowerCycleToOk(h, ftdi);
        CollectFrames(ftdi);
    }

    printf("FTDI: cable unplugged mid-session\n");
    {
        ftdi.Reset();
        Harness h;
        h.worker().Activate(OpenFtdi());
        Check(h.WaitState(ECUSTATE::OK, 10), "FTDI reaches OK");
        h.Mark();
        ftdi.FailDevice(1);
        Check(h.WaitEvent("device.lost", 10), "lost device is reported, not a crash");
        ftdi.FailDevice(0);
    }

    printf("FTDI: recovery boot modes\n");
    {
        ftdi.Reset();
        ftdi.SetIncomplete(1, 0);
        ftdi.SetPower(1);
        Harness h;
        h.worker().Activate(OpenFtdi());
        Check(h.WaitState(ECUSTATE::RECOVER_OLD, 10), "RECOVER_OLD detected");
        RunWrite(h, ftdi, 4096, 2);
        CollectFrames(ftdi);
    }
    {
        ftdi.Reset();
        ftdi.SetIncomplete(1, 1);
        ftdi.SetPower(1);
        Harness h;
        h.worker().Activate(OpenFtdi());
        Check(h.WaitState(ECUSTATE::RECOVER_NEW, 10), "RECOVER_NEW detected");
        RunWrite(h, ftdi, 4096, 3);
        CollectFrames(ftdi);
    }

    printf("J2534: FAST_INIT works\n");
    {
        j2534.Reset();
        j2534.SetFastInitMode(0);
        j2534.SetRejectKLineOnly(0);
        j2534.SetSplitReplies(0);
        Harness h;
        h.worker().Activate(OpenJ2534());
        Check(h.WaitState(ECUSTATE::OK, 10), "J2534 reaches OK");
        Check(j2534.GetConnectFlags() == 0x1200, "connected K-Line only, no driver checksum");
        Check(j2534.GetConfig(0x03) == 0, "LOOPBACK off");
        Check(j2534.GetConfig(0x0A) == 0, "P3_MIN 0");
        Check(j2534.GetConfig(0x0C) == 0, "P4_MIN 0");
        Check(j2534.GetConfig(0x16) == 0, "PARITY none");
        Check(j2534.GetConfig(0x14) == 70, "TINIL 70 ms, as HondaECU.init()");
        Check(j2534.GetConfig(0x15) == 200, "TWUP 200 ms, as HondaECU.init()");
        RunIdle(h, j2534);
        RunSecure(h, j2534);
        PowerCycleToOk(h, j2534);
        RunWrite(h, j2534, 8192, 4);
        PowerCycleToOk(h, j2534);
        CollectFrames(j2534);
        h.worker().Deactivate();
        Check(h.WaitFor([&] { return j2534.DeviceOpen() == 0; }, 5), "disconnect closes the J2534 device");
    }

    printf("J2534: driver has no FAST_INIT on ISO9141, K-Line-only flag refused, ECU takes 64-byte blocks\n");
    {
        j2534.Reset();
        j2534.SetFastInitMode(1);
        j2534.SetRejectKLineOnly(1);
        j2534.SetSplitReplies(1);
        j2534.SetOnly64(1);
        Harness h;
        h.worker().Activate(OpenJ2534());
        Check(h.WaitState(ECUSTATE::OK, 15), "reaches OK through the zero burst");
        Check(j2534.GetConnectFlags() == 0x0200, "fell back to flags 0x200");
        RunWrite(h, j2534, 8192, 5);
        Check(j2534.BlocksRejected() > 0, "128-byte block was refused first, so the 64-byte path ran");
        CollectFrames(j2534);
    }

    printf("J2534: driver reports FAST_INIT success but sends no pulse\n");
    {
        j2534.Reset();
        j2534.SetFastInitMode(2);
        j2534.SetRejectKLineOnly(0);
        j2534.SetSplitReplies(0);
        Harness h;
        h.worker().Activate(OpenJ2534());
        Check(h.WaitState(ECUSTATE::OK, 25), "falls over to the zero burst and reaches OK");
        RunSecure(h, j2534);
        CollectFrames(j2534);
    }

    TestFramesAgainstSource();

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("\n%d checks, %d failures, %.0f s\n", g_checks, g_failures, seconds);
    return g_failures ? 1 : 0;
}
