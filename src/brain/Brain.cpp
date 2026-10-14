// Brain.cpp —— int8 前向传播（无浮点、无 STL，适配 ESP32）
#include "Brain.h"

namespace brain {

// tanh 查表（x 归一化到 [-24..24] → int8）
// 49 项：x = -24,-23,...,0,...,23,24  对应索引 0..48
static const int8_t kTanhLut[49] = {
    // -24..-1
   -127,-126,-126,-125,-125,-124,-123,-122,
   -121,-118,-114,-109,-103, -97, -91, -84,
   -76, -68, -60, -51, -42, -32, -22, -12,
    // 0
      0,
    // 1..24
     12, 22, 32, 42, 51, 60, 68, 76,
    84, 91, 97,103,109,114,118,121,
    122,123,124,125,125,126,126,127
};

int8_t tanh8(int16_t x) {
    if (x < -24) x = -24;
    if (x >  24) x =  24;
    return kTanhLut[x + 24];
}

// 前向：
//   w[0..31]  = 8×4 输入→隐藏（行主序）
//   w[32..39] = 4×2 隐藏→输出
// 计算：h = tanh( sum(w_in[i]*in[i]) >> 4 )
//       o = tanh( sum(w_ho[j]*h[j]) >> 4 )
void forward(const int8_t *weights, const int8_t *in, int8_t *out) {
    int16_t h[HID];
    for (int j = 0; j < HID; ++j) {
        int32_t acc = 0; // 8 个 int8 乘积的和可超过 int16 范围
        for (int i = 0; i < IN; ++i) {
            acc += (int16_t)weights[j * IN + i] * (int16_t)in[i];
        }
        h[j] = tanh8((int16_t)(acc >> 4));  // 缩放：8 输入 * 满幅 → >>4
    }
    for (int k = 0; k < OUT; ++k) {
        int32_t acc = 0;
        for (int j = 0; j < HID; ++j) {
            acc += (int16_t)weights[BRAIN_W_COUNT - HID * OUT + j * OUT + k] * (int16_t)h[j];
        }
        out[k] = tanh8((int16_t)(acc >> 4));
    }
}

int8_t decide(const int8_t *weights, const int8_t *in) {
    int8_t out[OUT];
    forward(weights, in, out);
    return out[0];
}

// 从 32-bit 种子派生 8 维 int8「感觉」（确定性，可复现）
void makeInput(uint32_t seed, int8_t *in) {
    uint32_t s = seed ? seed : 0x12345678u;
    for (int i = 0; i < IN; ++i) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        in[i] = (int8_t)((int)(s >> 24) - 128);  // 取高 8 位
    }
}

} // namespace brain
