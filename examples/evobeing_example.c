/*
 * evobeing_example.c
 *
 * 在自己的程序里调用 evobeing 的示例。作者：zhenghuimin
 *
 * 编译运行（在仓库根目录）：
 *   cc -std=c11 -Iinclude examples/evobeing_example.c src/evobeing_baseline.c -lm -o evobeing_example
 *   ./evobeing_example
 *
 * 示例数据是代码里直接构造的：一个人典型的一天乘上每天不同的活跃系数，
 * 作为过去 14 天的历史；"今天"在几个时段故意做了偏离，方便看判定结果。
 */

#include "evobeing_baseline.h"

#include <stdio.h>

/* 典型的一天，0~23 点每小时的活动量（可以理解为步数） */
static const double zhenghuimin_typical_day[EVOBEING_HOURS] = {
    5, 4, 3, 3, 4, 7, 30, 190,            /* 00~07 点 */
    520, 620, 380, 330, 430, 470, 300, 280, /* 08~15 点 */
    330, 430, 560, 720, 380, 250, 110, 38   /* 16~23 点 */
};

/* 过去 14 天每天的整体活跃系数，1.0 表示和典型的一天一样 */
static const double zhenghuimin_day_factor[EVOBEING_WINDOW_DAYS] = {
    0.92, 1.05, 0.88, 1.10, 0.97, 1.02, 0.85,
    1.15, 0.95, 1.00, 1.08, 0.90, 1.03, 0.98
};

int main(void)
{
    evobeing_input evobeing_history; /* 输入 1：过去 14 天 × 24 小时 */
    evobeing_day evobeing_today;     /* 输入 2：要判定的那一天 */
    evobeing_result evobeing_res;    /* 输出 */
    int d;
    int h;
    int i;

    /* 1. 填历史数据。day 0 是最早的一天，day 13 是昨天。 */
    evobeing_input_init(&evobeing_history);
    for (d = 0; d < EVOBEING_WINDOW_DAYS; d++) {
        for (h = 0; h < EVOBEING_HOURS; h++) {
            int wear = 1;
            double value = zhenghuimin_typical_day[h] * zhenghuimin_day_factor[d];

            if (d == 3 && h >= 14 && h <= 16) {
                wear = 0; /* 第 3 天下午摘了手环：标 wear=0，不要写成 0 步 */
            }
            if (evobeing_input_set(&evobeing_history, d, h, wear, value) != 0) {
                fprintf(stderr, "历史数据不合法: day=%d hour=%d\n", d, h);
                return 1;
            }
        }
    }

    /* 2. 填今天的数据：先按典型的一天填，再改几个小时 */
    evobeing_day_init(&evobeing_today);
    for (h = 0; h < EVOBEING_HOURS; h++) {
        evobeing_day_set(&evobeing_today, h, 1, zhenghuimin_typical_day[h]);
    }
    evobeing_day_set(&evobeing_today, 3, 1, 260.0);   /* 凌晨 3 点起来走动 */
    evobeing_day_set(&evobeing_today, 8, 1, 150.0);   /* 上午明显活动少 */
    evobeing_day_set(&evobeing_today, 9, 1, 180.0);
    evobeing_day_set(&evobeing_today, 15, 0, 0.0);    /* 15 点没戴 */
    evobeing_day_set(&evobeing_today, 19, 1, 1800.0); /* 晚上去跑步了 */

    /* 3. 一次调用完成计算。cfg 传 NULL 表示用默认参数。 */
    if (evobeing_zhenghuimin_run(&evobeing_history, &evobeing_today, NULL,
                                 &evobeing_res) != 0) {
        fprintf(stderr, "evobeing 计算失败\n");
        return 1;
    }

    /* 4. 读结果 */
    printf("evobeing %s  作者 %s\n", EVOBEING_VERSION, EVOBEING_AUTHOR);
    printf("参与计算 %d 天，floor = %.1f\n\n",
           evobeing_res.baseline.days_used, evobeing_res.baseline.floor);

    printf("小时   今天    基线中位数   正常范围(P10~P90)   判定\n");
    for (h = 0; h < EVOBEING_HOURS; h++) {
        const evobeing_hour *bh = &evobeing_res.baseline.hours[h];
        const evobeing_deviation *dv = &evobeing_res.today[h];

        printf("%02d:00  ", h);
        if (dv->has_value) {
            printf("%6.0f  ", dv->value);
        } else {
            printf("%6s  ", "-");
        }
        if (bh->has_stats) {
            printf("%8.0f     %7.0f ~ %-7.0f     ", bh->median, bh->outer_low, bh->outer_high);
        } else {
            printf("%8s     %17s     ", "-", "-");
        }
        printf("%-16s %s\n", evobeing_state_name(dv->state), dv->reason);
    }

    printf("\n偏离时段：\n");
    for (i = 0; i < evobeing_res.n_blocks; i++) {
        printf("  %s\n", evobeing_res.blocks[i].text);
    }

    /*
     * 5. 分步接口：基线每天算一次，之后每来一个小时的数据就判一次。
     *    这里用今天 10 点的一个新值演示。
     */
    {
        evobeing_config zhenghuimin_cfg;
        evobeing_baseline zhenghuimin_base;
        evobeing_deviation zhenghuimin_dev;

        evobeing_config_default(&zhenghuimin_cfg);
        if (evobeing_fit(&evobeing_history, &zhenghuimin_cfg, &zhenghuimin_base) != 0) {
            fprintf(stderr, "evobeing_fit 失败\n");
            return 1;
        }
        zhenghuimin_dev = evobeing_compare(&zhenghuimin_base, &zhenghuimin_cfg, 10, 1, 120.0);
        printf("\n单独判定 10:00 = 120：%s，%s\n",
               evobeing_state_name(zhenghuimin_dev.state), zhenghuimin_dev.reason);
    }

    return 0;
}
