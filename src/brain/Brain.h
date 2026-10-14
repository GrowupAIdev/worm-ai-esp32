// Brain.h —— TinyBrain：int8 前馈网络 (8 → 4 → 2)
//
// 用途：把「当前网络/目标特征」(8 维 int8) 映射到一个决策分 (2 维 int8)，
// 权重直接存在 Genome::weights[40] 里 —— 即「大脑就是基因组」，
// 传播时大脑本身会被遗传/突变，符合拉马克式演化设定。

#pragma once
#include <stdint.h>
#include <stddef.h>

namespace brain {

enum InputDim  : int8_t { IN = 8 };
enum HiddenDim : int8_t { HID = 4 };
enum OutDim    : int8_t { OUT = 2 };
#define BRAIN_W_COUNT (IN * HID + HID * OUT)  // = 40，与 Genome::WEIGHT_COUNT 一致

// 简易激活：tanh(x) 的 int8 近似（查表，避免浮点）
int8_t tanh8(int16_t x);

// 前向：
//   weights: 40 个 int8 权重（来自 Genome）
//   in:      8 维 int8 输入
//   out:     2 维 int8 输出（调用方提供 OUT 大小的缓冲）
void forward(const int8_t *weights, const int8_t *in, int8_t *out);

// 决策：返回 out[0]（主信号），out[1] 作为置信度
int8_t decide(const int8_t *weights, const int8_t *in);

// 用 8 位随机数填充 in（用于无传感器时的「感觉噪声」）
void makeInput(uint32_t seed, int8_t *in);

} // namespace brain
