/*
 * test_hb.c — 单元测试（无外部依赖，make test 会运行）。
 *
 * 覆盖：统计原语、0/缺失/非法值、状态分档、floor、分位区间、
 *       静息时段（p_active 与量级两条路径）、判定、时段合并、文本、参数校验。
 */

#include "hb.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                \
    do {                                                                     \
        double _a = (a);                                                     \
        double _b = (b);                                                     \
        g_checks++;                                                          \
        if (fabs(_a - _b) > (eps)) {                                         \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d  %s=%.6f != %s=%.6f\n",              \
                    __FILE__, __LINE__, #a, _a, #b, _b);                     \
        }                                                                    \
    } while (0)

static void fill_hour(hb_input *in, int hour, const double *vals, int n)
{
    int d;
    for (d = 0; d < n; d++) {
        CHECK(hb_input_set(in, d, hour, 1, vals[d]) == 0);
    }
}

static void test_stat_primitives(void)
{
    double odd[] = {3.0, 1.0, 2.0};
    double even[] = {4.0, 1.0, 3.0, 2.0};
    double seq[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    double out = 0.0;

    CHECK(hb_median(odd, 3, &out) == 0);
    CHECK_NEAR(out, 2.0, 1e-12);
    CHECK(hb_median(even, 4, &out) == 0);
    CHECK_NEAR(out, 2.5, 1e-12);

    CHECK(hb_quantile(seq, 5, 25.0, &out) == 0);
    CHECK_NEAR(out, 2.0, 1e-12);
    CHECK(hb_quantile(seq, 5, 90.0, &out) == 0);
    CHECK_NEAR(out, 4.6, 1e-12);
    CHECK(hb_quantile(seq, 5, 0.0, &out) == 0);
    CHECK_NEAR(out, 1.0, 1e-12);
    CHECK(hb_quantile(seq, 5, 100.0, &out) == 0);
    CHECK_NEAR(out, 5.0, 1e-12);

    CHECK(hb_mad(seq, 5, 3.0, &out) == 0);
    CHECK_NEAR(out, 1.0, 1e-12);

    CHECK(hb_median(seq, 0, &out) == -1);
    CHECK(hb_median(NULL, 5, &out) == -1);
    CHECK(hb_median(seq, 5, NULL) == -1);
}

static void test_zero_and_wear(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;

    hb_config_default(&cfg);
    hb_input_init(&in);
    {
        double zeros[] = {0.0, 0.0, 0.0};
        fill_hour(&in, 10, zeros, 3);
    }
    CHECK(hb_fit(&in, &cfg, &b) == 0);
    CHECK(b.hours[10].n_valid == 3);
    CHECK(b.hours[10].n_zero == 3);
    CHECK(b.hours[10].n_missing == 0);
    CHECK(b.hours[10].status == HB_STATUS_BUILDING);
    CHECK_NEAR(b.hours[10].median, 0.0, 1e-12);
    CHECK_NEAR(b.hours[10].outer_low, 0.0, 1e-12);
    CHECK(b.hours[10].outer_high > 0.0);
    CHECK(b.hours[10].rest_expected == 1);

    /* 只把 day 3 的 15 点标记为未佩戴：n_valid 应为 1，而不是 4。 */
    hb_input_init(&in);
    {
        double one[] = {50.0, 60.0, 70.0};
        fill_hour(&in, 15, one, 3);
    }
    CHECK(hb_input_set(&in, 0, 15, 0, 0.0) == 0);
    CHECK(hb_fit(&in, &cfg, &b) == 0);
    CHECK(b.hours[15].n_valid == 2);
    CHECK(b.hours[15].n_missing == 1);
}

static void test_all_missing_hour(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;
    hb_deviation d;

    hb_config_default(&cfg);
    hb_input_init(&in);
    in.days = 14; /* 14 天全未佩戴 */
    CHECK(hb_fit(&in, &cfg, &b) == 0);
    CHECK(b.hours[3].n_valid == 0);
    CHECK(b.hours[3].n_missing == 14);
    CHECK(b.hours[3].status == HB_STATUS_INSUFFICIENT);
    CHECK(b.hours[3].has_stats == 0);
    CHECK(b.hours[3].band_available == 0);

    d = hb_compare(&b, &cfg, 3, 0, 0.0);
    CHECK(d.state == HB_DEV_NO_DATA);
    CHECK(d.has_value == 0);
    d = hb_compare(&b, &cfg, 3, 1, 120.0);
    CHECK(d.state == HB_DEV_NO_BASELINE);
}

static void test_floor_from_scale(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;
    double day[14];
    double zeros[14];
    int i;

    hb_config_default(&cfg);
    hb_input_init(&in);
    for (i = 0; i < 14; i++) {
        day[i] = 1000.0;
        zeros[i] = 0.0;
    }
    fill_hour(&in, 12, day, 14);
    fill_hour(&in, 2, zeros, 14);
    CHECK(hb_fit(&in, &cfg, &b) == 0);

    CHECK(b.has_scale == 1);
    CHECK_NEAR(b.scale, 1000.0, 1e-9);
    CHECK_NEAR(b.floor, 100.0, 1e-9); /* 10% × 1000 */
    CHECK_NEAR(b.hours[2].median, 0.0, 1e-12);
    CHECK_NEAR(b.hours[2].mad_raw, 0.0, 1e-12);
    CHECK_NEAR(b.hours[2].mad_eff, 100.0, 1e-9);
    CHECK_NEAR(b.hours[2].outer_high, 100.0, 1e-9); /* 0 + floor */
}

static void test_mad_zero_floored(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;
    double vals[14];
    int i;

    hb_config_default(&cfg);
    hb_input_init(&in);
    for (i = 0; i < 12; i++) {
        vals[i] = 0.0;
    }
    vals[12] = 500.0;
    vals[13] = 800.0;
    fill_hour(&in, 9, vals, 14);
    CHECK(hb_fit(&in, &cfg, &b) == 0);

    CHECK_NEAR(b.hours[9].median, 0.0, 1e-12);
    CHECK_NEAR(b.hours[9].mad_raw, 0.0, 1e-12);
    CHECK(b.hours[9].mad_eff > 0.0);
    CHECK(b.hours[9].outer_high > b.hours[9].median);
}

static void test_band_ordering(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;
    double vals[] = {700, 710, 720, 730, 740, 750, 760, 780,
                     800, 820, 840, 860, 880, 900};

    hb_config_default(&cfg);
    hb_input_init(&in);
    fill_hour(&in, 18, vals, 14);
    CHECK(hb_fit(&in, &cfg, &b) == 0);

    CHECK(b.hours[18].p10 <= b.hours[18].p25);
    CHECK(b.hours[18].p25 <= b.hours[18].median);
    CHECK(b.hours[18].median <= b.hours[18].p75);
    CHECK(b.hours[18].p75 <= b.hours[18].p90);
    CHECK(b.hours[18].outer_low <= b.hours[18].inner_low);
    CHECK(b.hours[18].inner_low <= b.hours[18].median);
    CHECK(b.hours[18].median <= b.hours[18].inner_high);
    CHECK(b.hours[18].inner_high <= b.hours[18].outer_high);
    CHECK(b.hours[18].outer_low > 0.0); /* 日间下沿不应被压到 0 */
}

static void test_degenerate_widened(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;
    double vals[14];
    int i;

    hb_config_default(&cfg);
    hb_input_init(&in);
    for (i = 0; i < 14; i++) {
        vals[i] = 800.0;
    }
    fill_hour(&in, 18, vals, 14);
    CHECK(hb_fit(&in, &cfg, &b) == 0);
    CHECK_NEAR(b.hours[18].mad_raw, 0.0, 1e-12);
    CHECK(b.hours[18].outer_high > b.hours[18].median);
    CHECK(b.hours[18].outer_low < b.hours[18].median);
}

static void test_rest_detection(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;
    double sparse[14];
    double level[14];
    double day[14];
    int i;

    hb_config_default(&cfg);

    /* 路径一：12 个 0 + 2 个非零 → p_active ≈ 0.14 */
    hb_input_init(&in);
    for (i = 0; i < 12; i++) {
        sparse[i] = 0.0;
    }
    sparse[12] = 500.0;
    sparse[13] = 600.0;
    fill_hour(&in, 10, sparse, 14);
    CHECK(hb_fit(&in, &cfg, &b) == 0);
    CHECK_NEAR(b.hours[10].p_active, 2.0 / 14.0, 1e-9);
    CHECK(b.hours[10].rest_expected == 1);

    /* 路径二：每天都有轻微活动，但量级远低于典型小时 → 仍判静息 */
    hb_input_init(&in);
    for (i = 0; i < 14; i++) {
        level[i] = (double)(3 + (i % 3));
        day[i] = 1000.0;
    }
    fill_hour(&in, 3, level, 14);
    fill_hour(&in, 12, day, 14);
    CHECK(hb_fit(&in, &cfg, &b) == 0);
    CHECK(b.hours[3].p_active > 0.99);
    CHECK(b.hours[3].rest_expected == 1);
    CHECK(b.hours[12].rest_expected == 0);
}

static void test_compare_states(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;
    hb_deviation d;
    double vals[] = {700, 710, 720, 730, 740, 750, 760, 780,
                     800, 820, 840, 860, 880, 900};

    hb_config_default(&cfg);
    hb_input_init(&in);
    fill_hour(&in, 18, vals, 14);
    CHECK(hb_fit(&in, &cfg, &b) == 0);
    CHECK(b.hours[18].status == HB_STATUS_COMPARABLE);

    d = hb_compare(&b, &cfg, 18, 1, 1000.0);
    CHECK(d.state == HB_DEV_HIGH);
    CHECK(d.has_z == 1);
    CHECK(d.has_delta == 1);
    CHECK(d.has_expected == 1);
    CHECK(strcmp(d.band_position, "above") == 0);
    CHECK_NEAR(d.expected, b.hours[18].median, 1e-12);

    d = hb_compare(&b, &cfg, 18, 1, 600.0);
    CHECK(d.state == HB_DEV_LOW);
    CHECK(strcmp(d.band_position, "below") == 0);

    d = hb_compare(&b, &cfg, 18, 1, 755.0);
    CHECK(d.state == HB_DEV_NORMAL);
    CHECK(strcmp(d.band_position, "inside") == 0);
}

static void test_rest_never_low(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;
    hb_deviation d;
    double night[14];
    double day[14];
    int i;

    hb_config_default(&cfg);
    hb_input_init(&in);
    for (i = 0; i < 14; i++) {
        night[i] = 0.0;
        day[i] = 1000.0;
    }
    fill_hour(&in, 3, night, 14);
    fill_hour(&in, 12, day, 14);
    CHECK(hb_fit(&in, &cfg, &b) == 0);
    CHECK(b.hours[3].rest_expected == 1);

    d = hb_compare(&b, &cfg, 3, 1, 0.0);
    CHECK(d.state == HB_DEV_NORMAL);
    d = hb_compare(&b, &cfg, 3, 1, 50.0); /* 低于 floor=100，不算异常 */
    CHECK(d.state == HB_DEV_NORMAL);
    d = hb_compare(&b, &cfg, 3, 1, 500.0);
    CHECK(d.state == HB_DEV_UNUSUAL_MOVEMENT);
}

static void test_unusual_threshold(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;
    hb_deviation d;

    hb_config_default(&cfg);
    cfg.rest_signal_min = 900.0; /* 绝对门槛高于区间上沿 */
    hb_input_init(&in);
    in.days = 14;
    CHECK(hb_fit(&in, &cfg, &b) == 0);
    d = hb_compare(&b, &cfg, 3, 1, 800.0);
    CHECK(d.state == HB_DEV_NO_BASELINE); /* 没有样本时仍然是 no_baseline */

    /* 有样本但区间很窄时，rest_signal_min 生效 */
    {
        double zero[14];
        int i;
        for (i = 0; i < 14; i++) {
            zero[i] = 0.0;
        }
        hb_input_init(&in);
        fill_hour(&in, 3, zero, 14);
        CHECK(hb_fit(&in, &cfg, &b) == 0);
        d = hb_compare(&b, &cfg, 3, 1, 500.0);
        CHECK(d.state == HB_DEV_NORMAL); /* 500 < 900 */
        d = hb_compare(&b, &cfg, 3, 1, 950.0);
        CHECK(d.state == HB_DEV_UNUSUAL_MOVEMENT);
    }
}

static void build_low_day(hb_baseline *b, hb_config *cfg)
{
    hb_input in;
    double v[14];
    int i;
    int hours[4] = {8, 9, 10, 18};

    hb_config_default(cfg);
    hb_input_init(&in);
    for (i = 0; i < 14; i++) {
        v[i] = 500.0;
    }
    for (i = 0; i < 4; i++) {
        fill_hour(&in, hours[i], v, 14);
    }
    CHECK(hb_fit(&in, cfg, b) == 0);
    CHECK(b->hours[8].status == HB_STATUS_COMPARABLE);
}

static void test_blocks_merge(void)
{
    hb_baseline b;
    hb_config cfg;
    hb_deviation devs[HB_HOURS];
    hb_block blocks[HB_HOURS];
    int nb;
    int h;

    build_low_day(&b, &cfg);
    for (h = 0; h < HB_HOURS; h++) {
        devs[h] = hb_compare(&b, &cfg, h, 0, 0.0);
    }
    devs[8] = hb_compare(&b, &cfg, 8, 1, 100.0);
    devs[9] = hb_compare(&b, &cfg, 9, 1, 120.0);
    devs[10] = hb_compare(&b, &cfg, 10, 1, 80.0);
    devs[18] = hb_compare(&b, &cfg, 18, 1, 100.0);

    nb = hb_summarize(devs, HB_HOURS, blocks, HB_HOURS);
    CHECK(nb == 2);
    CHECK(blocks[0].start_hour == 8);
    CHECK(blocks[0].end_hour == 10);
    CHECK(blocks[0].n_hours == 3);
    CHECK(blocks[0].state == HB_DEV_LOW);
    CHECK_NEAR(blocks[0].total, 300.0, 1e-9);
    CHECK_NEAR(blocks[0].expected, 1500.0, 1e-9);
    CHECK_NEAR(blocks[0].ratio, 0.2, 1e-9);
    CHECK(blocks[1].start_hour == 18);
    CHECK(blocks[1].end_hour == 18);
}

static void test_blocks_break_on_normal_and_missing(void)
{
    hb_baseline b;
    hb_config cfg;
    hb_deviation devs[HB_HOURS];
    hb_block blocks[HB_HOURS];
    int nb;
    int h;

    build_low_day(&b, &cfg);
    for (h = 0; h < HB_HOURS; h++) {
        devs[h] = hb_compare(&b, &cfg, h, 0, 0.0);
    }
    devs[8] = hb_compare(&b, &cfg, 8, 1, 100.0);
    devs[9] = hb_compare(&b, &cfg, 9, 1, 500.0); /* 正常，打断 */
    devs[10] = hb_compare(&b, &cfg, 10, 1, 100.0);
    nb = hb_summarize(devs, HB_HOURS, blocks, HB_HOURS);
    CHECK(nb == 2);
    CHECK(blocks[0].start_hour == 8 && blocks[0].end_hour == 8);
    CHECK(blocks[1].start_hour == 10 && blocks[1].end_hour == 10);

    devs[9] = hb_compare(&b, &cfg, 9, 0, 0.0); /* 缺失，同样打断 */
    nb = hb_summarize(devs, HB_HOURS, blocks, HB_HOURS);
    CHECK(nb == 2);
    CHECK(blocks[0].end_hour == 8);
    CHECK(blocks[1].start_hour == 10);
}

static void test_block_text(void)
{
    hb_baseline b;
    hb_config cfg;
    hb_deviation devs[HB_HOURS];
    hb_block blocks[HB_HOURS];
    char text[160];
    int nb;
    int h;

    build_low_day(&b, &cfg);
    for (h = 0; h < HB_HOURS; h++) {
        devs[h] = hb_compare(&b, &cfg, h, 0, 0.0);
    }
    devs[8] = hb_compare(&b, &cfg, 8, 1, 100.0);
    devs[9] = hb_compare(&b, &cfg, 9, 1, 120.0);
    devs[10] = hb_compare(&b, &cfg, 10, 1, 80.0);
    nb = hb_summarize(devs, HB_HOURS, blocks, HB_HOURS);
    CHECK(nb == 1);
    CHECK(hb_block_text(&blocks[0], text, sizeof(text)) == 0);
    CHECK(strstr(text, "08:00–11:00") != NULL);
    CHECK(strstr(text, "连续 3 小时") != NULL);
    CHECK(strstr(text, "20%") != NULL);
    CHECK(hb_block_text(&blocks[0], text, 8) == -1); /* 缓冲区不足 */
}

static void test_status_thresholds(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;
    double two[2] = {10.0, 20.0};
    double three[3] = {10.0, 20.0, 30.0};
    double seven[7] = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0};

    hb_config_default(&cfg);
    hb_input_init(&in);
    fill_hour(&in, 0, two, 2);
    fill_hour(&in, 1, three, 3);
    fill_hour(&in, 2, seven, 7);
    CHECK(hb_fit(&in, &cfg, &b) == 0);
    CHECK(b.hours[0].status == HB_STATUS_INSUFFICIENT);
    CHECK(b.hours[1].status == HB_STATUS_BUILDING);
    CHECK(b.hours[2].status == HB_STATUS_COMPARABLE);
    CHECK(b.window_days == 7);
}

static void test_input_validation(void)
{
    hb_input in;
    hb_config cfg;
    hb_baseline b;
    int i;

    hb_input_init(&in);
    CHECK(in.days == 0);
    CHECK(hb_input_set(&in, 0, 0, 1, 10.0) == 0);
    CHECK(in.days == 1);
    CHECK(hb_input_set(&in, 3, 5, 1, 10.0) == 0);
    CHECK(in.days == 4);
    CHECK(hb_input_set(&in, 14, 0, 1, 10.0) == -1);
    CHECK(hb_input_set(&in, 0, 24, 1, 10.0) == -1);
    CHECK(hb_input_set(&in, 0, 0, 2, 10.0) == -1);
    CHECK(hb_input_set(&in, 0, 0, 1, -1.0) == -1);
    CHECK(hb_input_set(&in, 0, 0, 1, NAN) == -1);
    CHECK(hb_input_set(&in, 0, 0, 0, 123.0) == 0); /* wear=0 忽略 value */

    hb_config_default(&cfg);
    CHECK(hb_fit(NULL, &cfg, &b) == -1);
    CHECK(hb_fit(&in, NULL, &b) == -1);
    CHECK(hb_fit(&in, &cfg, NULL) == -1);

    in.days = 0;
    CHECK(hb_fit(&in, &cfg, &b) == -1);

    hb_input_init(&in);
    in.days = 14;
    for (i = 0; i < 14; i++) {
        in.wear[i][5] = 1;
        in.value[i][5] = -3.0; /* 防御：非法值按缺失 */
    }
    CHECK(hb_fit(&in, &cfg, &b) == 0);
    CHECK(b.hours[5].n_valid == 0);
    CHECK(b.hours[5].status == HB_STATUS_INSUFFICIENT);
}

static void test_config_validation(void)
{
    hb_input in;
    hb_baseline b;
    hb_config cfg;

    hb_input_init(&in);
    in.days = 14;

    hb_config_default(&cfg);
    cfg.min_n_comparable = 2; /* < min_n_any */
    CHECK(hb_fit(&in, &cfg, &b) == -1);

    hb_config_default(&cfg);
    cfg.inner_q_low = 80.0; /* 与 outer 交叉 */
    cfg.outer_q_low = 50.0;
    CHECK(hb_fit(&in, &cfg, &b) == -1);

    hb_config_default(&cfg);
    cfg.floor_ratio = -1.0;
    CHECK(hb_fit(&in, &cfg, &b) == -1);

    hb_config_default(&cfg);
    cfg.rest_p_active = 1.5;
    CHECK(hb_fit(&in, &cfg, &b) == -1);
}

static void test_names(void)
{
    CHECK(strcmp(hb_state_name(HB_DEV_NO_DATA), "no_data") == 0);
    CHECK(strcmp(hb_state_name(HB_DEV_UNUSUAL_MOVEMENT), "unusual_movement") == 0);
    CHECK(strcmp(hb_status_name(HB_STATUS_BUILDING), "building") == 0);
    CHECK(strcmp(hb_status_name((hb_status)99), "unknown") == 0);
}

int main(void)
{
    test_stat_primitives();
    test_zero_and_wear();
    test_all_missing_hour();
    test_floor_from_scale();
    test_mad_zero_floored();
    test_band_ordering();
    test_degenerate_widened();
    test_rest_detection();
    test_compare_states();
    test_rest_never_low();
    test_unusual_threshold();
    test_blocks_merge();
    test_blocks_break_on_normal_and_missing();
    test_block_text();
    test_status_thresholds();
    test_input_validation();
    test_config_validation();
    test_names();

    printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
