CC ?= gcc-13
CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -Werror
BIN := bin

TOOLS := ext5_promote_dir ext5_compact ext5_demote_dir ext5_li_walk \
	ext5_li_info ext5_li_wait ext5_li_dir_policy lookup_bench \
	multidir_lookup_bench rename_bench \
	delta_bench paced_lookup_bench fstrace_replay

.PHONY: all test clean

all: $(addprefix $(BIN)/,$(TOOLS)) $(BIN)/libfstrace_record.so

$(BIN):
	mkdir -p $@

$(BIN)/ext5_promote_dir: src/fsctl/ext5_promote_dir.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/ext5_compact: src/fsctl/ext5_compact.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/ext5_demote_dir: src/fsctl/ext5_demote_dir.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/ext5_li_walk: src/fsctl/ext5_li_walk.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/ext5_li_dir_policy: src/fsctl/ext5_li_dir_policy.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/ext5_li_info: src/fsctl/ext5_li_info.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/ext5_li_wait: src/fsctl/ext5_li_wait.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/lookup_bench: src/benchmarks/lookup_bench.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/multidir_lookup_bench: src/benchmarks/multidir_lookup_bench.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/rename_bench: src/benchmarks/rename_bench.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/delta_bench: src/benchmarks/delta_bench.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

# Record and replay of an application's syscall stream (Fig. 10).
$(BIN)/fstrace_replay: src/replay/fstrace_replay.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN)/libfstrace_record.so: src/replay/fstrace_record.c | $(BIN)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -ldl

$(BIN)/paced_lookup_bench: src/benchmarks/paced_lookup_bench.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

test: all
	$(MAKE) -C tests test

clean:
	rm -f $(addprefix $(BIN)/,$(TOOLS))
	$(MAKE) -C tests clean
