# AI 蠕虫实验：ESP32-S3 固件与 Python 群体模拟

[English](README.md) | **简体中文**

项目包含两部分：ESP32-S3 固件通过传输协议 v3 交换 65 字节的 v2 基因组；Python 模拟器离线演示四阶段学习、遗忘和群体选择。
固件使用 8→4→2 的 `int8` 小型前馈网络做发送决策。节点需要预先烧录本固件，网络传输的内容是基因组数据。

**当前状态：实验原型。** 传输 v3 使用非阻塞会话、区分发现/接收的 ACK、有界重试与接收去重；已知邻居直接发送，并发发送期间继续收包。传输 v2 与 v3 不兼容，节点需统一重新烧录，详见 [协议与实现限制](docs/protocol.md)。模拟概率不代表硬件传输成功率，多板通信需硬件验证。

## 目录结构

```text
.
├── README.md                       # 英文默认首页
├── README.zh-CN.md                 # 中文说明
├── platformio.ini                  # ESP32-S3 / Arduino 构建配置
├── requirements-dev.txt            # 固件构建工具版本
├── include/
│   └── worm_secrets.example.h       # Wi-Fi 配置模板
├── src/
│   ├── main.cpp                    # Arduino setup / loop 入口
│   ├── worm_config.h               # 共享配置、常量和枚举
│   ├── brain/                      # Genome 与 TinyBrain
│   ├── net/                        # Wi-Fi、UDP、帧编解码与邻居表
│   ├── repl/                       # 基因组和笔记传输会话
│   └── worm/                       # 主循环与进化逻辑
├── sim/worm_sim.py                 # 仅依赖 Python 标准库的模拟器
├── docs/protocol.md                # 字节布局与现有实现限制
├── tests/                          # 使用 Wi-Fi/UDP 桩的本机 C++ 回归测试
├── tools/supervise.py              # Codex 监管使用的按变化验证入口
├── AGENTS.md                       # 项目开发规则
└── skills/                         # 项目专用技能目录
```

## 运行模拟器（无需硬件）

在仓库根目录运行：

```sh
python sim/worm_sim.py
```

Windows 控制台需要时可先设置 `$env:PYTHONUTF8 = '1'`。
默认参数为 300 个体、30 代、每次成功产生 3 个后代、随机种子 1。
四阶段基础概率为 `[0.90, 0.74, 0.90, 0.74]`，已习得阶段概率均为 `0.96`。

| 模型状态 | 计算 | 成功概率 |
| --- | --- | --- |
| 初始，全部未习得 | `0.90 × 0.74 × 0.90 × 0.74` | 44.4% |
| 四个阶段全部习得 | `0.96^4` | 84.9% |

默认运行最终群体均值和最佳个体均约为 84.9%。这来自给定概率和选择机制，调整参数后结果会改变。

## 构建固件

目标板为 `esp32-s3-devkitc-1`，框架为 Arduino。构建配置固定 `espressif32@7.1.3`，开发依赖固定 PlatformIO Core `6.2.0`。
Wi-Fi 与 UDP 使用平台自带库，没有额外的 `lib_deps`。

本机 Windows 使用 H 盘缓存，在 PowerShell 中设置：

```powershell
$env:PIP_CACHE_DIR = 'H:\CodexCache\pip'
$env:PLATFORMIO_CORE_DIR = 'H:\CodexCache\platformio'
$env:TEMP = 'H:\CodexCache\tmp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Force -Path $env:PIP_CACHE_DIR,$env:PLATFORMIO_CORE_DIR,$env:TEMP | Out-Null
```

安装构建工具并编译：

```sh
python -m pip install -r requirements-dev.txt
python -m platformio run -e esp32-s3-devkitc-1
```

### Wi-Fi 配置

在 PowerShell 中创建本地配置，然后编辑该文件。已有配置会保留：

```powershell
if (-not (Test-Path include/worm_secrets.h)) {
    Copy-Item include/worm_secrets.example.h include/worm_secrets.h
}
```

只在 `include/worm_secrets.h` 中填写 `WIFI_SSID` 和 `WIFI_PASS`。该文件已被 Git 忽略。
`src/worm_config.h` 会自动加载本地配置；没有本地文件时使用 `CHANGE_ME`，仍可完成编译。

### 烧录与串口

连接 ESP32-S3 的 USB 后执行：

```sh
python -m platformio run -e esp32-s3-devkitc-1 -t upload
python -m platformio device monitor -b 115200
```

烧录后固件会尝试连接配置的 Wi-Fi，并使用 UDP 端口 `9999`。

## 四步循环

| 阶段 | 固件当前行为 |
| --- | --- |
| FIND | 轮转选择有效邻居；无可用邻居时广播握手，选择首个应答节点 |
| PERM | TinyBrain 根据 8 维输入给出 GO / SKIP 决策；拒绝后仍有 75% 概率发送 |
| BUILD | 向已知邻居直接发送 `OFFER`，无邻居时先发现目标；等待匹配会话与阶段的 ACK 时继续收包 |
| SPREAD | 更新学习位图、代次与权重，并继续下一轮 |

固件使用十步连乘模型：未习得每步 `0.92`，已习得每步 `0.98`，理论全未习得约 43.4%，全习得约 81.7%。
v2 的 `solved` 为 16 位，可保留全部十步状态；8 个笔记槽仍由第 0/8 步、第 1/9 步共用。它与 Python 的四阶段模型具有不同参数和实现。

## 运行固件回归测试（无需硬件）

安装本机 `g++` 或 `clang++` 编译器，或将 `CXX` 设置为其可执行文件路径，然后运行：

```sh
python tests/run_native.py
```

测试使用 Arduino/Wi-Fi/UDP 桩编译实际固件模块，覆盖坏包、旧 ACK、重试上限、接收去重、并发接收、队列满载、满幅推理和代次上限。
Windows 有 H 盘时，测试构建存放在 `H:\CodexCache\worm-ai-tests`；可通过 `WORM_TEST_BUILD_DIR` 修改位置。

固定随机种子的 8 节点测试，每种信道配置运行 64 次传输：

| 注入的数据报丢失率 | 接收方已接收 | 发送方已确认 | 发送报文数 |
| --- | --- | --- | --- |
| 0% | 64/64 | 64/64 | 128 |
| 10% | 64/64 | 62/64 | 150 |
| 30% | 64/64 | 57/64 | 209 |

无丢包的已知邻居传输由旧版 5 个报文减至 2 个（减少 60%），应用层字节由 120 减至 98。重试沿用同一事务，缓存窗口内的重复接收不会再次孵化。
这些是执行实际 C++ 传输模块的确定性本机测试，不是无线实测，也不代表全局最优吞吐。最后的 ACK 丢失时，发送方在耗尽重试预算后仍可能无法确认。

## Codex 自动监管

```sh
python tools/supervise.py
```

通过 `CXX` 或 `--cxx` 指定本机编译器。检查器在 H 盘公开源码快照中运行本机测试、监管器测试、模拟器和 ESP32-S3 编译；排除本地 Wi-Fi 凭据，保留日志，防止重叠运行，并按源码/工具指纹跳过未变化的结果。外部环境变化后可用 `--force` 重新验证。
证据存放在 `H:\CodexCache\worm-ai-supervision\status.json`。同一失败未变化时返回 `unchanged_failure`，不会当作通过或反复执行同一失败作业。
本聊天的 Codex 任务每 6 小时运行一次；新故障最多尝试两轮有证据的修复并重新验证，保留已有工作，由用户决定 Git 提交与推送。涉及硬件或兼容性决策时需用户输入。
本地监管需要电脑与桌面应用保持运行，见 [官方定时任务说明](https://learn.chatgpt.com/docs/automations)。

## 本地提交前检查

```sh
git status --short --ignored
git add .
git diff --cached --stat
git diff --cached --check
git commit -m "chore: prepare repository for GitHub"
```

`.pio/`、构建输出、Python 缓存、浏览器运行记录和本地 Wi-Fi 配置均已加入忽略规则。
提交前确认暂存区只包含代码、示例配置和文档。

验证入口为本机回归测试、模拟器和固件编译。多板 UDP 通信与实际演化效果仍需硬件验证。去重覆盖最近 32 次已接收事务，最长保留 10 秒；重启后不保留，也不提供网络身份认证。
