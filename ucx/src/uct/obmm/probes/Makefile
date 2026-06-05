CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
LDFLAGS ?=
LDLIBS ?= -ldl

TARGETS = obmm_cc_ownership_probe obmm_cc_owned_path_probe \
	obmm_cc_batch_path_probe

all: $(TARGETS)

obmm_cc_ownership_probe: obmm_cc_ownership_probe.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

obmm_cc_owned_path_probe: obmm_cc_owned_path_probe.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

obmm_cc_batch_path_probe: obmm_cc_batch_path_probe.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGETS)
