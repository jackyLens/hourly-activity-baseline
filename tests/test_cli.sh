#!/bin/sh
# CLI 集成测试：需要先 make。任何断言失败都会以非 0 退出。
set -eu

BIN=${BIN:-build/hb_baseline}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

# 1) 正常输出与 golden 完全一致
"$BIN" tests/fixtures/baseline.csv tests/fixtures/today.csv > "$TMP/out.json"
diff -u tests/fixtures/expected_output.json "$TMP/out.json" || fail "golden output 不一致"

# 2) 没有 today 时 today 为 null、blocks 为空
"$BIN" tests/fixtures/baseline.csv -o "$TMP/out2.json" > "$TMP/stdout.txt"
[ -s "$TMP/out2.json" ] || fail "-o 没有写出文件"
if [ -s "$TMP/stdout.txt" ]; then fail "-o 时 stdout 应为空"; fi
grep -q '"today": null' "$TMP/out2.json" || fail "缺少 today: null"
grep -q '"blocks": \[\]' "$TMP/out2.json" || fail "缺少空 blocks"

# 3) 无表头 / 带注释 / CRLF 都应得到相同结果
tail -n +2 tests/fixtures/baseline.csv > "$TMP/noheader.csv"
"$BIN" "$TMP/noheader.csv" tests/fixtures/today.csv > "$TMP/out3.json"
diff -u tests/fixtures/expected_output.json "$TMP/out3.json" || fail "无表头输入结果不一致"

{ echo "# generated fixture"; cat "$TMP/noheader.csv"; } > "$TMP/comment.csv"
"$BIN" "$TMP/comment.csv" tests/fixtures/today.csv > "$TMP/out4.json"
diff -u tests/fixtures/expected_output.json "$TMP/out4.json" || fail "注释行输入结果不一致"

sed 's/$/\r/' tests/fixtures/baseline.csv > "$TMP/crlf.csv"
"$BIN" "$TMP/crlf.csv" tests/fixtures/today.csv > "$TMP/out5.json"
diff -u tests/fixtures/expected_output.json "$TMP/out5.json" || fail "CRLF 输入结果不一致"

# 4) 退出码：输入错误 2
expect_rc() {
    want=$1
    shift
    set +e
    "$@" >/dev/null 2>&1
    got=$?
    set -e
    [ "$got" -eq "$want" ] || fail "期望退出码 $want，实际 $got：$*"
}

printf 'day,hour,wear,activity\n0,0,2,10\n' > "$TMP/bad_wear.csv"
expect_rc 2 "$BIN" "$TMP/bad_wear.csv"

printf 'day,hour,wear,activity\n0,0,1,-5\n' > "$TMP/bad_negative.csv"
expect_rc 2 "$BIN" "$TMP/bad_negative.csv"

# 字段两侧空白（CSV 常见写法）应得到相同结果
sed 's/,/ , /g' tests/fixtures/baseline.csv > "$TMP/spaces.csv"
sed 's/,/ , /g' tests/fixtures/today.csv > "$TMP/today_spaces.csv"
"$BIN" "$TMP/spaces.csv" "$TMP/today_spaces.csv" > "$TMP/out6.json"
diff -u tests/fixtures/expected_output.json "$TMP/out6.json" || fail "字段空白输入结果不一致"

printf 'day,hour,wear,activity\n0,0,1,10abc\n' > "$TMP/extra.csv"
expect_rc 2 "$BIN" "$TMP/extra.csv"

printf 'day,hour,wear,activity\n0,0,1,10\n0,0,1,20\n' > "$TMP/dup.csv"
expect_rc 2 "$BIN" "$TMP/dup.csv"

printf 'day,hour,wear,activity\n0,24,1,10\n' > "$TMP/bad_hour.csv"
expect_rc 2 "$BIN" "$TMP/bad_hour.csv"

printf 'day,hour,wear,activity\n999999999999999999999,0,1,10\n' > "$TMP/overflow.csv"
expect_rc 2 "$BIN" "$TMP/overflow.csv"

printf 'day,hour,wear,activity\n1.5,0,1,10\n' > "$TMP/fraction.csv"
expect_rc 2 "$BIN" "$TMP/fraction.csv"

printf 'day,hour,wear,activity\n0,0,1\n' > "$TMP/no_activity.csv"
expect_rc 2 "$BIN" "$TMP/no_activity.csv"

printf 'day,hour,wear,activity\n0,0,0,\n' > "$TMP/empty_activity.csv"
expect_rc 0 "$BIN" "$TMP/empty_activity.csv"

awk 'BEGIN{printf "day,hour,wear,activity\n0,0,1,"; for(i=0;i<2000;i++) printf "1"; printf "\n"}' > "$TMP/long_line.csv"
expect_rc 2 "$BIN" "$TMP/long_line.csv"

printf '' > "$TMP/empty.csv"
expect_rc 2 "$BIN" "$TMP/empty.csv"

expect_rc 2 "$BIN" "$TMP/does_not_exist.csv"
expect_rc 1 "$BIN"
expect_rc 0 "$BIN" --help

# 5) 全部 wear=0：不崩溃，所有小时 insufficient
: > "$TMP/nowear.csv"
awk 'BEGIN{print "day,hour,wear,activity"; for(d=0;d<14;d++) for(h=0;h<24;h++) print d","h",0,0"}' > "$TMP/nowear.csv"
"$BIN" "$TMP/nowear.csv" > "$TMP/nowear.json"
grep -q '"n_valid": 0' "$TMP/nowear.json" || fail "全部未佩戴时应 n_valid=0"
grep -q '"status": "insufficient"' "$TMP/nowear.json" || fail "全部未佩戴时应 insufficient"

echo "cli tests passed"
