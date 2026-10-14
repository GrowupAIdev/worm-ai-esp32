// WormNet.h —— 蠕虫网络层
//
// 职责：
//   1. WiFi 连接 + UDP 收发（端口 net::PORT）
//   2. 协议帧编解码：[magic:2][type:1][src_sig:4][plen:2][payload...][crc16:2]
//   3. 对外只提供 send/receive 原语，不关心语义（语义在 Replicator/Worm）
//
// 发送帧头字段共 9 字节 + 2 字节 CRC，基因组有效载荷为 65 字节。
// HDR_SIZE 与发送/接收偏移已统一为 9，详情见 docs/protocol.md。

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "worm_config.h"
#include "../brain/Genome.h"

namespace net {

// 帧魔数（ASCII "WR"）
constexpr uint8_t MAGIC0 = 'W';
constexpr uint8_t MAGIC1 = 'R';
constexpr size_t  HDR_SIZE = 9;    // 帧头字节数：magic2+type1+src_sig4+plen2

// Frame 内嵌载荷上限：基因组 65 字节 + 余量；超出的帧直接丢弃
constexpr size_t  PAYLOAD_MAX = 96;

struct Frame {
    MsgType    type = MsgType::HELLO;
    uint32_t   src_sig = 0;        // 发送方 self_sig（内容指纹；回环按来源 IP 过滤）
    uint16_t   plen = 0;
    uint8_t    payload[PAYLOAD_MAX] = {}; // 内嵌缓冲：脱离 receive() 栈仍然有效
    IPAddress  peer_ip = IPAddress(0,0,0,0); // 对端 IP（来自 socket）
};

class WormNet {
public:
    bool connect();              // WiFi + UDP 初始化
    bool isReady() const { return ok_ && WiFi.status() == WL_CONNECTED; }

    // 向广播地址发一帧；payload 允许为 nullptr（plen=0）；src_sig 填在帧头
    bool send(MsgType t, uint32_t src_sig, const uint8_t *payload, uint16_t plen);
    // 发往指定 IP（应答/定向）
    bool sendTo(IPAddress ip, MsgType t, uint32_t src_sig, const uint8_t *payload, uint16_t plen);

    // 收帧；有帧返回 true 并填充 f；timeoutMs = 0 时无限等待
    bool receive(Frame *f, uint32_t timeoutMs = 0);

    // 是否已有 UDP 数据待收 —— ESP32 上必须先 parsePacket() 才会产生可读数据
    bool hasPending() {
        if (!isReady()) return false;
        if (sock_.available() > 0) return true;
        return sock_.parsePacket() > 0;   // 副作用：把包搬入接收缓冲，不消费
    }

    // —— 邻居表（供 SCAN 阶段直接取已知邻居）——
    struct PeerEntry {
        IPAddress ip;
        uint32_t  last_seen;
        bool      valid() const { return ip != IPAddress(0,0,0,0); }
    };
    static constexpr int PEER_TABLE = 8;
    void     notePeer(IPAddress ip, uint32_t now);  // 见到谁记谁（收到任何帧时调用）
    int      livePeers(uint32_t now, uint32_t ttlMs, IPAddress *out, int maxN) const;
    IPAddress peerAt(int idx) const { return peers_[idx%PEER_TABLE].ip; }

    // 从 OFFER/NOTE 的完整载荷取出已校验的基因组
    static bool extractGenome(const Frame &f, genome::Genome *out);

    // 把基因组打包成载荷：直接为完整基因组字节 [genome:65]
    static size_t packGenomePayload(uint8_t *buf, size_t len, const genome::Genome &g);

    void log(uint8_t level, const char *msg) const;

private:
    bool ok_ = false;
    WiFiUDP sock_;
    PeerEntry peers_[PEER_TABLE] = {};
};

} // namespace net
