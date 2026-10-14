#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
蠕虫 AI 群体进化模拟器
========================
演示拉马克式学习与选择如何改变群体的模型成功率：
  给定参数下，初始约 44.4%，全部阶段习得时约 84.9%。
  输出是模型概率，不是 ESP32 固件传输成功率的实测。

模型说明
--------
把一次「自我复制」拆成 4 个串行阶段（对应文章四步循环）：
    FIND    找目标   0.90  -> 0.96
    PERM    获权限   0.74  -> 0.96
    BUILD   建推理   0.90  -> 0.96
    SPREAD  继续传播 0.74  -> 0.96

未习得阶段走 naive 基础成功率；已习得（拉马克笔记生效）走 mastered 成功率。
单条蠕虫的成功率 = 4 个阶段成功率的连乘：
    naive      : 0.90*0.74*0.90*0.74 ≈ 44.4%
    mastered   : 0.96^4               ≈ 84.9%

进化机制（拉马克式）
-------------------
- 成功完成一轮复制的蠕虫，会以一定概率把某个「尚未打通」的阶段
  永久学会（相当于把避坑经验写进遗书 / 传递给下一代）。
- 每代存在随机遗忘（笔记退化），已学会的阶段有小概率丢失。
- 自我复制：一条成功的蠕虫产出 REPL 个后代。
- 网段饱和：种群超过 MESH 容量时，只保留成功率最高的个体
  （最聪明的经验集存活），并持续补入新的 naive 蠕虫。

运行： python sim/worm_sim.py
依赖： 仅标准库（random / statistics）。
"""

import random
import statistics

# ---- 模型参数（调整后输出概率会相应变化）----
B = [0.900, 0.740, 0.900, 0.740]   # 4 个 naive 阶段基础成功率
S = [0.960, 0.960, 0.960, 0.960]   # 4 个 mastered 阶段成功率
NAIVE_TOTAL = 0.90 * 0.74 * 0.90 * 0.74   # ≈ 44.4%
MASTERED_TOTAL = 0.96 ** 4                # ≈ 84.9%

N      = 300     # 初始存活蠕虫数
MESH   = 300     # 网段容量（饱和上限）
REPL   = 3       # 一条成功蠕虫每轮产生的后代数
GENS   = 30      # 模拟代数
SEED   = 1       # 随机种子（保证结果可复现）
P_GAIN = 0.35    # 一轮成功后，学会 1 个未打通阶段的概率
P_LOSS = 0.04    # 每代每个已学会阶段被遗忘的概率

STAGE_NAMES = ["FIND", "PERM", "BUILD", "SPREAD"]


def pipeline_rate(bits):
    """一条蠕虫一次完整复制的连乘成功率。"""
    r = 1.0
    for i, b in enumerate(bits):
        r *= S[i] if b else B[i]
    return r


def mutate(bits):
    """随机遗忘：已学会的阶段有小概率丢失。"""
    out = list(bits)
    for i in range(len(bits)):
        if random.random() < P_LOSS:
            out[i] = 0
    return out


def ascii_chart(rows, max_rows=12):
    """把 mean/best 成功率画成 ASCII 柱状图。

    rows: list of (gen, mean, best)
    """
    W = 60  # 满量程柱宽（百分比）
    bar = []
    for gen, mean, best in rows:
        mw = int(round(mean * W))
        bw = int(round(best * W))
        bar.append((gen, mw, bw))

    # 采样，保证行数不超过 max_rows
    if len(bar) > max_rows:
        step = len(bar) / max_rows
        sampled = [bar[int(i * step)] for i in range(max_rows)]
    else:
        sampled = bar

    out = []
    out.append("gen   成功率柱状图 (█=群体均值  ▓=均值→最佳 之间)")
    out.append("────" + "-" * (W + 4))
    for gen, mw, bw in sampled:
        mean = "█" * mw
        best = "▓" * max(0, bw - mw)
        out.append(f"{gen:3d}  {mean}{best}")
    out.append("     " + "─" * W)
    out.append(f"     0%{' '*(W-4)}100%")
    return "\n".join(out)


def main():
    random.seed(SEED)
    pop = [[0, 0, 0, 0] for _ in range(N)]   # 全部从 naive 起步

    rows = []
    print("蠕虫 AI 群体进化模拟器  (四阶段概率模型)")
    print("=" * 72)
    names = "  ".join(f"{STAGE_NAMES[i]} {B[i]}→{S[i]}" for i in range(4))
    print("阶段基础成功率 (naive→mastered):")
    print("   " + names)
    print(f"单条蠕虫成功率  initial={100*NAIVE_TOTAL:.1f}%   ceiling={100*MASTERED_TOTAL:.1f}%")
    print("-" * 72)

    header = (f"{'gen':>3} | {'worms':>5} | {'mean':>6} | {'median':>7} | "
              f"{'best':>6} | 已学会阶段分布")
    print(header)
    print("-" * 72)

    for g in range(GENS):
        kids = []
        # 1) 每只蠕虫尝试一次完整复制（4 阶段连乘决定是否成功）
        for bits in pop:
            if random.random() < pipeline_rate(bits):
                kb = list(bits)
                # 2) 拉马克：成功后学习一个未打通的阶段
                if random.random() < P_GAIN:
                    zs = [i for i in range(4) if not kb[i]]
                    if zs:
                        kb[random.choice(zs)] = 1
                kb = mutate(kb)
                # 3) 自我复制：产出 REPL 个后代
                kids.extend([kb] * REPL)

        if not kids:
            print(f"{g:>3} | extinction | —— 全部失败，灭绝")
            break

        # 4) 网段饱和：只保留最强的 MESH 个，并把空位补回 naive 新蠕虫
        if len(kids) > MESH:
            kids.sort(key=lambda b: -pipeline_rate(b))
            kids = kids[:MESH]
        while len(kids) < N:
            kids.append([0, 0, 0, 0])
        pop = kids

        rates = [pipeline_rate(b) for b in pop]
        # 已学会阶段的平均数量（0~4），直观体现拉马克积累
        solved_avg = statistics.mean(sum(b) for b in pop)
        if g % 2 == 0 or g == GENS - 1:
            print(
                f"{g:>3} | {len(pop):>5} | {100*statistics.mean(rates):5.1f}% | "
                f"{100*statistics.median(rates):6.1f}% | {100*max(rates):5.1f}% | "
                f"{solved_avg:.2f}/4 阶段"
            )
            rows.append((g, statistics.mean(rates), max(rates)))

    print("-" * 72)
    print("\n进化曲线（群体均值 vs 最佳个体）：")
    print(ascii_chart(rows))

    final_rates = [pipeline_rate(b) for b in pop]
    print("\n结论：")
    print(f"  初始成功率   ≈ {100*NAIVE_TOTAL:.1f}%")
    print(f"  演化后最佳  ≈ {100*max(final_rates):.1f}%")
    print(f"  群体均值    ≈ {100*statistics.mean(final_rates):.1f}%")
    print("  拉马克式进化（笔记/遗书把避坑经验传给后代）是成功率跃升的关键。")
    print("  以上为给定参数下的模型概率，非硬件实测。")
    print("=" * 72)


if __name__ == "__main__":
    main()
