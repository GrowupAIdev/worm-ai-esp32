# AI 蠕虫实验：ESP32-S3 固件与 Python 群体模拟

项目包含两部分：ESP32-S3 固件在节点之间交换 64 字节基因组与学习笔记；Python 模拟器离线演示四阶段学习、遗忘和群体选择。
固件使用 8→4→2 的 `int8` 小型前馈网络做发送决策。节点需要预先烧录本项目固件，网络传输的内容是基因组数据。

**当前状态：实验原型。** 模拟器可独立运行；固件尚有 UDP 接收与步骤位图问题，详见 [协议与实现限制](docs/protocol.md)。模拟概率不代表硬件传输成功率。

## 目录结构

```text
.
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
| FIND | 从邻居表选择节点；空表时猜测与本机 IP 前三段相同的地址 |
| PERM | TinyBrain 根据 8 维输入给出 GO / SKIP 决策 |
| BUILD | 发送握手、基因组 `OFFER` 和学习笔记 `NOTE` |
| SPREAD | 更新学习位图、代次与权重，并继续下一轮 |

固件使用十步连乘模型：未习得每步 `0.92`，已习得每步 `0.98`，理论全未习得约 43.4%，全习得约 81.7%。
当前 `solved` 只有 8 位，无法保留全部十步的学习状态。它与 Python 的四阶段模型具有不同参数和实现。

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

验证入口为模拟器和固件编译。多板 UDP 通信、接收确认与实际演化效果仍需修复现有问题后进行硬件验证。
