/*
 * hb.c — 按小时的活动量个人基线：拟合、判定、时段合并。
 *
 * 口径（与设计文档一致）：
 *   1. wear=0 视为缺失，不补 0；wear=1 且 value=0 是有效观测。
 *   2. 基线点用中位数；区间用经验分位数（P25–P75 / P10–P90）。
 *   3. 分位区间窄于 floor 时用 median ± floor 撑开。
 *   4. floor = max(floor_min, floor_ratio × “典型活动小时”的中位量级)。
 *   5. 静息时段只判“异常活动”，不判“偏低”。
 *   6. z 用 MAD（带 floor 下限）计算，仅作连续强度，不参与告警。
 */

#include "hb.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAD_SCALE 1.4826
#define HB_MAX_VALUES HB_HOURS /* 统计原语允许的最大样本量 */

static int cmp_double(const void *pa, const void *pb)
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

static int sort_copy(const double *v, int n, double *out)
{
    if (v == NULL || out == NULL || n <= 0 || n > HB_MAX_VALUES) {
        return 0;
    }
    memcpy(out, v, sizeof(double) * (size_t)n);
    qsort(out, (size_t)n, sizeof(double), cmp_double);
    return 1;
}

int hb_median(const double *v, int n, double *out)
{
    double tmp[HB_MAX_VALUES];
    if (out == NULL || !sort_copy(v, n, tmp)) {
        return -1;
    }
    if (n % 2 != 0) {
        *out = tmp[n / 2];
    } else {
        *out = (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0;
    }
    return 0;
}

int hb_mad(const double *v, int n, double center, double *out)
{
    double dev[HB_MAX_VALUES];
    int i;
    if (v == NULL || out == NULL || n <= 0 || n > HB_MAX_VALUES) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        dev[i] = fabs(v[i] - center);
    }
    return hb_median(dev, n, out);
}

int hb_quantile(const double *v, int n, double q, double *out)
{
    double tmp[HB_MAX_VALUES];
    double rank;
    double frac;
    int lo;
    int hi;

    if (out == NULL || !sort_copy(v, n, tmp)) {
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

void hb_config_default(hb_config *cfg)
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

void hb_input_init(hb_input *in)
{
    if (in == NULL) {
        return;
    }
    memset(in, 0, sizeof(*in));
}

int hb_input_set(hb_input *in, int day, int hour, int wear, double value)
{
    if (in == NULL || day < 0 || day >= HB_WINDOW_DAYS || hour < 0 || hour >= HB_HOURS) {
        return -1;
    }
    if (wear != 0 && wear != 1) {
        return -1;
    }
    if (wear == 1 && (!isfinite(value) || value < 0.0)) {
        return -1;
    }
    in->value[day][hour] = (wear == 1) ? value : 0.0;
    in->wear[day][hour] = (unsigned char)wear;
    if (day + 1 > in->days) {
        in->days = day + 1;
    }
    return 0;
}

static int config_valid(const hb_config *cfg)
{
    if (cfg == NULL) {
        return 0;
    }
    if (cfg->min_n_any < 1 || cfg->min_n_comparable < cfg->min_n_any) {
        return 0;
    }
    if (cfg->inner_q_low < 0.0 || cfg->inner_q_high > 100.0 ||
        cfg->outer_q_low < 0.0 || cfg->outer_q_high > 100.0) {
        return 0;
    }
    if (cfg->inner_q_low > cfg->inner_q_high || cfg->outer_q_low > cfg->outer_q_high) {
        return 0;
    }
    if (cfg->outer_q_low > cfg->inner_q_low || cfg->inner_q_high > cfg->outer_q_high) {
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

int hb_fit(const hb_input *in, const hb_config *cfg, hb_baseline *out)
{
    double vals[HB_HOURS][HB_WINDOW_DAYS];
    int nv[HB_HOURS];
    double meds[HB_HOURS];
    int nmed = 0;
    int h;
    int d;

    if (in == NULL || out == NULL || !config_valid(cfg)) {
        return -1;
    }
    if (in->days < 1 || in->days > HB_WINDOW_DAYS) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->window_days = in->days;

    /* 第一遍：收集每小时有效值，并算中位数。 */
    for (h = 0; h < HB_HOURS; h++) {
        hb_hour *hb = &out->hours[h];
        int n = 0;
        int i;

        hb->hour = h;
        for (d = 0; d < in->days; d++) {
            double x;
            if (!in->wear[d][h]) {
                continue;
            }
            x = in->value[d][h];
            if (!isfinite(x) || x < 0.0) {
                continue; /* 防御：wear=1 但值非法时按缺失处理 */
            }
            vals[h][n++] = x;
        }
        nv[h] = n;
        hb->n_valid = n;
        hb->n_missing = in->days - n;
        hb->status = (n < cfg->min_n_any) ? HB_STATUS_INSUFFICIENT
                    : (n < cfg->min_n_comparable) ? HB_STATUS_BUILDING
                    : HB_STATUS_COMPARABLE;

        if (n < cfg->min_n_any) {
            hb->has_stats = 0;
            hb->rest_expected = (n == 0) ? 1 : 0;
            continue;
        }

        hb_median(vals[h], n, &hb->median);
        hb_mad(vals[h], n, hb->median, &hb->mad_raw);
        for (i = 0; i < n; i++) {
            if (vals[h][i] == 0.0) {
                hb->n_zero++;
            }
        }
        hb->p_active = (double)(n - hb->n_zero) / (double)n;
        {
            double pos[HB_WINDOW_DAYS];
            int np = 0;
            for (i = 0; i < n; i++) {
                if (vals[h][i] > 0.0) {
                    pos[np++] = vals[h][i];
                }
            }
            hb->active_level = 0.0;
            if (np > 0) {
                hb_median(pos, np, &hb->active_level);
            }
        }
        hb->has_stats = 1;
        if (hb->median > 0.0) {
            meds[nmed++] = hb->median;
        }
    }

    /* floor 来自“典型活动小时”的量级，而不是每小时的 10%×0。 */
    out->has_scale = (nmed > 0) ? 1 : 0;
    if (out->has_scale) {
        hb_median(meds, nmed, &out->scale);
    }
    out->floor = cfg->floor_min;
    if (out->has_scale) {
        double f = cfg->floor_ratio * out->scale;
        if (f > out->floor) {
            out->floor = f;
        }
    }

    /* 第二遍：分位区间、静息标记。 */
    for (h = 0; h < HB_HOURS; h++) {
        hb_hour *hb = &out->hours[h];
        double p10;
        double p25;
        double p75;
        double p90;
        double inner_low;
        double outer_low;

        hb->floor = out->floor;
        if (!hb->has_stats) {
            continue;
        }
        hb->mad_eff = (hb->mad_raw > out->floor) ? hb->mad_raw : out->floor;

        hb_quantile(vals[h], hb->n_valid, cfg->outer_q_low, &p10);
        hb_quantile(vals[h], hb->n_valid, cfg->inner_q_low, &p25);
        hb_quantile(vals[h], hb->n_valid, cfg->inner_q_high, &p75);
        hb_quantile(vals[h], hb->n_valid, cfg->outer_q_high, &p90);
        hb->p10 = p10;
        hb->p25 = p25;
        hb->p75 = p75;
        hb->p90 = p90;

        inner_low = (p25 < hb->median - out->floor) ? p25 : hb->median - out->floor;
        outer_low = (p10 < hb->median - out->floor) ? p10 : hb->median - out->floor;
        hb->inner_low = (inner_low < 0.0) ? 0.0 : inner_low;
        hb->outer_low = (outer_low < 0.0) ? 0.0 : outer_low;
        hb->inner_high = (p75 > hb->median + out->floor) ? p75 : hb->median + out->floor;
        hb->outer_high = (p90 > hb->median + out->floor) ? p90 : hb->median + out->floor;
        hb->band_available = 1;

        if (hb->p_active < cfg->rest_p_active) {
            hb->rest_expected = 1;
        } else if (out->has_scale &&
                   hb->median < cfg->rest_level_ratio * out->scale) {
            hb->rest_expected = 1;
        } else {
            hb->rest_expected = 0;
        }
    }

    return 0;
}

hb_deviation hb_compare(const hb_baseline *b, const hb_config *cfg,
                        int hour, int has_value, double value)
{
    hb_deviation dev;
    const hb_hour *hb;
    double sigma;
    double threshold;
    int above;
    int below;

    memset(&dev, 0, sizeof(dev));
    dev.hour = hour;
    dev.band_position = "unknown";
    dev.state = HB_DEV_NO_DATA;

    if (b == NULL || cfg == NULL || hour < 0 || hour >= HB_HOURS) {
        dev.reason = "非法的 hour 或空指针。";
        return dev;
    }
    hb = &b->hours[hour];

    if (!has_value || !isfinite(value)) {
        dev.reason = "这一小时没有佩戴数据，不做比较（不当作 0）。";
        return dev;
    }
    dev.has_value = 1;
    dev.value = value;

    if (hb->status == HB_STATUS_INSUFFICIENT || !hb->has_stats) {
        dev.state = HB_DEV_NO_BASELINE;
        dev.reason = "该小时有效样本不足，暂不建立基线。";
        return dev;
    }

    sigma = MAD_SCALE * hb->mad_eff;
    if (sigma > 0.0) {
        dev.has_z = 1;
        dev.z = (value - hb->median) / sigma;
    }
    dev.has_delta = 1;
    dev.delta = value - hb->median;
    dev.has_expected = 1;
    dev.expected = hb->median;
    dev.state = HB_DEV_NORMAL;

    above = (value > hb->outer_high);
    below = (value < hb->outer_low);
    dev.band_position = above ? "above" : (below ? "below" : "inside");

    if (hb->rest_expected) {
        /* 静息时段只报“异常活动”，永不报“偏低”。 */
        threshold = (hb->outer_high > cfg->rest_signal_min) ? hb->outer_high
                                                            : cfg->rest_signal_min;
        if (value > threshold) {
            dev.state = HB_DEV_UNUSUAL_MOVEMENT;
            dev.reason = "这个时段你平时基本不动，今天出现了明显活动。";
        } else {
            dev.state = HB_DEV_NORMAL;
            dev.reason = "这个时段你平时基本不动，今天的活动量符合这一特征。";
        }
        return dev;
    }

    if (above) {
        dev.state = HB_DEV_HIGH;
        dev.reason = "高于你个人该时段 80% 的历史范围（P10–P90）。";
    } else if (below) {
        dev.state = HB_DEV_LOW;
        dev.reason = "低于你个人该时段 80% 的历史范围（P10–P90）。";
    } else {
        dev.state = HB_DEV_NORMAL;
        dev.reason = "在你个人该时段的常见范围内。";
    }
    return dev;
}

int hb_summarize(const hb_deviation *devs, int n, hb_block *out, int max_blocks)
{
    int count = 0;
    int i;

    if (devs == NULL || out == NULL || n < 0 || n > HB_HOURS || max_blocks < 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        hb_state s = devs[i].state;
        double expected;
        hb_block *blk;

        if (s != HB_DEV_HIGH && s != HB_DEV_LOW && s != HB_DEV_UNUSUAL_MOVEMENT) {
            continue;
        }
        expected = devs[i].has_expected ? devs[i].expected : 0.0;

        if (count > 0) {
            hb_block *last = &out[count - 1];
            if (last->state == s && devs[i].hour == last->end_hour + 1) {
                last->end_hour = devs[i].hour;
                last->n_hours++;
                last->total += devs[i].value;
                last->expected += expected;
                last->has_ratio = (last->expected > 0.0) ? 1 : 0;
                last->ratio = last->has_ratio ? last->total / last->expected : 0.0;
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
        blk->has_ratio = (expected > 0.0) ? 1 : 0;
        blk->ratio = blk->has_ratio ? blk->total / blk->expected : 0.0;
    }
    return count;
}

const char *hb_state_name(hb_state s)
{
    switch (s) {
    case HB_DEV_NO_DATA:
        return "no_data";
    case HB_DEV_NO_BASELINE:
        return "no_baseline";
    case HB_DEV_NORMAL:
        return "normal";
    case HB_DEV_HIGH:
        return "high";
    case HB_DEV_LOW:
        return "low";
    case HB_DEV_UNUSUAL_MOVEMENT:
        return "unusual_movement";
    default:
        return "unknown";
    }
}

const char *hb_status_name(hb_status s)
{
    switch (s) {
    case HB_STATUS_INSUFFICIENT:
        return "insufficient";
    case HB_STATUS_BUILDING:
        return "building";
    case HB_STATUS_COMPARABLE:
        return "comparable";
    default:
        return "unknown";
    }
}

int hb_block_text(const hb_block *blk, char *buf, size_t buflen)
{
    const char *verb;
    char span[40];
    int end_hour;
    int n;

    if (blk == NULL || buf == NULL || buflen == 0) {
        return -1;
    }
    switch (blk->state) {
    case HB_DEV_HIGH:
        verb = "高于平时";
        break;
    case HB_DEV_LOW:
        verb = "低于平时";
        break;
    case HB_DEV_UNUSUAL_MOVEMENT:
        verb = "出现异常活动";
        break;
    default:
        verb = "正常";
        break;
    }

    end_hour = blk->end_hour + 1;
    if (end_hour > HB_HOURS) {
        end_hour = HB_HOURS;
    }
    (void)snprintf(span, sizeof(span), "%02d:00–%02d:00", blk->start_hour, end_hour);

    if (blk->state == HB_DEV_UNUSUAL_MOVEMENT) {
        n = snprintf(buf, buflen, "%s %s（合计 %.0f）", span, verb, blk->total);
    } else if (blk->n_hours == 1) {
        if (blk->has_ratio) {
            n = snprintf(buf, buflen, "%s %s（合计 %.0f，为平时同期的 %.0f%%）",
                         span, verb, blk->total, blk->ratio * 100.0);
        } else {
            n = snprintf(buf, buflen, "%s %s（合计 %.0f）", span, verb, blk->total);
        }
    } else if (blk->has_ratio) {
        n = snprintf(buf, buflen,
                     "%s 连续 %d 小时%s（合计 %.0f，为平时同期的 %.0f%%）",
                     span, blk->n_hours, verb, blk->total, blk->ratio * 100.0);
    } else {
        n = snprintf(buf, buflen, "%s 连续 %d 小时%s（合计 %.0f）",
                     span, blk->n_hours, verb, blk->total);
    }

    if (n < 0 || (size_t)n >= buflen) {
        return -1;
    }
    return 0;
}
