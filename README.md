# hourly-activity-baseline（evobeing）

作者：zhenghuimin

拿一个人过去最多 14 天、每小时的活动量，给一天里 0~23 点每个小时各算一条个人基线，
然后看某一天每个小时是比平时多、比平时少，还是正常。比如“今天上午 8~11 点只有平时的 38%”
“凌晨 3 点平时不动，今天动了”这类结论。

代码是 C11，只依赖标准库和 libm，不分配堆内存，没有全局变量。
集成的时候只需要 `include/evobeing_baseline.h` 和 `src/evobeing_baseline.c` 两个文件。

## 目录

```
include/evobeing_baseline.h    接口：结构体和函数声明
src/evobeing_baseline.c        算法实现
src/evobeing_main.c            命令行程序（读 CSV，写 JSON），可以不用
examples/evobeing_example.c    C 调用示例
examples/data/                 示例输入 history.csv、today.csv，以及对应输出 output.json
```

命名规则：对外的类型、函数、宏都以 `evobeing_` / `EVOBEING_` 开头；
一次算完的入口叫 `evobeing_zhenghuimin_run()`；源文件内部的 static 函数用 `zhenghuimin_` 前缀。

## 接入

把上面两个文件放进工程，编译时带上 `-lm`：

```bash
cc -std=c11 -O2 -Iinclude your_code.c src/evobeing_baseline.c -lm -o your_app
```

C99 也能编，C++ 里可以直接 `#include`（头文件带了 `extern "C"`）。

仓库里自带的 Makefile 会编出命令行程序和示例：

```bash
make          # 生成 build/evobeing_baseline 和 build/evobeing_example
make run      # 跑一遍示例和命令行程序
```

## 调用示例

最常见的用法：填好历史数据和当天数据，调一次 `evobeing_zhenghuimin_run()`，然后读结果。

```c
#include <stdio.h>
#include "evobeing_baseline.h"

evobeing_input  evobeing_history;   /* 输入：过去 14 天 × 24 小时 */
evobeing_day    evobeing_today;     /* 输入：要判定的那一天 */
evobeing_result evobeing_res;       /* 输出 */
int d, h, i;

/* 历史数据：day 0 是最早的一天，13 是最近一天 */
evobeing_input_init(&evobeing_history);
for (d = 0; d < 14; d++) {
    for (h = 0; h < 24; h++) {
        /* wear: 这个小时戴够了填 1，没戴或数据不可信填 0（此时 value 随便填） */
        evobeing_input_set(&evobeing_history, d, h, wear[d][h], steps[d][h]);
    }
}

/* 当天数据 */
evobeing_day_init(&evobeing_today);
for (h = 0; h < 24; h++) {
    evobeing_day_set(&evobeing_today, h, today_wear[h], today_steps[h]);
}

/* 计算；第三个参数传 NULL 表示用默认参数 */
if (evobeing_zhenghuimin_run(&evobeing_history, &evobeing_today, NULL, &evobeing_res) != 0) {
    /* 输入不合法，比如一天数据都没有 */
}

/* 每小时的判定 */
for (h = 0; h < 24; h++) {
    const evobeing_deviation *dv = &evobeing_res.today[h];
    printf("%02d:00 %s %s\n", h, evobeing_state_name(dv->state), dv->reason);
}

/* 合并后的偏离时段 */
for (i = 0; i < evobeing_res.n_blocks; i++) {
    printf("%s\n", evobeing_res.blocks[i].text);
}
```

能直接编译运行的完整版本在 `examples/evobeing_example.c`，输出大致是：

```
03:00  ...  unusual_movement 这个时段你平时基本不动，今天出现了明显活动。
08:00  ...  low              低于你个人该时段 80% 的历史范围（P10–P90）。
...
偏离时段：
  03:00–04:00 出现异常活动（合计 260）
  08:00–10:00 连续 2 小时低于平时（合计 330，为平时同期的 29%）
  19:00–20:00 高于平时（合计 1800，为平时同期的 253%）
```

如果是每来一个小时的数据就要判一次，可以把拟合和判定分开：基线每天算一次，判定随时调。

```c
evobeing_config  cfg;
evobeing_baseline base;

evobeing_config_default(&cfg);
evobeing_fit(&evobeing_history, &cfg, &base);              /* 每天一次 */

evobeing_deviation dv = evobeing_compare(&base, &cfg,      /* 每小时一次 */
                                         hour, has_value, value);
```

## 输入

### 历史数据 `evobeing_input`

| 字段 | 类型 | 说明 |
|---|---|---|
| `value[d][h]` | `double` | 第 d 天第 h 小时的活动量，有限且 >= 0 |
| `wear[d][h]` | `unsigned char` | 1 = 这个小时佩戴充分；0 = 缺失 |
| `days` | `int` | 实际天数 1~14 |

`d` 取 0~13，0 是最早的一天；`h` 取 0~23，按本地时间。

建议用 `evobeing_input_set()` 写入，它会检查范围并自动更新 `days`（等于写过的最大 day + 1）。
直接写数组也行，但要自己设置 `days`。

几条约定：

- `wear` 由上游决定，比如这个小时戴了 45 分钟以上才算 1。只戴了一小会儿的小时请标 0，
  不要当成“活动量很低”传进来，否则基线会被拉低。
- `wear = 0` 的小时会被跳过，不会当成 0。
- `wear = 1, value = 0` 是有效数据，意思是戴着但没动，夜里很常见。
- 天数不够 14 天也能算，样本少的小时会标成 `building` 或 `insufficient`（见下文）。

### 当天数据 `evobeing_day`

`value[24]` 和 `wear[24]`，含义同上。用 `evobeing_day_init()` 初始化后所有小时都是“没戴”，
再用 `evobeing_day_set()` 填有数据的小时。不需要判定当天时，`evobeing_zhenghuimin_run()` 的
`today` 参数传 `NULL`，只算基线。

## 输出 `evobeing_result`

```c
typedef struct {
    evobeing_baseline  baseline;      /* 24 小时基线 */
    int                has_today;     /* 传了 today 才为 1 */
    evobeing_deviation today[24];     /* 当天每小时的判定，下标就是小时 */
    int                n_blocks;
    evobeing_block     blocks[24];    /* 连续偏离时段 */
} evobeing_result;
```

结构体大约 10 KB，放栈上没问题。下面列常用字段，完整定义看头文件注释。

### `baseline.hours[h]`：每小时的基线

| 字段 | 说明 |
|---|---|
| `status` | `INSUFFICIENT`：有效样本 < 3，不建基线；`BUILDING`：3~6 个，有基线但不建议对用户下结论；`COMPARABLE`：>= 7 个 |
| `has_stats` | 为 0 时下面的统计量都没有意义 |
| `n_valid` / `n_missing` / `n_zero` | 有效样本数、缺失数、有效样本里等于 0 的个数 |
| `median` | 基线点（中位数） |
| `p10` `p25` `p75` `p90` | 分位数 |
| `inner_low` ~ `inner_high` | 常见范围，基本是 P25~P75 |
| `outer_low` ~ `outer_high` | 判定边界，基本是 P10~P90，超出就算偏高/偏低 |
| `rest_expected` | 1 表示这是静息时段（一般是夜里） |
| `p_active` / `active_level` | 有活动的天数占比；有活动时的中位强度 |

`baseline.floor` 是区间最小半宽，`baseline.scale` 是这个人典型活动小时的量级，`baseline.days_used` 是参与计算的天数。

### `today[h]`：当天每小时的判定

| 字段 | 说明 |
|---|---|
| `state` | 见下表 |
| `baseline_status` | 这个小时基线的成熟度，`BUILDING` 时建议只画图不给结论 |
| `has_value` / `value` | 当天的值 |
| `has_expected` / `expected` | 平时的值（基线中位数） |
| `has_delta` / `delta` | `value - expected` |
| `has_z` / `z` | 偏离强度 `(value - median) / (1.4826 × mad_eff)`，只用来排序，不参与判定 |
| `band_position` | `"below"` / `"inside"` / `"above"` / `"unknown"` |
| `reason` | 一句中文说明，UTF-8，指向静态字符串，不用释放 |

`state` 的取值：

| 枚举 | 名称字符串 | 含义 |
|---|---|---|
| `EVOBEING_STATE_NO_DATA` | `no_data` | 当天这个小时没戴，不比较 |
| `EVOBEING_STATE_NO_BASELINE` | `no_baseline` | 这个小时历史样本不够 |
| `EVOBEING_STATE_NORMAL` | `normal` | 正常 |
| `EVOBEING_STATE_HIGH` | `high` | 高于 P90 |
| `EVOBEING_STATE_LOW` | `low` | 低于 P10 |
| `EVOBEING_STATE_UNUSUAL_MOVEMENT` | `unusual_movement` | 静息时段出现明显活动 |

### `blocks[i]`：连续偏离时段

相邻小时只要判定相同（都是 high、都是 low 或都是 unusual_movement）就合并成一段。

| 字段 | 说明 |
|---|---|
| `start_hour` / `end_hour` | 起止小时，都包含 |
| `n_hours` | 小时数 |
| `state` | `HIGH` / `LOW` / `UNUSUAL_MOVEMENT` |
| `total` / `expected` | 时段内当天合计、平时合计 |
| `has_ratio` / `ratio` | `total / expected`，异常活动或平时为 0 时没有 |
| `text` | 现成的一句话，如“08:00–11:00 连续 3 小时低于平时（合计 581，为平时同期的 38%）” |

## 函数

| 函数 | 说明 | 返回 |
|---|---|---|
| `evobeing_input_init(in)` | 历史数据清零 | - |
| `evobeing_input_set(in, day, hour, wear, value)` | 写一条历史数据，同一格重复写会覆盖 | 0 成功，-1 参数越界或值不合法 |
| `evobeing_day_init(today)` | 当天数据清零 | - |
| `evobeing_day_set(today, hour, wear, value)` | 写当天一个小时 | 0 / -1 |
| `evobeing_config_default(cfg)` | 填默认参数 | - |
| `evobeing_zhenghuimin_run(history, today, cfg, out)` | 拟合 + 判定 + 合并时段，`today`、`cfg` 可传 NULL | 0 / -1 |
| `evobeing_fit(in, cfg, out)` | 只拟合基线 | 0 / -1 |
| `evobeing_compare(b, cfg, hour, has_value, value)` | 判定一个小时，`has_value = 0` 表示没戴 | `evobeing_deviation` |
| `evobeing_summarize(devs, n, out, max)` | 把按小时排好的判定合并成时段 | 时段数，-1 参数不合法 |
| `evobeing_state_name(s)` / `evobeing_status_name(s)` | 枚举转字符串 | 静态字符串 |

返回 -1 的情况：指针为空、`days` 不在 1~14、参数自相矛盾（比如 P10 大于 P25）。
函数都是可重入的，多线程各用各的结构体就行。

## 参数

`evobeing_config_default()` 给的默认值，一般不用改：

| 字段 | 默认 | 说明 |
|---|---|---|
| `min_n_any` | 3 | 有效样本少于这个数不建基线 |
| `min_n_comparable` | 7 | 达到这个数才算 `COMPARABLE` |
| `inner_q_low` / `inner_q_high` | 25 / 75 | 常见范围 |
| `outer_q_low` / `outer_q_high` | 10 / 90 | 判定边界 |
| `floor_ratio` | 0.10 | 区间最小半宽 = 0.10 × scale |
| `floor_min` | 1.0 | 区间最小半宽的绝对下限 |
| `rest_p_active` | 0.20 | 有活动的天数不到 20%，算静息时段 |
| `rest_level_ratio` | 0.15 | 中位数不到 0.15 × scale，也算静息时段 |
| `rest_signal_min` | 0 | 静息时段报异常活动的最低活动量 |

## 算法

每个小时单独算，互不影响。

1. 取这个小时所有 `wear = 1` 的值，个数记为 n。n < 3 不建基线；3 <= n < 7 标 `building`；n >= 7 标 `comparable`。
2. 基线点取中位数。常见范围取 P25~P75，判定边界取 P10~P90（线性插值，和 numpy 默认一致）。
3. 区间太窄时撑开。`floor = max(floor_min, floor_ratio × scale)`，其中 scale 是 24 个小时中位数里大于 0 的那些再取中位数。
   如果分位区间比 `median ± floor` 还窄，就用 `median ± floor`，下沿不低于 0。
   这样几天数值碰巧一样时，不会因为一点点波动就报异常。
4. 静息时段：有活动的天数占比 < 0.20，或者中位数 < 0.15 × scale。这种时段（通常是睡觉时间）
   只判“有没有明显活动”，不判“偏低”，因为少动本来就是正常的。

判定规则：

```
当天没戴                                  -> no_data，不当成 0
这个小时 insufficient                      -> no_baseline
静息时段：value > max(outer_high, rest_signal_min) -> unusual_movement，否则 normal
其他时段：value > outer_high -> high；value < outer_low -> low；否则 normal
```

为什么区间用分位数而不是 `中位数 ± k × MAD`：活动量不会小于 0，分布右偏，天与天之间差别也大。
用 MAD 算出来的区间下沿基本都被截到 0，“偏低”永远判不出来。拿示例数据看几个小时：

| 小时 | 中位数 | 中位数 ± 2.8σ | P10~P90 | P25~P75 |
|---|---:|---|---|---|
| 09:00 | 634 | 0 ~ 1381 | 419 ~ 868 | 455 ~ 715 |
| 10:00 | 350 | 0 ~ 746 | 159 ~ 446 | 204 ~ 426 |
| 19:00 | 1050 | 0 ~ 2121 | 600 ~ 3289 | 753 ~ 1210 |

所以点估计和 z 用中位数/MAD，区间用分位数。

计算量很小：每个小时最多 14 个数排序几次，所有数组都在栈上。

## 命令行程序

`src/evobeing_main.c` 是一个读 CSV、输出 JSON 的小程序，方便不写代码直接看效果，
也可以当作怎么把文件数据喂给接口的参考。main() 里只有读输入、调 `evobeing_zhenghuimin_run()`、写输出三步。

```bash
build/evobeing_baseline [-o out.json] <history.csv> [today.csv]
```

history.csv，每行 `day,hour,wear,activity`：

```
day,hour,wear,activity
0,0,1,7.0
0,1,1,4.0
0,4,0,
...
```

today.csv，每行 `hour,wear,activity`：

```
hour,wear,activity
0,1,4.0
3,1,260.0
15,0,0
...
```

- 第一行可以是表头，`#` 开头的是注释，空行忽略，CRLF 也行，字段两边可以有空格。
- `wear = 0` 时 activity 可以留空。没出现的 (day, hour) 当作没戴，不过最好把 336 行都写全。
- 同一个 (day, hour) 出现两次、day/hour/wear 写成小数、行尾有多余字符、单行超过 511 个字符，都会报错退出。

输出 JSON 的字段和上面的结构体一一对应，样例见 `examples/data/output.json`。没给 today.csv 时 `today` 是 `null`，`blocks` 是空数组。

退出码：0 成功，1 命令行用法不对，2 输入文件有问题（打不开、格式不对、重复行等），3 计算或写出失败。

## 已知限制

- 窗口固定 14 天，样本不够时降级为 `building` / `insufficient`，不会硬算。
- 不区分工作日和周末。14 天里每个小时只有 4 个左右周末样本，单独算区间会很不稳。
- 没有趋势或时间衰减，受伤、换工作、换季这种变化要过一阵子基线才跟上。
- n = 14 时 P10/P90 大约就是第 2 小和第 2 大的值，本身有噪声，所以才有 `status` 这个字段。
- 这只是描述“和自己平时比”的统计，不是运动建议，也不说明原因。
