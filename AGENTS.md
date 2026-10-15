# 项目规则

- `README.md` 是英文默认首页，`README.zh-CN.md` 是中文版本；保留双向语言入口，使用步骤和项目状态需同步更新。
- `src/` 是 ESP32-S3 / Arduino 固件；`sim/worm_sim.py` 是独立的标准库模拟器。
- 模块边界：`brain/` 管理基因组与推理，`net/` 管理 UDP，`repl/` 管理传输会话，`worm/` 驱动主循环。
- Wi-Fi 凭据放在被忽略的 `include/worm_secrets.h`；提交示例文件时只使用占位值。
- 保持基因组版本、字节布局和 UDP 协议的兼容性。涉及协议修改时同步更新 `docs/protocol.md`。
- 用户已批准传输 v3：基因组 v2 的 65 字节布局保持；传输帧带会话号，ACK 区分阶段。升级后需统一烧录，勿恢复空 ACK 的隐式成功判断。
- 修改基因组的经验、权重或代次后调用 `Genome::refresh()`，确保适应度与自身指纹同步。
- 模拟参数与固件模型不同。调整学习、遗忘或成功率参数时说明原因，不把模拟结果写成硬件实测。
- 验证入口：`python tests/run_native.py`、`python sim/worm_sim.py` 和 `python -m platformio run -e esp32-s3-devkitc-1`。本机测试需要 `g++`/`clang++`，可用 `CXX` 指定路径。
- 硬件相关行为需要板卡验证；仅编译通过时，在说明中明确验证范围。
- 传输循环使用有界非阻塞 `poll()`；每阶段最多 3 次发送，重复事务只重发 ACK。调整重试、队列、去重或退避时运行丢包/并发回归。
- 自动监管入口为 `python tools/supervise.py`，证据与快照在 `H:\CodexCache\worm-ai-supervision`；同一未变化失败最多处理两轮，不循环放宽验收。监管不自动提交、推送或烧录。
