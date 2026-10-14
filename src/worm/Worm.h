// Worm.h —— 蠕虫主体：四步循环驱动的核心
//
// 每一步对应文章四步：
//   1. 找目标  (SCAN)     —— 网络扫描，收集邻居画像
//   2. 获权限  (PERM)     —— TinyBrain 根据画像决策「值不值得复制」
//   3. 建推理  (BUILD)    —— 复制会话，把基因组 + 遗书传输过去
//   4. 继续传播 (SPREAD)  —— 拉马克进化：成功 -> 解锁未掌握阶段
//
// 循环往复，代次递进。

#pragma once
#include <Arduino.h>
#include "worm_config.h"
#include "brain/Genome.h"
#include "brain/Brain.h"
#include "net/WormNet.h"
#include "repl/Replicator.h"

class Worm {
public:
    Worm();
    void begin();                       // WiFi + UDP 初始化
    void loop();                        // 状态机主循环（部分操作含超时等待）

    // 统计
    uint32_t generations() const { return gen_; }
    float    fitness() const { return genome_.pipelineRate(); }

private:
    // —— 四步对应 ——
    bool step1_scan(uint32_t budgetMs);   // 收集邻居
    bool step2_perm();                     // 大脑决策
    bool step3_build(IPAddress target);   // 复制会话（发送）
    bool step4_spread(bool success);      // 拉马克进化

    // —— 接收模式 ——
    bool handleIncoming(uint32_t budgetMs); // 被动接收并孵化

    // —— 成员 ——
    genome::Genome genome_;
    net::WormNet   net_;
    Replicator     repl_;
    int8_t  input_[brain::IN];
    uint32_t gen_ = 0;
    uint32_t lastAction_ = 0;
    uint32_t cooldownMs_ = 5000;  // 上次动作后的冷却时长（按结果自适应）
    uint32_t peerCount_  = 0;
    int      peerCursor_ = 0;     // 邻居轮转游标（均摊传播目标）
    IPAddress lastPeer_  = IPAddress(0,0,0,0);
};
