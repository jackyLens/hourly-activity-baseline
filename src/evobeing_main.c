/*
 * evobeing_main.c
 *
 * evobeing 命令行程序。作者：zhenghuimin
 *
 * main() 只做三件事：读 CSV → 调 evobeing_zhenghuimin_run() → 写 JSON。
 * 计算逻辑全部在 evobeing_baseline.c 里。
 *
 * 用法：
 *   evobeing_baseline [-o out.json] <history.csv> [today.csv]
 *
 * 退出码：0 成功，1 命令行用法错误，2 输入文件有问题，3 计算或写出失败。
 */

#include "evobeing_baseline.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZHENGHUIMIN_MAX_LINE 512             /* CSV 单行上限（含换行） */
#define ZHENGHUIMIN_ERR_LEN  (ZHENGHUIMIN_MAX_LINE + 256)

/* ================================================================ 命令行 */

static void zhenghuimin_usage(FILE *f, const char *prog)
{
    fprintf(f,
            "evobeing %s (%s)\n"
            "用法: %s [-o out.json] <history.csv> [today.csv]\n"
            "\n"
            "  history.csv   过去 1~14 天的逐小时数据，列: day,hour,wear,activity\n"
            "  today.csv     要判定的那一天，列: hour,wear,activity（可选）\n"
            "  -o, --out F   把 JSON 写入文件 F（默认输出到 stdout）\n"
            "  -h, --help    显示本帮助\n",
            EVOBEING_VERSION, EVOBEING_AUTHOR, prog);
}

/* 返回 0 继续执行；1 已打印帮助，正常退出；-1 用法错误 */
static int zhenghuimin_parse_args(int argc, char **argv, const char **history_path,
                                  const char **today_path, const char **out_path)
{
    int nfiles = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            zhenghuimin_usage(stdout, argv[0]);
            return 1;
        }
        if (strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-o 后面缺少文件名\n");
                return -1;
            }
            *out_path = argv[++i];
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "未知选项: %s\n", a);
            zhenghuimin_usage(stderr, argv[0]);
            return -1;
        }
        if (nfiles == 0) {
            *history_path = a;
        } else if (nfiles == 1) {
            *today_path = a;
        } else {
            fprintf(stderr, "参数过多: %s\n", a);
            zhenghuimin_usage(stderr, argv[0]);
            return -1;
        }
        nfiles++;
    }
    if (nfiles == 0) {
        zhenghuimin_usage(stderr, argv[0]);
        return -1;
    }
    return 0;
}

/* ================================================================ CSV 输入 */

/* 一行 CSV 解析出来的字段。当天文件没有 day 列，day 固定为 -1。 */
typedef struct {
    int day;
    int hour;
    int wear;
    int has_activity;
    double activity;
} zhenghuimin_row;

static const char *zhenghuimin_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static void zhenghuimin_strip_eol(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

/* 整数用 strtol 严格解析：溢出、小数、空字段都算失败 */
static int zhenghuimin_parse_int(const char **pp, int *out)
{
    const char *p = zhenghuimin_skip_ws(*pp);
    char *end = NULL;
    long v;

    if (*p == '\0' || *p == ',') {
        return 0;
    }
    errno = 0;
    v = strtol(p, &end, 10);
    if (end == p || errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        return 0;
    }
    *out = (int)v;
    *pp = end;
    return 1;
}

static int zhenghuimin_parse_double(const char **pp, double *out)
{
    const char *p = zhenghuimin_skip_ws(*pp);
    char *end = NULL;
    double v;

    if (*p == '\0' || *p == ',') {
        return 0;
    }
    v = strtod(p, &end);
    if (end == p) {
        return 0;
    }
    *out = v;
    *pp = end;
    return 1;
}

/*
 * 解析一行。with_day = 1 时格式是 day,hour,wear[,activity]，
 * 否则是 hour,wear[,activity]。activity 可以省略或留空；
 * 行尾有多余字符算格式错误。
 */
static int zhenghuimin_parse_row(const char *line, int with_day, zhenghuimin_row *row)
{
    const char *p = line;
    int ints[3];
    int n_ints = with_day ? 3 : 2;
    int i;

    for (i = 0; i < n_ints; i++) {
        if (i > 0) {
            p = zhenghuimin_skip_ws(p);
            if (*p != ',') {
                return 0;
            }
            p++;
        }
        if (!zhenghuimin_parse_int(&p, &ints[i])) {
            return 0;
        }
    }

    row->has_activity = 0;
    row->activity = 0.0;
    p = zhenghuimin_skip_ws(p);
    if (*p == ',') {
        p = zhenghuimin_skip_ws(p + 1);
        if (*p != '\0') {
            if (!zhenghuimin_parse_double(&p, &row->activity)) {
                return 0;
            }
            row->has_activity = 1;
        }
    }
    if (*zhenghuimin_skip_ws(p) != '\0') {
        return 0;
    }

    row->day = with_day ? ints[0] : -1;
    row->hour = ints[n_ints - 2];
    row->wear = ints[n_ints - 1];
    return 1;
}

/* 检查一行的取值范围。通过返回 1，否则把原因写进 err 并返回 0。 */
static int zhenghuimin_check_row(const zhenghuimin_row *r, int with_day, int lineno,
                                 char *err, size_t errlen)
{
    if (with_day && (r->day < 0 || r->day >= EVOBEING_WINDOW_DAYS)) {
        (void)snprintf(err, errlen, "第 %d 行 day=%d 越界（0..%d）",
                       lineno, r->day, EVOBEING_WINDOW_DAYS - 1);
        return 0;
    }
    if (r->hour < 0 || r->hour >= EVOBEING_HOURS) {
        (void)snprintf(err, errlen, "第 %d 行 hour=%d 越界（0..%d）",
                       lineno, r->hour, EVOBEING_HOURS - 1);
        return 0;
    }
    if (r->wear != 0 && r->wear != 1) {
        (void)snprintf(err, errlen, "第 %d 行 wear=%d，只能是 0 或 1", lineno, r->wear);
        return 0;
    }
    if (r->wear == 1 && !r->has_activity) {
        (void)snprintf(err, errlen, "第 %d 行 wear=1 但没有 activity", lineno);
        return 0;
    }
    if (r->wear == 1 && (!isfinite(r->activity) || r->activity < 0.0)) {
        (void)snprintf(err, errlen, "第 %d 行 activity 必须是非负的有限数", lineno);
        return 0;
    }
    return 1;
}

/*
 * 读一个 CSV 文件。
 *   history != NULL：按历史文件读（day,hour,wear,activity）
 *   today   != NULL：按当天文件读（hour,wear,activity）
 * 两者传一个即可。允许一行表头、# 注释行、空行、CRLF 行尾。
 * 成功返回 0；失败返回 -1，原因写进 err。
 */
static int zhenghuimin_read_csv(const char *path, evobeing_input *history,
                                evobeing_day *today, char *err, size_t errlen)
{
    const int with_day = (history != NULL);
    unsigned char seen[EVOBEING_WINDOW_DAYS][EVOBEING_HOURS];
    char line[ZHENGHUIMIN_MAX_LINE];
    int lineno = 0;
    int header_skipped = 0;
    int rows = 0;
    int rc = -1;
    FILE *f;

    f = fopen(path, "r");
    if (f == NULL) {
        (void)snprintf(err, errlen, "打不开文件: %s", path);
        return -1;
    }
    memset(seen, 0, sizeof(seen));
    if (with_day) {
        evobeing_input_init(history);
    } else {
        evobeing_day_init(today);
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        zhenghuimin_row row;
        const char *p;
        int d;
        int set_rc;

        lineno++;
        if (strchr(line, '\n') == NULL && !feof(f)) {
            (void)snprintf(err, errlen, "第 %d 行太长（超过 %d 个字符）",
                           lineno, ZHENGHUIMIN_MAX_LINE - 1);
            goto done;
        }
        zhenghuimin_strip_eol(line);
        p = zhenghuimin_skip_ws(line);
        if (*p == '\0' || *p == '#') {
            continue;
        }
        /* 第一行非数字开头的内容当作表头跳过，之后再出现就报错 */
        if (!isdigit((unsigned char)p[0]) && p[0] != '-' && p[0] != '+') {
            if (!header_skipped && rows == 0) {
                header_skipped = 1;
                continue;
            }
            (void)snprintf(err, errlen, "第 %d 行无法解析: %s", lineno, p);
            goto done;
        }
        if (!zhenghuimin_parse_row(p, with_day, &row)) {
            (void)snprintf(err, errlen, "第 %d 行格式不对，应为 %s", lineno,
                           with_day ? "day,hour,wear,activity" : "hour,wear,activity");
            goto done;
        }
        if (!zhenghuimin_check_row(&row, with_day, lineno, err, errlen)) {
            goto done;
        }
        d = with_day ? row.day : 0;
        if (seen[d][row.hour]) {
            if (with_day) {
                (void)snprintf(err, errlen, "第 %d 行重复: day=%d, hour=%d",
                               lineno, row.day, row.hour);
            } else {
                (void)snprintf(err, errlen, "第 %d 行重复: hour=%d", lineno, row.hour);
            }
            goto done;
        }
        seen[d][row.hour] = 1;

        set_rc = with_day ? evobeing_input_set(history, row.day, row.hour, row.wear, row.activity)
                          : evobeing_day_set(today, row.hour, row.wear, row.activity);
        if (set_rc != 0) {
            (void)snprintf(err, errlen, "第 %d 行写入失败", lineno);
            goto done;
        }
        rows++;
    }

    if (ferror(f)) {
        (void)snprintf(err, errlen, "读取 %s 时出错", path);
    } else if (rows == 0) {
        (void)snprintf(err, errlen, "%s 里没有数据行", path);
    } else {
        rc = 0;
    }

done:
    fclose(f);
    return rc;
}

/* ================================================================ JSON 输出 */

static void zhenghuimin_jnum(FILE *f, int has, double v, int prec)
{
    if (!has || !isfinite(v)) {
        fputs("null", f);
        return;
    }
    if (v == 0.0) {
        v = 0.0; /* 不输出 -0.0 */
    }
    fprintf(f, "%.*f", prec, v);
}

static void zhenghuimin_jstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s != NULL && *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc((int)c, f);
        } else if (c < 0x20) {
            fprintf(f, "\\u%04x", c);
        } else {
            fputc((int)c, f);
        }
    }
    fputc('"', f);
}

static void zhenghuimin_write_hour(FILE *f, const evobeing_hour *h)
{
    const int s = h->has_stats;

    fprintf(f, "    {\"hour\": %d, \"status\": ", h->hour);
    zhenghuimin_jstr(f, evobeing_status_name(h->status));
    fprintf(f, ", \"n_valid\": %d, \"n_missing\": %d, \"n_zero\": %d",
            h->n_valid, h->n_missing, h->n_zero);
    fputs(", \"median\": ", f);       zhenghuimin_jnum(f, s, h->median, 1);
    fputs(", \"mad\": ", f);          zhenghuimin_jnum(f, s, h->mad_raw, 1);
    fputs(", \"mad_eff\": ", f);      zhenghuimin_jnum(f, s, h->mad_eff, 1);
    fputs(", \"p10\": ", f);          zhenghuimin_jnum(f, s, h->p10, 1);
    fputs(", \"p25\": ", f);          zhenghuimin_jnum(f, s, h->p25, 1);
    fputs(", \"p75\": ", f);          zhenghuimin_jnum(f, s, h->p75, 1);
    fputs(", \"p90\": ", f);          zhenghuimin_jnum(f, s, h->p90, 1);
    fputs(", \"inner_low\": ", f);    zhenghuimin_jnum(f, s, h->inner_low, 1);
    fputs(", \"inner_high\": ", f);   zhenghuimin_jnum(f, s, h->inner_high, 1);
    fputs(", \"outer_low\": ", f);    zhenghuimin_jnum(f, s, h->outer_low, 1);
    fputs(", \"outer_high\": ", f);   zhenghuimin_jnum(f, s, h->outer_high, 1);
    fputs(", \"p_active\": ", f);     zhenghuimin_jnum(f, s, h->p_active, 2);
    fputs(", \"active_level\": ", f); zhenghuimin_jnum(f, s, h->active_level, 1);
    fprintf(f, ", \"rest_expected\": %s, \"band_available\": %s}",
            h->rest_expected ? "true" : "false", h->band_available ? "true" : "false");
}

static void zhenghuimin_write_deviation(FILE *f, const evobeing_deviation *d)
{
    fprintf(f, "    {\"hour\": %d, \"value\": ", d->hour);
    zhenghuimin_jnum(f, d->has_value, d->value, 1);
    fputs(", \"state\": ", f);
    zhenghuimin_jstr(f, evobeing_state_name(d->state));
    fputs(", \"baseline_status\": ", f);
    zhenghuimin_jstr(f, evobeing_status_name(d->baseline_status));
    fputs(", \"z\": ", f);        zhenghuimin_jnum(f, d->has_z, d->z, 2);
    fputs(", \"delta\": ", f);    zhenghuimin_jnum(f, d->has_delta, d->delta, 1);
    fputs(", \"expected\": ", f); zhenghuimin_jnum(f, d->has_expected, d->expected, 1);
    fputs(", \"band_position\": ", f);
    zhenghuimin_jstr(f, d->band_position);
    fputs(", \"reason\": ", f);
    zhenghuimin_jstr(f, d->reason);
    fputc('}', f);
}

static void zhenghuimin_write_block(FILE *f, const evobeing_block *b)
{
    fprintf(f, "    {\"start_hour\": %d, \"end_hour\": %d, \"n_hours\": %d, \"state\": ",
            b->start_hour, b->end_hour, b->n_hours);
    zhenghuimin_jstr(f, evobeing_state_name(b->state));
    fputs(", \"total\": ", f);    zhenghuimin_jnum(f, 1, b->total, 1);
    fputs(", \"expected\": ", f); zhenghuimin_jnum(f, 1, b->expected, 1);
    fputs(", \"ratio\": ", f);    zhenghuimin_jnum(f, b->has_ratio, b->ratio, 2);
    fputs(", \"text\": ", f);
    zhenghuimin_jstr(f, b->text);
    fputc('}', f);
}

static void zhenghuimin_write_json(FILE *f, const evobeing_result *res)
{
    const evobeing_baseline *b = &res->baseline;
    int i;

    fputs("{\n", f);
    fputs("  \"method\": \"evobeing_hourly_baseline\",\n", f);
    fprintf(f, "  \"version\": \"%s\",\n", EVOBEING_VERSION);
    fprintf(f, "  \"author\": \"%s\",\n", EVOBEING_AUTHOR);
    fprintf(f, "  \"window_days\": %d,\n", EVOBEING_WINDOW_DAYS);
    fprintf(f, "  \"days_used\": %d,\n", b->days_used);
    fprintf(f, "  \"hour_count\": %d,\n", EVOBEING_HOURS);
    fputs("  \"floor\": ", f);
    zhenghuimin_jnum(f, 1, b->floor, 1);
    fputs(",\n  \"scale\": ", f);
    zhenghuimin_jnum(f, b->has_scale, b->scale, 1);
    fputs(",\n  \"baseline\": [\n", f);
    for (i = 0; i < EVOBEING_HOURS; i++) {
        zhenghuimin_write_hour(f, &b->hours[i]);
        fputs((i + 1 < EVOBEING_HOURS) ? ",\n" : "\n", f);
    }
    fputs("  ],\n", f);

    if (!res->has_today) {
        fputs("  \"today\": null,\n  \"blocks\": []\n}\n", f);
        return;
    }
    fputs("  \"today\": [\n", f);
    for (i = 0; i < EVOBEING_HOURS; i++) {
        zhenghuimin_write_deviation(f, &res->today[i]);
        fputs((i + 1 < EVOBEING_HOURS) ? ",\n" : "\n", f);
    }
    fputs("  ],\n  \"blocks\": [\n", f);
    for (i = 0; i < res->n_blocks; i++) {
        zhenghuimin_write_block(f, &res->blocks[i]);
        fputs((i + 1 < res->n_blocks) ? ",\n" : "\n", f);
    }
    fputs("  ]\n}\n", f);
}

/* 写到文件或 stdout。成功返回 0，打不开文件返回 2，写出失败返回 3。 */
static int zhenghuimin_write_output(const char *path, const evobeing_result *res)
{
    FILE *f = stdout;
    int rc = 0;

    if (path != NULL) {
        f = fopen(path, "w");
        if (f == NULL) {
            fprintf(stderr, "无法写入文件: %s\n", path);
            return 2;
        }
    }
    zhenghuimin_write_json(f, res);
    if (fflush(f) != 0 || ferror(f)) {
        fprintf(stderr, "写出 JSON 时出错\n");
        rc = 3;
    }
    if (f != stdout) {
        fclose(f);
    }
    return rc;
}

/* ================================================================ main */

int main(int argc, char **argv)
{
    /*
     * ---------------------------------------------------------- 输入
     *
     * evobeing_history_path   历史数据 CSV，必填（第 1 个文件参数）
     *     每行 day,hour,wear,activity
     *       day       整数 0..13    相对天数，0 是最早一天，13 是最近一天
     *       hour      整数 0..23    本地时间的小时
     *       wear      整数 0 / 1    1 = 该小时佩戴充分；0 = 没戴或不可信
     *       activity  浮点 >= 0     活动量；wear = 0 时会被忽略，可以留空
     *
     * evobeing_today_path     待判定那一天的 CSV，可选（第 2 个文件参数）
     *     每行 hour,wear,activity，含义同上。没写到的小时当作没戴。
     *
     * zhenghuimin_out_path    JSON 输出文件，可选（-o 指定），不指定就写 stdout
     *
     * 读进内存后：
     *     evobeing_history   evobeing_input   历史矩阵 value[14][24] + wear[14][24]
     *     evobeing_today     evobeing_day     当天 value[24] + wear[24]
     */
    const char *evobeing_history_path = NULL;
    const char *evobeing_today_path = NULL;
    const char *zhenghuimin_out_path = NULL;
    evobeing_input evobeing_history;
    evobeing_day evobeing_today;

    /*
     * ---------------------------------------------------------- 输出
     *
     * evobeing_res   evobeing_result
     *     .baseline        24 小时基线（中位数、分位区间、成熟度等）
     *     .today[24]       当天每小时的判定（没给 today.csv 时 has_today = 0）
     *     .blocks[n_blocks] 连续偏离时段及一句话描述
     */
    evobeing_result evobeing_res;

    char err[ZHENGHUIMIN_ERR_LEN];
    int rc;

    rc = zhenghuimin_parse_args(argc, argv, &evobeing_history_path,
                                &evobeing_today_path, &zhenghuimin_out_path);
    if (rc != 0) {
        return (rc > 0) ? 0 : 1;
    }

    if (zhenghuimin_read_csv(evobeing_history_path, &evobeing_history, NULL,
                             err, sizeof(err)) != 0) {
        fprintf(stderr, "输入错误: %s\n", err);
        return 2;
    }
    if (evobeing_today_path != NULL &&
        zhenghuimin_read_csv(evobeing_today_path, NULL, &evobeing_today,
                             err, sizeof(err)) != 0) {
        fprintf(stderr, "输入错误: %s\n", err);
        return 2;
    }

    if (evobeing_zhenghuimin_run(&evobeing_history,
                                 evobeing_today_path != NULL ? &evobeing_today : NULL,
                                 NULL, /* 默认参数 */
                                 &evobeing_res) != 0) {
        fprintf(stderr, "计算失败：输入或参数不合法\n");
        return 3;
    }

    return zhenghuimin_write_output(zhenghuimin_out_path, &evobeing_res);
}
