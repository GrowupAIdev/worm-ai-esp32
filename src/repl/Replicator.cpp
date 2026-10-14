// Replicator.cpp —— 复制会话实现
#include "Replicator.h"
#include <string.h>

using namespace genome;
using namespace net;

void Replicator::reset() {
    state_          = Step::IDLE;
    expectedNext_   = Step::IDLE;
    rxGenomeGot_    = false;
    rxNoteGot_      = false;
    memset(rxGenomeBuf_, 0, sizeof(rxGenomeBuf_));
    rxNote_         = Genome();
}

bool Replicator::runSender(WormNet &net, IPAddress ip, const Genome &src,
                           uint32_t mySig, uint32_t timeoutMs) {
    reset();
    // 1) 广播 HANDSHAKE，再按目标 ip 筛选 ACK
    net.send(MsgType::HANDSHAKE, mySig, nullptr, 0);

    // 2) 等待 ACK（目标确认通道建立；按对端 IP 校验，防止误收第三方的 ACK）
    uint32_t t0 = millis();
    bool gotAck = false;
    while (!gotAck) {
        Frame f;
        if (!net.receive(&f, 30)) {
            if ((millis() - t0) > timeoutMs) break;
            continue;
        }
        gotAck = (f.type == MsgType::ACK) && (f.src_sig != mySig)
               && (f.peer_ip == ip);
    }

    // 3) 定向发送基因组（BUILD + SPREAD）
    uint8_t gbuf[3 + genome::SIZE];
    size_t glen = net::WormNet::packGenomePayload(gbuf, sizeof(gbuf), src);
    bool sentG = net.sendTo(ip, MsgType::OFFER, mySig, gbuf, (uint16_t)glen);

    // 4) 发送拉马克笔记（遗书）
    uint8_t nbuf[3 + genome::SIZE];
    size_t nlen = net::WormNet::packGenomePayload(nbuf, sizeof(nbuf), src);
    bool sentN = net.sendTo(ip, MsgType::NOTE, mySig, nbuf, (uint16_t)nlen);

    // 5) 优雅收尾
    net.sendTo(ip, MsgType::BYE, mySig, nullptr, 0);

    return sentG && sentN;
}

bool Replicator::runReceiver(WormNet &net, Genome *outGenome,
                             uint32_t mySig, uint32_t timeoutMs,
                             const Frame *first) {
    reset();

    // 可选：调用方已读到的首帧（通常是 HANDSHAKE），先处理掉
    if (first) {
        if (first->src_sig != mySig) {
            if (first->type == MsgType::HANDSHAKE) {
                net.sendTo(first->peer_ip, MsgType::ACK, mySig, nullptr, 0);
            } else if (first->type == MsgType::OFFER) {
                if (first->plen == 3 + genome::SIZE) {
                    memcpy(rxGenomeBuf_, first->payload, first->plen);
                    rxGenomeGot_ = true;
                }
            } else if (first->type == MsgType::NOTE) {
                if (net::WormNet::extractGenome(*first, &rxNote_) && rxNote_.check()) {
                    rxNoteGot_ = true;
                }
            }
        }
    }

    // 收到 HANDSHAKE -> 立即回 ACK（回给对端 IP）
    // 随后等待 OFFER + NOTE
    uint32_t t0 = millis();
    while (!(rxGenomeGot_ && rxNoteGot_)) {
        Frame f;
        if (!net.receive(&f, 50)) {
            if ((millis() - t0) > timeoutMs) break;
            continue;
        }
        if (f.src_sig == mySig) continue;

        if (f.type == MsgType::HANDSHAKE) {
            // 回复 ACK 确认通道（发给来源 IP，而不是 src_sig 猜的 IP）
            net.sendTo(f.peer_ip, MsgType::ACK, mySig, nullptr, 0);
        } else if (f.type == MsgType::OFFER) {
            if (f.plen == 3 + genome::SIZE) {
                memcpy(rxGenomeBuf_, f.payload, f.plen);
                rxGenomeGot_ = true;
            }
        } else if (f.type == MsgType::NOTE) {
            if (net::WormNet::extractGenome(f, &rxNote_) && rxNote_.check()) {
                rxNoteGot_ = true;
            }
        }
    }

    if (!rxGenomeGot_) return false;

    // 解析收到的基因组
    Genome recv;
    if (!recv.deserialize(rxGenomeBuf_ + 3, genome::SIZE)) return false;
    if (!recv.check()) return false;

    // 优先采用「笔记」中的经验（若笔记代次更高），否则用基因组本身
    Genome next = recv;
    if (rxNoteGot_ && (rxNote_.generation >= recv.generation)) {
        next = rxNote_;
        if (!next.check()) next = recv;
    }

    // 继承 + 拉马克变异
    next.adoptParent(recv);
    next.mutate((unsigned)millis());
    if (outGenome) *outGenome = next;
    state_ = Step::RX_OK;
    return true;
}
