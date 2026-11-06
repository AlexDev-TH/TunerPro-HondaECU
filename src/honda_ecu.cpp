// TunerPro-HondaECU, GNU GPL v3. Based on HondaECU by Ryan Hope, changed in November 2026.
// Port of eculib/honda.py.
//
// The eculib copy shipped with HondaECU 3.3.0 is decompiled and the nesting of send_command
// and detect_ecu_state came out scrambled. The checks and their order below are the same as
// in that file, put back into the only structure in which every branch is reachable.
#include "honda_ecu.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string Hex(const Bytes& data) {
    std::string out;
    char buf[4];
    for (uint8_t b : data) {
        std::snprintf(buf, sizeof(buf), "%02x ", b);
        out += buf;
    }
    if (!out.empty()) out.pop_back();
    return out;
}

const struct { const char* code; const char* text; } kDtc[] = {
    {"01-01", "MAP sensor circuit low voltage"},
    {"01-02", "MAP sensor circuit high voltage"},
    {"02-01", "MAP sensor performance problem"},
    {"07-01", "ECT sensor circuit low voltage"},
    {"07-02", "ECT sensor circuit high voltage"},
    {"08-01", "TP sensor circuit low voltage"},
    {"08-02", "TP sensor circuit high voltage"},
    {"09-01", "IAT sensor circuit low voltage"},
    {"09-02", "IAT sensor circuit high voltage"},
    {"11-01", "VS sensor no signal"},
    {"12-01", "No.1 primary injector circuit malfunction"},
    {"13-01", "No.2 primary injector circuit malfunction"},
    {"14-01", "No.3 primary injector circuit malfunction"},
    {"15-01", "No.4 primary injector circuit malfunction"},
    {"16-01", "No.1 secondary injector circuit malfunction"},
    {"17-01", "No.2 secondary injector circuit malfunction"},
    {"18-01", "CMP sensor no signal"},
    {"19-01", "CKP sensor no signal"},
    {"21-01", "O2 sensor low voltage"},
    {"21-02", "O2 sensor high voltage"},
    {"23-01", "O2 sensor heater malfunction"},
    {"25-02", "Knock sensor circuit malfunction"},
    {"25-03", "Knock sensor circuit malfunction"},
    {"29-01", "IACV circuit malfunction"},
    {"33-02", "ECM EEPROM malfunction"},
    {"34-01", "ECV POT low voltage malfunction"},
    {"34-02", "ECV POT high voltage malfunction"},
    {"35-01", "EGCA malfunction"},
    {"36-01", "A/F sensor malfunction"},
    {"38-01", "A/F sensor heater malfunction"},
    {"48-01", "No.3 secondary injector circuit malfunction"},
    {"49-01", "No.4 secondary injector circuit malfunction"},
    {"51-01", "HESD linear solenoid malfunction"},
    {"54-01", "Bank angle sensor circuit low voltage"},
    {"54-02", "Bank angle sensor circuit high voltage"},
    {"56-01", "Knock sensor IC malfunction"},
    {"82-01", "Fast idle solenoid valve malfunction"},
    {"86-01", "Serial communication malfunction"},
    {"88-01", "EVAP purge control solenoid valve malfunction"},
    {"91-01", "Ignition coil primary circuit malfunction"},
};

}  // namespace

const char* EcuStateName(ECUSTATE state) {
    switch (state) {
        case ECUSTATE::OFF: return "OFF";
        case ECUSTATE::UNKNOWN: return "UNKNOWN";
        case ECUSTATE::OK: return "OK";
        case ECUSTATE::FLASH: return "FLASH";
        case ECUSTATE::SECURE: return "SECURE";
        case ECUSTATE::RECOVER_OLD: return "RECOVER_OLD";
        case ECUSTATE::RECOVER_NEW: return "RECOVER_NEW";
    }
    return "?";
}

const char* DtcDescription(const std::string& code) {
    for (const auto& d : kDtc) {
        if (code == d.code) return d.text;
    }
    return nullptr;
}

const char* ValidationName(Validation v) {
    switch (v) {
        case Validation::kGood: return "good";
        case Validation::kFixed: return "fixed";
        default: return "bad";
    }
}

uint8_t checksum8bitHonda(const uint8_t* data, size_t size) {
    unsigned sum = 0;
    for (size_t i = 0; i < size; ++i) sum += data[i];
    return static_cast<uint8_t>(((sum ^ 0xFF) + 1) & 0xFF);
}

uint8_t checksum8bit(const Bytes& data) {
    int sum = 0;
    for (uint8_t b : data) sum += b;
    return static_cast<uint8_t>(0xFF - ((sum - 1) >> 8));
}

Bytes format_read(uint32_t location) {
    // struct.pack('>I') bytes tmp[0..3], returned as [tmp[1], tmp[3], tmp[2]]
    return Bytes{static_cast<uint8_t>(location >> 16), static_cast<uint8_t>(location), static_cast<uint8_t>(location >> 8)};
}

Bytes format_message(const Bytes& mtype, const Bytes& data, int* ml, int* dl) {
    const size_t msgsize = 2 + mtype.size() + data.size();
    Bytes msg = mtype;
    msg.push_back(static_cast<uint8_t>(msgsize));
    msg.insert(msg.end(), data.begin(), data.end());
    msg.push_back(checksum8bitHonda(msg));
    if (ml) *ml = static_cast<int>(mtype.size());
    if (dl) *dl = static_cast<int>(data.size());
    return msg;
}

Validation do_validation(Bytes& byts, long cksum) {
    bool fixed = false;
    if (cksum >= 0 && static_cast<size_t>(cksum) < byts.size()) {
        // validate_checksums: byts[cksum] = checksum over every other byte
        unsigned sum = 0;
        for (size_t i = 0; i < byts.size(); ++i) {
            if (i != static_cast<size_t>(cksum)) sum += byts[i];
        }
        byts[cksum] = static_cast<uint8_t>(((sum ^ 0xFF) + 1) & 0xFF);
        fixed = true;
    }
    if (checksum8bitHonda(byts) != 0) return Validation::kBad;
    return fixed ? Validation::kFixed : Validation::kGood;
}

void HondaECU::init() {
    dev->wake();
}

std::optional<Bytes> HondaECU::send(const Bytes& buf, int ml) {
    const double timeout = dev->timeout;
    const size_t mlen = buf.size();
    if (!dev->write(buf)) return std::nullopt;
    Bytes readbuffer;
    size_t r = mlen + ml + 1;
    const double starttime = Now();
    while (readbuffer.size() < r) {
        Bytes tempbuf = dev->read();
        readbuffer.insert(readbuffer.end(), tempbuf.begin(), tempbuf.end());
        if (Now() - starttime > timeout) return std::nullopt;
    }
    r = mlen + readbuffer[r - 1];
    while (readbuffer.size() < r) {
        Bytes tempbuf = dev->read();
        readbuffer.insert(readbuffer.end(), tempbuf.begin(), tempbuf.end());
        if (Now() - starttime > timeout) return std::nullopt;
    }
    return Bytes(readbuffer.begin() + mlen, readbuffer.end());
}

std::optional<CommandReply> HondaECU::send_command(const Bytes& mtype, const Bytes& data) {
    const int retries = dev->retries;
    int ml = 0;
    const Bytes msg = format_message(mtype, data, &ml);
    int r = 0;
    while (r <= retries) {
        if (debug) debug(std::to_string(r) + " > [" + Hex(msg) + "]");
        std::optional<Bytes> resp = send(msg, ml);
        if (resp && resp->size() > static_cast<size_t>(ml) + 1) {
            const Bytes& rb = *resp;
            if (checksum8bitHonda(rb.data(), rb.size() - 1) == rb.back()) {
                if (debug) debug(std::to_string(r) + " < [" + Hex(rb) + "]");
                bool valid = false;
                if (ml == 3) {
                    valid = rb[0] == (mtype[0] | 0x10) && rb[1] == (mtype[1] | 0x10);
                } else if (ml == 2) {
                    valid = rb[0] == mtype[0] && rb[1] == mtype[1];
                } else if (ml == 1) {
                    valid = rb[0] == (mtype[0] & 0x0F);
                }
                if (valid) {
                    CommandReply reply;
                    reply.rmtype.assign(rb.begin(), rb.begin() + ml);
                    reply.rml = rb[ml];
                    reply.rdata.assign(rb.begin() + ml + 1, rb.end() - 1);
                    reply.rdl = reply.rml - 2 - ml;
                    return reply;
                }
            } else {
                dev->stats.checksum_errors++;
            }
        }
        r++;
        dev->stats.retries++;
    }
    return std::nullopt;
}

bool HondaECU::ping(uint8_t mode) {
    return send_command({0xFE}, {mode}).has_value();
}

ECUSTATE HondaECU::detect_ecu_state() {
    const int saved_retries = dev->retries;
    dev->retries = 0;
    ECUSTATE state = ECUSTATE::OFF;
    if (dev->kline()) {
        state = ECUSTATE::UNKNOWN;
        init();
        init();
        ping();
        auto t0 = send_command({0x72}, {0x71, 0x00});
        if (t0) {
            const Bytes& d = t0->rdata;
            if (d.size() >= 7 && d[5] == 0x00 && d[6] == 0x00) {
                if (send_command({0x7D}, {0x01, 0x01, 0x03})) {
                    state = ECUSTATE::RECOVER_OLD;
                } else if (send_command({0x7B}, {0x00, 0x01, 0x04})) {
                    state = ECUSTATE::RECOVER_NEW;
                }
            } else {
                state = ECUSTATE::OK;
            }
        } else if (send_command({0x7E}, {0x01, 0x01, 0x00})) {
            state = ECUSTATE::FLASH;
        } else if (send_command({0x82, 0x82, 0x10}, {0x00})) {
            state = ECUSTATE::SECURE;
        }
    }
    dev->retries = saved_retries;
    // An adapter that cannot watch the wire cannot tell "K-Line dead" from "ECU silent", and
    // the OFF path is what the rest of HondaECU uses to notice a key cycle.
    if (state == ECUSTATE::UNKNOWN && dev->cannot_sense_kline()) state = ECUSTATE::OFF;
    return state;
}

std::map<int, std::pair<int, Bytes>> HondaECU::probe_tables() {
    const int saved_retries = dev->retries;
    dev->retries = 0;
    std::map<int, std::pair<int, Bytes>> ret;
    static const uint8_t tables[] = {0x10, 0x11, 0x13, 0x17, 0x20, 0x21, 0x60, 0x61, 0x63, 0x67, 0x70, 0x71, 0xD0, 0xD1};
    for (uint8_t t : tables) {
        auto info = send_command({0x72}, {0x71, t});
        if (!info) {
            ret.clear();
            break;
        }
        if (info->rdl > 2) ret[t] = {info->rdl, info->rdata};
    }
    dev->retries = saved_retries;
    return ret;
}

void HondaECU::do_init_recover() {
    send_command({0x7B}, {0x00, 0x02, 0x76, 0x03, 0x17});
    send_command({0x7B}, {0x00, 0x03, 0x75, 0x05, 0x13});
}

void HondaECU::do_init_write() {
    send_command({0x7D}, {0x01, 0x02, 0x50, 0x47, 0x4D});
    send_command({0x7D}, {0x01, 0x03, 0x2D, 0x46, 0x49});
}

std::optional<int> HondaECU::get_write_status() {
    auto info = send_command({0x7E}, {0x01, 0x01, 0x00});
    if (info && info->rdata.size() > 1) return info->rdata[1];
    return std::nullopt;
}

bool HondaECU::do_erase() {
    send_command({0x7E}, {0x01, 0x02});
    send_command({0x7E}, {0x01, 0x03, 0x00, 0x00});
    get_write_status();
    send_command({0x7E}, {0x01, 0x0B, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF});
    get_write_status();
    send_command({0x7E}, {0x01, 0x0E, 0x01, 0x90});
    Sleep(40);
    auto info = send_command({0x7E}, {0x01, 0x04, 0xFF});
    return info && info->rdata.size() > 1 && info->rdata[1] == 0x00;
}

bool HondaECU::do_post_write() {
    send_command({0x7E}, {0x01, 0x08});
    Sleep(500);
    get_write_status();
    send_command({0x7E}, {0x01, 0x09});
    Sleep(500);
    get_write_status();
    send_command({0x7E}, {0x01, 0x0A});
    Sleep(500);
    get_write_status();
    send_command({0x7E}, {0x01, 0x0C});
    Sleep(500);
    if (get_write_status() == 0x0F) {
        auto info = send_command({0x7E}, {0x01, 0x0D});
        return info && info->rdata.size() > 1 && info->rdata[1] == 0x0F;
    }
    return false;
}

bool HondaECU::pgmfi_read_flash_bytes(uint32_t location, int size, Bytes* out) {
    if (size <= 12) {
        Bytes data = format_read(location);
        data.push_back(static_cast<uint8_t>(size));
        auto info = send_command({0x82, 0x82, 0x00}, data);
        if (info && info->rml == size + 5) {
            *out = info->rdata;
            return true;
        }
    }
    return false;
}

bool HondaECU::pgmfi_read_eeprom_word(uint8_t location, Bytes* out) {
    auto info = send_command({0x82, 0x82, 0x10}, {location});
    if (info && info->rml == 7) {
        *out = info->rdata;
        return true;
    }
    return false;
}

bool HondaECU::pgmfi_write_eeprom_word(uint8_t location, uint8_t d0, uint8_t d1) {
    auto info = send_command({0x82, 0x82, 0x14}, {location, d0, d1});
    return info && info->rml == 5;
}

bool HondaECU::pgmfi_format_eeprom_FF() {
    auto info = send_command({0x82, 0x82, 0x18});
    return info && info->rml == 5;
}

bool HondaECU::pgmfi_format_eeprom_00() {
    auto info = send_command({0x82, 0x82, 0x19});
    return info && info->rml == 5;
}
