// Genome.cpp —— 基因组序列化 / 校验 / 进化操作
#include "Genome.h"

namespace genome {

// ---------------- 序列化 ----------------
bool Genome::serialize(uint8_t *buf, size_t len) const {
    if (!buf || len < SIZE) return false;
    buf[0]  = version;
    buf[1]  = generation & 0xFF;
    buf[2]  = (generation >> 8) & 0xFF;
    buf[3]  = fitness & 0xFF;
    buf[4]  = (fitness >> 8) & 0xFF;
    buf[5]  = solved & 0xFF;
    buf[6]  = (solved >> 8) & 0xFF;
    buf[7]  = parent_sig & 0xFF;
    buf[8]  = (parent_sig >> 8) & 0xFF;
    buf[9]  = (parent_sig >> 16) & 0xFF;
    buf[10] = (parent_sig >> 24) & 0xFF;
    buf[11] = self_sig & 0xFF;
    buf[12] = (self_sig >> 8) & 0xFF;
    buf[13] = (self_sig >> 16) & 0xFF;
    buf[14] = (self_sig >> 24) & 0xFF;
    for (int i = 0; i < WEIGHT_COUNT; ++i) buf[WEIGHT_OFF + i] = (uint8_t)weights[i];
    for (int i = 0; i < 8; ++i)            buf[NOTE_OFF + i]   = (uint8_t)note[i];

    uint16_t c = crc16(buf, PAYLOAD_END);
    buf[CRC_OFF]     = c & 0xFF;
    buf[CRC_OFF + 1] = (c >> 8) & 0xFF;
    return true;
}

bool Genome::deserialize(const uint8_t *buf, size_t len) {
    if (!buf || len != SIZE || buf[0] != VERSION) return false;
    const uint16_t c = crc16(buf, PAYLOAD_END);
    if (c != (uint16_t)(buf[CRC_OFF] | (buf[CRC_OFF + 1] << 8))) return false;

    // 校验全部通过后才更新对象，坏帧不会污染已有基因组。
    Genome next;
    next.version     = buf[0];
    next.generation  = buf[1] | (buf[2] << 8);
    next.fitness     = buf[3] | (buf[4] << 8);
    next.solved      = (uint16_t)(buf[5] | (buf[6] << 8));
    next.parent_sig  = buf[7] | ((uint32_t)buf[8] << 8) | ((uint32_t)buf[9] << 16) | ((uint32_t)buf[10] << 24);
    next.self_sig    = buf[11] | ((uint32_t)buf[12] << 8) | ((uint32_t)buf[13] << 16) | ((uint32_t)buf[14] << 24);
    for (int i = 0; i < WEIGHT_COUNT; ++i) next.weights[i] = (int8_t)buf[WEIGHT_OFF + i];
    for (int i = 0; i < 8; ++i) next.note[i] = (int8_t)buf[NOTE_OFF + i];
    if (!next.check()) return false;
    *this = next;
    return true;
}

// ---------------- CRC16 (CCITT-FALSE) ----------------
uint16_t Genome::crc16(const uint8_t *buf, size_t len) {
    return crc16Of(buf, len);
}

uint16_t Genome::crc16Of(const uint8_t *buf, size_t len) {
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= ((uint16_t)*buf++) << 8;
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
            else              crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

bool Genome::check() const {
    const uint16_t solvedMask = (uint16_t)((1u << N_STEPS) - 1u);
    if (version != VERSION || (solved & ~solvedMask) || fitness != computeFitness()) return false;
    uint8_t buf[SIZE];
    if (!serialize(buf, SIZE)) return false;
    for (int i = 11; i < 15; ++i) buf[i] = 0; // 指纹计算时自身签名字段清零
    return self_sig == sign(buf, PAYLOAD_END);
}

// ---------------- 身份签名 ----------------
void Genome::refresh() {
    fitness = computeFitness();
    self_sig = 0;
    uint8_t buf[SIZE];
    serialize(buf, sizeof(buf));
    self_sig = sign(buf, PAYLOAD_END);
}

uint32_t Genome::sign(const uint8_t *buf, size_t len) {
    // 简易 FNV-1a，作为基因指纹 / 防重复感染
    uint32_t h = 0x811c9dc5u;
    while (len--) {
        h ^= *buf++;
        h *= 0x01000193u;
    }
    return h;
}

// ---------------- 进化操作 ----------------
void Genome::mutate(unsigned int seed, float rate) {
    // xorshift32 轻量 PRNG（不依赖 STL rand，跨平台可复现）
    uint32_t s = seed ? seed : 0x9E3779B9u;
    auto next = [&s]() { uint32_t r = s; r ^= r << 13; r ^= r >> 17; r ^= r << 5; s = r; return r; };
    auto roll = [&next]() { return ((float)(next() % 10000)) / 10000.0f; };

    for (int i = 0; i < WEIGHT_COUNT; ++i) {
        if (roll() < rate) {
            // 对称扰动：-15..+15（修复旧版 -31..0 只减不增的负漂移）
            int v = (int)weights[i] + (int)(next() % 31) - 15;
            if (v > 127)  v = 127;
            if (v < -128) v = -128;
            weights[i] = (int8_t)v;
        }
    }
    // 笔记轻微遗忘
    for (int i = 0; i < 8; ++i) {
        if (roll() < evo::NOTE_DECAY) note[i] = (int8_t)(note[i] / 2);
    }
}

void Genome::learnStep(uint8_t step) {
    if (step >= N_STEPS) return;
    solved |= (uint16_t)(1u << step);
    // 学会该步 => 把对应经验记进 note（拉马克）；标记含步号，8/9 步不再与 0/1 步混淆
    int slot = (step % 8);
    note[slot] = (int8_t)(0x40 + step);
}

void Genome::forgetStep(uint8_t step) {
    if (step >= N_STEPS) return;
    solved &= (uint16_t)~(1u << step);
    int slot = (step % 8);
    if (note[slot] == (int8_t)(0x40 + step)) note[slot] = 0; // 只清自己的标记
}

uint8_t Genome::stepsSolved() const {
    uint8_t n = 0;
    for (int i = 0; i < N_STEPS; ++i) if (solved & (1u << i)) ++n;
    return n;
}

// 一次完整复制（N_STEPS 步）的连乘成功率：
//   已学会的步走 STEP_MASTERED(0.98)，未学会的步走 STEP_NAIVE(0.92)
//   0 学会: 0.92^10 ≈ 43.4%   10 学会: 0.98^10 ≈ 81.7%
float Genome::pipelineRate() const {
    float r = 1.0f;
    for (int i = 0; i < N_STEPS; ++i) {
        r *= (solved & (1u << i)) ? evo::STEP_MASTERED : evo::STEP_NAIVE;
    }
    return r;
}

// fitness = 连乘成功率 × 10000（0..10000），作为跨节点可比的适应度
uint16_t Genome::computeFitness() const {
    return (uint16_t)(pipelineRate() * 10000.0f + 0.5f);
}

// ---------------- 遗传 ----------------
Genome Genome::child() const {
    Genome g = *this;     // 值拷贝（POD）
    g.generation = (generation >= 0xFFFFu) ? generation : generation + 1;
    g.parent_sig = self_sig;
    g.self_sig   = 0;     // 重新签名
    g.version    = VERSION;
    return g;
}

void Genome::adoptParent(const Genome &parent) {
    parent_sig = parent.self_sig;
    if (generation < parent.generation) generation = parent.generation;
    if (generation < 0xFFFFu) ++generation;
    self_sig = 0; // 孵化变异结束后调用 refresh()
}

void Genome::reseedNote() {
    for (int i = 0; i < 8; ++i) note[i] = 0;
}

} // namespace genome
