#include "brain/Genome.h"
#include "brain/Brain.h"
#include "net/WormNet.h"
#include "repl/Replicator.h"
#include "worm/Worm.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using genome::Genome;
using net::Frame;
using net::WormNet;
const IPAddress peer(10, 0, 0, 2), other(10, 0, 0, 3);

#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (0)

// Construct canonical protocol fixtures independently of Genome::refresh().
static void seal(Genome &g) {
    g.fitness = g.computeFitness(); g.self_sig = 0;
    uint8_t bytes[genome::SIZE];
    REQUIRE(g.serialize(bytes, sizeof(bytes)));
    g.self_sig = Genome::sign(bytes, genome::PAYLOAD_END);
}
static Genome parent(uint16_t generation = 7) {
    Genome g;
    g.generation = generation; g.learnStep(8); g.learnStep(9);
    for (int i = 0; i < genome::WEIGHT_COUNT; ++i) g.weights[i] = (int8_t)(i - 20);
    seal(g); return g;
}
static Frame control(MsgType type, IPAddress ip = peer, uint32_t sig = 123) {
    Frame f; f.type = type; f.peer_ip = ip; f.src_sig = sig; return f;
}
static Frame offer(const Genome &g, IPAddress ip = peer, MsgType type = MsgType::OFFER) {
    Frame f = control(type, ip, g.self_sig);
    f.plen = genome::SIZE; REQUIRE(g.serialize(f.payload, sizeof(f.payload))); return f;
}
static testio::Packet packet(const Frame &f) {
    std::vector<uint8_t> b(net::HDR_SIZE + f.plen + 2);
    b[0] = 'W'; b[1] = 'R'; b[2] = (uint8_t)f.type;
    for (int i = 0; i < 4; ++i) b[3 + i] = (uint8_t)(f.src_sig >> (i * 8));
    b[7] = (uint8_t)f.plen; b[8] = (uint8_t)(f.plen >> 8);
    std::copy_n(f.payload, f.plen, b.begin() + net::HDR_SIZE);
    uint16_t crc = Genome::crc16(b.data(), b.size() - 2);
    b[b.size() - 2] = (uint8_t)crc; b.back() = (uint8_t)(crc >> 8);
    return {f.peer_ip, b};
}
static void enqueue(const Frame &f) { testio::incoming.push_back(packet(f)); }
static MsgType typeOf(const testio::Packet &p) { return (MsgType)p.bytes.at(2); }
static int sentCount(MsgType type) {
    int count = 0;
    for (const auto &p : testio::sent) if (typeOf(p) == type) ++count;
    return count;
}

static void genomeRoundtrip() {
    Genome g = parent();
    for (uint8_t step = 0; step < genome::N_STEPS; ++step) g.learnStep(step);
    seal(g);
    REQUIRE(g.stepsSolved() == 10); REQUIRE(g.solved == 0x3FF);
    REQUIRE(g.fitness == 8171); REQUIRE(g.check());
    uint8_t bytes[genome::SIZE]; REQUIRE(g.serialize(bytes, sizeof(bytes)));
    REQUIRE(bytes[0] == 2 && bytes[5] == 0xFF && bytes[6] == 3);
    Genome read; REQUIRE(read.deserialize(bytes, sizeof(bytes)));
    REQUIRE(read.solved == g.solved && read.self_sig == g.self_sig);
    REQUIRE(Genome::crc16((const uint8_t *)"123456789", 9) == 0x29B1);
    g.forgetStep(0); REQUIRE(g.note[0] == 0x48 && (g.solved & (1u << 8)));
    g.forgetStep(8); REQUIRE(g.note[0] == 0);
    g.generation = 65535; seal(g);
    REQUIRE(g.child().generation == 65535);
    Genome child = g; child.adoptParent(g); seal(child);
    REQUIRE(child.generation == 65535 && child.parent_sig == g.self_sig);
}

static void genomeRejectsInvalid() {
    Genome saved = parent(), read = saved;
    uint8_t original[genome::SIZE], bytes[genome::SIZE];
    REQUIRE(saved.serialize(original, sizeof(original)));
    std::memcpy(bytes, original, sizeof(bytes)); bytes[15] ^= 1;
    REQUIRE(!read.deserialize(bytes, sizeof(bytes)));
    REQUIRE(read.serialize(bytes, sizeof(bytes)));
    REQUIRE(std::memcmp(bytes, original, sizeof(bytes)) == 0);
    REQUIRE(!read.deserialize(original, sizeof(original) - 1));
    Genome bad = saved; bad.version = 1; bad.serialize(bytes, sizeof(bytes));
    REQUIRE(!read.deserialize(bytes, sizeof(bytes)));
    bad = saved; bad.solved |= 0x8000; seal(bad); bad.serialize(bytes, sizeof(bytes));
    REQUIRE(!read.deserialize(bytes, sizeof(bytes)));
    bad = saved; bad.fitness = 10000; bad.self_sig = 0; bad.serialize(bytes, sizeof(bytes));
    bad.self_sig = Genome::sign(bytes, genome::PAYLOAD_END); bad.serialize(bytes, sizeof(bytes));
    REQUIRE(!read.deserialize(bytes, sizeof(bytes)));
    bad = saved; bad.self_sig ^= 1; bad.serialize(bytes, sizeof(bytes));
    REQUIRE(!read.deserialize(bytes, sizeof(bytes)));
}

static void brainFullRange() {
    int8_t weights[40], inputs[8], outputs[2];
    std::fill_n(weights, 32, 60); std::fill_n(weights + 32, 8, 1);
    std::fill_n(inputs, 8, 127); brain::forward(weights, inputs, outputs);
    REQUIRE(outputs[0] == 127 && outputs[1] == 127);
    std::fill_n(weights, 40, 127); std::fill_n(inputs, 8, 1);
    brain::forward(weights, inputs, outputs);
    REQUIRE(outputs[0] == 127 && outputs[1] == 127);
    std::fill_n(inputs, 8, -128); brain::forward(weights, inputs, outputs);
    REQUIRE(outputs[0] == -127 && outputs[1] == -127);
}

static void udpRoundtrip() {
    WormNet net; REQUIRE(net.connect());
    Frame expected = offer(parent()); enqueue(expected);
    REQUIRE(net.hasPending()); Frame first; REQUIRE(net.receive(&first, 30));
    REQUIRE(first.plen == genome::SIZE && first.peer_ip == peer);
    REQUIRE(std::memcmp(first.payload, expected.payload, genome::SIZE) == 0);
    enqueue(control(MsgType::HELLO, other)); Frame second;
    REQUIRE(net.receive(&second, 30));
    REQUIRE(std::memcmp(first.payload, expected.payload, genome::SIZE) == 0);
    Genome read; REQUIRE(WormNet::extractGenome(first, &read));
    REQUIRE(!WormNet::extractGenome(first, nullptr));
    ++first.plen; REQUIRE(!WormNet::extractGenome(first, &read));
}

static void udpRejectsMalformed() {
    WormNet net; REQUIRE(net.connect());
    auto valid = packet(control(MsgType::HELLO));
    auto bad = valid; bad.bytes[0] ^= 1; testio::incoming.push_back(bad);
    bad = valid; bad.bytes.pop_back(); testio::incoming.push_back(bad);
    bad = valid; bad.bytes.push_back(0); testio::incoming.push_back(bad);
    bad = valid; bad.bytes.back() ^= 1; testio::incoming.push_back(bad);
    Frame unknown = control((MsgType)7); testio::incoming.push_back(packet(unknown));
    bad = valid; bad.bytes.resize(1000, 0); testio::incoming.push_back(bad);
    Frame extra = control(MsgType::ACK); extra.plen = 1; enqueue(extra);
    enqueue(control(MsgType::HELLO, other, 999));
    Frame read; REQUIRE(net.receive(&read, 100)); REQUIRE(read.src_sig == 999);
}

static void udpNoiseTimeout() {
    WormNet net; REQUIRE(net.connect());
    testio::onPoll = [] { testio::incoming.push_back({peer, {0, 1, 2}}); };
    uint32_t start = testio::now; Frame read;
    REQUIRE(!net.receive(&read, 20)); REQUIRE(testio::now - start < 30);
    testio::now = UINT32_MAX - 5;
    start = testio::now; REQUIRE(!net.receive(&read, 20)); REQUIRE(testio::now - start < 30);
}

static void networkFailures() {
    WormNet net; REQUIRE(net.connect());
    REQUIRE(!net.sendTo(peer, MsgType::OFFER, 1, nullptr, 1));
    testio::shortWrite = true; REQUIRE(!net.send(MsgType::HELLO, 1, nullptr, 0));
    REQUIRE(testio::sent.empty()); testio::shortWrite = false;
    testio::packetEnd = 0; REQUIRE(!net.send(MsgType::HELLO, 1, nullptr, 0));
    WiFi.statusValue = 0; REQUIRE(!net.isReady()); REQUIRE(!net.hasPending());
    REQUIRE(!net.send(MsgType::HELLO, 1, nullptr, 0));
    testio::packetEnd = 1; WiFi.autoConnect = false;
    REQUIRE(!net.connect()); WiFi.statusValue = WL_CONNECTED;
    REQUIRE(!net.isReady()); // UDP must be rebound after a failed reconnect.
    WiFi.autoConnect = true; REQUIRE(net.connect()); REQUIRE(net.isReady());
}

static void peerTableWraparound() {
    WormNet net;
    for (int i = 0; i < 8; ++i) net.notePeer(IPAddress(10, 0, 0, (uint8_t)(10 + i)), UINT32_MAX - 20 + i);
    net.notePeer(other, 10);
    IPAddress peers[8]; REQUIRE(net.livePeers(10, 100, peers, 8) == 8);
    REQUIRE(peers[0] == other); // The oldest pre-wrap entry must be replaced.
    REQUIRE(net.livePeers(10, 100, nullptr, 8) == 0);
}

static void senderDefersWithoutLooping() {
    WormNet net; Replicator repl; REQUIRE(net.connect());
    Genome mine = parent(), foreign = parent(8);
    testio::onSend = [&](const testio::Packet &p) {
        if (typeOf(p) == MsgType::HANDSHAKE) enqueue(offer(foreign, other));
    };
    uint32_t start = testio::now;
    REQUIRE(!repl.runSender(net, peer, mine, mine.self_sig, 20));
    REQUIRE(testio::now - start < 35); REQUIRE(repl.hasDeferred());
    testio::onSend = {}; start = testio::now;
    REQUIRE(!repl.runSender(net, peer, mine, mine.self_sig, 20));
    REQUIRE(testio::now - start < 35); REQUIRE(repl.hasDeferred());
    Frame read; REQUIRE(repl.popDeferred(&read)); REQUIRE(read.peer_ip == other);
    REQUIRE(!repl.hasDeferred());
}

static void senderConfirmed() {
    WormNet net; Replicator repl; REQUIRE(net.connect());
    Genome mine = parent(), foreign = parent(9);
    testio::onSend = [&](const testio::Packet &p) {
        if (typeOf(p) == MsgType::HANDSHAKE) {
            enqueue(offer(foreign, other)); enqueue(control(MsgType::ACK));
        } else if (typeOf(p) == MsgType::OFFER) {
            enqueue(control(MsgType::ACK, other)); enqueue(control(MsgType::ACK));
        }
    };
    REQUIRE(repl.runSender(net, IPAddress(255, 255, 255, 255), mine, mine.self_sig, 50));
    REQUIRE(sentCount(MsgType::OFFER) == 1 && sentCount(MsgType::BYE) == 1);
    REQUIRE(testio::sent[1].ip == peer); REQUIRE(repl.hasDeferred());
    REQUIRE(typeOf(testio::sent.back()) == MsgType::BYE);
}

static void senderNoiseTimeouts() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); Genome mine = parent();
    testio::onPoll = [] { enqueue(control(MsgType::HELLO)); };
    uint32_t start = testio::now;
    REQUIRE(!repl.runSender(net, peer, mine, mine.self_sig, 20));
    REQUIRE(testio::now - start < 40); REQUIRE(sentCount(MsgType::OFFER) == 0);
    testio::onSend = [](const testio::Packet &p) {
        if (typeOf(p) == MsgType::HANDSHAKE) enqueue(control(MsgType::ACK));
    };
    start = testio::now;
    REQUIRE(!repl.runSender(net, peer, mine, mine.self_sig, 30));
    REQUIRE(testio::now - start < (uint32_t)net::RX_CONFIRM_MS + 40);
}

static void senderWriteFailures() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); Genome mine = parent();
    testio::packetBegin = false; uint32_t start = testio::now;
    REQUIRE(!repl.runSender(net, peer, mine, mine.self_sig, 3000));
    REQUIRE(testio::now - start < 10);
    testio::packetBegin = true;
    testio::onSend = [](const testio::Packet &p) {
        if (typeOf(p) == MsgType::HANDSHAKE) {
            enqueue(control(MsgType::ACK)); testio::packetEnd = 0;
        }
    };
    start = testio::now;
    REQUIRE(!repl.runSender(net, peer, mine, mine.self_sig, 3000));
    REQUIRE(testio::now - start < 20);
}

static void receiverRefreshes() {
    WormNet net; Replicator repl; REQUIRE(net.connect());
    Genome source = parent(), child; Frame first = offer(source);
    REQUIRE(repl.runReceiver(net, &child, 55, 30, &first));
    REQUIRE(child.generation == source.generation + 1 && child.parent_sig == source.self_sig);
    REQUIRE(child.fitness == child.computeFitness()); REQUIRE(child.check());
    REQUIRE(child.self_sig != source.self_sig); REQUIRE(sentCount(MsgType::ACK) == 1);
    uint8_t bytes[genome::SIZE]; child.serialize(bytes, sizeof(bytes));
    Genome read; REQUIRE(read.deserialize(bytes, sizeof(bytes)));
}

static void receiverRecoversAfterBadOffer() {
    WormNet net; Replicator repl; REQUIRE(net.connect());
    Genome source = parent(), child; Frame bad = offer(source); bad.payload[15] ^= 1;
    enqueue(offer(source));
    REQUIRE(repl.runReceiver(net, &child, 55, 30, &bad));
    REQUIRE(child.parent_sig == source.self_sig && sentCount(MsgType::ACK) == 1);
}

static void receiverBindsSender() {
    WormNet net; Replicator repl; REQUIRE(net.connect());
    Genome source = parent(), foreign = parent(100), child;
    foreign.learnStep(0); seal(foreign);
    Frame first = control(MsgType::HANDSHAKE, peer, source.self_sig);
    enqueue(offer(foreign, other, MsgType::NOTE)); enqueue(offer(source));
    REQUIRE(repl.runReceiver(net, &child, 55, 40, &first));
    REQUIRE(child.generation == 8 && child.solved == source.solved);
    REQUIRE(child.parent_sig == source.self_sig && repl.hasDeferred());
    Frame deferred; REQUIRE(repl.popDeferred(&deferred)); REQUIRE(deferred.peer_ip == other);
    enqueue(offer(foreign)); // Same IP, different handshake fingerprint.
    uint32_t start = testio::now;
    REQUIRE(!repl.runReceiver(net, &child, 55, 20, &first));
    REQUIRE(testio::now - start < 35); REQUIRE(repl.hasDeferred());
}

static void receiverSameGenomeAndNullOutput() {
    WormNet net; Replicator repl; REQUIRE(net.connect());
    Genome source = parent(), child; Frame first = offer(source);
    REQUIRE(repl.runReceiver(net, &child, source.self_sig, 20, &first));
    const int acks = sentCount(MsgType::ACK);
    REQUIRE(!repl.runReceiver(net, nullptr, 55, 20, &first));
    REQUIRE(sentCount(MsgType::ACK) == acks);
}

static void wormGenerationAndHeartbeat() {
    Worm worm; worm.begin();
    testio::forceZeroRandom = true; // Exercise a permitted send, independently of the 75% exploration draw.
    Genome source = parent(10);
    for (int step = 0; step < genome::N_STEPS; ++step) source.learnStep(step);
    seal(source); enqueue(offer(source));
    testio::now = 100; worm.loop(); REQUIRE(worm.generations() == 11);
    source.generation = 2; seal(source); enqueue(offer(source));
    testio::now = 200; worm.loop(); REQUIRE(worm.generations() == 3);
    source.generation = 65535; seal(source); enqueue(offer(source));
    testio::now = 300; worm.loop(); REQUIRE(worm.generations() == 65535);
    testio::onSend = [](const testio::Packet &p) {
        if (typeOf(p) == MsgType::HANDSHAKE || typeOf(p) == MsgType::OFFER) enqueue(control(MsgType::ACK));
    };
    testio::now = 6000; enqueue(control(MsgType::HELLO)); worm.loop();
    REQUIRE(sentCount(MsgType::HELLO) > 0); REQUIRE(sentCount(MsgType::OFFER) == 1);
    REQUIRE(worm.generations() == 65535);
    for (const auto &p : testio::sent) {
        if (typeOf(p) != MsgType::OFFER) continue;
        Genome outgoing;
        REQUIRE(outgoing.deserialize(p.bytes.data() + net::HDR_SIZE, genome::SIZE));
        REQUIRE(outgoing.generation == 65535 && outgoing.check());
    }
}

int main(int argc, char **argv) {
    const struct { const char *name; void (*run)(); } cases[] = {
        {"genome_roundtrip", genomeRoundtrip}, {"genome_rejects_invalid", genomeRejectsInvalid},
        {"brain_full_range", brainFullRange}, {"udp_roundtrip", udpRoundtrip},
        {"udp_rejects_malformed", udpRejectsMalformed}, {"udp_noise_timeout", udpNoiseTimeout},
        {"network_failures", networkFailures}, {"peer_table_wraparound", peerTableWraparound},
        {"sender_defers_without_looping", senderDefersWithoutLooping}, {"sender_confirmed", senderConfirmed},
        {"sender_noise_timeouts", senderNoiseTimeouts}, {"sender_write_failures", senderWriteFailures},
        {"receiver_refreshes", receiverRefreshes}, {"receiver_recovers_after_bad_offer", receiverRecoversAfterBadOffer},
        {"receiver_binds_sender", receiverBindsSender}, {"receiver_same_genome_and_null_output", receiverSameGenomeAndNullOutput},
        {"worm_generation_and_heartbeat", wormGenerationAndHeartbeat},
    };
    int count = 0, failures = 0;
    for (const auto &test : cases) {
        if (argc > 1 && test.name != std::string(argv[1])) continue;
        testio::reset(); ++count;
        std::cout << "RUN " << test.name << std::endl;
        try { test.run(); }
        catch (const std::exception &error) {
            ++failures; std::cerr << "FAIL: " << error.what() << std::endl;
            for (const auto &line : testio::logs) std::cerr << line << std::endl;
        }
    }
    std::cout << count - failures << "/" << count << " regression cases passed" << std::endl;
    return failures || count == 0 ? 1 : 0;
}
