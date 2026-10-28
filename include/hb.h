/*
 * hb.h — 按小时的活动量个人基线（C 实现）
 *
 * 输入：14 天 × 24 小时的活动量 + 小时级佩戴标记（0/1）。
 * 输出：24 个基线点、个人区间、以及某一天每个小时的偏离判定。
 *
 * 约定：
 *   - 活动量无单位，所有数值使用同一量纲。
 *   - wear = 1 表示该小时佩戴充分、value 有效；wear = 0 表示未佩戴/无效，
 *     value 被忽略，且**不补 0**。
 *   - value = 0 且 wear = 1 是有效观测（戴了但没动）。
 */

#ifndef HB_H
#define HB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HB_HOURS 24
#define HB_WINDOW_DAYS 14

/* 拟合参数。hb_config_default() 填入推荐默认值。 */
typedef struct {
    int min_n_any;             /* 任何统计量的硬下限，默认 3 */
    int min_n_comparable;      /* 可比较门槛，默认 7 */
    double inner_q_low;        /* 常见范围下分位，默认 25 */
    double inner_q_high;       /* 常见范围上分位，默认 75 */
    double outer_q_low;        /* 不寻常边界下分位，默认 10 */
    double outer_q_high;       /* 不寻常边界上分位，默认 90 */
    double floor_ratio;        /* 区间最小半宽 = ratio × 典型小时量级，默认 0.10 */
    double floor_min;          /* 绝对下限，默认 1.0 */
    double rest_p_active;      /* 有活动天数占比低于此值判静息，默认 0.20 */
    double rest_level_ratio;   /* 中位量级低于此比例判静息，默认 0.15 */
    double rest_signal_min;    /* 静息时段“异常活动”的绝对门槛，默认 0.0 */
} hb_config;

/* 输入矩阵。days 取值 1..14；未写入的小时按 wear=0 处理。 */
typedef struct {
    int days;
    double value[HB_WINDOW_DAYS][HB_HOURS];
    unsigned char wear[HB_WINDOW_DAYS][HB_HOURS];
} hb_input;

typedef enum {
    HB_STATUS_INSUFFICIENT = 0, /* n < min_n_any */
    HB_STATUS_BUILDING = 1,     /* min_n_any <= n < min_n_comparable */
    HB_STATUS_COMPARABLE = 2    /* n >= min_n_comparable */
} hb_status;

/* 单个小时的基线。has_stats = 0 时除计数外的字段均无意义。 */
typedef struct {
    int hour;
    int n_valid;
    int n_missing;
    int n_zero;
    hb_status status;
    int has_stats;

    double median;
    double mad_raw;
    double mad_eff;
    double floor;

    double p10, p25, p75, p90;
    double inner_low, inner_high;
    double outer_low, outer_high;

    double p_active;
    double active_level;
    int rest_expected;   /* 仅当 has_stats=1 时有意义 */
    int band_available;
} hb_hour;

/* 24 点基线。 */
typedef struct {
    hb_hour hours[HB_HOURS];
    double floor;
    double scale;
    int has_scale;
    int days_used;  /* 实际参与拟合的天数（1..HB_WINDOW_DAYS） */
} hb_baseline;

typedef enum {
    HB_DEV_NO_DATA = 0,          /* 该小时未佩戴，不做比较 */
    HB_DEV_NO_BASELINE = 1,      /* 该小时样本不足 */
    HB_DEV_NORMAL = 2,
    HB_DEV_HIGH = 3,
    HB_DEV_LOW = 4,
    HB_DEV_UNUSUAL_MOVEMENT = 5  /* 静息时段出现明显活动 */
} hb_state;

/* 单小时判定结果。reason 指向静态字符串，无需释放。 */
typedef struct {
    int hour;
    int has_value;
    double value;
    hb_state state;
    int has_z;
    double z;
    int has_delta;
    double delta;
    int has_expected;
    double expected;
    hb_status baseline_status;  /* 该小时基线的成熟度，供调用方决定是否对外下结论 */
    const char *band_position;  /* "below" / "inside" / "above" / "unknown" */
    const char *reason;
} hb_deviation;

/* 连续同向偏离合并后的时段。 */
typedef struct {
    int start_hour;
    int end_hour; /* 含 */
    int n_hours;
    hb_state state;
    double total;
    double expected;
    int has_ratio;
    double ratio;
} hb_block;

/* ---- 配置与输入 ---- */

void hb_config_default(hb_config *cfg);
void hb_input_init(hb_input *in);

/* 写入一个观测。成功返回 0；参数越界返回 -1。
 * wear=0 时 value 被忽略（可传 0）。重复写同一 (day,hour) 会覆盖。 */
int hb_input_set(hb_input *in, int day, int hour, int wear, double value);

/* ---- 计算 ---- */

/* 拟合 24 点基线。成功返回 0；参数非法返回 -1。 */
int hb_fit(const hb_input *in, const hb_config *cfg, hb_baseline *out);

/* 比较某天某小时。has_value=0 表示该小时未佩戴。 */
hb_deviation hb_compare(const hb_baseline *b, const hb_config *cfg,
                        int hour, int has_value, double value);

/* 把逐小时判定合并成连续时段，返回写入的块数；参数非法返回 -1。
 * devs 必须按 hour 0..23 升序排列。 */
int hb_summarize(const hb_deviation *devs, int n, hb_block *out, int max_blocks);

/* ---- 名称与文本 ---- */

const char *hb_state_name(hb_state s);
const char *hb_status_name(hb_status s);

/* 生成可读文本（UTF-8）。成功返回 0；缓冲区不足返回 -1。 */
int hb_block_text(const hb_block *blk, char *buf, size_t buflen);

/* ---- 统计原语（为测试与复用暴露；n 必须在 1..24） ---- */

int hb_median(const double *v, int n, double *out);
int hb_mad(const double *v, int n, double center, double *out);
int hb_quantile(const double *v, int n, double q, double *out);

#ifdef __cplusplus
}
#endif

#endif /* HB_H */
