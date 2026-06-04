CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
LDFLAGS ?=
LDLIBS ?= -ldl

all: obmm_cc_ownership_probe

obmm_cc_ownership_probe: obmm_cc_ownership_probe.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f obmm_cc_ownership_probe
