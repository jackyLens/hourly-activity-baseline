# hourly-activity-baseline

给定过去 14 天每小时的活动量（带小时级佩戴标记），为 0–23 点各算一个个人基线点与区间，
并判定某一天每个小时是否偏离自己的常态。

C11，无第三方依赖。`main()` 只做输入解析和输出，统计与判定全部是 `hb.c` 里的函数调用。

```
baseline.csv (14×24)  ──hb_fit──►  24 点基线  ──hb_compare──►  24 条判定  ──hb_summarize──►  连续时段
```

---

## 目录

| 路径 | 内容 |
|---|---|
| `include/hb.h` | 公共 API：数据结构、函数声明 |
| `src/hb.c` | 拟合、判定、时段合并、统计原语 |
| `src/hb_cli.c` | `main()`：CSV 解析 + JSON 输出 |
| `tests/test_hb.c` | 567 个定点断言 |
| `tests/test_props.c` | 随机化不变量测试（3000 组输入，232 万次检查） |
| `tests/test_cli.sh` | CLI 集成测试（golden、错误码、无表头/注释/CRLF） |
| `tests/fixtures/` | 14 天 × 24 小时夹具 + 期望输出 |
| `tools/gen_fixtures.py` | 重建夹具（可选，不参与构建） |


## 一、I/O 契约

### 1.1 输入 `baseline.csv`

长表，逗号分隔，允许**一行表头**、`#` 注释行、CRLF 行尾。

| 列 | 类型 | 约束 | 含义 |
|---|---|---|---|
| `day` | 整数 | `0..13` | 相对天数；**0 是最早的一天**，13 是最近一天 |
| `hour` | 整数 | `0..23` | 本地墙钟小时 |
| `wear` | 整数 | `0` / `1` | 该小时是否佩戴充分 |
| `activity` | 浮点 | `wear=1` 时必须是有限且 ≥ 0 的数 | 活动量 |

规则：

- 每个 `(day,hour)` 至多一行，重复即报错。
- 缺行等价于 `wear=0`（缺失），但建议显式给出全部 336 行。
- `day` 不要求连续；`days_used = max(day)+1`，取值范围 1..14。
- 字段两侧允许空格/制表符；单行上限 511 字符，超长行直接报错而不是被截断成两行。
- 整数用 `strtol` 严格解析：溢出、`1.5` 这类小数、`10abc` 这类尾随字符都会被拒绝。
- `wear=1` 必须由上游只在“该小时佩戴充分”时上报（例如 ≥45 分钟）。**部分佩戴不要写成低活动量**，否则基线会被系统性拉低。
- `wear=0` 时 `activity` 被忽略，可以写 `0` 或留空。
- `wear=1, activity=0` 是**有效观测**：戴了但没动。

### 1.2 输入 `today.csv`

| 列 | 类型 | 约束 | 含义 |
|---|---|---|---|
| `hour` | 整数 | `0..23` | 小时 |
| `wear` | 整数 | `0` / `1` | 是否佩戴 |
| `activity` | 浮点 | 同上 | 活动量 |

缺行表示该小时未佩戴（`no_data`）。没有 `today.csv` 时，输出里 `today` 为 `null`。

### 1.3 输出 JSON

```json
{
  "method": "hourly_personal_baseline_c_v1",
  "window_days": 14,
  "days_used": 14,
  "hour_count": 24,
  "floor": 29.4,
  "scale": 294.0,
  "baseline": [ /* 24 项 */ ],
  "today":    [ /* 24 项，或 null */ ],
  "blocks":   [ /* 连续时段 */ ]
}
```

- `window_days` 是固定窗口容量（14）；`days_used` 是本次实际参与拟合的天数（1..14）。

`baseline[i]`：

| 字段 | 类型 | 含义 |
|---|---|---|
| `hour` | int | 0..23 |
| `status` | string | `insufficient` / `building` / `comparable` |
| `n_valid` | int | 有效样本数（wear=1 且值合法） |
| `n_missing` | int | 缺失数（wear=0 或值非法） |
| `n_zero` | int | 有效样本中 `activity == 0` 的个数 |
| `median` | number\|null | 基线点；样本不足时为 null |
| `mad` | number\|null | 原始 MAD |
| `mad_eff` | number\|null | `max(mad, floor)`，用于 z |
| `p10` `p25` `p75` `p90` | number\|null | 经验分位数 |
| `inner_low` `inner_high` | number\|null | 常见范围（P25–P75，必要时用 floor 加宽） |
| `outer_low` `outer_high` | number\|null | 不寻常边界（P10–P90，必要时用 floor 加宽） |
| `p_active` | number\|null | 非零天数占比 |
| `active_level` | number\|null | 有活动时的中位强度 |
| `rest_expected` | bool | 是否静息时段；仅当 `band_available=true` 时有意义 |
| `band_available` | bool | 是否有可用区间 |

`today[i]`：

| 字段 | 类型 | 含义 |
|---|---|---|
| `hour` | int | 0..23 |
| `value` | number\|null | 当天值；未佩戴为 null |
| `state` | string | `no_data` / `no_baseline` / `normal` / `high` / `low` / `unusual_movement` |
| `baseline_status` | string | 该小时基线的成熟度；`building` 时不要对外下结论 |
| `z` | number\|null | 稳健强度，仅参考，不参与告警 |
| `delta` | number\|null | `value - median` |
| `expected` | number\|null | 该小时基线中位数 |
| `band_position` | string | `below` / `inside` / `above` / `unknown` |
| `reason` | string | 面向用户的解释（UTF-8） |

`blocks[i]`：

| 字段 | 类型 | 含义 |
|---|---|---|
| `start_hour` / `end_hour` | int | 含首含尾 |
| `n_hours` | int | 连续小时数 |
| `state` | string | `high` / `low` / `unusual_movement` |
| `total` | number | 时段内实际值合计 |
| `expected` | number | 时段内基线中位数合计 |
| `ratio` | number\|null | `total / expected`；`expected=0` 或 `unusual_movement` 时为 null |
| `text` | string | 可直接展示的句子 |

### 1.4 命令行

```bash
build/hb_baseline [选项] <baseline.csv> [today.csv]

  -o, --out F   把 JSON 写入 F（默认 stdout）
  -h, --help    帮助
```

| 退出码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 用法错误 |
| 2 | 输入错误（打不开、字段非法、重复行等） |
| 3 | 计算失败（参数非法、内部错误） |

---

## 二、算法

对每个小时 `h` 独立计算：

```text
1) 取有效样本 V（wear=1 且 activity 有限、>= 0），n = |V|
   n < 3        → insufficient（不建基线，不做比较）
   3 <= n < 7   → building（可画，但不对外下结论）
   n >= 7       → comparable

2) 基线点     = median(V)
   常见范围   = P25(V) – P75(V)
   不寻常边界 = P10(V) – P90(V)

3) 区间兜底：若分位区间窄于 floor，用 median ± floor 撑开
   floor = max(floor_min, floor_ratio × scale)
   scale = 24 个小时中位数里 > 0 的那些的中位数

4) 零膨胀描述：
   p_active     = 非零天数 / n
   active_level = median(V 中 > 0 的部分)
   静息判定     = p_active < 0.20  或  median < 0.15 × scale

5) 稳健强度（只用于排序/审计）：
   z = (x − median) / (1.4826 × max(MAD, floor))
```

判定：

```text
value 缺失                  → no_data（不当作 0）
该小时 insufficient          → no_baseline
静息时段：
    value > max(outer_high, rest_signal_min) → unusual_movement
    否则                                      → normal（永不报 low）
其他时段：
    value > outer_high → high
    value < outer_low  → low
    否则                → normal
```

`hb_summarize()` 把连续同向的小时合并成时段，并给出“合计多少、是平时同期的百分之多少”。

---

## 三、区间用分位数

活动量下界是 0、右偏、日间波动大。若沿用 `median ± k(n)·1.4826·MAD`（n=14 时 k≈2.80），
区间下沿几乎总是被压到 0：

| 小时 | 中位数 | `±kσ` 区间 | P10–P90 | P25–P75 |
|---|---:|---|---|---|
| 09:00 | 634 | **0 – 1381** | 419 – 868 | 455 – 715 |
| 10:00 | 350 | **0 – 746** | 159 – 446 | 204 – 426 |
| 19:00 | 1050 | **0 – 2121** | 600 – 3289 | 753 – 1210 |

下沿为 0 意味着“偏低”永远判不出来。因此：**点估计与 z 用中位数/MAD，区间用经验分位数**。
`floor` 还会把过窄的分位区间（例如 3 个完全相同的值）撑开，避免把测量噪声判成异常。

---

## 四、C API

```c
#include "hb.h"

hb_input in;
hb_input_init(&in);
hb_input_set(&in, day, hour, wear, activity);   /* 越界返回 -1 */

hb_config cfg;
hb_config_default(&cfg);

hb_baseline b;
if (hb_fit(&in, &cfg, &b) != 0) { /* 参数非法 */ }

hb_deviation dev = hb_compare(&b, &cfg, hour, has_value, value);

hb_deviation devs[HB_HOURS];
hb_block blocks[HB_HOURS];
int n = hb_summarize(devs, HB_HOURS, blocks, HB_HOURS);
```

- 无 `malloc`，所有缓冲区由调用方提供；`reason` 指向静态字符串。
- 无全局状态，可重入。
- 统计原语 `hb_median` / `hb_mad` / `hb_quantile` 也对外暴露，便于复用与测试。

参数（`hb_config` 默认值）：

| 字段 | 默认 | 含义 |
|---|---|---|
| `min_n_any` | 3 | 任何统计量的硬下限 |
| `min_n_comparable` | 7 | 可比较门槛 |
| `inner_q_low/high` | 25 / 75 | 常见范围 |
| `outer_q_low/high` | 10 / 90 | 不寻常边界 |
| `floor_ratio` | 0.10 | 区间最小半宽比例 |
| `floor_min` | 1.0 | 区间最小半宽绝对值 |
| `rest_p_active` | 0.20 | 静息判定：有活动天数占比 |
| `rest_level_ratio` | 0.15 | 静息判定：量级比例 |
| `rest_signal_min` | 0.0 | 静息时段异常活动的绝对门槛 |

CLI 使用默认值；需要改参数时直接调用 C API。

---

## 五、复杂度与限制

- 时间 `O(24 × 14 log 14)`，空间 `O(24 × 14)`，全在栈上。
- 窗口固定 14 天；样本更少时按 `building` / `insufficient` 降级，不硬算。
- 不做工作日/周末分层：14 天里每小时只剩约 4 个周末样本，区间会不稳。
- 不做趋势/衰减：受伤、换工作、季节变化会让 14 天基线滞后。
- `n=14` 的分位数本身有噪声（P10/P90 接近第 2/12 个次序统计量），`status` 已暴露这一点。
- 只是描述性基线，不是运动处方，也不代表因果。

---
