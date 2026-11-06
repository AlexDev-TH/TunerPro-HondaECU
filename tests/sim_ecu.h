// A simulated ECU that behaves the way HondaECU's code expects a real one to: the same states,
// the same reply layouts its parsers index into. It proves the port and the transports agree
// with HondaECU's logic. It says nothing about real hardware timing.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using SimBytes = std::vector<uint8_t>;

class SimEcu {
public:
    enum Mode { kNormal, kSecure, kFlash, kRecoverOld, kRecoverNew };

    std::recursive_mutex mutex;
    bool powered = true;
    bool awake = false;
    Mode mode = kNormal;
    int ms_low_needed = 60;
    bool only64 = false;
    bool write_incomplete = false;
    bool recover_new_on_boot = false;
    int password_stage = 0;
    bool password_window = true;
    int erase_polls = 0;
    bool all_written = false;
    int post_steps = 0;
    int next_block = 0;
    int blocks_seen = 0;
    int blocks_rejected = 0;
    int block_bad_checksum = 0;
    int pulses = 0;
    int flashcount = 7;
    uint8_t id[5] = {0x01, 0x02, 0x13, 0x05, 0x01};
    SimBytes flash;
    SimBytes eeprom;
    std::vector<std::pair<int, int>> dtc_current = {{7, 1}, {12, 1}};
    std::vector<std::pair<int, int>> dtc_past = {{7, 1}, {54, 2}};
    std::vector<SimBytes> frames;  // every frame received while awake

    SimEcu() { restore_images(); }

    void restore_images() {
        flash.assign(16 * 1024 + 7, 0);
        for (size_t i = 0; i < flash.size(); ++i) flash[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
        fix_flash_checksum();
        eeprom.assign(512, 0);
        // Not periodic at 256, otherwise the two halves match and HondaECU keeps only one.
        for (size_t i = 0; i < eeprom.size(); ++i) eeprom[i] = static_cast<uint8_t>(i * 3 + 1 + (i >> 8) * 11);
    }

    void fix_flash_checksum() {
        unsigned sum = 0;
        for (size_t i = 1; i < flash.size(); ++i) sum += flash[i];
        flash[0] = static_cast<uint8_t>(((sum ^ 0xFF) + 1) & 0xFF);
    }

    void set_power(bool on) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        powered = on;
        awake = false;
        password_stage = 0;
        if (on) {
            password_window = true;
            if (write_incomplete) {
                mode = recover_new_on_boot ? kRecoverNew : kRecoverOld;
            } else {
                mode = kNormal;
            }
        }
    }

    void wake_pulse(double ms) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        pulses++;
        if (powered && ms >= ms_low_needed) awake = true;
    }

    static uint8_t cks(const SimBytes& b) {
        unsigned sum = 0;
        for (uint8_t x : b) sum += x;
        return static_cast<uint8_t>(((sum ^ 0xFF) + 1) & 0xFF);
    }

    static SimBytes reply1(uint8_t type, const SimBytes& data) {
        SimBytes r = {static_cast<uint8_t>(type & 0x0F), static_cast<uint8_t>(data.size() + 3)};
        r.insert(r.end(), data.begin(), data.end());
        r.push_back(cks(r));
        return r;
    }

    static SimBytes reply3(uint8_t sub, const SimBytes& data) {
        SimBytes r = {0x92, 0x92, sub, static_cast<uint8_t>(data.size() + 5)};
        r.insert(r.end(), data.begin(), data.end());
        r.push_back(cks(r));
        return r;
    }

    // The frame as sent, returns the reply bytes (empty for silence).
    SimBytes handle(const SimBytes& f) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!powered || !awake || f.size() < 3) return {};
        SimBytes body(f.begin(), f.end() - 1);
        if (cks(body) != f.back()) return {};
        frames.push_back(f);
        const bool triple = f.size() >= 5 && f[0] == 0x82 && f[1] == 0x82;
        if (triple) {
            if (f[3] != f.size()) return {};
            return handle_triple(f[2], SimBytes(f.begin() + 4, f.end() - 1));
        }
        if (f[1] != f.size()) return {};
        return handle_single(f[0], SimBytes(f.begin() + 2, f.end() - 1));
    }

private:
    SimBytes table(uint8_t t) {
        SimBytes d = {0x71, t};
        switch (t) {
            case 0x00:
                d.insert(d.end(), id, id + 5);
                if (mode == kRecoverOld || mode == kRecoverNew) {
                    d[5] = 0;
                    d[6] = 0;
                }
                break;
            case 0x11: {
                const uint8_t v[20] = {0x05, 0xDC, 0x20, 0x10, 0x60, 0x70, 0x60, 0x70, 0x60, 0x65, 0, 0, 125, 40, 0x10, 0x00, 0x90, 0x12, 0x20, 0x00};
                d.insert(d.end(), v, v + 20);
                break;
            }
            case 0x20: {
                const uint8_t v[3] = {0x40, 0x80, 0x20};
                d.insert(d.end(), v, v + 3);
                break;
            }
            default: break;
        }
        return d;
    }

    SimBytes handle_single(uint8_t type, const SimBytes& d) {
        if (type == 0xFE) return d.size() == 1 ? reply1(0xFE, d) : SimBytes{};
        switch (mode) {
            case kNormal: return handle_normal(type, d);
            case kFlash: return handle_flash(type, d);
            case kRecoverOld:
            case kRecoverNew: return handle_recover(type, d);
            case kSecure: return {};
        }
        return {};
    }

    SimBytes handle_normal(uint8_t type, const SimBytes& d) {
        if (type == 0x72 && d.size() == 2) {
            if (d[0] == 0x71) return reply1(0x72, table(d[1]));
            if (d[0] == 0x74 || d[0] == 0x73) {
                const auto& list = d[0] == 0x74 ? dtc_current : dtc_past;
                SimBytes r = {d[0], d[1], static_cast<uint8_t>(list.size()), 0, 0, 0, 0, 0, 0};
                if (d[1] == 1) {
                    for (size_t i = 0; i < list.size() && i < 3; ++i) {
                        r[3 + i * 2] = static_cast<uint8_t>(list[i].first);
                        r[4 + i * 2] = static_cast<uint8_t>(list[i].second);
                    }
                }
                return reply1(0x72, r);
            }
            if (d[0] == 0x60 && d[1] == 0x03) {
                dtc_current.clear();
                dtc_past.clear();
                return reply1(0x72, {0x60, 0x00});
            }
            if (d[0] == 0x00 && d[1] == 0xF0) return reply1(0x72, {0x00, 0xF0});
            return {};
        }
        if (type == 0x27 && d.size() == 8 && d[0] == 0xE0) {
            static const char first[] = "HelloHo";
            static const char second[] = "wAreYou";
            if (!password_window) return {};
            if (password_stage == 0 && !memcmp(&d[1], first, 7)) {
                password_stage = 1;
                return reply1(0x27, {0xE0, 'I', ' ', 'a', 'm'});
            }
            if (password_stage == 1 && !memcmp(&d[1], second, 7)) {
                mode = kSecure;
                return reply1(0x27, {0xE0, 'F', 'i', 'n', '.'});
            }
            return {};
        }
        if (type == 0x7D && d.size() >= 2 && d[0] == 0x01) {
            if (d[1] == 0x01 && d.size() == 3 && d[2] == 0x03) return reply1(0x7D, {0x01, 0x01, 0x03, 0x00, static_cast<uint8_t>(flashcount)});
            if (d[1] == 0x02 && d.size() == 5 && d[2] == 0x50 && d[3] == 0x47 && d[4] == 0x4D) return reply1(0x7D, {0x01, 0x02});
            if (d[1] == 0x03 && d.size() == 5 && d[2] == 0x2D && d[3] == 0x46 && d[4] == 0x49) {
                mode = kFlash;
                return reply1(0x7D, {0x01, 0x03});
            }
        }
        return {};
    }

    SimBytes handle_recover(uint8_t type, const SimBytes& d) {
        if (type == 0x72 && d.size() == 2 && d[0] == 0x71) return reply1(0x72, table(d[1]));
        if (mode == kRecoverOld) {
            if (type == 0x7D && d.size() >= 2 && d[0] == 0x01) {
                if (d[1] == 0x01 && d.size() == 3 && d[2] == 0x03) return reply1(0x7D, {0x01, 0x01, 0x03, 0x00, static_cast<uint8_t>(flashcount)});
                if (d[1] == 0x02 && d.size() == 5) return reply1(0x7D, {0x01, 0x02});
                if (d[1] == 0x03 && d.size() == 5) {
                    mode = kFlash;
                    return reply1(0x7D, {0x01, 0x03});
                }
            }
            return {};
        }
        if (type == 0x7B && d.size() >= 3 && d[0] == 0x00) {
            if (d[1] == 0x01 && d[2] == 0x04) return reply1(0x7B, {0x00, 0x01, 0x04});
            if (d[1] == 0x02 && d.size() == 5 && d[2] == 0x76 && d[3] == 0x03 && d[4] == 0x17) return reply1(0x7B, {0x00, 0x02});
            if (d[1] == 0x03 && d.size() == 5 && d[2] == 0x75 && d[3] == 0x05 && d[4] == 0x13) return reply1(0x7B, {0x00, 0x03});
        }
        if (type == 0x72 && d.size() == 2 && d[0] == 0x00 && d[1] == 0xF1) return reply1(0x72, {0x00, 0xF1});
        if (type == 0x27 && d.size() == 3 && d[0] == 0x00 && d[1] == 0x01 && d[2] == 0x00) {
            mode = kFlash;
            return reply1(0x27, {0x00, 0x01});
        }
        return {};
    }

    SimBytes handle_flash(uint8_t type, const SimBytes& d) {
        if (type != 0x7E || d.size() < 2 || d[0] != 0x01) return {};
        switch (d[1]) {
            case 0x01: return reply1(0x7E, {0x01, static_cast<uint8_t>(post_steps >= 4 ? 0x0F : 0x00)});
            case 0x02:
            case 0x03:
            case 0x0B:
            case 0x0E: return reply1(0x7E, {d[1], 0x00});
            case 0x04:
                erase_polls = 3;
                write_incomplete = true;
                next_block = 0;
                all_written = false;
                post_steps = 0;
                return reply1(0x7E, {0x04, 0x00});
            case 0x05:
                if (erase_polls > 0) {
                    erase_polls--;
                    return reply1(0x7E, {0x05, 0x01});
                }
                std::fill(flash.begin(), flash.end(), 0xFF);
                return reply1(0x7E, {0x05, 0x00});
            case 0x06: return handle_block(d);
            case 0x07: return reply1(0x7E, {0x07, 0x00});
            case 0x08:
            case 0x09:
            case 0x0A:
            case 0x0C:
                post_steps++;
                return reply1(0x7E, {d[1], 0x00});
            case 0x0D:
                if (all_written && post_steps >= 4) write_incomplete = false;
                return reply1(0x7E, {0x0D, static_cast<uint8_t>(all_written && post_steps >= 4 ? 0x0F : 0x00)});
            default: return {};
        }
    }

    SimBytes handle_block(const SimBytes& d) {
        // 01 06 start(2) data... end(2) c1 c2
        if (d.size() < 8) return {};
        const size_t datalen = d.size() - 8;
        if (datalen != 128 && datalen != 64) return {};
        blocks_seen++;
        if (only64 && datalen != 64) {
            blocks_rejected++;
            return {};
        }
        SimBytes x(d.begin() + 2, d.end() - 2);
        int sum = 0;
        for (uint8_t b : x) sum += b;
        const uint8_t c1 = static_cast<uint8_t>(0xFF - ((sum - 1) >> 8));
        const uint8_t c2 = cks(x);
        if (d[d.size() - 2] != c1 || d[d.size() - 1] != c2) {
            block_bad_checksum++;
            return {};
        }
        const int start = d[2] << 8 | d[3];
        const int end = d[d.size() - 4] << 8 | d[d.size() - 3];
        const int z = static_cast<int>(datalen / 16);
        if (start != next_block) return {};
        const size_t at = static_cast<size_t>(start) * 16;
        if (at + datalen > flash.size()) flash.resize(at + datalen, 0xFF);
        memcpy(&flash[at], &d[4], datalen);
        next_block = start + z;
        if (end == 0) all_written = true;
        return reply1(0x7E, {0x06, 0x00});
    }

    SimBytes handle_triple(uint8_t sub, const SimBytes& d) {
        if (mode != kSecure) return {};
        switch (sub) {
            case 0x00: {
                if (d.size() != 4 || d[3] < 1 || d[3] > 12) return {};
                const size_t location = (size_t(d[0]) << 16) | d[1] | (size_t(d[2]) << 8);
                if (location + d[3] > flash.size()) return {};
                return reply3(sub, SimBytes(flash.begin() + location, flash.begin() + location + d[3]));
            }
            case 0x10:
                if (d.size() != 1) return {};
                return reply3(sub, {eeprom[d[0] * 2], eeprom[d[0] * 2 + 1]});
            case 0x14:
                if (d.size() != 3) return {};
                eeprom[d[0] * 2] = d[1];
                eeprom[d[0] * 2 + 1] = d[2];
                return reply3(sub, {});
            case 0x18:
                std::fill(eeprom.begin(), eeprom.end(), 0xFF);
                return reply3(sub, {});
            case 0x19:
                std::fill(eeprom.begin(), eeprom.end(), 0x00);
                return reply3(sub, {});
            default: return {};
        }
    }
};

inline SimEcu& Sim() {
    static SimEcu sim;
    return sim;
}

// Test control surface, exported by both fake driver DLLs.
#define SIM_EXPORTS                                                                                              \
    extern "C" __declspec(dllexport) void Sim_SetPower(int on) { Sim().set_power(on != 0); }                     \
    extern "C" __declspec(dllexport) void Sim_Reset() {                                                          \
        std::lock_guard<std::recursive_mutex> lock(Sim().mutex);                                                 \
        Sim().restore_images();                                                                                  \
        Sim().mode = SimEcu::kNormal;                                                                            \
        Sim().awake = false;                                                                                     \
        Sim().powered = true;                                                                                    \
        Sim().frames.clear();                                                                                    \
        Sim().write_incomplete = false;                                                                          \
        Sim().recover_new_on_boot = false;                                                                       \
        Sim().only64 = false;                                                                                    \
        Sim().pulses = 0;                                                                                        \
        Sim().blocks_rejected = 0;                                                                               \
        Sim().block_bad_checksum = 0;                                                                            \
        Sim().dtc_current = {{7, 1}, {12, 1}};                                                                   \
        Sim().dtc_past = {{7, 1}, {54, 2}};                                                                      \
    }                                                                                                            \
    extern "C" __declspec(dllexport) void Sim_SetOnly64(int on) { Sim().only64 = on != 0; }                      \
    extern "C" __declspec(dllexport) void Sim_SetIncomplete(int incomplete, int recover_new) {                   \
        Sim().write_incomplete = incomplete != 0;                                                                \
        Sim().recover_new_on_boot = recover_new != 0;                                                            \
    }                                                                                                            \
    extern "C" __declspec(dllexport) int Sim_GetFlash(uint8_t* out, int cap) {                                   \
        std::lock_guard<std::recursive_mutex> lock(Sim().mutex);                                                 \
        int n = static_cast<int>(Sim().flash.size());                                                            \
        if (out && cap >= n) memcpy(out, Sim().flash.data(), n);                                                 \
        return n;                                                                                                \
    }                                                                                                            \
    extern "C" __declspec(dllexport) int Sim_GetEeprom(uint8_t* out, int cap) {                                  \
        std::lock_guard<std::recursive_mutex> lock(Sim().mutex);                                                 \
        int n = static_cast<int>(Sim().eeprom.size());                                                           \
        if (out && cap >= n) memcpy(out, Sim().eeprom.data(), n);                                                \
        return n;                                                                                                \
    }                                                                                                            \
    extern "C" __declspec(dllexport) void Sim_SetEeprom(const uint8_t* in, int n) {                              \
        std::lock_guard<std::recursive_mutex> lock(Sim().mutex);                                                 \
        Sim().eeprom.assign(in, in + n);                                                                         \
    }                                                                                                            \
    extern "C" __declspec(dllexport) int Sim_FrameCount() {                                                      \
        std::lock_guard<std::recursive_mutex> lock(Sim().mutex);                                                 \
        return static_cast<int>(Sim().frames.size());                                                            \
    }                                                                                                            \
    extern "C" __declspec(dllexport) int Sim_GetFrame(int index, uint8_t* out, int cap) {                        \
        std::lock_guard<std::recursive_mutex> lock(Sim().mutex);                                                 \
        if (index < 0 || index >= static_cast<int>(Sim().frames.size())) return 0;                               \
        const SimBytes& f = Sim().frames[index];                                                                 \
        int n = static_cast<int>(f.size());                                                                      \
        if (cap >= n) memcpy(out, f.data(), n);                                                                  \
        return n;                                                                                                \
    }                                                                                                            \
    extern "C" __declspec(dllexport) void Sim_ClearFrames() {                                                    \
        std::lock_guard<std::recursive_mutex> lock(Sim().mutex);                                                 \
        Sim().frames.clear();                                                                                    \
    }                                                                                                            \
    extern "C" __declspec(dllexport) int Sim_Pulses() { return Sim().pulses; }                                   \
    extern "C" __declspec(dllexport) int Sim_BlocksRejected() { return Sim().blocks_rejected; }                  \
    extern "C" __declspec(dllexport) int Sim_BadBlockChecksums() { return Sim().block_bad_checksum; }
