/*
 * evobeing_baseline.c
 *
 * evobeing 按小时活动量个人基线的实现。作者：zhenghuimin
 *
 * 计算规则：
 *   1. wear = 0 算缺失，不补 0；wear = 1 且 value = 0 是有效数据。
 *   2. 基线点用中位数，区间用经验分位数（P25~P75、P10~P90）。
 *   3. 分位区间比 floor 还窄时，用 median ± floor 撑开。
 *   4. floor = max(floor_min, floor_ratio × scale)，
 *      scale 是 24 个小时中位数里大于 0 的那些再取中位数。
 *   5. 静息时段只判"异常活动"，不判"偏低"。
 *   6. z 用 MAD（不小于 floor）计算，只作参考，不参与判定。
 *
 * 文件内部的辅助函数统一用 zhenghuimin_ 前缀，对外接口用 evobeing_ 前缀。
 */

#include "evobeing_baseline.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZHENGHUIMIN_MAD_SCALE  1.4826
#define ZHENGHUIMIN_MAX_VALUES EVOBEING_HOURS /* 统计函数一次最多处理的样本数 */

/* ------------------------------------------------------------ 统计函数 */

static int zhenghuimin_cmp_double(const void *pa, const void *pb)
{
    double a = *(const double *)pa;
    double b = *(const double *)pb;
    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

static int zhenghuimin_sort_copy(const double *v, int n, double *out)
{
    if (v == NULL || out == NULL || n <= 0 || n > ZHENGHUIMIN_MAX_VALUES) {
        return 0;
    }
    memcpy(out, v, sizeof(double) * (size_t)n);
    qsort(out, (size_t)n, sizeof(double), zhenghuimin_cmp_double);
    return 1;
}

static int zhenghuimin_median(const double *v, int n, double *out)
{
    double tmp[ZHENGHUIMIN_MAX_VALUES];
    if (out == NULL || !zhenghuimin_sort_copy(v, n, tmp)) {
        return -1;
    }
    if (n % 2 != 0) {
        *out = tmp[n / 2];
    } else {
        /* 先各自除以 2 再相加，两个很大的数直接相加可能溢出成 inf */
        *out = tmp[n / 2 - 1] / 2.0 + tmp[n / 2] / 2.0;
    }
    return 0;
}

static int zhenghuimin_mad(const double *v, int n, double center, double *out)
{
    double dev[ZHENGHUIMIN_MAX_VALUES];
    int i;
    if (v == NULL || out == NULL || n <= 0 || n > ZHENGHUIMIN_MAX_VALUES) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        dev[i] = fabs(v[i] - center);
    }
    return zhenghuimin_median(dev, n, out);
}

/* 线性插值分位数（与 numpy 默认的 linear 一致），q 取 0..100 */
static int zhenghuimin_quantile(const double *v, int n, double q, double *out)
{
    double tmp[ZHENGHUIMIN_MAX_VALUES];
    double rank;
    double frac;
    int lo;
    int hi;

    if (out == NULL || !zhenghuimin_sort_copy(v, n, tmp)) {
        return -1;
    }
    if (q <= 0.0) {
        *out = tmp[0];
        return 0;
    }
    if (q >= 100.0) {
        *out = tmp[n - 1];
        return 0;
    }
    rank = (double)(n - 1) * q / 100.0;
    lo = (int)rank;
    hi = (lo + 1 < n) ? lo + 1 : n - 1;
    frac = rank - (double)lo;
    *out = tmp[lo] + (tmp[hi] - tmp[lo]) * frac;
    return 0;
}

/* ------------------------------------------------------------ 配置与输入 */

void evobeing_config_default(evobeing_config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->min_n_any = 3;
    cfg->min_n_comparable = 7;
    cfg->inner_q_low = 25.0;
    cfg->inner_q_high = 75.0;
    cfg->outer_q_low = 10.0;
    cfg->outer_q_high = 90.0;
    cfg->floor_ratio = 0.10;
    cfg->floor_min = 1.0;
    cfg->rest_p_active = 0.20;
    cfg->rest_level_ratio = 0.15;
    cfg->rest_signal_min = 0.0;
}

static int zhenghuimin_value_ok(int wear, double value)
{
    if (wear != 0 && wear != 1) {
        return 0;
    }
    if (wear == 1 && (!isfinite(value) || value < 0.0)) {
        return 0;
    }
    return 1;
}

void evobeing_input_init(evobeing_input *in)
{
    if (in == NULL) {
        return;
    }
    memset(in, 0, sizeof(*in));
}

int evobeing_input_set(evobeing_input *in, int day, int hour, int wear, double value)
{
    if (in == NULL || day < 0 || day >= EVOBEING_WINDOW_DAYS ||
        hour < 0 || hour >= EVOBEING_HOURS || !zhenghuimin_value_ok(wear, value)) {
        return -1;
    }
    in->value[day][hour] = (wear == 1) ? value : 0.0;
    in->wear[day][hour] = (unsigned char)wear;
    if (day + 1 > in->days) {
        in->days = day + 1;
    }
    return 0;
}

void evobeing_day_init(evobeing_day *today)
{
    if (today == NULL) {
        return;
    }
    memset(today, 0, sizeof(*today));
}

int evobeing_day_set(evobeing_day *today, int hour, int wear, double value)
{
    if (today == NULL || hour < 0 || hour >= EVOBEING_HOURS ||
        !zhenghuimin_value_ok(wear, value)) {
        return -1;
    }
    today->value[hour] = (wear == 1) ? value : 0.0;
    today->wear[hour] = (unsigned char)wear;
    return 0;
}

static int zhenghuimin_config_valid(const evobeing_config *cfg)
{
    if (cfg == NULL) {
        return 0;
    }
    if (cfg->min_n_any < 1 || cfg->min_n_comparable < cfg->min_n_any) {
        return 0;
    }
    if (!isfinite(cfg->inner_q_low) || !isfinite(cfg->inner_q_high) ||
        !isfinite(cfg->outer_q_low) || !isfinite(cfg->outer_q_high) ||
        !isfinite(cfg->floor_ratio) || !isfinite(cfg->floor_min) ||
        !isfinite(cfg->rest_p_active) || !isfinite(cfg->rest_level_ratio) ||
        !isfinite(cfg->rest_signal_min)) {
        return 0;
    }
    /* 分位数要求 0 <= outer_low <= inner_low <= inner_high <= outer_high <= 100 */
    if (cfg->outer_q_low < 0.0 || cfg->outer_q_low > cfg->inner_q_low ||
        cfg->inner_q_low > cfg->inner_q_high || cfg->inner_q_high > cfg->outer_q_high ||
        cfg->outer_q_high > 100.0) {
        return 0;
    }
    if (cfg->floor_ratio < 0.0 || cfg->floor_min < 0.0 ||
        cfg->rest_level_ratio < 0.0 || cfg->rest_signal_min < 0.0) {
        return 0;
    }
    if (cfg->rest_p_active < 0.0 || cfg->rest_p_active > 1.0) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------ 拟合 */

int evobeing_fit(const evobeing_input *in, const evobeing_config *cfg,
                 evobeing_baseline *out)
{
    double vals[EVOBEING_HOURS][EVOBEING_WINDOW_DAYS];
    double meds[EVOBEING_HOURS];
    int nmed = 0;
    int h;
    int d;

    if (in == NULL || out == NULL || !zhenghuimin_config_valid(cfg)) {
        return -1;
    }
    if (in->days < 1 || in->days > EVOBEING_WINDOW_DAYS) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->days_used = in->days;

    /* 第一遍：取出每个小时的有效值，算中位数、MAD 和零值占比 */
    for (h = 0; h < EVOBEING_HOURS; h++) {
        evobeing_hour *eh = &out->hours[h];
        double pos[EVOBEING_WINDOW_DAYS];
        int np = 0;
        int n = 0;
        int i;

        eh->hour = h;
        for (d = 0; d < in->days; d++) {
            double x;
            if (!in->wear[d][h]) {
                continue;
            }
            x = in->value[d][h];
            if (!isfinite(x) || x < 0.0) {
                continue; /* 数组被直接写入了非法值时，按缺失处理 */
            }
            vals[h][n++] = x;
        }
        eh->n_valid = n;
        eh->n_missing = in->days - n;
        eh->status = (n < cfg->min_n_any)        ? EVOBEING_STATUS_INSUFFICIENT
                   : (n < cfg->min_n_comparable) ? EVOBEING_STATUS_BUILDING
                                                 : EVOBEING_STATUS_COMPARABLE;
        if (n < cfg->min_n_any) {
            eh->has_stats = 0;
            eh->rest_expected = 0; /* 没有基线就不判静息 */
            continue;
        }

        zhenghuimin_median(vals[h], n, &eh->median);
        zhenghuimin_mad(vals[h], n, eh->median, &eh->mad_raw);
        for (i = 0; i < n; i++) {
            if (vals[h][i] == 0.0) {
                eh->n_zero++;
            } else {
                pos[np++] = vals[h][i];
            }
        }
        eh->p_active = (double)(n - eh->n_zero) / (double)n;
        eh->active_level = 0.0;
        if (np > 0) {
            zhenghuimin_median(pos, np, &eh->active_level);
        }
        eh->has_stats = 1;
        if (eh->median > 0.0) {
            meds[nmed++] = eh->median;
        }
    }

    /* floor 按"典型活动小时"的量级取，避免夜间小时的 10% × 0 = 0 */
    out->has_scale = (nmed > 0) ? 1 : 0;
    if (out->has_scale) {
        zhenghuimin_median(meds, nmed, &out->scale);
    }
    out->floor = cfg->floor_min;
    if (out->has_scale) {
        double f = cfg->floor_ratio * out->scale;
        if (!isfinite(f)) {
            return -1;
        }
        if (f > out->floor) {
            out->floor = f;
        }
    }
    if (!isfinite(out->floor)) {
        return -1;
    }

    /* 第二遍：分位区间和静息标记 */
    for (h = 0; h < EVOBEING_HOURS; h++) {
        evobeing_hour *eh = &out->hours[h];
        double lo_floor;
        double hi_floor;
        double inner_low;
        double outer_low;

        eh->floor = out->floor;
        if (!eh->has_stats) {
            continue;
        }
        eh->mad_eff = (eh->mad_raw > out->floor) ? eh->mad_raw : out->floor;

        zhenghuimin_quantile(vals[h], eh->n_valid, cfg->outer_q_low, &eh->p10);
        zhenghuimin_quantile(vals[h], eh->n_valid, cfg->inner_q_low, &eh->p25);
        zhenghuimin_quantile(vals[h], eh->n_valid, cfg->inner_q_high, &eh->p75);
        zhenghuimin_quantile(vals[h], eh->n_valid, cfg->outer_q_high, &eh->p90);

        lo_floor = eh->median - out->floor;
        hi_floor = eh->median + out->floor;
        if (!isfinite(hi_floor)) {
            hi_floor = DBL_MAX; /* 活动量极大时上沿截到 DBL_MAX，保持有限 */
        }
        inner_low = (eh->p25 < lo_floor) ? eh->p25 : lo_floor;
        outer_low = (eh->p10 < lo_floor) ? eh->p10 : lo_floor;
        eh->inner_low = (inner_low < 0.0) ? 0.0 : inner_low;
        eh->outer_low = (outer_low < 0.0) ? 0.0 : outer_low;
        eh->inner_high = (eh->p75 > hi_floor) ? eh->p75 : hi_floor;
        eh->outer_high = (eh->p90 > hi_floor) ? eh->p90 : hi_floor;
        eh->band_available = 1;

        if (eh->p_active < cfg->rest_p_active) {
            eh->rest_expected = 1;
        } else if (out->has_scale && eh->median < cfg->rest_level_ratio * out->scale) {
            eh->rest_expected = 1;
        } else {
            eh->rest_expected = 0;
        }
    }

    return 0;
}

/* ------------------------------------------------------------ 判定 */

evobeing_deviation evobeing_compare(const evobeing_baseline *b,
                                    const evobeing_config *cfg,
                                    int hour, int has_value, double value)
{
    evobeing_deviation dev;
    const evobeing_hour *eh;
    double sigma;
    int above;
    int below;

    memset(&dev, 0, sizeof(dev));
    dev.hour = hour;
    dev.band_position = "unknown";
    dev.state = EVOBEING_STATE_NO_DATA;
    dev.baseline_status = EVOBEING_STATUS_INSUFFICIENT;

    if (b == NULL || cfg == NULL || hour < 0 || hour >= EVOBEING_HOURS) {
        dev.reason = "hour 越界或传入了空指针。";
        return dev;
    }
    eh = &b->hours[hour];
    dev.baseline_status = eh->status;

    if (!has_value || !isfinite(value)) {
        dev.reason = "这一小时没有佩戴数据，不做比较（不当作 0）。";
        return dev;
    }
    dev.has_value = 1;
    dev.value = value;

    if (eh->status == EVOBEING_STATUS_INSUFFICIENT || !eh->has_stats) {
        dev.state = EVOBEING_STATE_NO_BASELINE;
        dev.reason = "该小时有效样本不足，暂不建立基线。";
        return dev;
    }

    sigma = ZHENGHUIMIN_MAD_SCALE * eh->mad_eff;
    if (sigma > 0.0) {
        dev.has_z = 1;
        dev.z = (value - eh->median) / sigma;
    }
    dev.has_delta = 1;
    dev.delta = value - eh->median;
    dev.has_expected = 1;
    dev.expected = eh->median;

    above = (value > eh->outer_high);
    below = (value < eh->outer_low);
    dev.band_position = above ? "above" : (below ? "below" : "inside");

    if (eh->rest_expected) {
        /* 静息时段只看有没有明显活动，活动少是正常的 */
        double threshold = (eh->outer_high > cfg->rest_signal_min) ? eh->outer_high
                                                                   : cfg->rest_signal_min;
        if (value > threshold) {
            dev.state = EVOBEING_STATE_UNUSUAL_MOVEMENT;
            dev.reason = "这个时段你平时基本不动，今天出现了明显活动。";
        } else {
            dev.state = EVOBEING_STATE_NORMAL;
            dev.reason = "这个时段你平时基本不动，今天的活动量符合这一特征。";
        }
        return dev;
    }

    if (above) {
        dev.state = EVOBEING_STATE_HIGH;
        dev.reason = "高于你个人该时段 80% 的历史范围（P10–P90）。";
    } else if (below) {
        dev.state = EVOBEING_STATE_LOW;
        dev.reason = "低于你个人该时段 80% 的历史范围（P10–P90）。";
    } else {
        dev.state = EVOBEING_STATE_NORMAL;
        dev.reason = "在你个人该时段的常见范围内。";
    }
    return dev;
}

/* ------------------------------------------------------------ 时段合并 */

static void zhenghuimin_update_ratio(evobeing_block *blk)
{
    blk->has_ratio = (blk->state != EVOBEING_STATE_UNUSUAL_MOVEMENT &&
                      blk->expected > 0.0 &&
                      isfinite(blk->total) && isfinite(blk->expected)) ? 1 : 0;
    blk->ratio = blk->has_ratio ? blk->total / blk->expected : 0.0;
}

/* 生成时段描述。缓冲区不够时返回 -1。 */
static int zhenghuimin_block_text(const evobeing_block *blk, char *buf, size_t buflen)
{
    const char *verb;
    char span[40];
    char head[128];
    int end_hour;
    int n;

    if (blk == NULL || buf == NULL || buflen == 0) {
        return -1;
    }
    switch (blk->state) {
    case EVOBEING_STATE_HIGH:
        verb = "高于平时";
        break;
    case EVOBEING_STATE_LOW:
        verb = "低于平时";
        break;
    case EVOBEING_STATE_UNUSUAL_MOVEMENT:
        verb = "出现异常活动";
        break;
    default:
        verb = "正常";
        break;
    }

    end_hour = blk->end_hour + 1;
    if (end_hour > EVOBEING_HOURS) {
        end_hour = EVOBEING_HOURS;
    }
    (void)snprintf(span, sizeof(span), "%02d:00–%02d:00", blk->start_hour, end_hour);

    /* 异常活动不写"连续 N 小时"，也没有百分比（夜里基线接近 0，比值没意义） */
    if (blk->n_hours == 1 || blk->state == EVOBEING_STATE_UNUSUAL_MOVEMENT) {
        (void)snprintf(head, sizeof(head), "%s %s", span, verb);
    } else {
        (void)snprintf(head, sizeof(head), "%s 连续 %d 小时%s", span, blk->n_hours, verb);
    }
    if (blk->has_ratio) {
        n = snprintf(buf, buflen, "%s（合计 %.0f，为平时同期的 %.0f%%）",
                     head, blk->total, blk->ratio * 100.0);
    } else {
        n = snprintf(buf, buflen, "%s（合计 %.0f）", head, blk->total);
    }

    if (n < 0 || (size_t)n >= buflen) {
        return -1;
    }
    return 0;
}

int evobeing_summarize(const evobeing_deviation *devs, int n,
                       evobeing_block *out, int max_blocks)
{
    int count = 0;
    int i;

    if (devs == NULL || out == NULL || n < 0 || n > EVOBEING_HOURS || max_blocks < 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        evobeing_state s = devs[i].state;
        double expected;
        evobeing_block *blk;

        if (s != EVOBEING_STATE_HIGH && s != EVOBEING_STATE_LOW &&
            s != EVOBEING_STATE_UNUSUAL_MOVEMENT) {
            continue;
        }
        expected = devs[i].has_expected ? devs[i].expected : 0.0;

        /* 和上一个时段同向且小时相邻，就并进去 */
        if (count > 0) {
            evobeing_block *last = &out[count - 1];
            if (last->state == s && devs[i].hour == last->end_hour + 1) {
                last->end_hour = devs[i].hour;
                last->n_hours++;
                last->total += devs[i].value;
                last->expected += expected;
                zhenghuimin_update_ratio(last);
                continue;
            }
        }
        if (count >= max_blocks) {
            break;
        }
        blk = &out[count++];
        memset(blk, 0, sizeof(*blk));
        blk->start_hour = devs[i].hour;
        blk->end_hour = devs[i].hour;
        blk->n_hours = 1;
        blk->state = s;
        blk->total = devs[i].value;
        blk->expected = expected;
        zhenghuimin_update_ratio(blk);
    }

    for (i = 0; i < count; i++) {
        if (zhenghuimin_block_text(&out[i], out[i].text, sizeof(out[i].text)) != 0) {
            out[i].text[0] = '\0'; /* 数值大到写不下时留空 */
        }
    }
    return count;
}

/* ------------------------------------------------------------ 一站式入口 */

int evobeing_zhenghuimin_run(const evobeing_input *history,
                             const evobeing_day *today,
                             const evobeing_config *cfg,
                             evobeing_result *out)
{
    evobeing_config zhenghuimin_cfg;
    int h;
    int n;

    if (history == NULL || out == NULL) {
        return -1;
    }
    if (cfg == NULL) {
        evobeing_config_default(&zhenghuimin_cfg);
        cfg = &zhenghuimin_cfg;
    }

    memset(out, 0, sizeof(*out));
    if (evobeing_fit(history, cfg, &out->baseline) != 0) {
        return -1;
    }

    out->has_today = (today != NULL) ? 1 : 0;
    for (h = 0; h < EVOBEING_HOURS; h++) {
        int has_value = 0;
        double value = 0.0;
        if (today != NULL && today->wear[h]) {
            value = today->value[h];
            has_value = (isfinite(value) && value >= 0.0) ? 1 : 0;
        }
        /* 没传 today 时也填满 24 条 NO_DATA，调用方读到的 reason 不会是空指针 */
        out->today[h] = evobeing_compare(&out->baseline, cfg, h, has_value, value);
    }

    n = evobeing_summarize(out->today, EVOBEING_HOURS, out->blocks, EVOBEING_HOURS);
    if (n < 0) {
        return -1;
    }
    out->n_blocks = n;
    return 0;
}

/* ------------------------------------------------------------ 名称 */

const char *evobeing_state_name(evobeing_state s)
{
    switch (s) {
    case EVOBEING_STATE_NO_DATA:
        return "no_data";
    case EVOBEING_STATE_NO_BASELINE:
        return "no_baseline";
    case EVOBEING_STATE_NORMAL:
        return "normal";
    case EVOBEING_STATE_HIGH:
        return "high";
    case EVOBEING_STATE_LOW:
        return "low";
    case EVOBEING_STATE_UNUSUAL_MOVEMENT:
        return "unusual_movement";
    default:
        return "unknown";
    }
}

const char *evobeing_status_name(evobeing_status s)
{
    switch (s) {
    case EVOBEING_STATUS_INSUFFICIENT:
        return "insufficient";
    case EVOBEING_STATUS_BUILDING:
        return "building";
    case EVOBEING_STATUS_COMPARABLE:
        return "comparable";
    default:
        return "unknown";
    }
}
