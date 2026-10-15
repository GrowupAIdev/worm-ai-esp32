#include "Replicator.h"

using genome::Genome;
using net::Frame;
using net::WormNet;

bool Replicator::start(WormNet &net, IPAddress ip, const Genome &src) {
    if (busy() || pendingResult_ || !net.isReady() || !src.check() ||
        ip == WiFi.localIP() || ip == IPAddress(0,0,0,0)) return false;
    if (WormNet::packGenomePayload(txBytes_, sizeof(txBytes_), src) != genome::SIZE) return false;
    if (counter_ == 0) counter_ = esp_random();
    if (++counter_ == 0) ++counter_;
    session_ = counter_; target_ = ip; txSig_ = src.self_sig;
    phase_ = ip == IPAddress(255,255,255,255) ? Phase::DISCOVER : Phase::RECEIPT;
    phaseStart_ = millis(); attempts_ = 0; ++stats_.started;
    sendCurrent(net, phaseStart_);
    return true;
}

void Replicator::sendCurrent(WormNet &net, uint32_t now) {
    if (attempts_ > 0) ++stats_.retries;
    if (phase_ == Phase::DISCOVER) net.sendTo(target_, MsgType::HANDSHAKE, txSig_, nullptr, 0, session_);
    else net.sendTo(target_, MsgType::OFFER, txSig_, txBytes_, sizeof(txBytes_), session_);
    lastSend_ = now;
    retryWait_ = (net::RETRY_BASE_MS << attempts_) + esp_random() % (net::RETRY_JITTER_MS + 1);
    ++attempts_;
}

void Replicator::finish(bool success) {
    phase_ = Phase::IDLE; pendingResult_ = true; result_ = success;
    if (success) ++stats_.confirmed;
    else ++stats_.failed;
}

bool Replicator::takeResult(bool *success) {
    if (!success || !pendingResult_) return false;
    *success = result_; pendingResult_ = false; return true;
}

bool Replicator::takeReceived(Genome *out) {
    if (!out || receivedN_ == 0) return false;
    *out = received_[0];
    for (int i = 1; i < receivedN_; ++i) received_[i - 1] = received_[i];
    --receivedN_; return true;
}

void Replicator::acknowledge(WormNet &net, const Frame &f, uint32_t mySig) {
    const uint8_t stage = (uint8_t)f.type;
    net.sendTo(f.peer_ip, MsgType::ACK, mySig, &stage, 1, f.session);
}

void Replicator::handle(WormNet &net, const Frame &f, uint32_t mySig, uint32_t now) {
    if (f.peer_ip == WiFi.localIP() || f.peer_ip == IPAddress(0,0,0,0)) return;
    if (f.type == MsgType::HANDSHAKE) {
        acknowledge(net, f, mySig);
    } else if (f.type == MsgType::ACK) {
        if (!busy() || f.session != session_ || now - phaseStart_ >= net::TX_STAGE_MS) return;
        if (phase_ == Phase::DISCOVER && f.payload[0] == (uint8_t)MsgType::HANDSHAKE) {
            target_ = f.peer_ip; phase_ = Phase::RECEIPT;
            phaseStart_ = now; attempts_ = 0; sendCurrent(net, now);
        } else if (phase_ == Phase::RECEIPT && f.peer_ip == target_ &&
                   f.payload[0] == (uint8_t)MsgType::OFFER) finish(true);
    } else if (f.type == MsgType::OFFER) {
        Genome candidate;
        if (!WormNet::extractGenome(f, &candidate) || candidate.self_sig != f.src_sig) {
            ++stats_.invalid; return;
        }
        for (const auto &seen : seen_) {
            if (seen.session != f.session || seen.ip != f.peer_ip || now - seen.at > net::DEDUP_TTL_MS) continue;
            if (seen.sig != f.src_sig) { ++stats_.invalid; return; }
            ++stats_.duplicates; acknowledge(net, f, mySig); return;
        }
        if (receivedN_ == RX_QUEUE) { ++stats_.overflow; return; }
        const Genome parent = candidate;
        candidate.adoptParent(parent);
        candidate.mutate(esp_random(), evo::ADOPT_MUTATION_RATE);
        candidate.refresh();
        received_[receivedN_++] = candidate;
        Seen &seen = seen_[seenCursor_];
        seen.ip = f.peer_ip; seen.session = f.session; seen.sig = f.src_sig; seen.at = now;
        seenCursor_ = (seenCursor_ + 1) % SEEN_MAX;
        ++stats_.received; acknowledge(net, f, mySig);
    }
}

void Replicator::poll(WormNet &net, uint32_t mySig) {
    if (!net.isReady()) { if (busy()) finish(false); return; }
    for (int i = 0; i < net::RX_BATCH && net.hasPending(); ++i) {
        Frame frame;
        if (net.poll(&frame)) handle(net, frame, mySig, millis());
    }
    if (!busy()) return;
    const uint32_t now = millis();
    if (now - phaseStart_ >= net::TX_STAGE_MS) { finish(false); return; }
    if (now - lastSend_ >= retryWait_) {
        if (attempts_ >= net::MAX_ATTEMPTS) finish(false);
        else sendCurrent(net, now);
    }
}
