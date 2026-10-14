// Replicator.h —— 复制器：驱动一次「传输基因组 + 拉马克笔记」的完整会话
//
// 会话状态机（对应文章 BUILD + SPREAD 两步）：
//   广播 HANDSHAKE -> 等待 ACK -> 定向 OFFER -> 定向 NOTE -> BYE
//
// 角色：
//   SENDER —— 主动复制（把自己传给目标）
//   RECEIVER —— 被动接收（把收到的基因组孵化成本机新一代）
//
// 本实现的有效载荷为 [solved:1][fitness:2][genome:64]，不分片。

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

private:
    Step  state_ = Step::IDLE;
    Step  expectedNext_ = Step::IDLE;
    // 接收方缓冲：拼装收到帧
    uint8_t rxGenomeBuf_[genome::SIZE + 3] = {0}; // [solved:1][fitness:2][genome]
    bool  rxGenomeGot_ = false;
    bool  rxNoteGot_   = false;
    genome::Genome rxNote_;

    void reset();
};
