"""生成测试夹具（14 天 × 24 小时）。只在需要重建夹具时运行。

数据刻意包含：
- 夜间大量 0（wear=1、value=0）；
- 未佩戴（wear=0），含 04:00 整点全部缺失；
- 只戴了 18 分钟的小时（按 wear=0 处理）；
- 两次大运动（异常值）。

用法：python3 tools/gen_fixtures.py
"""

from __future__ import annotations

import math
import random
from pathlib import Path

HOURS = 24
DAYS = 14
SEED = 7

BASE = {
    0: 6, 1: 4, 2: 3, 3: 3, 4: 4, 5: 7, 6: 30, 7: 190,
    8: 520, 9: 620, 10: 380, 11: 330, 12: 430, 13: 470,
    14: 300, 15: 280, 16: 330, 17: 430, 18: 560, 19: 720,
    20: 380, 21: 250, 22: 110, 23: 38,
}

ROOT = Path(__file__).resolve().parent.parent
FIX = ROOT / "tests" / "fixtures"


def make_days() -> list[list[float | None]]:
    rng = random.Random(SEED)
    days: list[list[float | None]] = []
    for d in range(DAYS):
        factor = min(1.35, max(0.55, rng.gauss(1.0, 0.18)))
        if d == 4:
            factor = 0.45
        days.append([float(round(BASE[h] * factor * math.exp(rng.gauss(0, 0.30))))
                     for h in range(HOURS)])

    for d, h in ((2, 10), (6, 13), (9, 12)):
        days[d][h] = 0.0
    days[7][19] = 4200.0
    days[12][19] = 3800.0
    for d, h in ((3, 15), (8, 9), (11, 19), (1, 23), (2, 23), (4, 23),
                 (6, 23), (7, 23), (9, 23), (10, 23)):
        days[d][h] = None
    for d in range(DAYS):
        days[d][4] = None
    return days


def make_today() -> list[float | None]:
    rng = random.Random(SEED + 1)
    today: list[float | None] = [
        float(round(BASE[h] * 0.62 * math.exp(rng.gauss(0, 0.25)))) for h in range(HOURS)
    ]
    today[3] = 260.0
    today[10] = 0.0
    today[18] = 190.0
    today[15] = None
    return today


def main() -> None:
    FIX.mkdir(parents=True, exist_ok=True)
    days = make_days()
    today = make_today()

    # 5/14 只佩戴 18 分钟：上游按 wear=0 上报
    partial = (5, 14)

    with (FIX / "baseline.csv").open("w", encoding="utf-8", newline="\n") as f:
        f.write("day,hour,wear,activity\n")
        for d in range(DAYS):
            for h in range(HOURS):
                v = days[d][h]
                wear = 0 if (v is None or (d, h) == partial) else 1
                f.write(f"{d},{h},{wear},{0.0 if wear == 0 else v}\n")

    with (FIX / "today.csv").open("w", encoding="utf-8", newline="\n") as f:
        f.write("hour,wear,activity\n")
        for h in range(HOURS):
            v = today[h]
            wear = 0 if v is None else 1
            f.write(f"{h},{wear},{0.0 if wear == 0 else v}\n")

    print(f"wrote {FIX/'baseline.csv'} and {FIX/'today.csv'}")


if __name__ == "__main__":
    main()
