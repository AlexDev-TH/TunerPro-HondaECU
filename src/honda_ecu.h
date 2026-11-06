// TunerPro-HondaECU, GNU GPL v3. Based on HondaECU by Ryan Hope, changed in November 2026.
// Port of eculib/honda.py. Function names follow the Python so the two can be compared.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "kline_adapter.h"

enum class ECUSTATE { OFF, UNKNOWN, OK, FLASH, SECURE, RECOVER_OLD, RECOVER_NEW };

const char* EcuStateName(ECUSTATE state);
// honda.DTC. Returns nullptr for a code that is not in the table.
const char* DtcDescription(const std::string& code);

uint8_t checksum8bitHonda(const uint8_t* data, size_t size);
inline uint8_t checksum8bitHonda(const Bytes& data) { return checksum8bitHonda(data.data(), data.size()); }
uint8_t checksum8bit(const Bytes& data);
Bytes format_read(uint32_t location);
// Builds mtype + [msgsize] + data + [checksum]; ml and dl come back through the pointers.
Bytes format_message(const Bytes& mtype, const Bytes& data, int* ml = nullptr, int* dl = nullptr);

enum class Validation { kGood, kFixed, kBad };
// honda.do_validation. cksum < 0 means "do not fix". May patch byts[cksum].
Validation do_validation(Bytes& byts, long cksum = -1);
const char* ValidationName(Validation v);

struct CommandReply {
    Bytes rmtype;
    uint8_t rml = 0;  // whole-frame length byte
    Bytes rdata;
    int rdl = 0;
};

class HondaECU {
public:
    explicit HondaECU(std::unique_ptr<KlineAdapter> adapter) : dev(std::move(adapter)) {}

    std::unique_ptr<KlineAdapter> dev;
    std::function<void(const std::string&)> debug;

    void init();
    std::optional<Bytes> send(const Bytes& buf, int ml);
    std::optional<CommandReply> send_command(const Bytes& mtype, const Bytes& data = {});
    bool ping(uint8_t mode = 0x72);
    ECUSTATE detect_ecu_state();
    std::map<int, std::pair<int, Bytes>> probe_tables();
    void do_init_recover();
    void do_init_write();
    std::optional<int> get_write_status();
    bool do_erase();
    bool do_post_write();
    bool pgmfi_read_flash_bytes(uint32_t location, int size, Bytes* out);
    bool pgmfi_read_eeprom_word(uint8_t location, Bytes* out);
    bool pgmfi_write_eeprom_word(uint8_t location, uint8_t d0, uint8_t d1);
    bool pgmfi_format_eeprom_FF();
    bool pgmfi_format_eeprom_00();
};
