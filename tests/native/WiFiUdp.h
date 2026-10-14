#pragma once
#include "WiFi.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <vector>

namespace testio {
struct Packet { IPAddress ip; std::vector<uint8_t> bytes; };
inline std::deque<Packet> incoming;
inline std::vector<Packet> sent;
inline std::function<void(const Packet &)> onSend;
inline std::function<void()> onPoll;
inline bool udpBegin = true;
inline bool packetBegin = true;
inline bool shortWrite = false;
inline int packetEnd = 1;
inline void reset() {
    now = 0; randomState = 1; forceZeroRandom = false; WiFi = TestWiFi{};
    incoming.clear(); sent.clear(); onSend = {}; onPoll = {};
    logs.clear();
    udpBegin = true; packetBegin = true; shortWrite = false; packetEnd = 1;
}
}

// Model ESP32 WiFiUDP's parse-before-read and persistent receive buffer.
class WiFiUDP {
public:
    int begin(uint16_t) { return testio::udpBegin ? 1 : 0; }
    void stop() { flush(); testio::incoming.clear(); }
    int beginPacket(IPAddress ip, uint16_t) {
        tx_ = testio::Packet{ip, {}};
        return testio::packetBegin ? 1 : 0;
    }
    size_t write(const uint8_t *buf, size_t len) {
        const size_t written = testio::shortWrite && len ? len - 1 : len;
        tx_.bytes.assign(buf, buf + written);
        return written;
    }
    int endPacket() {
        if (testio::packetEnd != 1) return testio::packetEnd;
        testio::sent.push_back(tx_);
        if (testio::onSend) testio::onSend(tx_);
        return 1;
    }
    int parsePacket() {
        if (available()) return 0;
        if (testio::incoming.empty() && testio::onPoll) testio::onPoll();
        if (testio::incoming.empty()) return 0;
        rx_ = testio::incoming.front(); testio::incoming.pop_front(); offset_ = 0;
        peer_ = rx_.ip;
        return available();
    }
    int available() { return (int)(rx_.bytes.size() - offset_); }
    int read(uint8_t *buf, size_t len) {
        size_t count = std::min(len, rx_.bytes.size() - offset_);
        std::copy_n(rx_.bytes.data() + offset_, count, buf);
        offset_ += count;
        return (int)count;
    }
    void flush() { rx_.bytes.clear(); offset_ = 0; }
    IPAddress remoteIP() { return peer_; }
private:
    testio::Packet tx_, rx_;
    size_t offset_ = 0;
    IPAddress peer_;
};
