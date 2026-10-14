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
constexpr uint32_t COOLDOWN_OK_MS    = 1500;  // 复制成功：短冷却，趁热继续传播
constexpr uint32_t COOLDOWN_FAIL_MS  = 5000;  // 复制失败：长冷却，防风暴
constexpr uint32_t COOLDOWN_SKIP_MS  = 1000;  // PERM 拒绝：无交互，稍作停顿即可
constexpr uint32_t SCAN_BUDGET_MS    = 150;   // FIND 阶段最多等待这么久
constexpr uint32_t RX_SESSION_MS     = 1500;  // 接收复制会话的预算
constexpr uint32_t PEER_TTL_MS       = 6000;  // 邻居表条目有效期
constexpr uint32_t RECONNECT_MS      = 10000; // 断线重连间隔
const IPAddress BCAST_IP(255,255,255,255);    // 广播寻宿主目标
} // namespace

Worm::Worm() { /* 空：randomSeed 放在 begin()（setup 内）里调 */ }

void Worm::begin() {
    randomSeed(analogRead(0));

    // 初始基因组：全随机 + 全部「未避坑」
    genome_ = Genome();
    uint32_t seed0 = esp_random();
    genome_.mutate(seed0, 1.0f);
    genome_.reseedNote();
    genome_.refresh();
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

    // —— 优先处理正在进来的复制会话 / 心跳 / 会话期间缓存的延迟帧 ——
    if (net_.hasPending() || repl_.hasDeferred()) {
        handleIncoming(RX_SESSION_MS);
        now = millis(); // 收包不阻止心跳和主动传播，接收等待可能消耗时间
        if (!net_.isReady()) return;
    }

    // —— 周期性广播存在（HELLO 是 FIND 的一半：别人也能发现我）——
    static uint32_t lastHello = 0;
    if (now - lastHello >= HELLO_PERIOD_MS) {
        lastHello = now;
        net_.send(MsgType::HELLO, genome_.self_sig, nullptr, 0);
    }

    // —— 冷却门：按上次动作结果自适应（成功快、失败慢、拒绝稍歇）——
    if (now - lastAction_ < cooldownMs_) return;

    // ============ 四步循环 ============
    // 1) FIND：选择目标节点（已知邻居轮转；表空则广播寻宿主）
    if (!step1_scan(SCAN_BUDGET_MS)) {
        return; // 没找到目标，等下次
    }

    // 2) PERM：TinyBrain 根据环境特征决定是否发送（带探索地板）
    bool attempt = step2_perm();

    if (attempt) {
        // 3) BUILD + SPREAD：把基因组送过去
        bool ok = step3_build(lastPeer_);
        // 4) 拉马克进化：只在真实交互后奖惩（拒绝传播不惩罚）
        step4_spread(ok);
        cooldownMs_ = ok ? COOLDOWN_OK_MS : COOLDOWN_FAIL_MS;
    } else {
        cooldownMs_ = COOLDOWN_SKIP_MS; // 无交互：不遗忘、不变异
    }
    lastAction_ = millis();
}

bool Worm::step1_scan(uint32_t budgetMs) {
    (void)budgetMs;
    uint32_t now = millis();
    IPAddress peers[net::WormNet::PEER_TABLE];
    int n = net_.livePeers(now, PEER_TTL_MS, peers, net::WormNet::PEER_TABLE);
    peerCount_ = n;
    IPAddress myip = WiFi.localIP();

    // 已知邻居：轮转选择，均摊传播目标（原实现总打第一个，其余邻居饿死）
    for (int k = 0; k < n; ++k) {
        int i = (peerCursor_ + k) % n;
        if (peers[i] != myip) {
            peerCursor_ = (i + 1) % n;
            lastPeer_ = peers[i];
            char line[64];
            snprintf(line, sizeof(line), "[find] 发现宿主 %d.%d.%d.%d",
                     lastPeer_[0], lastPeer_[1], lastPeer_[2], lastPeer_[3]);
            Serial.println(line);
            return true;
        }
    }

    // 邻居表空：广播 HANDSHAKE 全网找宿主
    // （任意存活节点回 ACK 即成为真实目标；替代原 1/254 随机猜 IP）
    lastPeer_ = BCAST_IP;
    Serial.println(F("[find] 邻居表空，广播寻找宿主"));
    return true;
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
    // 探索地板：未训练的脑接近抛硬币，保底 PERM_FLOOR_PCT% 概率放行，
    // 维持感染压下限（脑仍可对明显糟糕的环境说“不”）
    bool ok = (v >= 0) ||
              ((uint8_t)(esp_random() % 100) < (unsigned)evo::PERM_FLOOR_PCT);
    char line[80];
    snprintf(line, sizeof(line), "[perm] brain 决策 %d (peers=%d solved=%d fit=%d) -> %s",
             (int)v, peerCount_, genome_.stepsSolved(), genome_.fitness,
             ok ? "GO" : "SKIP");
    Serial.println(line);
    return ok;
}

bool Worm::step3_build(IPAddress target) {
    Serial.println(F("[build] 开始传输基因组 (BUILD+SPREAD)"));
    // 超时分级：已知邻居给足握手时间；广播寻宿用短超时快速失败
    uint32_t t = (target == BCAST_IP) ? (uint32_t)net::RX_BCAST_ACK_MS
                                      : (uint32_t)net::RX_BUF_TIMEOUT_MS;
    bool ok = repl_.runSender(net_, target, genome_, genome_.self_sig, t);
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
        if (genome_.generation < 0xFFFFu) ++genome_.generation;
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
    genome_.refresh();
    return true;
}

bool Worm::handleIncoming(uint32_t budgetMs) {
    Frame f;
    // 优先消费发送会话期间缓存的延迟帧（OFFER/NOTE），其次读 socket
    if (!repl_.popDeferred(&f) && !net_.receive(&f, 10)) return false;

    if (f.peer_ip == WiFi.localIP()) return true; // 自己的广播

    switch (f.type) {
        case MsgType::HELLO:
        case MsgType::BYE:
            // 仅刷新邻居表，已在 receive() 内 notePeer
            return true;

        case MsgType::HANDSHAKE:
        case MsgType::OFFER:
        case MsgType::NOTE: {
            // 我是候选宿主：接收对方的基因组（NOTE 也可能携带来自延迟队列的会话）
            Genome next;
            bool ok = repl_.runReceiver(net_, &next, genome_.self_sig,
                                        budgetMs, &f);
            if (ok) {
                // 进化抉择：适应度不降、且步数不减才收养（fitness 现为真实 pipelineRate×10000）
                if (next.fitness > genome_.fitness ||
                    (next.fitness == genome_.fitness &&
                     next.stepsSolved() >= genome_.stepsSolved())) {
                    genome_ = next;
                    gen_ = next.generation;
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
