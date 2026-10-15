// v3 UDP transport; see docs/protocol.md.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "worm_config.h"
#include "../brain/Genome.h"

namespace net {
constexpr uint8_t MAGIC0 = 'W', MAGIC1 = 'R';
constexpr size_t HDR_SIZE = 14;
constexpr size_t PAYLOAD_MAX = 96;
static_assert(PAYLOAD_MAX >= genome::SIZE, "genome must fit in one frame");

struct Frame {
    MsgType type = MsgType::HELLO;
    uint32_t src_sig = 0;
    uint32_t session = 0;
    uint16_t plen = 0;
    uint8_t payload[PAYLOAD_MAX] = {};
    IPAddress peer_ip = IPAddress(0,0,0,0);
};

class WormNet {
public:
    bool connect();
    bool isReady() const { return ok_ && WiFi.status() == WL_CONNECTED; }
    bool send(MsgType type, uint32_t sig, const uint8_t *payload, uint16_t len, uint32_t session = 0);
    bool sendTo(IPAddress ip, MsgType type, uint32_t sig, const uint8_t *payload, uint16_t len, uint32_t session = 0);
    // Nonblocking: consume at most one datagram, including malformed packets.
    bool poll(Frame *out);
    // Convenience blocking API; the firmware loop uses poll(), not receive().
    bool receive(Frame *out, uint32_t timeoutMs = 0);
    bool hasPending() {
        if (!isReady()) return false;
        return sock_.available() > 0 || sock_.parsePacket() > 0;
    }

    struct PeerEntry {
        IPAddress ip;
        uint32_t last_seen = 0, last_failure = 0;
        uint8_t failures = 0;
        bool valid() const { return ip != IPAddress(0,0,0,0); }
    };
    static constexpr int PEER_TABLE = 8;
    void notePeer(IPAddress ip, uint32_t now);
    int livePeers(uint32_t now, uint32_t ttlMs, IPAddress *out, int maxN) const;
    bool canContact(IPAddress ip, uint32_t now) const;
    void noteTransfer(IPAddress ip, bool success, uint32_t now);
    static bool extractGenome(const Frame &frame, genome::Genome *out);
    static size_t packGenomePayload(uint8_t *buf, size_t len, const genome::Genome &g);
    void log(uint8_t level, const char *message) const;

private:
    bool ok_ = false;
    WiFiUDP sock_;
    PeerEntry peers_[PEER_TABLE] = {};
};
} // namespace net
