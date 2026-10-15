# AI Worm Experiment: ESP32-S3 Firmware and Python Population Simulator

**English** | [简体中文](README.zh-CN.md)

This project has two parts: ESP32-S3 firmware exchanges 65-byte v2 genomes using transport v3, while an offline Python simulator demonstrates learning, forgetting, and population selection across four stages.
The firmware uses a small 8→4→2 `int8` feedforward network to decide whether to send. Each node must already be flashed with this firmware; the network messages carry genome data.

**Status: experimental prototype.** Transport v3 uses nonblocking sessions, distinct discovery/receipt ACKs, bounded retries, and duplicate suppression. Known peers receive genomes directly; concurrent sends keep receiving. All nodes must be reflashed together because transport v2 is incompatible. See [Protocol and implementation limitations (Chinese)](docs/protocol.md).
The simulator reports model probabilities. Multi-board communication still requires hardware validation.

## Repository layout

```text
.
├── README.md                       # Default English overview
├── README.zh-CN.md                 # Chinese overview
├── platformio.ini                  # ESP32-S3 / Arduino build configuration
├── requirements-dev.txt            # Firmware build tool version
├── include/
│   └── worm_secrets.example.h       # Wi-Fi configuration template
├── src/
│   ├── main.cpp                    # Arduino setup / loop entry point
│   ├── worm_config.h               # Shared configuration, constants, and enums
│   ├── brain/                      # Genome and TinyBrain
│   ├── net/                        # Wi-Fi, UDP, frame encoding, and peer table
│   ├── repl/                       # Genome and learning-note transfer sessions
│   └── worm/                       # Main loop and evolution logic
├── sim/worm_sim.py                 # Python standard-library simulator
├── docs/protocol.md                # Wire formats and current implementation limits
├── tests/                          # Host C++ regression tests with Wi-Fi/UDP stubs
├── tools/supervise.py              # Change-aware validation for Codex supervision
├── AGENTS.md                       # Project development rules
└── skills/                         # Project-specific skills
```

## Run the simulator (no hardware required)

From the repository root:

```sh
python sim/worm_sim.py
```

For a Windows console, set `$env:PYTHONUTF8 = '1'` if needed.
The defaults are 300 individuals, 30 generations, three offspring per successful reproduction, and random seed 1.
The four baseline stage probabilities are `[0.90, 0.74, 0.90, 0.74]`; each learned stage has a success probability of `0.96`.

| Model state | Calculation | Success probability |
| --- | --- | --- |
| Initial population, no stages learned | `0.90 × 0.74 × 0.90 × 0.74` | 44.4% |
| All four stages learned | `0.96^4` | 84.9% |

With the default settings, both the final population mean and the best individual reach approximately 84.9%.
This result follows from the configured probabilities and selection mechanism; changing the parameters changes the results.

## Build the firmware

The target board is `esp32-s3-devkitc-1`, using the Arduino framework.
The build pins `espressif32@7.1.3` and PlatformIO Core `6.2.0`.
Wi-Fi and UDP use libraries bundled with the platform; no additional `lib_deps` are required.

The Windows development environment used for this project stores caches on drive H. Configure it in PowerShell:

```powershell
$env:PIP_CACHE_DIR = 'H:\CodexCache\pip'
$env:PLATFORMIO_CORE_DIR = 'H:\CodexCache\platformio'
$env:TEMP = 'H:\CodexCache\tmp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Force -Path $env:PIP_CACHE_DIR,$env:PLATFORMIO_CORE_DIR,$env:TEMP | Out-Null
```

Install the build tool and compile:

```sh
python -m pip install -r requirements-dev.txt
python -m platformio run -e esp32-s3-devkitc-1
```

### Wi-Fi configuration

Create a local configuration file in PowerShell, then edit it. This command preserves an existing file:

```powershell
if (-not (Test-Path include/worm_secrets.h)) {
    Copy-Item include/worm_secrets.example.h include/worm_secrets.h
}
```

Set `WIFI_SSID` and `WIFI_PASS` only in `include/worm_secrets.h`. Git ignores this file.
`src/worm_config.h` loads it automatically. Without a local configuration file, the firmware uses `CHANGE_ME` placeholders and can still be compiled.

### Upload and serial monitor

Connect the ESP32-S3 over USB, then run:

```sh
python -m platformio run -e esp32-s3-devkitc-1 -t upload
python -m platformio device monitor -b 115200
```

After flashing, the firmware attempts to join the configured Wi-Fi network and uses UDP port `9999`.

## Four-stage loop

| Stage | Current firmware behavior |
| --- | --- |
| FIND | Rotate through live peers; when none are available, broadcast a handshake and select the first responder |
| PERM | TinyBrain produces a GO / SKIP decision from eight input features; a rejected decision still has a 75% chance to send |
| BUILD | Send `OFFER` directly to known peers, or discover a peer first; continue receiving while awaiting a matching session/stage ACK |
| SPREAD | Update the learning bitmap, generation, and weights, then begin another cycle |

The firmware uses a ten-step multiplicative model: each unlearned step succeeds with probability `0.92`, and each learned step with probability `0.98`.
The theoretical probabilities are approximately 43.4% with no steps learned and 81.7% with all ten learned.
The v2 `solved` field has 16 bits and retains all ten steps. The eight note slots still share entries between steps 0/8 and 1/9.
Its parameters and implementation differ from the Python simulator's four-stage model.

## Run firmware regression tests (no hardware required)

Install a host `g++` or `clang++` compiler, or set `CXX` to its executable path, then run:

```sh
python tests/run_native.py
```

These tests compile the actual firmware modules against Arduino/Wi-Fi/UDP stubs and cover malformed packets, stale ACKs, retry limits, duplicate suppression, concurrent receiving, queue pressure, full-range inference, and generation limits.
On Windows with drive H, test builds use `H:\CodexCache\worm-ai-tests`; `WORM_TEST_BUILD_DIR` overrides the location.

The fixed-seed, eight-node test runs 64 transfers per channel configuration:

| Injected datagram loss | Receiver accepted | Sender confirmed | Datagrams sent |
| --- | --- | --- | --- |
| 0% | 64/64 | 64/64 | 128 |
| 10% | 64/64 | 62/64 | 150 |
| 30% | 64/64 | 57/64 | 209 |

For known peers without loss, one transfer uses two datagrams instead of the old five (60% fewer), and 98 application bytes instead of 120. Retries resend the same transaction; duplicate receipt does not create another child within the cache window.
These are deterministic host tests of the C++ transport, not radio measurements or a claim of globally optimal throughput. Lost final ACKs can still leave the sender uncertain after its retry budget.

## Codex supervision

```sh
python tools/supervise.py
```

Set `CXX` or use `--cxx` for the host compiler. The checker validates a public source snapshot on drive H with native tests, supervisor tests, the simulator, and the ESP32-S3 build. It excludes local Wi-Fi credentials, preserves logs, prevents overlapping runs, and skips an unchanged source/tool fingerprint. Use `--force` after an external environment change.
Evidence is stored in `H:\CodexCache\worm-ai-supervision\status.json`. A repeated unchanged failure is reported as `unchanged_failure`, rather than silently passing or repeatedly running the same failing job.
The Codex schedule for this chat runs every six hours. It may make up to two evidence-based repair attempts for a new incident, then revalidate; it preserves existing work and leaves Git commits and pushes to the user. Hardware or compatibility decisions require user input.
Keep the computer and desktop app running for local supervision. See [official scheduled-task documentation](https://learn.chatgpt.com/docs/automations).

## Local pre-commit checks

```sh
git status --short --ignored
git add .
git diff --cached --stat
git diff --cached --check
git commit -m "chore: prepare repository for GitHub"
```

Ignore rules cover `.pio/`, build outputs, Python caches, browser run records, and local Wi-Fi configuration.
Before committing, confirm that the staging area contains only source code, example configuration, and documentation.

Validation entry points are the host regression tests, the simulator, and the firmware build.
Multi-board UDP communication and actual evolution behavior still require hardware testing. Duplicate suppression covers the most recent 32 accepted transfers for up to ten seconds; it is not durable across reboot and does not provide network authentication.
