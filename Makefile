.DEFAULT_GOAL = all

NAME = t3
VERSION ?= unknown
PREFIX ?= /usr/local
OPTFLAGS ?= -g
CFLAGS = -Wall $(OPTFLAGS) -DVERSION='"$(VERSION)"'

BINDIR = $(PREFIX)/bin
BIN = $(NAME)
INSTBIN = $(addprefix $(BINDIR)/,$(BIN))

MAN1DIR = $(PREFIX)/share/man/man1
MAN1 = $(NAME).1
INSTMAN1 = $(addprefix $(MAN1DIR)/,$(MAN1))

.SUFFIXES: .x .1

.x.1:
	help2man \
	    --section=1 \
	    --include=$< \
	    --no-info \
	    ./$(basename $<) -o $@

$(NAME).1: $(BIN)

$(BINDIR)/%: %
	@mkdir -p $(@D)
	cp $< $@
	chmod 555 $@

$(MAN1DIR)/%: %
	@mkdir -p $(@D)
	cp $< $@
	chmod 444 $@

.PHONY: all install lint format clean stress options-test test bench
all: $(BIN) $(MAN1)

install: $(INSTBIN) $(INSTMAN1)

lint: $(BIN).c
	clang-tidy $< -- $(CFLAGS)

format: $(BIN).c
	clang-format -i $<

clean:
	-rm -f $(BIN) $(MAN1)

TESTS_DIR = tests
TESTS = $(basename $(wildcard $(TESTS_DIR)/*.args))

# Allocate a unique scratch-directory name but do not create it: the test
# rules `mkdir -p $(TESTTMPDIR)` it on demand, so plain `make`/`make all`/
# `make install` never leave an empty temp directory behind.
TESTTMPDIR := $(shell mktemp -d -u "$${TMPDIR:-/tmp}/t3-tests.XXXXXX")
define TEST_template =
  $(eval testname := $(notdir $(test)))

  .INTERMEDIATE: $(test).sh
  $(test).sh: $(test).args
	@mkdir -p $$(TESTTMPDIR)
	@echo "#!/bin/sh" > $$@
	@printf '%s' './$$(BIN) "$$$$1" ' >> $$@
	@cat $(test).args >> $$@
	@chmod +x $$@

  # If the test files aren't present or make is invoked with
  # REGENERATE_TEST_RESULTS=1, automatically outputs from args.
  $(test).rc: $(test).sh $(if $(REGENERATE_TEST_RESULTS),FORCE)
	@echo; echo "--> INFO: regenerating test outputs for $(test)"
	$$< $(test).log > $(test).out 2> $(test).err; echo $$$$? > $(test).rc
	@echo; echo "--> done ... adding to git"
	git add $(test).rc

  $(test).log $(test).out $(test).err: $(test).rc
	touch $(test).log $(test).out $(test).err
	git add -f $(test).log $(test).out $(test).err

  .PHONY: $(test)/run
  $(test)/run: $(test).sh $$(BIN) $(if $(wildcard .git),$(test).rc $(test).log $(test).out $(test).err)
	@expectedrc=`cat $$(@D).rc`; \
	echo "+ $$< $$(TESTTMPDIR)/$(testname).log > $$(TESTTMPDIR)/$(testname).out 2> $$(TESTTMPDIR)/$(testname).err"; \
	$$< $$(TESTTMPDIR)/$(testname).log > $$(TESTTMPDIR)/$(testname).out 2> $$(TESTTMPDIR)/$(testname).err; \
	actualrc=$$$$?; \
	if [ $$$$actualrc -ne $$$$expectedrc ]; then \
	    echo "Failed test $$(@D)"; \
	    echo "Was expecting a return code of $$$$expectedrc and instead got return code $$$$actualrc"; \
	    echo "See contents of $$(@D) and $$(TESTTMPDIR)/$(testname)"; \
	    exit 1; \
	fi

  .PHONY: $(test)/log
  $(test)/log: $(test)/run
	@if [ -e $$(@D).log ]; then \
	    cmp $$(TESTTMPDIR)/$(testname).log $$(@D).log || ( \
	        echo "Failed test $$(@D)"; \
		diff -u $$(@D).log $$(TESTTMPDIR)/$(testname).log; \
	        exit 1; \
	    ); \
	elif [ -s $$(TESTTMPDIR)/$(testname).log ]; then \
	    echo "Failed test $$(@D)"; \
	    echo "Produced STDOUT in $$(TESTTMPDIR)/$(testname).log when we were not expecting any"; \
	    exit 1; \
	fi

  .PHONY: $(test)/out
  $(test)/out: $(test)/run
	@if [ -e $$(@D).out ]; then \
	    cmp $$(TESTTMPDIR)/$(testname).out $$(@D).out || ( \
	        echo "Failed test $$(@D)"; \
		diff -u $$(@D).out $$(TESTTMPDIR)/$(testname).out; \
	        exit 1; \
	    ); \
	elif [ -s $$(TESTTMPDIR)/$(testname).out ]; then \
	    echo "Failed test $$(@D)"; \
	    echo "Produced STDOUT in $$(TESTTMPDIR)/$(testname).out when we were not expecting any"; \
	    exit 1; \
	fi

  .PHONY: $(test)/err
  $(test)/err: $(test)/run
	@if [ -e $$(@D).err ]; then \
	    cmp $$(TESTTMPDIR)/$(testname).err $$(@D).err || ( \
	        echo "Failed test $$(@D)"; \
		diff -u $$(@D).err $$(TESTTMPDIR)/$(testname).err; \
	        exit 1; \
	    ); \
	elif [ -s $$(TESTTMPDIR)/$(testname).err ]; then \
	    echo "Failed test $$(@D)"; \
	    echo "Produced STDERR in $$(TESTTMPDIR)/$(testname).err when we were not expecting any"; \
	    exit 1; \
	fi

  .PHONY: $(test)/test
  $(test)/test: $(test)/log $(test)/out $(test)/err

  .PHONY: test
  test: $(test)/test

endef

$(foreach test,$(TESTS),$(eval $(call TEST_template)))

# The partial write test depends on the compiled tests/midline-flush binary.
tests/midline-flush.sh: tests/midline-flush

# Concurrency stress test: run a highly-threaded generator under t3 and verify
# that no output is dropped, duplicated, or garbled.  Tunable via the command
# line, e.g. `make stress STRESS_THREADS=32 STRESS_LINES=5000`.
STRESS_THREADS ?= 16
STRESS_LINES ?= 1000

tests/stress: tests/stress.c
	$(CC) $(CFLAGS) -pthread $< -o $@

stress: $(BIN) tests/stress tests/run-stress tests/check-stream.awk
	@T3=./$(BIN) STRESS_THREADS=$(STRESS_THREADS) STRESS_LINES=$(STRESS_LINES) \
	    tests/run-stress

# Behavioral tests for the tee-compatible options (--append, --output-error)
# that the golden-file harness cannot express.
options-test: $(BIN) tests/run-options
	@T3=./$(BIN) tests/run-options

# Run the stress and option tests as part of the standard `make test` suite.
test: stress options-test

# Build tests: if flox is available, exercise both package builds to catch
# Nix-sandbox-specific issues (missing dependencies, race conditions) that
# don't surface in local builds.
FLOX := $(shell command -v flox 2>/dev/null)

.PHONY: flox-build-test
ifneq ($(FLOX),)
flox-build-test:
	rm -f result-t3-buildCache
	$(FLOX) build t3
	$(FLOX) build nixpkgs-t3
else
flox-build-test:
	@echo "--> INFO: skipping flox build tests (flox not in PATH)"
endif

test: flox-build-test

# Benchmark: estimate t3's marginal per-line overhead. Manual only - not part
# of `make test`. Tunable, e.g. `make bench COUNT=2000000 WIDTHS="32 128"`.
tests/bench: tests/bench.c
	$(CC) $(CFLAGS) $< -o $@

bench: $(BIN) tests/bench tests/bench-overhead
	@T3=./$(BIN) tests/bench-overhead

# Once tests are complete (and successful), remove test results.
test:
	@rm -rf $(TESTTMPDIR)

# Phony target to force re-evaluation of targets.
.PHONY: FORCE
FORCE:
