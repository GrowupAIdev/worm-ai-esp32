// Replicator.cpp —— 复制会话实现
#include "Replicator.h"
#include <string.h>

using namespace genome;
using namespace net;

// reset 只清当前会话状态；defQ_ 是跨会话缓存，由 popDeferred 消费
void Replicator::reset() {
    state_          = Step::IDLE;
    rxGenomeGot_    = false;
    rxNoteGot_      = false;
    memset(rxGenomeBuf_, 0, sizeof(rxGenomeBuf_));
    rxNote_         = Genome();
    offerer_        = IPAddress(0,0,0,0);
    offererSig_     = 0;
}

// ---------------- 延迟帧队列 ----------------
void Replicator::pushDeferred(const Frame &f) {
    if (defN_ >= DEFQ_MAX) {
        for (int i = 1; i < DEFQ_MAX; ++i) defQ_[i-1] = defQ_[i]; // 丢最旧
        --defN_;
    }
    defQ_[defN_++] = f;
}

bool Replicator::popDeferred(Frame *out) {
    if (!out || defN_ == 0) return false;
    *out = defQ_[0];
    for (int i = 1; i < defN_; ++i) defQ_[i-1] = defQ_[i];
    --defN_;
    return true;
}

// ---------------- 发送方等待期间的旁路帧分类 ----------------
bool Replicator::classifyWhileWaiting(WormNet &net, const Frame &f,
                                      IPAddress filterIp, bool anyFilter,
                                      uint32_t mySig, IPAddress *ackFrom) {
    if (f.peer_ip == WiFi.localIP()) return false; // 按 IP 过滤回环，其他节点可拥有相同基因组
    switch (f.type) {
        case MsgType::HANDSHAKE:
            // 对方也在找宿主，当前会话有截止时间；对方的 OFFER 留待会话结束处理。
            if (f.plen == 0) net.sendTo(f.peer_ip, MsgType::ACK, mySig, nullptr, 0);
            return false;
        case MsgType::OFFER:
        case MsgType::NOTE:
            // 会话外收到的基因组：缓存，会话结束后以接收方身份处理
            {
                Genome candidate;
                if (WormNet::extractGenome(f, &candidate) &&
                    (f.type != MsgType::OFFER || f.src_sig == candidate.self_sig)) pushDeferred(f);
            }
            return false;
        case MsgType::ACK:
            if (f.plen == 0 && (anyFilter || f.peer_ip == filterIp)) {
                if (ackFrom) *ackFrom = f.peer_ip;
                return true;
            }
            return false;
        default: // HELLO / BYE：邻居表已在 receive() 内刷新
            return false;
    }
}

// ---------------- 发送方 ----------------
bool Replicator::runSender(WormNet &net, IPAddress ip, const Genome &src,
                           uint32_t mySig, uint32_t timeoutMs) {
    reset();
    if (!src.check() || src.self_sig != mySig) return false;
    bool anyTarget = (ip == IPAddress(255,255,255,255));

    // 1) HANDSHAKE：已知邻居定向发送；广播模式全网寻找宿主
    if (!net.sendTo(ip, MsgType::HANDSHAKE, mySig, nullptr, 0)) return false;

    // 2) 等待 ACK；旁路帧（他人 HANDSHAKE/OFFER）不吞掉，分类处理
    IPAddress target = ip;
    bool gotAck = false;
    uint32_t t0 = millis();
    while (!gotAck) {
        uint32_t elapsed = millis() - t0;
        if (elapsed >= timeoutMs) break;
        Frame f;
        uint32_t left = timeoutMs - elapsed;
        // 只读 socket，不能把刚暂存的帧取出再放回同一个延迟队列。
        if (!net.receive(&f, left < 30 ? left : 30)) continue;
        IPAddress from;
        if (classifyWhileWaiting(net, f, ip, anyTarget, mySig, &from)) {
            target = from;   // 广播模式：由首个 ACK 解析出真实宿主
            gotAck = true;
        }
    }
    if (!gotAck) return false;  // 无人应答：如实失败（不再盲目发 OFFER）

    // 3) 定向发送基因组（BUILD + SPREAD）；载荷即完整基因组，无冗余前缀/NOTE
    uint8_t gbuf[genome::SIZE];
    size_t glen = net::WormNet::packGenomePayload(gbuf, sizeof(gbuf), src);
    if (glen != genome::SIZE || !net.sendTo(target, MsgType::OFFER, mySig, gbuf, (uint16_t)glen)) return false;

    // 4) 等待接收确认 ACK（接收方解析校验通过后回复）
    bool confirmed = false;
    t0 = millis();
    while (!confirmed) {
        uint32_t elapsed = millis() - t0;
        if (elapsed >= (uint32_t)net::RX_CONFIRM_MS) break;
        Frame f;
        uint32_t left = (uint32_t)net::RX_CONFIRM_MS - elapsed;
        if (!net.receive(&f, left < 30 ? left : 30)) continue;
        IPAddress from;
        if (classifyWhileWaiting(net, f, target, false, mySig, &from)) confirmed = true;
    }
    // 5) 等确认结束后再收尾，不把 BYE 提前到 OFFER 与 ACK 之间。
    net.sendTo(target, MsgType::BYE, mySig, nullptr, 0);
    return confirmed;
}

// ---------------- 接收方 ----------------
void Replicator::processRxFrame(WormNet &net, const Frame &f, uint32_t mySig) {
    if (f.peer_ip == WiFi.localIP()) return;
    switch (f.type) {
        case MsgType::HANDSHAKE:
            if (f.plen != 0) break;
            if (offerer_ == IPAddress(0,0,0,0)) {
                offerer_ = f.peer_ip;
                offererSig_ = f.src_sig;
            }
            if (f.peer_ip == offerer_ && f.src_sig == offererSig_)
                net.sendTo(f.peer_ip, MsgType::ACK, mySig, nullptr, 0);
            break;
        case MsgType::OFFER:
        case MsgType::NOTE: {
            Genome candidate;
            if (!WormNet::extractGenome(f, &candidate)) break;
            if (f.type == MsgType::OFFER && f.src_sig != candidate.self_sig) break;
            if (offerer_ != IPAddress(0,0,0,0) &&
                (f.peer_ip != offerer_ || f.src_sig != offererSig_)) {
                pushDeferred(f);
                break;
            }
            offerer_ = f.peer_ip;
            offererSig_ = f.src_sig;
            if (f.type == MsgType::OFFER) {
                memcpy(rxGenomeBuf_, f.payload, genome::SIZE);
                rxGenomeGot_ = true;
            } else {
                rxNote_ = candidate;
                rxNoteGot_ = true;
            }
            break;
        }
        default:
            break;
    }
}

bool Replicator::runReceiver(WormNet &net, Genome *outGenome,
                             uint32_t mySig, uint32_t timeoutMs,
                             const Frame *first) {
    reset();
    if (!outGenome) return false;

    // 可选：调用方已读到的首帧（通常是 HANDSHAKE 或缓存的 OFFER）
    if (first) processRxFrame(net, *first, mySig);

    // 等到 OFFER 即可孵化（NOTE 为可选增强，不再作为必要条件）
    uint32_t t0 = millis();
    uint8_t deferredLeft = defN_; // 本会话只检查旧队列一次，异源帧留给下一会话
    while (!rxGenomeGot_) {
        uint32_t elapsed = millis() - t0;
        if (elapsed >= timeoutMs) break;
        Frame f;
        if (deferredLeft > 0) {
            --deferredLeft;
            if (!popDeferred(&f)) continue;
        } else {
            uint32_t left = timeoutMs - elapsed;
            if (!net.receive(&f, left < 50 ? left : 50)) continue;
        }
        processRxFrame(net, f, mySig);
    }

    if (!rxGenomeGot_) return false;

    // 解析收到的基因组（deserialize 内含 CRC + 版本校验）
    Genome recv;
    if (!recv.deserialize(rxGenomeBuf_, genome::SIZE)) return false;
    if (!recv.check()) return false;

    // 接收确认：解析通过才回 ACK；空 ACK 的时序限制见 docs/protocol.md
    if (offerer_ != IPAddress(0,0,0,0)) {
        net.sendTo(offerer_, MsgType::ACK, mySig, nullptr, 0);
    }

    // 优先采用「笔记」中的经验（若笔记代次更高），否则用基因组本身
    Genome next = recv;
    if (rxNoteGot_ && (rxNote_.generation >= recv.generation)) {
        next = rxNote_;
        if (!next.check()) next = recv;
    }

    // 继承 + 拉马克变异（变异率减半：接收孵化不再系统性削弱强基因组）
    next.adoptParent(recv);
    next.mutate((unsigned)millis(), evo::ADOPT_MUTATION_RATE);
    next.refresh();
    *outGenome = next;
    state_ = Step::RX_OK;
    return true;
}
