CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -ldl

TARGETS := obmm_cc_probe

.PHONY: all clean

all: $(TARGETS)

obmm_cc_probe: obmm_cc_probe.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGETS)
