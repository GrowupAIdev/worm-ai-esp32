#pragma once
#include <Arduino.h>
#include "brain/Genome.h"
#include "brain/Brain.h"
#include "net/WormNet.h"
#include "repl/Replicator.h"

class Worm {
public:
    void begin();
    void loop();
    uint32_t generations() const { return gen_; }
    float fitness() const { return genome_.pipelineRate(); }
    const Replicator::Stats &transferStats() const { return repl_.stats(); }

private:
    bool step1_scan();
    bool step2_perm();
    void step4_spread(bool success);
    void adoptReceived();
    genome::Genome genome_;
    net::WormNet net_;
    Replicator repl_;
    int8_t input_[brain::IN] = {};
    uint32_t gen_ = 0, lastAction_ = 0, cooldownMs_ = 5000;
    uint32_t lastHello_ = 0, helloPeriod_ = 2000, lastReconnect_ = 0;
    uint32_t peerCount_ = 0;
    int peerCursor_ = 0;
    IPAddress lastPeer_ = IPAddress(0,0,0,0);
};
