# Copyright (c) 2026 Huawei Technologies Co., Ltd. ALL RIGHTS RESERVED.
#
# See file LICENSE for terms.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
TARGET  := obmm_shm_cleanup

.PHONY: all clean

all: $(TARGET)

$(TARGET): obmm_shm_cleanup.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
