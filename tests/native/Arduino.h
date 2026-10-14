#pragma once
#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <string>
#include <vector>

namespace testio {
inline uint32_t now = 0;
inline uint32_t randomState = 1;
inline bool forceZeroRandom = false;
inline std::vector<std::string> logs;
}

// Advance fake time on every clock read, including under continuous traffic.
inline uint32_t millis() { return testio::now++; }
inline void delay(uint32_t ms) { testio::now += ms; }
inline void randomSeed(uint32_t seed) { testio::randomState = seed ? seed : 1; }
inline uint32_t esp_random() {
    if (testio::forceZeroRandom) return 0;
    uint32_t &s = testio::randomState;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}
inline long random(long bound) { return bound > 0 && !testio::forceZeroRandom ? esp_random() % bound : 0; }
inline int analogRead(int) { return 17; }
#define F(text) text
struct TestSerial {
    void begin(unsigned long) {}
    void println(const char *text) { testio::logs.emplace_back(text); }
};
inline TestSerial Serial;
