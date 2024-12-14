
.DEFAULT_GOAL = all

NAME = t3
VERSION = 1.0.0
PREFIX ?= /usr/local
CFLAGS = -Wall -g -DVERSION='"$(VERSION)"'

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

.PHONY: all install clean
all: $(BIN) $(MAN1)

install: $(INSTBIN) $(INSTMAN1)

clean:
	-rm -f $(BIN) $(MAN1)

TESTS_DIR = tests
TESTS = $(basename $(wildcard $(TESTS_DIR)/*.args))
TESTTMPDIR := $(shell mktemp -d)
define TEST_template =
  $(eval testname := $(notdir $(test)))

  .INTERMEDIATE: $(test).sh
  $(test).sh: $(test).args
	@mkdir -p $$(TESTTMPDIR)
	@echo "#!/bin/sh" > $$@
	@echo -n './$$(BIN) "$$$$1" ' >> $$@
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
	        echo "Was expecting on STDOUT:"; \
	        cat $$(@D).log; \
	        echo "Observed on STDOUT:"; \
	        cat $$(TESTTMPDIR)/$(testname).log; \
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
	        echo "Was expecting on STDOUT:"; \
	        cat $$(@D).out; \
	        echo "Observed on STDOUT:"; \
	        cat $$(TESTTMPDIR)/$(testname).out; \
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
	        echo "Was expecting on STDERR:"; \
	        cat $$(@D).err; \
	        echo "Observed on STDERR:"; \
	        cat $$(TESTTMPDIR)/$(testname).err; \
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
tests/midline-flush/run: tests/midline-flush

# Once tests are complete (and successful), remove test results.
test:
	@rm -rf $(TESTTMPDIR)

# Phony target to force re-evaluation of targets.
.PHONY: FORCE
FORCE:
