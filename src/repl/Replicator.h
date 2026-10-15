// Bounded, nonblocking genome transfer. RX remains active while TX waits for ACKs.
#pragma once
#include "../net/WormNet.h"

class Replicator {
public:
    struct Stats {
        uint32_t started = 0, confirmed = 0, failed = 0, retries = 0;
        uint32_t received = 0, duplicates = 0, invalid = 0, overflow = 0;
    };
    bool start(net::WormNet &net, IPAddress ip, const genome::Genome &src);
    void poll(net::WormNet &net, uint32_t mySig);
    bool busy() const { return phase_ != Phase::IDLE; }
    bool takeResult(bool *success);
    bool takeReceived(genome::Genome *out);
    IPAddress target() const { return target_; }
    uint32_t session() const { return session_; }
    const Stats &stats() const { return stats_; }

private:
    enum class Phase : uint8_t { IDLE, DISCOVER, RECEIPT };
    static constexpr int RX_QUEUE = 4;
    static constexpr int SEEN_MAX = 32;
    struct Seen {
        IPAddress ip;
        uint32_t session = 0, sig = 0, at = 0;
    };
    Phase phase_ = Phase::IDLE;
    IPAddress target_;
    uint32_t session_ = 0, counter_ = 0, txSig_ = 0;
    uint32_t phaseStart_ = 0, lastSend_ = 0, retryWait_ = 0;
    uint8_t attempts_ = 0;
    bool pendingResult_ = false, result_ = false;
    uint8_t txBytes_[genome::SIZE] = {};
    genome::Genome received_[RX_QUEUE];
    uint8_t receivedN_ = 0;
    Seen seen_[SEEN_MAX] = {};
    uint8_t seenCursor_ = 0;
    Stats stats_;

    void sendCurrent(net::WormNet &net, uint32_t now);
    void finish(bool success);
    void handle(net::WormNet &net, const net::Frame &frame, uint32_t mySig, uint32_t now);
    void acknowledge(net::WormNet &net, const net::Frame &frame, uint32_t mySig);
};
