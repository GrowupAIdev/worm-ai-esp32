// Genome.h —— 蠕虫基因组：可序列化 / 可变异 / 可继承的字节串
//
// 基因组是蠕虫唯一传给后代的东西。它同时承载两类「基因」：
//   A. 行为基因  —— int8 神经权重组成的微型决策脑（TinyBrain）
//   B. 拉马克笔记 —— solved 位图 + fitness + 「遗书」经验
//      （把避坑经验写进笔记，传给下一代）
//
// 二进制布局（小端，紧凑无对齐）：
//   offset size  字段
//   ------ ----  ---------------------------------------------
//      0    1     version
//      1    2     generation            (uint16)
//      3    2     fitness               (uint16, 0..10000 表示 0..1.0000)
//      5    1     solved                (8-bit 位图，见 docs/protocol.md 中的限制)
//      6    4     parent_sig            (父本签名, uint32)
//     10    4     self_sig              (自身签名, uint32)
//     14    40    weights[int8]         (N_IN*N_H + N_H*N_OUT)
//     54    8     note                  (拉马克遗书, int8[8])
//     62    2     crc16                 (对 [0..61] 的校验)
//   ------
//   total = 64 字节  —— 一次 UDP 分片即可完整传输
#pragma once

#include <cstdint>
#include <cstddef>

#include "../worm_config.h"

namespace genome {

constexpr size_t SIZE = 64;               // 完整基因组字节数
constexpr size_t PAYLOAD_END = 62;        // CRC 之前有效载荷结束
constexpr size_t WEIGHT_OFF = 14;         // 权重区起点
constexpr size_t NOTE_OFF   = 54;         // 笔记区起点
constexpr size_t CRC_OFF    = 62;         // CRC 位置

struct Genome {
    uint8_t  version     = VERSION;
    uint16_t generation  = 0;
    uint16_t fitness     = 0;             // 0..10000
    uint8_t  solved      = 0;             // 当前仅能保存 8 位；N_STEPS = 10
    uint32_t parent_sig  = 0;
    uint32_t self_sig    = 0;
    int8_t   weights[WEIGHT_COUNT] = {0};
    int8_t   note[8]     = {0};           // 拉马克遗书

    // —— 序列化 ——
    bool serialize(uint8_t *buf, size_t len) const;      // 写入 buf
    bool deserialize(const uint8_t *buf, size_t len);    // 从 buf 读入 self

    // —— 校验 ——
    static uint16_t crc16(const uint8_t *buf, size_t len);
    bool check() const;                              // 重新序列化后的 CRC 自洽
    static uint16_t crc16Of(const uint8_t *buf, size_t len);

    // —— 进化操作 ——
    void mutate(unsigned int seed, float rate = evo::MUTATION_RATE); // 扰动权重 + 遗忘笔记
    void learnStep(uint8_t step);                     // 拉马克：学会某步（设 solved 位）
    void forgetStep(uint8_t step);                    // 每代随机遗忘（笔记退化）
    uint8_t stepsSolved() const;                      // 已学会步数
    float   pipelineRate() const;                     // 一次复制连乘成功率

    // —— 遗传 ——
    Genome child() const;                             // 复制出后代
    void    adoptParent(const Genome &parent);        // 记录父本 + 代次 +1
    void    reseedNote();                             // 失败槽位重置回 naive 经验

    // —— 身份签名 ——
    static uint32_t sign(const uint8_t *buf, size_t len);
};

} // namespace genome
