// Replicator.h —— 复制器：驱动一次「传输基因组 + 拉马克笔记」的完整会话
//
// 会话状态机（对应文章 BUILD + SPREAD 两步）：
//   HANDSHAKE -> 等待 ACK -> 定向 OFFER -> 等待确认 ACK -> BYE
//
// 角色：
//   SENDER —— 主动复制（把自己传给目标）
//   RECEIVER —— 被动接收（把收到的基因组孵化成本机新一代）
//
// 本实现的载荷为完整基因组字节 [genome:65]，不分片。

#pragma once
#include <Arduino.h>
#include "worm_config.h"
#include "../brain/Genome.h"
#include "../net/WormNet.h"

class Replicator {
public:
    enum class Role : uint8_t { NONE, SENDER, RECEIVER };
    enum class Step : uint8_t {
        IDLE,       // 空闲
        PING,       // 发 HANDSHAKE 或等待 HANDSHAKE
        TX_GENOME,  // 发 OFFER(基因组)
        TX_NOTE,    // 发 NOTE(遗书)
        TX_DONE,    // 发 BYE
        RX_WAIT,    // 接收方等待 OFFER/NOTE
        RX_OK,      // 接收完成
    };

    // 以 SENDER 身份把 genome 复制到 ip
    bool  runSender(net::WormNet &net, IPAddress ip, const genome::Genome &src,
                    uint32_t mySig, uint32_t timeoutMs = 3000);
    // 以 RECEIVER 身份监听并孵化新一代；成功返回 true，outGenome 为孵化结果
    // first: 可选——调用方已读到的首帧（如 HANDSHAKE），省去一次重复读取
    bool  runReceiver(net::WormNet &net, genome::Genome *outGenome,
                      uint32_t mySig, uint32_t timeoutMs = 3000,
                      const net::Frame *first = nullptr);

    // —— 延迟帧队列：发送会话期间收到的 OFFER/NOTE 不丢弃，暂存待会话结束后处理 ——
    bool hasDeferred() const { return defN_ > 0; }
    bool popDeferred(net::Frame *out);

private:
    static constexpr int DEFQ_MAX = 4;

    Step  state_ = Step::IDLE;
    // 接收方缓冲：拼装收到帧
    uint8_t rxGenomeBuf_[genome::SIZE] = {0}; // [genome:65]
    bool  rxGenomeGot_ = false;
    bool  rxNoteGot_   = false;
    genome::Genome rxNote_;
    IPAddress offerer_;                 // OFFER 来源 IP（用于回确认 ACK）
    uint32_t offererSig_ = 0;           // 本会话握手/载荷的发送方指纹
    net::Frame defQ_[DEFQ_MAX];         // 会话期间缓存的旁路帧
    uint8_t  defN_ = 0;

    void reset();                       // 只清会话状态，保留 defQ_（跨会话缓存）
    void pushDeferred(const net::Frame &f);
    // 接收方处理一帧（HANDSHAKE→ACK / OFFER→缓冲 / NOTE→笔记）
    void processRxFrame(net::WormNet &net, const net::Frame &f, uint32_t mySig);
    // 发送方等待期间的旁路帧分类：他人 HANDSHAKE→回 ACK；
    // OFFER/NOTE→入延迟队列；匹配的 ACK→返回 true（ackFrom = 对端 IP）
    bool classifyWhileWaiting(net::WormNet &net, const net::Frame &f,
                              IPAddress filterIp, bool anyFilter,
                              uint32_t mySig, IPAddress *ackFrom);
};
