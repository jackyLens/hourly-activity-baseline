/*
 * test_props.c — 随机化不变量测试。
 *
 * 用固定种子的 xorshift 生成 3000 组输入（不同天数、缺失率、零值比例、
 * 量级与部分参数），检查输出是否始终满足契约与内部一致性。
 * 与 test_hb.c 的定点用例互补：这里追求覆盖面，不依赖具体数值。
 */

#include "hb.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned long long g_rng = 0x9E3779B97F4A7C15ULL;
static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d  %s (iter data seed lost)\n",        \
                    __FILE__, __LINE__, #cond);                              \
        }                                                                    \
    } while (0)

static unsigned long long rnd64(void)
{
    unsigned long long x = g_rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng = x;
    return x;
}

static unsigned rnd(unsigned n)
{
    return (unsigned)(rnd64() % (unsigned long long)n);
}

static double rnd_value(void)
{
    switch (rnd(8)) {
    case 0:
        return 0.0;
    case 1:
        return (double)rnd(5);
    case 2:
        return (double)rnd(100);
    case 3:
        return (double)rnd(5000);
    case 4:
        return (double)rnd(1000000);
    case 5:
        return 0.0;
    case 6:
        return (double)rnd(50) / 4.0;
    default:
        return (double)rnd(20000);
    }
}

static int state_is_alert(hb_state s)
{
    return s == HB_DEV_HIGH || s == HB_DEV_LOW || s == HB_DEV_UNUSUAL_MOVEMENT;
}

static void check_hour(const hb_hour *hb, const hb_config *cfg, int days)
{
    CHECK(hb->hour >= 0 && hb->hour < HB_HOURS);
    CHECK(hb->n_valid >= 0 && hb->n_missing >= 0);
    CHECK(hb->n_valid + hb->n_missing == days);
    CHECK(hb->n_zero >= 0 && hb->n_zero <= hb->n_valid);
    CHECK(hb->status == HB_STATUS_INSUFFICIENT ||
          hb->status == HB_STATUS_BUILDING ||
          hb->status == HB_STATUS_COMPARABLE);
    CHECK(hb->rest_expected == 0 || hb->rest_expected == 1);
    CHECK(hb->band_available == hb->has_stats);

    if (hb->status == HB_STATUS_INSUFFICIENT) {
        CHECK(hb->has_stats == 0);
    }
    if (hb->status == HB_STATUS_BUILDING) {
        CHECK(hb->n_valid >= cfg->min_n_any);
        CHECK(hb->n_valid < cfg->min_n_comparable);
    }
    if (hb->status == HB_STATUS_COMPARABLE) {
        CHECK(hb->has_stats == 1);
        CHECK(hb->n_valid >= cfg->min_n_comparable);
    }
    if (!hb->has_stats) {
        return;
    }

    CHECK(hb->mad_eff >= hb->floor - 1e-9);
    CHECK(hb->p10 <= hb->p25 + 1e-9);
    CHECK(hb->p25 <= hb->median + 1e-9);
    CHECK(hb->median <= hb->p75 + 1e-9);
    CHECK(hb->p75 <= hb->p90 + 1e-9);
    CHECK(hb->outer_low <= hb->inner_low + 1e-9);
    CHECK(hb->inner_low <= hb->median + 1e-9);
    CHECK(hb->median <= hb->inner_high + 1e-9);
    CHECK(hb->inner_high <= hb->outer_high + 1e-9);
    CHECK(hb->inner_low >= 0.0);
    CHECK(hb->outer_low >= 0.0);
    CHECK(hb->p_active >= 0.0 && hb->p_active <= 1.0);
    CHECK(hb->active_level >= 0.0);
    if (hb->p_active < cfg->rest_p_active) {
        CHECK(hb->rest_expected == 1);
    }
}

static void check_deviation(const hb_deviation *dev, const hb_hour *hb,
                            const hb_config *cfg, int has_value)
{
    CHECK(dev->hour >= 0 && dev->hour < HB_HOURS);
    CHECK(dev->baseline_status == hb->status);

    if (!has_value) {
        CHECK(dev->state == HB_DEV_NO_DATA);
        CHECK(dev->has_value == 0);
        return;
    }
    if (!hb->has_stats) {
        CHECK(dev->state == HB_DEV_NO_BASELINE);
        return;
    }

    if (dev->state == HB_DEV_HIGH) {
        CHECK(dev->value > hb->outer_high);
    } else if (dev->state == HB_DEV_LOW) {
        CHECK(dev->value < hb->outer_low);
    } else if (dev->state == HB_DEV_UNUSUAL_MOVEMENT) {
        CHECK(hb->rest_expected == 1);
        CHECK(dev->value > hb->outer_high);
        CHECK(dev->value > cfg->rest_signal_min);
    } else if (dev->state == HB_DEV_NORMAL) {
        if (hb->rest_expected) {
            double gate = hb->outer_high > cfg->rest_signal_min
                              ? hb->outer_high : cfg->rest_signal_min;
            CHECK(dev->value <= gate);
        } else {
            CHECK(dev->value >= hb->outer_low);
            CHECK(dev->value <= hb->outer_high);
        }
    } else {
        CHECK(0); /* 不应出现 no_data / no_baseline */
    }
}

static void check_blocks(const hb_deviation *devs, const hb_baseline *b,
                         const hb_config *cfg)
{
    hb_block blocks[HB_HOURS];
    int nb = hb_summarize(devs, HB_HOURS, blocks, HB_HOURS);
    int i;

    CHECK(nb >= 0 && nb <= HB_HOURS);
    for (i = 0; i < nb; i++) {
        const hb_block *blk = &blocks[i];
        CHECK(blk->start_hour >= 0 && blk->start_hour < HB_HOURS);
        CHECK(blk->end_hour >= blk->start_hour && blk->end_hour < HB_HOURS);
        CHECK(blk->n_hours == blk->end_hour - blk->start_hour + 1);
        CHECK(state_is_alert(blk->state));
        CHECK(blk->total >= 0.0);
        if (blk->state == HB_DEV_UNUSUAL_MOVEMENT) {
            CHECK(blk->has_ratio == 0);
        }
        if (i > 0) {
            CHECK(blk->start_hour > blocks[i - 1].end_hour);
        }
        {
            char buf[256];
            int rc = hb_block_text(blk, buf, sizeof(buf));
            CHECK(rc == 0);
            CHECK(strlen(buf) > 0);
            if (rc == 0) {
                CHECK(strlen(buf) < sizeof(buf));
            }
        }
    }
    (void)b;
    (void)cfg;
}

static void sweep_block_text(void)
{
    hb_block blk;
    char buf[256];
    size_t len;

    memset(&blk, 0, sizeof(blk));
    blk.start_hour = 8;
    blk.end_hour = 10;
    blk.n_hours = 3;
    blk.state = HB_DEV_LOW;
    blk.total = 581.0;
    blk.expected = 1514.5;
    blk.has_ratio = 1;
    blk.ratio = 581.0 / 1514.5;

    for (len = 1; len <= sizeof(buf); len++) {
        int rc = hb_block_text(&blk, buf, len);
        CHECK(rc == 0 || rc == -1);
        if (rc == 0) {
            CHECK(strlen(buf) < len);
        }
    }
    CHECK(hb_block_text(NULL, buf, sizeof(buf)) == -1);
    CHECK(hb_block_text(&blk, NULL, sizeof(buf)) == -1);
    CHECK(hb_block_text(&blk, buf, 0) == -1);
}

int main(void)
{
    int iter;

    for (iter = 0; iter < 3000; iter++) {
        hb_input in;
        hb_config cfg;
        hb_baseline b;
        int days = (int)rnd(HB_WINDOW_DAYS) + 1;
        int d;
        int h;
        int ok;

        hb_input_init(&in);
        in.days = days;
        for (d = 0; d < days; d++) {
            for (h = 0; h < HB_HOURS; h++) {
                int wear = (rnd(100) < 78) ? 1 : 0;
                double v = rnd_value();
                ok = hb_input_set(&in, d, h, wear, v);
                CHECK(ok == 0);
            }
        }

        hb_config_default(&cfg);
        if (iter % 4 == 0) {
            cfg.rest_signal_min = (double)rnd(2000);
        }
        if (iter % 7 == 0) {
            cfg.floor_ratio = (double)rnd(40) / 100.0;
        }
        if (iter % 11 == 0) {
            cfg.min_n_any = 1 + (int)rnd(5);
            cfg.min_n_comparable = cfg.min_n_any + (int)rnd(8);
        }

        if (hb_fit(&in, &cfg, &b) != 0) {
            CHECK(0);
            continue;
        }
        CHECK(b.days_used == days);
        CHECK(isfinite(b.floor) && b.floor >= 0.0);
        if (b.has_scale) {
            CHECK(b.scale > 0.0);
            CHECK(b.floor >= cfg.floor_ratio * b.scale - 1e-9);
        }

        for (h = 0; h < HB_HOURS; h++) {
            const hb_hour *hb = &b.hours[h];
            hb_deviation dev;
            int has_value = (rnd(100) < 85) ? 1 : 0;
            double value = rnd_value();

            check_hour(hb, &cfg, days);
            dev = hb_compare(&b, &cfg, h, has_value, value);
            check_deviation(&dev, hb, &cfg, has_value);
        }

        {
            hb_deviation devs[HB_HOURS];
            for (h = 0; h < HB_HOURS; h++) {
                int has_value = (rnd(100) < 85) ? 1 : 0;
                devs[h] = hb_compare(&b, &cfg, h, has_value, rnd_value());
            }
            check_blocks(devs, &b, &cfg);
        }
    }

    sweep_block_text();

    printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
