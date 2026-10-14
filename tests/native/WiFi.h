#pragma once
#include "Arduino.h"

class IPAddress {
public:
    IPAddress() : value_(0) {}
    explicit IPAddress(uint32_t value) : value_(value) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
        : value_(a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24)) {}
    uint8_t operator[](size_t index) const { return (uint8_t)(value_ >> (index * 8)); }
    bool operator==(IPAddress other) const { return value_ == other.value_; }
    bool operator!=(IPAddress other) const { return !(*this == other); }
private:
    uint32_t value_;
};

constexpr int WL_CONNECTED = 3;
constexpr int WIFI_AP_STA = 3;
struct TestWiFi {
    int statusValue = WL_CONNECTED;
    bool autoConnect = true;
    IPAddress ip = IPAddress(10, 0, 0, 1);
    void mode(int) {}
    void begin(const char *, const char *) { if (autoConnect) statusValue = WL_CONNECTED; }
    int status() const { return statusValue; }
    IPAddress localIP() const { return ip; }
};
inline TestWiFi WiFi;
