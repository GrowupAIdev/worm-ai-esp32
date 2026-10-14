// WormNet.h —— 蠕虫网络层
//
// 职责：
//   1. WiFi 连接 + UDP 收发（端口 net::PORT）
//   2. 协议帧编解码：[magic:2][type:1][src_sig:4][plen:2][payload...][crc16:2]
//   3. 对外只提供 send/receive 原语，不关心语义（语义在 Replicator/Worm）
//
// 发送帧头字段共 9 字节 + 2 字节 CRC，基因组有效载荷为 67 字节。
// 当前 HDR_SIZE 与接收偏移尚未对齐，详情见 docs/protocol.md。

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
constexpr size_t  HDR_SIZE = 10;   // 帧头字节数

struct Frame {
    MsgType    type;
    uint32_t   src_sig;    // 发送方 self_sig（防自接收/防重复）
    uint16_t   plen;
    const uint8_t *payload = nullptr;  // 当前接收缓冲的生命周期问题见 docs/protocol.md
    IPAddress  peer_ip = IPAddress(0,0,0,0); // 对端 IP（来自 socket）
};

class WormNet {
public:
    bool connect();              // WiFi + UDP 初始化
    bool isReady() const { return ok_; }

    // 向广播地址发一帧；payload 允许为 nullptr（plen=0）；src_sig 填在帧头
    bool send(MsgType t, uint32_t src_sig, const uint8_t *payload, uint16_t plen);
    // 发往指定 IP（应答/定向）
    bool sendTo(IPAddress ip, MsgType t, uint32_t src_sig, const uint8_t *payload, uint16_t plen);

    // 收帧；有帧返回 true 并填充 f；timeoutMs = 0 时无限等待
    bool receive(Frame *f, uint32_t timeoutMs = 0);

    // 是否已有 UDP 数据待收（不取出）—— 供 Worm 判断能否进入接收会话
    bool hasPending() { return sock_.available() > 0; }

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

    // 把收到的帧里的「基因组」取出来（HELLO/OFFER 帧常用）
    static bool extractGenome(const Frame &f, genome::Genome *out);

    // 把基因组打包成「携带帧」payload：[solved:1][fitness:2][genome:64]
    static size_t packGenomePayload(uint8_t *buf, size_t len, const genome::Genome &g);

    void log(uint8_t level, const char *msg) const;

private:
    bool ok_ = false;
    WiFiUDP sock_;
    PeerEntry peers_[PEER_TABLE] = {};
};

} // namespace net
