#include "Worm.h"

namespace {
constexpr uint32_t HELLO_MS = 2000, RECONNECT_MS = 10000, PEER_TTL_MS = 6000;
constexpr uint32_t COOLDOWN_OK_MS = 1500, COOLDOWN_FAIL_MS = 1000, COOLDOWN_SKIP_MS = 1000;
uint32_t jitter() { return esp_random() % 251; }
}

void Worm::begin() {
    randomSeed(esp_random());
    genome_ = genome::Genome();
    genome_.mutate(esp_random(), 1.0f);
    genome_.reseedNote(); genome_.refresh();
    gen_ = genome_.generation;
    Serial.println(F("[worm] genome v2 / transport v3"));
    net_.connect();
    lastAction_ = lastHello_ = lastReconnect_ = millis();
    cooldownMs_ = 5000 + jitter(); helloPeriod_ = HELLO_MS + jitter();
}

void Worm::adoptReceived() {
    genome::Genome next;
    while (repl_.takeReceived(&next)) {
        if (next.fitness >= genome_.fitness && next.stepsSolved() >= genome_.stepsSolved()) {
            genome_ = next; gen_ = next.generation;
            Serial.println(F("[evo] adopted genome"));
        }
    }
}

void Worm::loop() {
    repl_.poll(net_, genome_.self_sig);
    adoptReceived();
    bool success;
    if (repl_.takeResult(&success)) {
        net_.noteTransfer(repl_.target(), success, millis());
        step4_spread(success);
        cooldownMs_ = (success ? COOLDOWN_OK_MS : COOLDOWN_FAIL_MS) + jitter();
        lastAction_ = millis();
        Serial.println(success ? F("[build] confirmed") : F("[build] failed after bounded retries"));
    }
    const uint32_t now = millis();
    if (!net_.isReady()) {
        if (now - lastReconnect_ >= RECONNECT_MS) {
            lastReconnect_ = now; net_.connect();
        }
        delay(1); return;
    }
    if (now - lastHello_ >= helloPeriod_) {
        lastHello_ = now; helloPeriod_ = HELLO_MS + jitter();
        net_.send(MsgType::HELLO, genome_.self_sig, nullptr, 0);
    }
    if (repl_.busy() || now - lastAction_ < cooldownMs_) return;
    if (!step1_scan()) return;
    if (step2_perm()) {
        repl_.start(net_, lastPeer_, genome_);
    } else {
        cooldownMs_ = COOLDOWN_SKIP_MS + jitter(); lastAction_ = now;
    }
}

bool Worm::step1_scan() {
    IPAddress peers[net::WormNet::PEER_TABLE];
    const uint32_t now = millis();
    const int count = net_.livePeers(now, PEER_TTL_MS, peers, net::WormNet::PEER_TABLE);
    peerCount_ = count;
    for (int k = 0; k < count; ++k) {
        const int index = (peerCursor_ + k) % count;
        if (net_.canContact(peers[index], now)) {
            peerCursor_ = (index + 1) % count; lastPeer_ = peers[index]; return true;
        }
    }
    if (count > 0) return false; // Do not bypass a live peer's backoff with a broadcast.
    lastPeer_ = IPAddress(255,255,255,255); return true;
}

bool Worm::step2_perm() {
    brain::makeInput(esp_random(), input_);
    input_[0] = (int8_t)(peerCount_ & 0x7F);
    input_[1] = (int8_t)genome_.stepsSolved();
    input_[2] = (int8_t)(genome_.fitness >> 8);
    input_[3] = (int8_t)(gen_ & 0x7F);
    return brain::decide(genome_.weights, input_) >= 0 ||
           esp_random() % 100 < (unsigned)evo::PERM_FLOOR_PCT;
}

void Worm::step4_spread(bool success) {
    if (success) {
        for (int step = 0; step < genome::N_STEPS; ++step)
            if (random(100) < 80) genome_.learnStep(step);
        if (genome_.generation < 65535u) ++genome_.generation;
        gen_ = genome_.generation;
    } else {
        genome_.forgetStep((uint8_t)random(genome::N_STEPS));
        genome_.mutate(esp_random(), evo::MUTATION_RATE);
        genome_.reseedNote();
    }
    genome_.refresh();
}
