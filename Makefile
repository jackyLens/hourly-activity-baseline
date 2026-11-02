CC       ?= cc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -pedantic
CPPFLAGS += -Iinclude
LDLIBS   += -lm
BUILD    := build

LIB_SRC  := src/evobeing_baseline.c
LIB_HDR  := include/evobeing_baseline.h

.PHONY: all run clean

all: $(BUILD)/evobeing_baseline $(BUILD)/evobeing_example

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/evobeing_baseline.o: $(LIB_SRC) $(LIB_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $(LIB_SRC) -o $@

# 命令行程序：读 CSV，输出 JSON
$(BUILD)/evobeing_baseline: src/evobeing_main.c $(BUILD)/evobeing_baseline.o $(LIB_HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) src/evobeing_main.c $(BUILD)/evobeing_baseline.o $(LDLIBS) -o $@

# C 调用示例
$(BUILD)/evobeing_example: examples/evobeing_example.c $(BUILD)/evobeing_baseline.o $(LIB_HDR)
	$(CC) $(CFLAGS) $(CPPFLAGS) examples/evobeing_example.c $(BUILD)/evobeing_baseline.o $(LDLIBS) -o $@

run: all
	$(BUILD)/evobeing_example
	$(BUILD)/evobeing_baseline examples/data/history.csv examples/data/today.csv

clean:
	rm -rf $(BUILD)
