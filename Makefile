CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
CPPFLAGS += -Iinclude
LDLIBS += -lm
BUILD := build

.PHONY: all test test-asan clean

all: $(BUILD)/hb_baseline $(BUILD)/test_hb $(BUILD)/test_props

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/hb.o: src/hb.c include/hb.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c src/hb.c -o $@

$(BUILD)/hb_cli.o: src/hb_cli.c include/hb.h | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c src/hb_cli.c -o $@

$(BUILD)/hb_baseline: $(BUILD)/hb_cli.o $(BUILD)/hb.o
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_hb: tests/test_hb.c $(BUILD)/hb.o | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_hb.c $(BUILD)/hb.o $(LDLIBS) -o $@

$(BUILD)/test_props: tests/test_props.c $(BUILD)/hb.o | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) tests/test_props.c $(BUILD)/hb.o $(LDLIBS) -o $@

test: all
	$(BUILD)/test_hb
	$(BUILD)/test_props
	sh tests/test_cli.sh

# 在 ASan/UBSan 下重跑单元测试：抓越界、未定义行为。
test-asan:
	mkdir -p $(BUILD)
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(CPPFLAGS) tests/test_hb.c src/hb.c $(LDLIBS) -o $(BUILD)/test_hb_asan
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(CPPFLAGS) tests/test_props.c src/hb.c $(LDLIBS) -o $(BUILD)/test_props_asan
	$(BUILD)/test_hb_asan
	$(BUILD)/test_props_asan

clean:
	rm -rf $(BUILD)
