// WormNet.cpp —— UDP 收发 + 帧编解码（仅依赖 Arduino / ESP32 WiFi 栈）
#include "WormNet.h"
#include <string.h>

using namespace genome;

namespace net {

// ---- CRC16-CCITT (0xFFFF 初始, 0x1021 多项式)，与 Genome::crc16 同一算法 ----
static uint16_t crc16(const uint8_t *buf, size_t len) {
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= ((uint16_t)*buf++) << 8;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}
static void putU32(uint8_t *p, uint32_t v){p[0]=v&0xFF;p[1]=(v>>8)&0xFF;p[2]=(v>>16)&0xFF;p[3]=(v>>24)&0xFF;}
static void putU16(uint8_t *p, uint16_t v){p[0]=v&0xFF;p[1]=(v>>8)&0xFF;}
static uint32_t getU32(const uint8_t *p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint16_t getU16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}

bool WormNet::connect() {
    log((uint8_t)LogLevel::INFO, "[net] connecting WiFi ...");
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > 10000) { log((uint8_t)LogLevel::ERR, "[net] WiFi timeout"); return false; }
        delay(200);
    }
    char line[40];
    IPAddress ip = WiFi.localIP();
    snprintf(line, sizeof(line), "[net] ip=%d.%d.%d.%d port=%u",
             ip[0], ip[1], ip[2], ip[3], (unsigned)PORT);
    log((uint8_t)LogLevel::INFO, line);

    if (!sock_.begin(PORT)) { log((uint8_t)LogLevel::ERR, "[net] udp begin fail"); return false; }
    ok_ = true;
    return true;
}

bool WormNet::send(MsgType t, uint32_t src_sig, const uint8_t *payload, uint16_t plen) {
    return sendTo(IPAddress(255, 255, 255, 255), t, src_sig, payload, plen);
}

bool WormNet::sendTo(IPAddress ip, MsgType t, uint32_t src_sig, const uint8_t *payload, uint16_t plen) {
    if (!ok_ || plen > MTU) return false;
    uint8_t buf[HDR_SIZE + MTU + 2];
    size_t off = 0;
    buf[off++] = MAGIC0; buf[off++] = MAGIC1;
    buf[off++] = (uint8_t)t;
    putU32(buf + off, src_sig); off += 4;
    putU16(buf + off, plen);    off += 2;
    if (plen && payload) { memcpy(buf + off, payload, plen); off += plen; }
    uint16_t c = crc16(buf, off);
    buf[off++] = (uint8_t)(c & 0xFF); buf[off++] = (uint8_t)(c >> 8);

    if (sock_.beginPacket(ip, PORT) <= 0) return false;
    int n = sock_.write(buf, off);
    sock_.endPacket();
    return n == (int)off;
}

bool WormNet::receive(Frame *f, uint32_t timeoutMs) {
    if (!ok_ || !f) return false;
    uint32_t t0 = millis();
    for (;;) {
        if (sock_.available() > 0) {
            uint8_t buf[HDR_SIZE + MTU + 2];
            int n = sock_.read(buf, sizeof(buf));
            if (n < (int)HDR_SIZE + 2) return false;               // 短帧丢弃
            if (buf[0] != MAGIC0 || buf[1] != MAGIC1) return false; // 非蠕虫帧
            uint16_t plen = getU16(buf + 6);
            if ((size_t)(HDR_SIZE + plen + 2) > (size_t)n) return false;
            uint16_t crc  = getU16(buf + HDR_SIZE + plen);
            if (crc16(buf, HDR_SIZE + plen) != crc) return false;   // 校验失败
            f->type    = (MsgType)buf[2];
            f->src_sig = getU32(buf + 3);
            f->plen    = plen;
            f->payload = buf + HDR_SIZE;
            f->peer_ip = sock_.remoteIP();
            notePeer(f->peer_ip, millis());
            return true;
        }
        if (timeoutMs) {
            if ((millis() - t0) > timeoutMs) return false;
        } else {
            delay(1);  // 无限等待模式：让出 CPU
        }
    }
}

// ---------------- 基因组打包 / 解包 ----------------
// 携带基因组帧的 payload 布局：[solved:1][fitness:2][genome:64]
size_t WormNet::packGenomePayload(uint8_t *buf, size_t len, const Genome &g) {
    if (!buf || len < 3 + genome::SIZE) return 0;
    buf[0] = g.solved;
    putU16(buf + 1, g.fitness);
    g.serialize(buf + 3, genome::SIZE);
    return 3 + genome::SIZE;
}

bool WormNet::extractGenome(const Frame &f, Genome *out) {
    if (f.plen < 3 + genome::SIZE) return false;
    Genome tmp;
    if (!tmp.deserialize(f.payload + 3, f.plen - 3)) return false;
    if (!tmp.check()) return false;
    *out = tmp;
    return true;
}

// —— 邻居表 ——
void WormNet::notePeer(IPAddress ip, uint32_t now) {
    if (ip == IPAddress(0,0,0,0)) return;
    for (auto &p : peers_) {
        if (p.ip == ip) { p.last_seen = now; return; }
    }
    // 找不到就替换最旧的槽位
    int worst = 0;
    for (int i = 1; i < PEER_TABLE; ++i)
        if (peers_[i].last_seen < peers_[worst].last_seen) worst = i;
    peers_[worst].ip = ip;
    peers_[worst].last_seen = now;
}

int WormNet::livePeers(uint32_t now, uint32_t ttlMs, IPAddress *out, int maxN) const {
    int n = 0;
    for (const auto &p : peers_) {
        if (p.valid() && (now - p.last_seen) <= ttlMs && n < maxN) {
            out[n++] = p.ip;
        }
    }
    return n;
}

void WormNet::log(uint8_t level, const char *msg) const {
    if (level <= (uint8_t)WORM_LOG_LEVEL && msg) Serial.println(msg);
}

} // namespace net
