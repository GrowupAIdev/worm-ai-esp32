// worm_config.h —— 蠕虫 AI 全局配置 / 常量 / 类型 / 枚举
// 单源定义，供 brain / net / repl / worm 各模块共享。
//
// 设计目标（对应文章四步循环）：
//   1) FIND   找目标   —— 局域网扫描，找到可复制的新宿主
//   2) PERM   获权限   —— 决策并通过握手确认节点应答
//   3) BUILD  建推理   —— 把基因组载入运行环境（TinyBrain 醒来）
//   4) SPREAD 继续传播 —— 传输基因组 + 把「笔记/遗书」传给下一代
#pragma once

#include <cstdint>

// ============================ WiFi ============================
// 私有配置在 include/worm_secrets.h；示例见 worm_secrets.example.h。
#if __has_include("worm_secrets.h")
#include "worm_secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "CHANGE_ME"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "CHANGE_ME"
#endif

// ============================ 网络协议 ========================
namespace net {
constexpr uint16_t PORT             = 9999;    // 蠕虫 UDP 端口
constexpr uint32_t BROADCAST_ADDR   = 0xFFFFFFFF; // 255.255.255.255
constexpr uint16_t MTU              = 512;     // 单帧最大负载
constexpr int      RX_BUF_TIMEOUT_MS = 2000;   // 等待应答的最长时间
} // namespace net

// 协议帧类型（魔数 + 类型 + 长度 + 负载 + CRC16）
enum class MsgType : uint8_t {
    HELLO     = 0x01,  // 周期性广播「我是一只蠕虫」
    HANDSHAKE = 0x02,  // 请求建立复制通道
    OFFER     = 0x03,  // 携带完整基因组
    ACK       = 0x04,  // 确认握手
    NOTE      = 0x05,  // 拉马克笔记 / 遗书（传给下一代）
    BYE       = 0x06   // 优雅退出
};

// ============================ 基因组 ==========================
// 基因组 = 一段可序列化、可变异、可继承的字节。
// 布局（见 brain/Genome.h）：
//   [version][generation][fitness][solved(8-bit)]
//   [parent_sig][self_sig][int8 权重 ...]
namespace genome {
constexpr uint8_t VERSION    = 1;
constexpr int  N_IN  = 8;      // TinyBrain 输入维度
constexpr int  N_H   = 4;      // 隐层
constexpr int  N_OUT = 2;      // 输出
constexpr int  N_STEPS = 10;   // 一次复制的「boot 步骤」数（文章 10 步）
constexpr int  WEIGHT_COUNT = N_IN * N_H + N_H * N_OUT; // 32 + 8 = 40
constexpr int  NOTE_FIELDS  = N_STEPS;  // 模型步骤数；当前 solved 存储宽度为 8 位
} // namespace genome

// ============================ 进化 / 拉马克 ====================
namespace evo {
// 每步未避坑成功率（0.92 -> 10 步后 ≈ 43.4%）
constexpr float STEP_NAIVE    = 0.92f;
// 每步已避坑成功率（0.98 -> 10 步后 ≈ 81.7%）
constexpr float STEP_MASTERED = 0.98f;
// 每代笔记退化率（遗书会慢慢失真的 2%）
constexpr float NOTE_DECAY    = 0.02f;
// 遗传 / 变异
constexpr float MUTATION_RATE = 0.05f;   // 每基因位被扰动的概率
constexpr int   POP_LIMIT     = 256;     // 本地可容纳的基因池上限
} // namespace evo

// ============================ 状态机 ==========================
enum class WormState : uint8_t {
    IDLE,        // 空闲，等待网络
    SCAN,        // 找目标（FIND）
    HANDSHAKE,   // 获权限（PERM）
    REPLICATE,   // 建推理 + 传输基因组（BUILD + SPREAD）
    EVOLVE       // 拉马克进化（写笔记 / 变异）
};

// 日志级别
enum class LogLevel : uint8_t { ERR = 0, WARN = 1, INFO = 2, DBG = 3 };
#ifndef WORM_LOG_LEVEL
#define WORM_LOG_LEVEL LogLevel::INFO
#endif
