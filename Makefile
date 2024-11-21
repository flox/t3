
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
TESTS = $(basename $(wildcard $(TESTS_DIR)/*.rc))
TESTTMPDIR := $(shell mktemp -d)
define TEST_template =
  .PHONY: $(test)/run
  $(test)/run: $$(BIN)
	@mkdir -p $$(TESTTMPDIR)/$(TESTS_DIR)
	@expectedrc=`cat $$(@D).rc`; \
	echo '+ ./$$< $$(@D) > $$(TESTTMPDIR)/$$(@D).out 2> $$(TESTTMPDIR)/$$(@D).err' >&2; \
	./$$< $$(@D) > $$(TESTTMPDIR)/$$(@D).out 2> $$(TESTTMPDIR)/$$(@D).err; \
	actualrc=$$$$?; \
	if [ $$$$actualrc -ne $$$$expectedrc ]; then \
	    echo "Failed test $$(@D)"; \
	    echo "Was expecting a return code of $$$$expectedrc and instead got return code $$$$actualrc"; \
	    echo "See contents of $$(@D) and $$(TESTTMPDIR)/$$(@D)"; \
	    exit 1; \
	fi

  .PHONY: $(test)/out
  $(test)/out: $(test)/run
	@if [ -e $$(@D).out ]; then \
	    cmp $$(TESTTMPDIR)/$$(@D).out $$(@D).out || ( \
	        echo "Failed test $$(@D)"; \
	        echo "Was expecting on STDOUT:"; \
	        cat $$(@D).out; \
	        echo "Observed on STDOUT:"; \
	        cat $$(TESTTMPDIR)/$$(@D).out; \
	        exit 1; \
	    ); \
	elif [ -s $$(TESTTMPDIR)/$$(@D).out ]; then \
	    echo "Failed test $$(@D)"; \
	    echo "Produced STDOUT in $$(TESTTMPDIR)/$$(@D).out when we were not expecting any"; \
	    exit 1; \
	fi

  .PHONY: $(test)/err
  $(test)/err: $(test)/run
	@if [ -e $$(@D).err ]; then \
	    cmp $$(TESTTMPDIR)/$$(@D).err $$(@D).err || ( \
	        echo "Failed test $$(@D)"; \
	        echo "Was expecting on STDERR:"; \
	        cat $$(@D).err; \
	        echo "Observed on STDERR:"; \
	        cat $$(TESTTMPDIR)/$$(@D).err; \
	        exit 1; \
	    ); \
	elif [ -s $$(TESTTMPDIR)/$$(@D).err ]; then \
	    echo "Failed test $$(@D)"; \
	    echo "Produced STDERR in $$(TESTTMPDIR)/$$(@D).err when we were not expecting any"; \
	    exit 1; \
	fi

  .PHONY: $(test)/test
  $(test)/test: $(test)/out $(test)/err

  .PHONY: test
  test: $(test)/test

endef

$(foreach test,$(TESTS),$(eval $(call TEST_template)))

# Once tests are complete (and successful), remove test results.
test:
	-rm -rf $(TESTTMPDIR)
