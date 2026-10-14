// Worm.cpp —— 蠕虫主状态机：FIND -> PERM -> BUILD -> SPREAD + 拉马克进化
#include "Worm.h"
#include <Arduino.h>
#include <string.h>
#include "brain/Genome.h"
#include "brain/Brain.h"
#include "net/WormNet.h"
#include "repl/Replicator.h"

using namespace net;
using namespace brain;
using namespace genome;

namespace {
constexpr uint32_t HELLO_PERIOD_MS   = 2000;  // 周期性广播「我是一只蠕虫」
constexpr uint32_t COOLDOWN_MS       = 5000;  // 一次复制后的冷却，避免风暴
constexpr uint32_t SCAN_BUDGET_MS    = 150;   // FIND 阶段最多等待这么久
constexpr uint32_t RX_SESSION_MS     = 1500;  // 接收复制会话的预算
constexpr uint32_t PEER_TTL_MS       = 6000;  // 邻居表条目有效期
constexpr uint32_t RECONNECT_MS      = 10000; // 断线重连间隔
} // namespace

Worm::Worm() { /* 空：randomSeed 放在 begin()（setup 内）里调 */ }

// —— 静态辅助：给基因组重新签名并刷新 CRC ——
static void resign(Genome *g) {
    uint8_t buf[genome::SIZE];
    g->self_sig = 0;
    g->serialize(buf, sizeof(buf));
    g->self_sig = Genome::sign(buf, genome::PAYLOAD_END);
    g->serialize(buf, sizeof(buf)); // 再刷一次 CRC
}

void Worm::begin() {
    randomSeed(analogRead(0));

    // 初始基因组：全随机 + 全部「未避坑」
    genome_ = Genome();
    uint32_t seed0 = esp_random();
    genome_.mutate(seed0, 1.0f);
    genome_.reseedNote();
    resign(&genome_);
    gen_ = genome_.generation;

    Serial.println(F("[worm] 蠕虫醒来 (AI Worm begin)"));
    if (!net_.connect()) {
        Serial.println(F("[worm] 网络未就绪，循环里尝试重连"));
    }
}

void Worm::loop() {
    // —— 网络恢复 ——
    if (!net_.isReady()) {
        static uint32_t lastAttempt = 0;
        uint32_t now = millis();
        if (now - lastAttempt >= RECONNECT_MS) {
            lastAttempt = now;
            net_.connect();
        }
        delay(50);
        return;
    }

    uint32_t now = millis();

    // —— 优先处理正在进来的复制会话 / 心跳 ——
    if (net_.hasPending()) {
        handleIncoming(RX_SESSION_MS);
        return;
    }

    // —— 周期性广播存在（HELLO 是 FIND 的一半：别人也能发现我）——
    static uint32_t lastHello = 0;
    if (now - lastHello >= HELLO_PERIOD_MS) {
        lastHello = now;
        net_.send(MsgType::HELLO, genome_.self_sig, nullptr, 0);
    }

    // —— 冷却门：刚复制完就歇一会儿，防止局域网风暴 ——
    if (now - lastAction_ < COOLDOWN_MS) return;

    // ============ 四步循环 ============
    // 1) FIND：选择目标节点（结果记入 lastPeer_）
    if (!step1_scan(SCAN_BUDGET_MS)) {
        return; // 没找到目标，等下次
    }

    // 2) PERM：TinyBrain 根据环境特征决定是否发送
    bool ok = step2_perm();

    // 3) BUILD + SPREAD：把基因组 + 笔记送过去
    if (ok) ok = step3_build(lastPeer_);

    // 4) 拉马克进化：成功=记住经验，失败=忘掉并变异
    step4_spread(ok);
    lastAction_ = millis();
}

bool Worm::step1_scan(uint32_t budgetMs) {
    (void)budgetMs;
    uint32_t now = millis();
    IPAddress peers[net::WormNet::PEER_TABLE];
    int n = net_.livePeers(now, PEER_TTL_MS, peers, net::WormNet::PEER_TABLE);
    peerCount_ = n;

    IPAddress myip = WiFi.localIP();
    for (int i = 0; i < n; ++i) {
        if (peers[i] != myip) {
            lastPeer_ = peers[i];
            char line[64];
            snprintf(line, sizeof(line), "[find] 发现宿主 %d.%d.%d.%d",
                     lastPeer_[0], lastPeer_[1], lastPeer_[2], lastPeer_[3]);
            Serial.println(line);
            return true;
        }
    }

    // 邻居表空：随机猜一个同网段 IP（文章：蠕虫靠「猜」扩散）
    uint8_t last = (uint8_t)random(1, 255);
    lastPeer_ = IPAddress(myip[0], myip[1], myip[2], last);
    Serial.println("[find] 邻居表空，随机猜目标");
    return true; // 猜了就返回 true，让 step2 去验证
}

bool Worm::step2_perm() {
    // PERM：TinyBrain 根据环境特征决定是否发送
    uint32_t seed = esp_random();
    brain::makeInput(seed, input_);

    // 用真实特征覆盖「感觉噪声」的 4 个维度
    input_[0] = (int8_t)(peerCount_ & 0x7F);               // 邻居数量
    input_[1] = (int8_t)genome_.stepsSolved();             // 当前避坑数
    input_[2] = (int8_t)(genome_.fitness >> 8);            // 适应度
    input_[3] = (int8_t)(gen_ & 0x7F);                     // 代数

    int8_t v = brain::decide(genome_.weights, input_);
    bool ok = v >= 0;
    char line[80];
    snprintf(line, sizeof(line), "[perm] brain 决策 %d (peers=%d solved=%d fit=%d) -> %s",
             (int)v, peerCount_, genome_.stepsSolved(), genome_.fitness,
             ok ? "GO" : "SKIP");
    Serial.println(line);
    return ok;
}

bool Worm::step3_build(IPAddress target) {
    Serial.println(F("[build] 开始传输基因组 (BUILD+SPREAD)"));
    bool ok = repl_.runSender(net_, target, genome_, genome_.self_sig,
                              net::RX_BUF_TIMEOUT_MS);
    Serial.println(ok ? F("[build] 复制成功") : F("[build] 复制失败"));
    return ok;
}

bool Worm::step4_spread(bool success) {
    uint32_t seed = esp_random();
    if (success) {
        // 拉马克·成功：这次走通的路，下一代会更顺
        for (int s = 0; s < genome::N_STEPS; ++s) {
            if (random(100) < 80) genome_.learnStep(s); // 大概率把各步都标记为已避坑
        }
        genome_.generation = gen_ + 1;
        gen_ = genome_.generation;
        Serial.println(F("[evo] 拉马克·成功：经验写入，代数+1"));
    } else {
        // 拉马克·失败：忘掉一个坑，再整体变异一下
        int s = random(N_STEPS);
        genome_.forgetStep(s);
        genome_.mutate(seed, evo::MUTATION_RATE);
        genome_.reseedNote();
        char line[80];
        snprintf(line, sizeof(line), "[evo] 拉马克·失败：忘掉 step%d 并变异", s);
        Serial.println(line);
    }
    resign(&genome_); // 签名/CRC 刷新
    return true;
}

bool Worm::handleIncoming(uint32_t budgetMs) {
    Frame f;
    if (!net_.receive(&f, 10)) return false;

    if (f.src_sig == genome_.self_sig) return true; // 自己的广播

    switch (f.type) {
        case MsgType::HELLO:
        case MsgType::BYE:
            // 仅刷新邻居表，已在 receive() 内 notePeer
            return true;

        case MsgType::HANDSHAKE:
        case MsgType::OFFER: {
            // 我是候选宿主：接收对方的基因组
            Genome next;
            bool ok = repl_.runReceiver(net_, &next, genome_.self_sig,
                                        budgetMs, &f);
            if (ok) {
                // 进化抉择：只收养比我强的后代
                if (next.fitness >= genome_.fitness ||
                    next.stepsSolved() >= genome_.stepsSolved()) {
                    genome_ = next;
                    if (next.generation > gen_) gen_ = next.generation;
                    Serial.println(F("[evo] 收养更优基因组"));
                } else {
                    Serial.println(F("[evo] 拒绝弱基因组"));
                }
            }
            return ok;
        }

        default:
            return false;
    }
}
