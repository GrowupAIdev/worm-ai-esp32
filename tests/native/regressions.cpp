#include "brain/Genome.h"
#include "brain/Brain.h"
#include "net/WormNet.h"
#include "repl/Replicator.h"
#include "worm/Worm.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
using genome::Genome;
using net::Frame;
using net::WormNet;
const IPAddress peer(10,0,0,2), other(10,0,0,3);
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (0)

static void seal(Genome &g) {
    g.fitness = g.computeFitness(); g.self_sig = 0;
    uint8_t bytes[genome::SIZE]; REQUIRE(g.serialize(bytes, sizeof(bytes)));
    g.self_sig = Genome::sign(bytes, genome::PAYLOAD_END);
}
static Genome parent(uint16_t generation = 7) {
    Genome g; g.generation = generation; g.learnStep(8); g.learnStep(9);
    for (int i = 0; i < genome::WEIGHT_COUNT; ++i) g.weights[i] = (int8_t)(i - 20);
    seal(g); return g;
}
static Frame control(MsgType type, IPAddress ip = peer, uint32_t sig = 123, uint32_t session = 1) {
    Frame f; f.type = type; f.peer_ip = ip; f.src_sig = sig;
    f.session = type == MsgType::HELLO ? 0 : session;
    return f;
}
static Frame ack(MsgType stage, uint32_t session, IPAddress ip = peer) {
    Frame f = control(MsgType::ACK, ip, 123, session);
    f.plen = 1; f.payload[0] = (uint8_t)stage; return f;
}
static Frame offer(const Genome &g, IPAddress ip = peer, uint32_t session = 1) {
    Frame f = control(MsgType::OFFER, ip, g.self_sig, session);
    f.plen = genome::SIZE; REQUIRE(g.serialize(f.payload, sizeof(f.payload))); return f;
}
static uint32_t u32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static testio::Packet packet(const Frame &f) {
    std::vector<uint8_t> b(net::HDR_SIZE + f.plen + 2);
    b[0] = 'W'; b[1] = 'R'; b[2] = (uint8_t)f.type; b[3] = net::WIRE_VERSION;
    for (int i = 0; i < 4; ++i) { b[4+i] = (uint8_t)(f.src_sig >> (i*8)); b[8+i] = (uint8_t)(f.session >> (i*8)); }
    b[12] = (uint8_t)f.plen; b[13] = (uint8_t)(f.plen >> 8);
    std::copy_n(f.payload, f.plen, b.begin() + net::HDR_SIZE);
    const uint16_t crc = Genome::crc16(b.data(), b.size() - 2);
    b[b.size()-2] = (uint8_t)crc; b.back() = (uint8_t)(crc >> 8); return {f.peer_ip,b};
}
static void enqueue(const Frame &f) { testio::incoming.push_back(packet(f)); }
static MsgType typeOf(const testio::Packet &p) { return (MsgType)p.bytes.at(2); }
static uint32_t sessionOf(const testio::Packet &p) { return u32(p.bytes.data()+8); }
static int sentCount(MsgType type) {
    int count = 0; for (const auto &p : testio::sent) if (typeOf(p) == type) ++count; return count;
}
static void exhaust(Replicator &repl, WormNet &net) {
    for (int i = 0; i < 30 && repl.busy(); ++i) { testio::now += 50; repl.poll(net, 55); }
    REQUIRE(!repl.busy());
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
    WormNet net; REQUIRE(net.connect()); Frame expected = offer(parent());
    enqueue(expected); REQUIRE(net.hasPending()); Frame first; REQUIRE(net.poll(&first));
    REQUIRE(first.session == 1 && first.plen == genome::SIZE && first.peer_ip == peer);
    REQUIRE(std::memcmp(first.payload, expected.payload, genome::SIZE) == 0);
    enqueue(control(MsgType::HELLO, other)); Frame second; REQUIRE(net.poll(&second));
    REQUIRE(std::memcmp(first.payload, expected.payload, genome::SIZE) == 0);
    Genome g; REQUIRE(WormNet::extractGenome(first, &g));
    REQUIRE(!WormNet::extractGenome(first, nullptr)); ++first.plen; REQUIRE(!WormNet::extractGenome(first, &g));
    REQUIRE(net.sendTo(peer, MsgType::OFFER, expected.src_sig, expected.payload, expected.plen, expected.session));
    REQUIRE(testio::sent.back().bytes == packet(expected).bytes);
}
static void udpRejectsMalformed() {
    WormNet net; REQUIRE(net.connect());
    auto valid = packet(control(MsgType::HELLO));
    auto bad = valid; bad.bytes[0] ^= 1; testio::incoming.push_back(bad);
    bad = valid; bad.bytes.pop_back(); testio::incoming.push_back(bad);
    bad = valid; bad.bytes.push_back(0); testio::incoming.push_back(bad);
    bad = valid; bad.bytes.back() ^= 1; testio::incoming.push_back(bad);
    bad = valid; bad.bytes[3] = 2; testio::incoming.push_back(bad);
    bad = valid; bad.bytes.resize(1000,0); testio::incoming.push_back(bad);
    enqueue(control((MsgType)7)); enqueue(control(MsgType::ACK)); // Empty legacy ACK.
    Frame invalidAck = ack(MsgType::HELLO,1); enqueue(invalidAck);
    Frame zeroId = offer(parent()); zeroId.session = 0; enqueue(zeroId);
    Frame shortOffer = offer(parent()); --shortOffer.plen; enqueue(shortOffer);
    enqueue(control(MsgType::HELLO,other,999));
    Frame read; REQUIRE(net.receive(&read,100)); REQUIRE(read.src_sig == 999);
}
static void udpNoiseTimeout() {
    WormNet net; REQUIRE(net.connect());
    testio::onPoll = [] { testio::incoming.push_back({peer,{0,1,2}}); };
    uint32_t start = testio::now; Frame read;
    REQUIRE(!net.receive(&read,20)); REQUIRE(testio::now-start < 35);
    testio::now = UINT32_MAX-5; start = testio::now;
    REQUIRE(!net.receive(&read,20)); REQUIRE(testio::now-start < 35);
}
static void networkFailures() {
    WormNet net; REQUIRE(net.connect());
    REQUIRE(!net.sendTo(peer,MsgType::OFFER,1,nullptr,1,1));
    testio::shortWrite = true; REQUIRE(!net.send(MsgType::HELLO,1,nullptr,0)); REQUIRE(testio::sent.empty());
    testio::shortWrite = false; testio::packetEnd = 0; REQUIRE(!net.send(MsgType::HELLO,1,nullptr,0));
    WiFi.statusValue = 0; REQUIRE(!net.isReady()); REQUIRE(!net.hasPending());
    testio::packetEnd = 1; WiFi.autoConnect = false; REQUIRE(!net.connect());
    WiFi.statusValue = WL_CONNECTED; REQUIRE(!net.isReady());
    WiFi.autoConnect = true; REQUIRE(net.connect());
}
static void peerBackoffAndWraparound() {
    WormNet net;
    for (int i=0;i<8;++i) net.notePeer(IPAddress(10,0,0,(uint8_t)(10+i)),UINT32_MAX-20+i);
    net.notePeer(other,10); IPAddress peers[8]; REQUIRE(net.livePeers(10,100,peers,8)==8); REQUIRE(peers[0]==other);
    REQUIRE(net.livePeers(10,100,nullptr,8)==0);
    net.noteTransfer(other,false,UINT32_MAX-5);
    REQUIRE(!net.canContact(other,100)); REQUIRE(net.canContact(other,1100));
    net.noteTransfer(other,false,1100);
    REQUIRE(!net.canContact(other,2100)); REQUIRE(net.canContact(other,3100));
    net.noteTransfer(other,true,3100); REQUIRE(net.canContact(other,3100));
}
static void directAndCorrelatedAck() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); Genome mine=parent();
    const uint32_t start=testio::now; REQUIRE(repl.start(net,peer,mine)); REQUIRE(testio::now-start<5);
    REQUIRE(sentCount(MsgType::HANDSHAKE)==0 && sentCount(MsgType::OFFER)==1);
    const uint32_t id=repl.session();
    REQUIRE(!repl.start(net,peer,mine)); REQUIRE(!repl.takeResult(nullptr));
    enqueue(ack(MsgType::OFFER,id+1)); enqueue(ack(MsgType::HANDSHAKE,id)); enqueue(ack(MsgType::OFFER,id,other));
    repl.poll(net,55); REQUIRE(repl.busy());
    enqueue(ack(MsgType::OFFER,id)); repl.poll(net,55); REQUIRE(!repl.busy());
    bool success=false; REQUIRE(repl.takeResult(&success)&&success); REQUIRE(!repl.takeResult(&success));
    REQUIRE(repl.stats().confirmed==1 && repl.stats().retries==0 && sentCount(MsgType::BYE)==0);
    REQUIRE(repl.start(net,peer,mine)); REQUIRE(repl.session()!=id);
    enqueue(ack(MsgType::OFFER,id)); repl.poll(net,55); REQUIRE(repl.busy());
}
static void discoveryStages() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); Genome mine=parent();
    REQUIRE(repl.start(net,IPAddress(255,255,255,255),mine)); uint32_t id=repl.session();
    REQUIRE(sentCount(MsgType::HANDSHAKE)==1 && sentCount(MsgType::OFFER)==0);
    enqueue(ack(MsgType::OFFER,id)); repl.poll(net,55); REQUIRE(sentCount(MsgType::OFFER)==0);
    enqueue(ack(MsgType::HANDSHAKE,id,other)); repl.poll(net,55);
    REQUIRE(sentCount(MsgType::OFFER)==1 && repl.target()==other);
    enqueue(ack(MsgType::HANDSHAKE,id,other)); repl.poll(net,55); REQUIRE(repl.busy());
    enqueue(ack(MsgType::OFFER,id,other)); repl.poll(net,55);
    bool success=false; REQUIRE(repl.takeResult(&success)&&success);
}
static void retransmissionSnapshot() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); Genome mine=parent();
    REQUIRE(repl.start(net,peer,mine)); auto original=testio::sent[0].bytes;
    mine.learnStep(0); mine.refresh(); testio::now+=250; repl.poll(net,mine.self_sig);
    REQUIRE(testio::sent.size()==2 && testio::sent.back().bytes==original);
    enqueue(ack(MsgType::OFFER,repl.session())); repl.poll(net,mine.self_sig);
    bool success=false; REQUIRE(repl.takeResult(&success)&&success); REQUIRE(repl.stats().retries==1);
}
static void boundedFailureAndDisconnect() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); Genome mine=parent();
    uint32_t start=testio::now; REQUIRE(repl.start(net,peer,mine)); exhaust(repl,net);
    bool success=true; REQUIRE(repl.takeResult(&success)&&!success);
    REQUIRE(sentCount(MsgType::OFFER)==3 && repl.stats().retries==2);
    REQUIRE(testio::now-start<net::TX_STAGE_MS+100);
    REQUIRE(repl.start(net,peer,mine)); WiFi.statusValue=0; repl.poll(net,55);
    REQUIRE(repl.takeResult(&success)&&!success);
    WiFi.statusValue=WL_CONNECTED; testio::packetBegin=false;
    REQUIRE(repl.start(net,peer,mine)); exhaust(repl,net);
    REQUIRE(repl.takeResult(&success)&&!success);
}
static void lateAckDoesNotConfirm() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); REQUIRE(repl.start(net,peer,parent()));
    enqueue(ack(MsgType::OFFER,repl.session())); testio::now+=net::TX_STAGE_MS+1;
    repl.poll(net,55); bool success=true; REQUIRE(repl.takeResult(&success)&&!success);
}
static void boundedPollUnderTraffic() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); REQUIRE(repl.start(net,peer,parent()));
    testio::onPoll=[] { enqueue(control(MsgType::HELLO)); };
    const uint32_t start=testio::now; repl.poll(net,55);
    REQUIRE(testio::now-start<25); REQUIRE(repl.busy());
    exhaust(repl,net); bool success=true; REQUIRE(repl.takeResult(&success)&&!success);
}
static void receiveAndDeduplicate() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); Genome source=parent(),child;
    enqueue(offer(source)); repl.poll(net,source.self_sig); REQUIRE(repl.takeReceived(&child));
    REQUIRE(child.generation==8 && child.parent_sig==source.self_sig && child.check() && child.self_sig!=source.self_sig);
    enqueue(offer(source)); enqueue(offer(source)); repl.poll(net,55);
    REQUIRE(!repl.takeReceived(&child)); REQUIRE(repl.stats().received==1 && repl.stats().duplicates==2);
    REQUIRE(sentCount(MsgType::ACK)==3);
    REQUIRE(testio::sent.back().bytes[net::HDR_SIZE]==(uint8_t)MsgType::OFFER);
    Genome different=parent(8); enqueue(offer(different)); repl.poll(net,55);
    REQUIRE(repl.stats().invalid==1 && sentCount(MsgType::ACK)==3);
    enqueue(offer(source,other)); repl.poll(net,55); REQUIRE(repl.takeReceived(&child));
    testio::now+=net::DEDUP_TTL_MS+1; enqueue(offer(source)); repl.poll(net,55); REQUIRE(repl.takeReceived(&child));
}
static void invalidOfferAndQueuePressure() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); Genome source=parent(),child;
    Frame bad=offer(source); bad.payload[15]^=1; enqueue(bad); repl.poll(net,55);
    REQUIRE(repl.stats().invalid==1 && sentCount(MsgType::ACK)==0);
    for (int i=1;i<=5;++i) enqueue(offer(source,peer,(uint32_t)i));
    repl.poll(net,55); REQUIRE(repl.stats().received==4 && repl.stats().overflow==1 && sentCount(MsgType::ACK)==4);
    enqueue(offer(source)); repl.poll(net,55); REQUIRE(sentCount(MsgType::ACK)==5); // Duplicate is ACKed even when full.
    REQUIRE(!repl.takeReceived(nullptr)); REQUIRE(repl.takeReceived(&child));
    enqueue(offer(source,peer,5)); repl.poll(net,55); REQUIRE(repl.stats().received==5 && sentCount(MsgType::ACK)==6);
}
static void receiverWhileSending() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); Genome mine=parent(),incoming=parent(100),child;
    REQUIRE(repl.start(net,peer,mine)); enqueue(offer(incoming,other,77));
    repl.poll(net,mine.self_sig); REQUIRE(repl.busy() && repl.takeReceived(&child));
    REQUIRE(child.parent_sig==incoming.self_sig && sentCount(MsgType::ACK)==1);
    enqueue(control(MsgType::HANDSHAKE,other,999,99)); repl.poll(net,55);
    REQUIRE(sentCount(MsgType::ACK)==2); REQUIRE(testio::sent.back().bytes[net::HDR_SIZE]==(uint8_t)MsgType::HANDSHAKE);
}
static void transactionWraparound() {
    WormNet net; Replicator repl; REQUIRE(net.connect()); testio::now=UINT32_MAX-30;
    REQUIRE(repl.start(net,peer,parent())); uint32_t start=testio::now; exhaust(repl,net);
    bool success=true; REQUIRE(repl.takeResult(&success)&&!success);
    REQUIRE(testio::now-start<net::TX_STAGE_MS+100);
}
static void wormGenerationAndHeartbeat() {
    testio::forceZeroRandom=true; Worm worm; worm.begin();
    Genome source=parent(10); for(int step=0;step<genome::N_STEPS;++step) source.learnStep(step);
    seal(source); enqueue(offer(source)); testio::now=100; worm.loop(); REQUIRE(worm.generations()==11);
    source.generation=2; seal(source); enqueue(offer(source,peer,2)); testio::now=200; worm.loop(); REQUIRE(worm.generations()==3);
    source.generation=65535; seal(source); enqueue(offer(source,peer,3)); testio::now=300; worm.loop(); REQUIRE(worm.generations()==65535);
    testio::onSend=[](const testio::Packet &p) { if(typeOf(p)==MsgType::OFFER) enqueue(ack(MsgType::OFFER,sessionOf(p))); };
    testio::now=6000; enqueue(control(MsgType::HELLO)); worm.loop();
    REQUIRE(sentCount(MsgType::HELLO)>0 && sentCount(MsgType::OFFER)==1);
    worm.loop(); REQUIRE(worm.generations()==65535 && worm.transferStats().confirmed==1);
    for (const auto &p:testio::sent) if(typeOf(p)==MsgType::OFFER) {
        Genome g; REQUIRE(g.deserialize(p.bytes.data()+net::HDR_SIZE,genome::SIZE)); REQUIRE(g.generation==65535);
    }
}
struct TrialNode {
    IPAddress ip;
    WormNet net;
    Replicator repl;
    Genome model;
    std::deque<testio::Packet> inbox;
    int started=0,done=0,confirmed=0;
};
static void networkTrial(int lossPercent) {
    constexpr int N=8, ROUNDS=8;
    std::array<TrialNode,N> nodes;
    for(int i=0;i<N;++i) {
        nodes[i].ip=IPAddress(10,0,0,(uint8_t)(10+i)); WiFi.ip=nodes[i].ip;
        REQUIRE(nodes[i].net.connect()); nodes[i].model=parent((uint16_t)(i+1));
    }
    uint32_t channel=12345; int dropped=0,datagrams=0,bytes=0;
    testio::onSend=[&](const testio::Packet &p) {
        ++datagrams; bytes+=(int)p.bytes.size();
        channel^=channel<<13; channel^=channel>>17; channel^=channel<<5;
        if((int)(channel%100)<lossPercent) { ++dropped; return; }
        for(auto &node:nodes) if(node.ip==p.ip && node.ip!=WiFi.ip) node.inbox.push_back({WiFi.ip,p.bytes});
    };
    const uint32_t start=testio::now; int done=0;
    for(int step=0;step<2500 && done<N*ROUNDS;++step) {
        done=0;
        for(int i=0;i<N;++i) {
            auto &node=nodes[i]; WiFi.ip=node.ip; testio::incoming.swap(node.inbox);
            node.repl.poll(node.net,node.model.self_sig);
            Genome received; while(node.repl.takeReceived(&received)) REQUIRE(received.check());
            bool success=false;
            if(node.repl.takeResult(&success)) { ++node.done; if(success) ++node.confirmed; }
            if(!node.repl.busy() && node.started<ROUNDS) {
                REQUIRE(node.repl.start(node.net,nodes[(i+1)%N].ip,node.model)); ++node.started;
            }
            testio::incoming.swap(node.inbox); done+=node.done;
        }
        testio::now+=5;
    }
    REQUIRE(done==N*ROUNDS);
    int confirmed=0,accepted=0,duplicates=0,retries=0,overflow=0;
    for(const auto &node:nodes) {
        confirmed+=node.confirmed; accepted+=node.repl.stats().received;
        duplicates+=node.repl.stats().duplicates; retries+=node.repl.stats().retries; overflow+=node.repl.stats().overflow;
    }
    REQUIRE(confirmed<=accepted && accepted<=N*ROUNDS && overflow==0);
    REQUIRE(retries<=2*N*ROUNDS && confirmed>0);
    if(lossPercent==0) REQUIRE(confirmed==64 && accepted==64 && datagrams==128 && duplicates==0 && bytes==6272);
    std::cout<<"BENCHMARK {\"nodes\":8,\"transfers\":64,\"loss_percent\":"<<lossPercent
             <<",\"confirmed\":"<<confirmed<<",\"accepted\":"<<accepted<<",\"duplicates\":"<<duplicates
             <<",\"retries\":"<<retries<<",\"datagrams\":"<<datagrams<<",\"bytes\":"<<bytes
             <<",\"dropped\":"<<dropped<<",\"virtual_ms\":"<<testio::now-start<<"}"<<std::endl;
    testio::onSend={};
}
static void zeroLossMesh() { networkTrial(0); }
static void tenPercentLossMesh() { networkTrial(10); }
static void thirtyPercentLossMesh() { networkTrial(30); }
int main(int argc,char **argv) {
    const struct { const char *name; void(*run)(); } cases[] = {
        {"genome_roundtrip",genomeRoundtrip},{"genome_rejects_invalid",genomeRejectsInvalid},{"brain_full_range",brainFullRange},
        {"udp_roundtrip",udpRoundtrip},{"udp_rejects_malformed",udpRejectsMalformed},{"udp_noise_timeout",udpNoiseTimeout},
        {"network_failures",networkFailures},{"peer_backoff_and_wraparound",peerBackoffAndWraparound},
        {"direct_and_correlated_ack",directAndCorrelatedAck},{"discovery_stages",discoveryStages},
        {"retransmission_snapshot",retransmissionSnapshot},{"bounded_failure_and_disconnect",boundedFailureAndDisconnect},
        {"late_ack_does_not_confirm",lateAckDoesNotConfirm},{"bounded_poll_under_traffic",boundedPollUnderTraffic},
        {"receive_and_deduplicate",receiveAndDeduplicate},{"invalid_offer_and_queue_pressure",invalidOfferAndQueuePressure},
        {"receiver_while_sending",receiverWhileSending},{"transaction_wraparound",transactionWraparound},
        {"worm_generation_and_heartbeat",wormGenerationAndHeartbeat},
        {"zero_loss_mesh",zeroLossMesh},{"ten_percent_loss_mesh",tenPercentLossMesh},{"thirty_percent_loss_mesh",thirtyPercentLossMesh},
    };
    int count=0,failures=0;
    for(const auto &test:cases) {
        if(argc>1 && test.name!=std::string(argv[1])) continue;
        testio::reset(); ++count; std::cout<<"RUN "<<test.name<<std::endl;
        try { test.run(); } catch(const std::exception &error) {
            ++failures; std::cerr<<"FAIL: "<<error.what()<<std::endl;
            for(const auto &line:testio::logs) std::cerr<<line<<std::endl;
        }
    }
    std::cout<<count-failures<<"/"<<count<<" regression cases passed"<<std::endl;
    return failures || !count ? 1:0;
}
