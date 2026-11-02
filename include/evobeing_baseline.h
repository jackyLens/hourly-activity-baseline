/*
 * evobeing_baseline.h
 *
 * evobeing 按小时活动量个人基线
 * 作者：zhenghuimin
 *
 * 用过去最多 14 天、每天 24 小时的活动量，给 0~23 点每个小时算出个人基线
 * （中位数 + 分位区间），再判断某一天的每个小时是偏高、偏低还是正常。
 *
 * 数据约定：
 *   - 活动量没有单位，只要求所有数值是同一个量纲（步数、计数、能量都行）。
 *   - wear = 1：这个小时佩戴充分，value 有效。
 *   - wear = 0：没戴或数据不可信，value 会被忽略，不会当成 0 处理。
 *   - wear = 1 且 value = 0 是有效数据，表示戴着但没动。
 *
 * 不分配堆内存，没有全局变量，可重入。只依赖 C 标准库和 libm。
 */

#ifndef EVOBEING_BASELINE_H
#define EVOBEING_BASELINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVOBEING_VERSION     "1.0.0"
#define EVOBEING_AUTHOR      "zhenghuimin"

#define EVOBEING_HOURS       24  /* 一天的小时数，h = 0..23 */
#define EVOBEING_WINDOW_DAYS 14  /* 历史窗口最多 14 天，d = 0..13 */
#define EVOBEING_TEXT_MAX    160 /* 时段描述文本缓冲区大小（含结尾 0） */

/* ------------------------------------------------------------------ 输入 */

/*
 * 历史数据：days 天 × 24 小时。
 *   value[d][h]  第 d 天第 h 小时的活动量，有限且 >= 0
 *   wear[d][h]   1 = 佩戴充分；0 = 缺失
 *   days         实际天数 1..14。d = 0 是最早的一天，d = days-1 是最近一天。
 *
 * 推荐用 evobeing_input_init() + evobeing_input_set() 填写，后者会校验参数
 * 并自动维护 days。直接写数组也可以，但需要自己设置 days。
 */
typedef struct {
    int days;
    double value[EVOBEING_WINDOW_DAYS][EVOBEING_HOURS];
    unsigned char wear[EVOBEING_WINDOW_DAYS][EVOBEING_HOURS];
} evobeing_input;

/*
 * 待判定的一天：24 小时。字段含义同 evobeing_input。
 * 用 evobeing_day_init() 初始化后，所有小时都是 wear = 0（未佩戴）。
 */
typedef struct {
    double value[EVOBEING_HOURS];
    unsigned char wear[EVOBEING_HOURS];
} evobeing_day;

/* 参数。一般直接用 evobeing_config_default() 的默认值。 */
typedef struct {
    int min_n_any;           /* 有效样本少于此数不建基线，默认 3 */
    int min_n_comparable;    /* 有效样本达到此数才算可比较，默认 7 */
    double inner_q_low;      /* 常见范围下分位，默认 25 */
    double inner_q_high;     /* 常见范围上分位，默认 75 */
    double outer_q_low;      /* 判定边界下分位，默认 10 */
    double outer_q_high;     /* 判定边界上分位，默认 90 */
    double floor_ratio;      /* 区间最小半宽 = floor_ratio × scale，默认 0.10 */
    double floor_min;        /* 区间最小半宽的绝对下限，默认 1.0 */
    double rest_p_active;    /* 有活动的天数占比低于此值视为静息时段，默认 0.20 */
    double rest_level_ratio; /* 中位数低于 rest_level_ratio × scale 视为静息时段，默认 0.15 */
    double rest_signal_min;  /* 静息时段报"异常活动"的最低活动量，默认 0 */
} evobeing_config;

/* ------------------------------------------------------------------ 输出 */

/* 某个小时基线的成熟度 */
typedef enum {
    EVOBEING_STATUS_INSUFFICIENT = 0, /* n < min_n_any，不建基线 */
    EVOBEING_STATUS_BUILDING     = 1, /* 有基线，但样本还少，不建议对用户下结论 */
    EVOBEING_STATUS_COMPARABLE   = 2  /* n >= min_n_comparable，可以正常比较 */
} evobeing_status;

/* 单个小时的基线。has_stats = 0 时只有计数字段有意义。 */
typedef struct {
    int hour;
    int n_valid;    /* 有效样本数 */
    int n_missing;  /* 缺失数 = days - n_valid */
    int n_zero;     /* 有效样本里 value == 0 的个数 */
    evobeing_status status;
    int has_stats;

    double median;  /* 基线点 */
    double mad_raw; /* 原始 MAD */
    double mad_eff; /* max(mad_raw, floor)，用于算 z */
    double floor;

    double p10, p25, p75, p90;
    double inner_low, inner_high; /* 常见范围（P25~P75，过窄时用 floor 撑开） */
    double outer_low, outer_high; /* 判定边界（P10~P90，过窄时用 floor 撑开） */

    double p_active;     /* 有活动（value > 0）的天数占比 */
    double active_level; /* value > 0 部分的中位数 */
    int rest_expected;   /* 1 = 静息时段（如夜里），只报异常活动，不报偏低 */
    int band_available;  /* 1 = 有可用区间 */
} evobeing_hour;

/* 24 小时基线 */
typedef struct {
    evobeing_hour hours[EVOBEING_HOURS];
    double floor;  /* 区间最小半宽 */
    double scale;  /* 24 个小时中位数里大于 0 的那些再取中位数 */
    int has_scale;
    int days_used; /* 本次参与计算的天数 */
} evobeing_baseline;

/* 单小时判定结果 */
typedef enum {
    EVOBEING_STATE_NO_DATA          = 0, /* 当天这个小时没戴 */
    EVOBEING_STATE_NO_BASELINE      = 1, /* 这个小时历史样本不够 */
    EVOBEING_STATE_NORMAL           = 2,
    EVOBEING_STATE_HIGH             = 3,
    EVOBEING_STATE_LOW              = 4,
    EVOBEING_STATE_UNUSUAL_MOVEMENT = 5  /* 平时不动的时段出现了明显活动 */
} evobeing_state;

/* 单小时判定。band_position 和 reason 指向静态字符串，不需要释放。 */
typedef struct {
    int hour;
    int has_value;
    double value;
    evobeing_state state;
    evobeing_status baseline_status; /* 该小时基线成熟度，BUILDING 时建议不对外展示结论 */
    int has_z;
    double z;            /* (value - median) / (1.4826 × mad_eff)，只作参考 */
    int has_delta;
    double delta;        /* value - median */
    int has_expected;
    double expected;     /* median */
    const char *band_position; /* "below" / "inside" / "above" / "unknown" */
    const char *reason;        /* 中文说明，UTF-8 */
} evobeing_deviation;

/* 连续同向偏离合并成的时段 */
typedef struct {
    int start_hour;
    int end_hour;   /* 包含 */
    int n_hours;
    evobeing_state state; /* HIGH / LOW / UNUSUAL_MOVEMENT */
    double total;    /* 时段内实际值之和 */
    double expected; /* 时段内基线中位数之和 */
    int has_ratio;
    double ratio;    /* total / expected */
    char text[EVOBEING_TEXT_MAX]; /* 可直接展示的一句话，UTF-8 */
} evobeing_block;

/* evobeing_zhenghuimin_run() 的完整结果 */
typedef struct {
    evobeing_baseline baseline;
    int has_today;                              /* 0 = 没有传当天数据 */
    evobeing_deviation today[EVOBEING_HOURS];   /* has_today = 1 时有效，下标即小时 */
    int n_blocks;
    evobeing_block blocks[EVOBEING_HOURS];
} evobeing_result;

/* ------------------------------------------------------------------ 函数 */

/* 填入默认参数。 */
void evobeing_config_default(evobeing_config *cfg);

/* 历史数据清零（所有小时 wear = 0，days = 0）。 */
void evobeing_input_init(evobeing_input *in);

/*
 * 写入一条历史数据。day 0..13，hour 0..23，wear 0/1；
 * wear = 1 时 value 必须有限且 >= 0，wear = 0 时 value 随便传。
 * 成功返回 0，参数不合法返回 -1。同一个 (day, hour) 重复写会覆盖。
 */
int evobeing_input_set(evobeing_input *in, int day, int hour, int wear, double value);

/* 当天数据清零（所有小时 wear = 0）。 */
void evobeing_day_init(evobeing_day *today);

/* 写入当天某小时的数据，规则同 evobeing_input_set()。成功返回 0，否则 -1。 */
int evobeing_day_set(evobeing_day *today, int hour, int wear, double value);

/*
 * 一次完成全部计算：拟合基线 → 逐小时判定 → 合并时段。
 *   history  必填
 *   today    可以传 NULL，此时只算基线，out->has_today = 0
 *   cfg      可以传 NULL，此时用默认参数
 *   out      结果
 * 成功返回 0；输入或参数不合法返回 -1。
 */
int evobeing_zhenghuimin_run(const evobeing_input *history,
                             const evobeing_day *today,
                             const evobeing_config *cfg,
                             evobeing_result *out);

/* ---- 分步接口：需要单独拿基线、或者每来一个小时判一次时用 ---- */

/* 拟合 24 小时基线。成功返回 0；参数不合法返回 -1。 */
int evobeing_fit(const evobeing_input *in, const evobeing_config *cfg,
                 evobeing_baseline *out);

/* 判定某一小时。has_value = 0 表示这个小时没戴。 */
evobeing_deviation evobeing_compare(const evobeing_baseline *b,
                                    const evobeing_config *cfg,
                                    int hour, int has_value, double value);

/*
 * 把逐小时判定合并成连续时段，同时生成 text。
 * devs 必须按小时升序。返回写入的时段数；参数不合法返回 -1。
 */
int evobeing_summarize(const evobeing_deviation *devs, int n,
                       evobeing_block *out, int max_blocks);

/* 枚举转字符串，如 "high"、"comparable"。 */
const char *evobeing_state_name(evobeing_state s);
const char *evobeing_status_name(evobeing_status s);

#ifdef __cplusplus
}
#endif

#endif /* EVOBEING_BASELINE_H */
