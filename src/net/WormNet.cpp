#include "WormNet.h"
#include <string.h>

namespace net {
static void putU32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8));
}
static void putU16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t getU32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t getU16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }

static bool validShape(MsgType type, uint32_t session, const uint8_t *payload, uint16_t plen) {
    switch (type) {
        case MsgType::HELLO: return session == 0 && plen == 0;
        case MsgType::HANDSHAKE: return session != 0 && plen == 0;
        case MsgType::OFFER: return session != 0 && plen == genome::SIZE;
        case MsgType::ACK:
            return session != 0 && plen == 1 && payload &&
                   (payload[0] == (uint8_t)MsgType::HANDSHAKE || payload[0] == (uint8_t)MsgType::OFFER);
        default: return false; // NOTE and BYE retain their numbers but are unused in v3.
    }
}

bool WormNet::connect() {
    ok_ = false;
    sock_.stop();
    for (auto &peer : peers_) peer = PeerEntry{};
    log((uint8_t)LogLevel::INFO, "[net] connecting WiFi ...");
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - started >= 10000) { log((uint8_t)LogLevel::ERR, "[net] WiFi timeout"); return false; }
        delay(200);
    }
    if (!sock_.begin(PORT)) { log((uint8_t)LogLevel::ERR, "[net] UDP bind failed"); return false; }
    ok_ = true;
    return true;
}

bool WormNet::send(MsgType type, uint32_t sig, const uint8_t *payload, uint16_t len, uint32_t session) {
    return sendTo(IPAddress(255,255,255,255), type, sig, payload, len, session);
}

bool WormNet::sendTo(IPAddress ip, MsgType type, uint32_t sig, const uint8_t *payload, uint16_t len, uint32_t session) {
    if (!isReady() || len > PAYLOAD_MAX || (len && !payload) || !validShape(type, session, payload, len)) return false;
    uint8_t buf[HDR_SIZE + PAYLOAD_MAX + 2];
    buf[0] = MAGIC0; buf[1] = MAGIC1; buf[2] = (uint8_t)type; buf[3] = WIRE_VERSION;
    putU32(buf + 4, sig);
    putU32(buf + 8, session);
    putU16(buf + 12, len);
    if (len) memcpy(buf + HDR_SIZE, payload, len);
    const size_t size = HDR_SIZE + len + 2;
    putU16(buf + size - 2, genome::Genome::crc16(buf, size - 2));
    if (sock_.beginPacket(ip, PORT) <= 0 || sock_.write(buf, size) != size) return false;
    return sock_.endPacket() == 1;
}

bool WormNet::poll(Frame *out) {
    if (!isReady() || !out || !hasPending()) return false;
    uint8_t buf[HDR_SIZE + PAYLOAD_MAX + 2];
    const int size = sock_.available();
    if (size > (int)sizeof(buf)) { sock_.flush(); return false; }
    const int count = sock_.read(buf, sizeof(buf));
    sock_.flush();
    if (count != size || count < (int)HDR_SIZE + 2) return false;
    if (buf[0] != MAGIC0 || buf[1] != MAGIC1 || buf[3] != WIRE_VERSION) return false;
    const uint16_t len = getU16(buf + 12);
    if (len > PAYLOAD_MAX || HDR_SIZE + len + 2 != (size_t)count) return false;
    if (genome::Genome::crc16(buf, count - 2) != getU16(buf + count - 2)) return false;
    const MsgType type = (MsgType)buf[2];
    const uint32_t session = getU32(buf + 8);
    if (!validShape(type, session, buf + HDR_SIZE, len)) return false;
    out->type = type; out->session = session; out->src_sig = getU32(buf + 4);
    out->plen = len; memcpy(out->payload, buf + HDR_SIZE, len);
    out->peer_ip = sock_.remoteIP();
    if (out->peer_ip != WiFi.localIP()) notePeer(out->peer_ip, millis());
    return true;
}

bool WormNet::receive(Frame *out, uint32_t timeoutMs) {
    if (!isReady() || !out) return false;
    const uint32_t start = millis();
    while (isReady()) {
        if (timeoutMs && millis() - start >= timeoutMs) return false;
        if (poll(out)) return true;
        delay(1);
    }
    return false;
}

size_t WormNet::packGenomePayload(uint8_t *buf, size_t len, const genome::Genome &g) {
    return buf && len >= genome::SIZE && g.serialize(buf, len) ? genome::SIZE : 0;
}

bool WormNet::extractGenome(const Frame &frame, genome::Genome *out) {
    if (!out || frame.plen != genome::SIZE) return false;
    return out->deserialize(frame.payload, frame.plen);
}

void WormNet::notePeer(IPAddress ip, uint32_t now) {
    if (ip == IPAddress(0,0,0,0) || ip == WiFi.localIP()) return;
    for (auto &p : peers_) {
        if (p.ip == ip) { p.last_seen = now; return; }
    }
    int oldest = 0;
    for (int i = 0; i < PEER_TABLE; ++i) {
        if (!peers_[i].valid()) { oldest = i; break; }
        if (now - peers_[i].last_seen > now - peers_[oldest].last_seen) oldest = i;
    }
    peers_[oldest] = PeerEntry{};
    peers_[oldest].ip = ip;
    peers_[oldest].last_seen = now;
}

int WormNet::livePeers(uint32_t now, uint32_t ttlMs, IPAddress *out, int maxN) const {
    if (!out || maxN <= 0) return 0;
    int count = 0;
    for (const auto &p : peers_) {
        if (p.valid() && now - p.last_seen <= ttlMs && count < maxN) out[count++] = p.ip;
    }
    return count;
}

bool WormNet::canContact(IPAddress ip, uint32_t now) const {
    for (const auto &p : peers_) {
        if (p.ip == ip && p.failures) return now - p.last_failure >= (1000u << (p.failures - 1));
    }
    return true;
}

void WormNet::noteTransfer(IPAddress ip, bool success, uint32_t now) {
    for (auto &p : peers_) {
        if (p.ip != ip) continue;
        if (success) p.failures = 0;
        else {
            if (p.failures < 4) ++p.failures;
            p.last_failure = now;
        }
        return;
    }
}

void WormNet::log(uint8_t level, const char *message) const {
    if (level <= (uint8_t)WORM_LOG_LEVEL && message) Serial.println(message);
}
} // namespace net
