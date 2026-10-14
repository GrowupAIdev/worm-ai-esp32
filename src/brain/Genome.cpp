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
    buf[6]  = parent_sig & 0xFF;
    buf[7]  = (parent_sig >> 8) & 0xFF;
    buf[8]  = (parent_sig >> 16) & 0xFF;
    buf[9]  = (parent_sig >> 24) & 0xFF;
    buf[10] = self_sig & 0xFF;
    buf[11] = (self_sig >> 8) & 0xFF;
    buf[12] = (self_sig >> 16) & 0xFF;
    buf[13] = (self_sig >> 24) & 0xFF;
    for (int i = 0; i < WEIGHT_COUNT; ++i) buf[WEIGHT_OFF + i] = (uint8_t)weights[i];
    for (int i = 0; i < 8; ++i)            buf[NOTE_OFF + i]   = (uint8_t)note[i];

    uint16_t c = crc16(buf, PAYLOAD_END);
    buf[CRC_OFF]     = c & 0xFF;
    buf[CRC_OFF + 1] = (c >> 8) & 0xFF;
    return true;
}

bool Genome::deserialize(const uint8_t *buf, size_t len) {
    if (!buf || len < SIZE) return false;
    // 重新按布局解析；直接写回字段
    version     = buf[0];
    generation  = buf[1] | (buf[2] << 8);
    fitness     = buf[3] | (buf[4] << 8);
    solved      = buf[5];
    parent_sig  = buf[6]  | (buf[7]  << 8) | (buf[8]  << 16) | ((uint32_t)buf[9]  << 24);
    self_sig    = buf[10] | (buf[11] << 8) | (buf[12] << 16) | ((uint32_t)buf[13] << 24);
    for (int i = 0; i < WEIGHT_COUNT; ++i) weights[i] = (int8_t)buf[WEIGHT_OFF + i];
    for (int i = 0; i < 8; ++i)            note[i]    = (int8_t)buf[NOTE_OFF + i];
    // 校验 CRC
    uint16_t c = crc16(buf, PAYLOAD_END);
    return c == (uint16_t)(buf[CRC_OFF] | (buf[CRC_OFF + 1] << 8));
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
    uint8_t buf[SIZE];
    if (!serialize(buf, SIZE)) return false;
    uint16_t c = crc16(buf, PAYLOAD_END);
    return c == (uint16_t)(buf[CRC_OFF] | (buf[CRC_OFF + 1] << 8));
}

// ---------------- 身份签名 ----------------
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
            int v = (int)weights[i] + (int)(next() & 0x1Fu) - 31; // 扰动范围 -31..0
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
    solved |= (1u << step);
    // 学会该步 => 把对应经验记进 note（拉马克）
    int slot = (step % 8);
    note[slot] = 0x40; // 一个可辨识的「已避坑」标记
}

void Genome::forgetStep(uint8_t step) {
    if (step >= N_STEPS) return;
    solved &= ~(1u << step);
    note[step % 8] = 0;
}

uint8_t Genome::stepsSolved() const {
    uint8_t n = 0;
    for (int i = 0; i < N_STEPS; ++i) if (solved & (1u << i)) ++n;
    return n;
}

// 一次完整复制（N_STEPS 步）的连乘成功率：
//   已学会的步走 STEP_MASTERED(0.98)，未学会的步走 STEP_NAIVE(0.92)
//   0 学会: 0.92^10 ≈ 43.4%   10 学会: 0.98^10 ≈ 81.7%
//   当前 solved 只有 8 位，无法保存第 8、9 步的已学会状态。
float Genome::pipelineRate() const {
    float r = 1.0f;
    for (int i = 0; i < N_STEPS; ++i) {
        r *= (solved & (1u << i)) ? evo::STEP_MASTERED : evo::STEP_NAIVE;
    }
    return r;
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
    if (generation < 0xFFFFu) ++generation;
}

void Genome::reseedNote() {
    for (int i = 0; i < 8; ++i) note[i] = 0;
}

} // namespace genome
