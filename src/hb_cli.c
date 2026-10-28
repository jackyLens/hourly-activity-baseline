/*
 * hb_cli.c — 命令行入口：只负责输入解析与输出，计算全部调用 hb.c。
 *
 * 用法：
 *   hb_baseline baseline.csv [today.csv] [-o out.json]
 *
 * 输入 CSV（长表，逗号分隔，无表头也可，允许一行表头）：
 *   baseline.csv: day,hour,wear,activity   （day 0..13，hour 0..23，wear 0/1）
 *   today.csv:    hour,wear,activity       （hour 0..23，wear 0/1）
 *
 * 输出：JSON（见 README 的 I/O 契约）。
 * 退出码：0 成功，1 用法错误，2 输入错误，3 计算失败。
 */

#include "hb.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 512

static void usage(FILE *f, const char *prog)
{
    fprintf(f,
            "用法: %s [选项] <baseline.csv> [today.csv]\n"
            "\n"
            "  baseline.csv  14 天 × 24 小时，列: day,hour,wear,activity\n"
            "  today.csv     当天 24 小时，列: hour,wear,activity（可选）\n"
            "  -o, --out F   把 JSON 写入 F（默认 stdout）\n"
            "  -h, --help    显示本帮助\n",
            prog);
}

static void strip_eol(char *s)
{
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t') {
            break;
        }
        s[--n] = '\0';
    }
}

static int starts_like_data(const char *p)
{
    return isdigit((unsigned char)p[0]) || p[0] == '-' || p[0] == '+';
}

static int read_baseline(const char *path, hb_input *in, char *err, size_t errlen)
{
    FILE *f;
    char line[MAX_LINE];
    unsigned char seen[HB_WINDOW_DAYS][HB_HOURS];
    int lineno = 0;
    int header_skipped = 0;
    int rows = 0;

    f = fopen(path, "r");
    if (f == NULL) {
        (void)snprintf(err, errlen, "打不开文件: %s", path);
        return -1;
    }
    memset(seen, 0, sizeof(seen));
    hb_input_init(in);

    while (fgets(line, sizeof(line), f) != NULL) {
        char *p;
        int day;
        int hour;
        int wear;
        int k;
        double val = 0.0;

        lineno++;
        strip_eol(line);
        p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            continue;
        }
        if (!starts_like_data(p)) {
            if (!header_skipped) {
                header_skipped = 1;
                continue;
            }
            (void)snprintf(err, errlen, "第 %d 行无法解析: %s", lineno, p);
            fclose(f);
            return -1;
        }

        k = sscanf(p, "%d,%d,%d,%lf", &day, &hour, &wear, &val);
        if (k < 3) {
            (void)snprintf(err, errlen, "第 %d 行字段不足（需要 day,hour,wear,activity）", lineno);
            fclose(f);
            return -1;
        }
        if (day < 0 || day >= HB_WINDOW_DAYS) {
            (void)snprintf(err, errlen, "第 %d 行 day=%d 越界（0..%d）", lineno, day,
                           HB_WINDOW_DAYS - 1);
            fclose(f);
            return -1;
        }
        if (hour < 0 || hour >= HB_HOURS) {
            (void)snprintf(err, errlen, "第 %d 行 hour=%d 越界（0..%d）", lineno, hour,
                           HB_HOURS - 1);
            fclose(f);
            return -1;
        }
        if (wear != 0 && wear != 1) {
            (void)snprintf(err, errlen, "第 %d 行 wear=%d 只能是 0 或 1", lineno, wear);
            fclose(f);
            return -1;
        }
        if (wear == 1 && (k < 4 || !isfinite(val) || val < 0.0)) {
            (void)snprintf(err, errlen, "第 %d 行 wear=1 需要非负且有限的 activity", lineno);
            fclose(f);
            return -1;
        }
        if (seen[day][hour]) {
            (void)snprintf(err, errlen, "第 %d 行重复的 day=%d,hour=%d", lineno, day, hour);
            fclose(f);
            return -1;
        }
        seen[day][hour] = 1;
        if (hb_input_set(in, day, hour, wear, val) != 0) {
            (void)snprintf(err, errlen, "第 %d 行写入失败", lineno);
            fclose(f);
            return -1;
        }
        rows++;
    }
    fclose(f);

    if (rows == 0) {
        (void)snprintf(err, errlen, "%s 没有有效数据行", path);
        return -1;
    }
    return 0;
}

static int read_today(const char *path, int has[HB_HOURS], double val[HB_HOURS],
                      char *err, size_t errlen)
{
    FILE *f;
    char line[MAX_LINE];
    unsigned char seen[HB_HOURS];
    int lineno = 0;
    int header_skipped = 0;
    int rows = 0;
    int h;

    for (h = 0; h < HB_HOURS; h++) {
        has[h] = 0;
        val[h] = 0.0;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        (void)snprintf(err, errlen, "打不开文件: %s", path);
        return -1;
    }
    memset(seen, 0, sizeof(seen));

    while (fgets(line, sizeof(line), f) != NULL) {
        char *p;
        int hour;
        int wear;
        int k;
        double v = 0.0;

        lineno++;
        strip_eol(line);
        p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            continue;
        }
        if (!starts_like_data(p)) {
            if (!header_skipped) {
                header_skipped = 1;
                continue;
            }
            (void)snprintf(err, errlen, "第 %d 行无法解析: %s", lineno, p);
            fclose(f);
            return -1;
        }

        k = sscanf(p, "%d,%d,%lf", &hour, &wear, &v);
        if (k < 2) {
            (void)snprintf(err, errlen, "第 %d 行字段不足（需要 hour,wear,activity）", lineno);
            fclose(f);
            return -1;
        }
        if (hour < 0 || hour >= HB_HOURS) {
            (void)snprintf(err, errlen, "第 %d 行 hour=%d 越界（0..%d）", lineno, hour,
                           HB_HOURS - 1);
            fclose(f);
            return -1;
        }
        if (wear != 0 && wear != 1) {
            (void)snprintf(err, errlen, "第 %d 行 wear=%d 只能是 0 或 1", lineno, wear);
            fclose(f);
            return -1;
        }
        if (wear == 1 && (k < 3 || !isfinite(v) || v < 0.0)) {
            (void)snprintf(err, errlen, "第 %d 行 wear=1 需要非负且有限的 activity", lineno);
            fclose(f);
            return -1;
        }
        if (seen[hour]) {
            (void)snprintf(err, errlen, "第 %d 行重复的 hour=%d", lineno, hour);
            fclose(f);
            return -1;
        }
        seen[hour] = 1;
        has[hour] = wear;
        val[hour] = (wear == 1) ? v : 0.0;
        rows++;
    }
    fclose(f);

    if (rows == 0) {
        (void)snprintf(err, errlen, "%s 没有有效数据行", path);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- JSON 输出 */

static void jnum(FILE *f, int has, double v, int prec)
{
    if (!has) {
        fputs("null", f);
        return;
    }
    if (v == 0.0) {
        v = fabs(v); /* 避免输出 -0.0 */
    }
    fprintf(f, "%.*f", prec, v);
}

static void jstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s != '\0'; s++) {
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

static void jbool(FILE *f, int v)
{
    fputs(v ? "true" : "false", f);
}

static void write_hour(FILE *f, const hb_hour *h)
{
    fputs("    {\"hour\": ", f);
    fprintf(f, "%d", h->hour);
    fputs(", \"status\": ", f);
    jstr(f, hb_status_name(h->status));
    fprintf(f, ", \"n_valid\": %d, \"n_missing\": %d, \"n_zero\": %d",
            h->n_valid, h->n_missing, h->n_zero);

    fputs(", \"median\": ", f);
    jnum(f, h->has_stats, h->median, 1);
    fputs(", \"mad\": ", f);
    jnum(f, h->has_stats, h->mad_raw, 1);
    fputs(", \"mad_eff\": ", f);
    jnum(f, h->has_stats, h->mad_eff, 1);
    fputs(", \"p10\": ", f);
    jnum(f, h->has_stats, h->p10, 1);
    fputs(", \"p25\": ", f);
    jnum(f, h->has_stats, h->p25, 1);
    fputs(", \"p75\": ", f);
    jnum(f, h->has_stats, h->p75, 1);
    fputs(", \"p90\": ", f);
    jnum(f, h->has_stats, h->p90, 1);
    fputs(", \"inner_low\": ", f);
    jnum(f, h->has_stats, h->inner_low, 1);
    fputs(", \"inner_high\": ", f);
    jnum(f, h->has_stats, h->inner_high, 1);
    fputs(", \"outer_low\": ", f);
    jnum(f, h->has_stats, h->outer_low, 1);
    fputs(", \"outer_high\": ", f);
    jnum(f, h->has_stats, h->outer_high, 1);
    fputs(", \"p_active\": ", f);
    jnum(f, h->has_stats, h->p_active, 2);
    fputs(", \"active_level\": ", f);
    jnum(f, h->has_stats, h->active_level, 1);
    fputs(", \"rest_expected\": ", f);
    jbool(f, h->rest_expected);
    fputs(", \"band_available\": ", f);
    jbool(f, h->band_available);
    fputc('}', f);
}

static void write_json(FILE *f, const hb_input *in, const hb_baseline *b,
                       const hb_deviation *devs, int have_today,
                       const hb_block *blocks, int nblocks)
{
    int h;

    fputs("{\n", f);
    fputs("  \"method\": \"hourly_personal_baseline_c_v1\",\n", f);
    fprintf(f, "  \"window_days\": %d,\n", b->window_days);
    fprintf(f, "  \"days_present\": %d,\n", in->days);
    fprintf(f, "  \"hour_count\": %d,\n", HB_HOURS);
    fputs("  \"floor\": ", f);
    jnum(f, 1, b->floor, 1);
    fputs(",\n  \"scale\": ", f);
    jnum(f, b->has_scale, b->scale, 1);
    fputs(",\n", f);

    fputs("  \"baseline\": [\n", f);
    for (h = 0; h < HB_HOURS; h++) {
        write_hour(f, &b->hours[h]);
        fputs((h + 1 < HB_HOURS) ? ",\n" : "\n", f);
    }
    fputs("  ],\n", f);

    if (!have_today) {
        fputs("  \"today\": null,\n", f);
        fputs("  \"blocks\": []\n", f);
    } else {
        fputs("  \"today\": [\n", f);
        for (h = 0; h < HB_HOURS; h++) {
            const hb_deviation *d = &devs[h];
            fputs("    {\"hour\": ", f);
            fprintf(f, "%d", d->hour);
            fputs(", \"value\": ", f);
            jnum(f, d->has_value, d->value, 1);
            fputs(", \"state\": ", f);
            jstr(f, hb_state_name(d->state));
            fputs(", \"z\": ", f);
            jnum(f, d->has_z, d->z, 2);
            fputs(", \"delta\": ", f);
            jnum(f, d->has_delta, d->delta, 1);
            fputs(", \"expected\": ", f);
            jnum(f, d->has_expected, d->expected, 1);
            fputs(", \"band_position\": ", f);
            jstr(f, d->band_position);
            fputs(", \"reason\": ", f);
            jstr(f, d->reason);
            fputc('}', f);
            fputs((h + 1 < HB_HOURS) ? ",\n" : "\n", f);
        }
        fputs("  ],\n", f);

        fputs("  \"blocks\": [\n", f);
        for (h = 0; h < nblocks; h++) {
            char text[160];
            const hb_block *blk = &blocks[h];
            if (hb_block_text(blk, text, sizeof(text)) != 0) {
                text[0] = '\0';
            }
            fputs("    {\"start_hour\": ", f);
            fprintf(f, "%d", blk->start_hour);
            fputs(", \"end_hour\": ", f);
            fprintf(f, "%d", blk->end_hour);
            fputs(", \"n_hours\": ", f);
            fprintf(f, "%d", blk->n_hours);
            fputs(", \"state\": ", f);
            jstr(f, hb_state_name(blk->state));
            fputs(", \"total\": ", f);
            jnum(f, 1, blk->total, 1);
            fputs(", \"expected\": ", f);
            jnum(f, 1, blk->expected, 1);
            fputs(", \"ratio\": ", f);
            jnum(f, blk->has_ratio, blk->ratio, 2);
            fputs(", \"text\": ", f);
            jstr(f, text);
            fputc('}', f);
            fputs((h + 1 < nblocks) ? ",\n" : "\n", f);
        }
        fputs("  ]\n", f);
    }
    fputs("}\n", f);
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *out_path = NULL;
    const char *files[2] = {NULL, NULL};
    int nfiles = 0;
    int i;
    int h;
    hb_input in;
    hb_config cfg;
    hb_baseline b;
    hb_deviation devs[HB_HOURS];
    hb_block blocks[HB_HOURS];
    int have_today = 0;
    int nb;
    int has_today[HB_HOURS];
    double val_today[HB_HOURS];
    char err[256];
    FILE *out = stdout;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "缺少 -o 的参数\n");
                return 1;
            }
            out_path = argv[++i];
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "未知选项: %s\n", a);
            usage(stderr, argv[0]);
            return 1;
        }
        if (nfiles >= 2) {
            fprintf(stderr, "参数过多: %s\n", a);
            usage(stderr, argv[0]);
            return 1;
        }
        files[nfiles++] = a;
    }

    if (nfiles < 1) {
        usage(stderr, argv[0]);
        return 1;
    }

    if (read_baseline(files[0], &in, err, sizeof(err)) != 0) {
        fprintf(stderr, "输入错误: %s\n", err);
        return 2;
    }
    if (nfiles == 2) {
        if (read_today(files[1], has_today, val_today, err, sizeof(err)) != 0) {
            fprintf(stderr, "输入错误: %s\n", err);
            return 2;
        }
        have_today = 1;
    }

    hb_config_default(&cfg);
    if (hb_fit(&in, &cfg, &b) != 0) {
        fprintf(stderr, "拟合失败：输入或参数非法\n");
        return 3;
    }

    for (h = 0; h < HB_HOURS; h++) {
        devs[h] = hb_compare(&b, &cfg, h, have_today ? has_today[h] : 0,
                             have_today ? val_today[h] : 0.0);
    }
    nb = hb_summarize(devs, HB_HOURS, blocks, HB_HOURS);
    if (nb < 0) {
        fprintf(stderr, "时段合并失败\n");
        return 3;
    }

    if (out_path != NULL) {
        out = fopen(out_path, "w");
        if (out == NULL) {
            fprintf(stderr, "写不了文件: %s\n", out_path);
            return 2;
        }
    }

    write_json(out, &in, &b, devs, have_today, blocks, nb);

    if (out != stdout) {
        fclose(out);
    }
    return 0;
}
