
# if you don't have make, or this makefile doesn't work for you,
# the simple commands you want are one of these:
#	gcc -g -o rca -D USE_EDITLINE rca.c -lmpdec -lm -leditline
# if you don't have editline, then try:
#	gcc -g -o rca -D USE_READLINE rca.c -lmpdec -lm -lreadline
# if you don't have either, then build without command-line editing:
#	gcc -g -o rca rca.c -lmpdec -lm
#
# if you don't have the mpdecimal library:
# debian/ubuntu:
# 	sudo apt install libmpdec-dev   (will pull in libmpdec4 or earlier)
# fedora/redhat:
# 	sudo dnf install mpdecimal mpdecimal-devel
# or download a tarball from its host site:
#	https://www.bytereef.org/mpdecimal/download.html

all: rca rca.1 copyrightcheck

# to override install paths, use: "make PREFIX=/home/me install"
PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man
MAN1DIR ?= $(MANDIR)/man1

CFLAGS += -Wall -Wextra -Wfloat-conversion -Wconversion  \
    -Warray-bounds=2 -Wformat-security -Wsign-conversion \
    -Wshift-overflow=2 -Wstrict-overflow=2 -pedantic -ffunction-sections

# normally, if mpdecimal is system-installed, it's just:
# LIBS += -lmpdec

# but if building against a local install, then you probably want
# a static link for libmpdec:
LIBS += -L/usr/local/mpdecimal/lib -Wl,-Bstatic -lmpdec -Wl,-Bdynamic
CFLAGS += -I /usr/local/mpdecimal/include

LIBS += -lm

# these are a means of finding unused functions:
# CFLAGS +=  -Wl,--gc-sections -Wl,--print-gc-sections

rca: rca.c
	gver="$$(git describe --dirty=+ 2>/dev/null || echo '+?')"; \
	gcc -g -o rca \
		$(CFLAGS) -DGITVERSION=\"$${gver}\" \
		rca.c $(LIBS)

VALGRIND_CHECK := $(shell which valgrind >/dev/null && \
			echo '-D DO_VALGRIND_CHECKS')
ifneq ($(VALGRIND_CHECK),)
 RUNVG=valgrind -q --leak-check=full
 CFLAGS += $(VALGRIND_CHECK)
endif


# build-time check for whether editline is available:
EDITLINE_CHECK := $(shell echo "int main() { return 0; }" > _tt.c; \
   $(CC) _tt.c -ledit -o _tt 2>/dev/null && echo YES; rm -f _tt.c _tt )

# due to licensing incompatibility, building against readline requires
# a manual step.  run this command:
#     make ENABLE_READLINE_BUILD=YES
#
# editline is preferred, since its license matches rca's.
# if you don't already have editline, use:
#   apt install libedit-dev    (on debian/ubuntu/etc)
#   dnf install libedit-devel      (on fedora/redhat/etc)

ifeq ($(ENABLE_READLINE_BUILD),YES)
    # linking against readline means GPL3 distribution terms will apply
    # to the built binary (even if libreadline is a shared library).
    $(info -- Building with readline support ---)
    $(info -- The binary will be subject to GPL3 distribution terms!! ---)
    # some systems might also require libncurses or libtinfo
    LIBS += -lreadline   # -lncurses -ltinfo
    CFLAGS += -D USE_READLINE
    # if you're using readline, and hitting Enter on an empty line
    # doesn't cause rca to echo the newline, it's due to a bug in some
    # builds of readline 8.2.  uncomment this as a workaround:
    #  CFLAGS += -D READLINE_NO_ECHO_BARE_NL

else ifeq ($(EDITLINE_CHECK),YES)
    $(info --- Building with editline support ---)
    LIBS += -ledit
    CFLAGS += -D USE_EDITLINE
else
    $(info No command editing support!!)
endif


rca.1: rca.man
	v="\"$$(rca version q)\""; \
	sed -e "s/VERSIONSTRING/$${v}/g" rca.man > rca.1

copyrightcheck:
	@year=$$(date +%Y) ;\
	echo Checking for $$year in copyrights... ; \
	grep -q "Copyright.*$$year" LICENSE && \
	grep -q "Copyright.*$$year" rca.c || \
	echo "  out of date."


# building the html into ".new" files keeps the git tree clean(er)
# most of the time.  we don't commit the html build products that
# often.  see htmldiff and htmlmv targets below.  put a copy
# of the readme in /tmp, for easier local browser viewing.
html: docs/index.html.new docs/rca-man.html.new docs/rca-help.html.new
	cp docs/index.html.new /tmp/rca-readme.html

docs/index.html.new: README.md
	docs/html_preamble >docs/index.html.new
	docs/gfm README.md >>docs/index.html.new

# target for testing/proofreading only.  all we deploy is the .md file.
docs/branch_info.html.new: BRANCH_INFO.md
	docs/html_preamble >docs/branch_info.html.new
	docs/gfm BRANCH_INFO.md >>docs/branch_info.html.new

docs/rca-man.html.new: rca.1
	MAN_KEEP_FORMATTING=1 MANWIDTH=75 \
	    man --no-justification --no-hyphenation --local-file rca.1 | \
	     aha -w -t "rca(1) man page" --style 'font-size:125%' | \
	      sed -e 's/text-decoration: *underline;/font-style:italic;/g' \
		> docs/rca-man.html.new

docs/rca-help.html.new: rca
	PAGER= ./rca help q | \
	    aha -t "rca calculator help text" --style 'font-size:125%' \
		> docs/rca-help.html.new

SHELL = /bin/bash
htmldiff:
	-diff -u <( links -dump docs/index.html ) \
		<( links -dump docs/index.html.new )
	-diff -u <( links -dump docs/rca-man.html ) \
		<( links -dump docs/rca-man.html.new )
	-diff -u <( links -dump docs/rca-help.html ) \
		<( links -dump docs/rca-help.html.new )

htmlmv:
	mv docs/index.html.new docs/index.html
	mv docs/rca-man.html.new docs/rca-man.html
	mv docs/rca-help.html.new docs/rca-help.html


# to release a new version:
#   update CHANGES file
#   vi rca.c (bump release version)
#   git commit -a     # "bumped to version vNN"
#   make release
#   make htmldiff   # check html files
#   make htmlmv
#   git commit docs  # "update docs for vNN"


release: tag clean all html versioncheck

tag: FORCE
	@git diff --quiet HEAD -- || (echo dirty tree; false)
	@tag=$$(sed -n -e 's/[^"]*"\([^"]*\).*/\1/p' -e '1q' rca.c); \
	read -p "Hit ^C to abort tagging with $$tag..."; \
	echo git tagging as $$tag;  \
	set -x; \
	git tag -a $$tag -m "Release version $$tag"

versioncheck:
	@echo rca: ;rca version q || true
	@echo "man & help": ; \
		tail -n 6 docs/*.new | \
		sed -n -e 's/<span .*//' \
			-e 's/^ *\( version v[0-9]\+.*\)/\1/p'

clean:
	rm -f rca rca rca.1 docs/index.html.new \
		docs/rca-man.html.new docs/rca-help.html.new
	rm -fr tests/tmp

# debian packaging uses this target, so be sure it always honors
# DESTDIR and INSTALL correctly
install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(MAN1DIR)
	$(INSTALL) -m 0755 rca $(DESTDIR)$(BINDIR)/rca
	$(INSTALL) -m 0644 rca.1 $(DESTDIR)$(MAN1DIR)/rca.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/rca
	rm -f $(DESTDIR)$(MAN1DIR)/rca.1



# test files are simply verbatim output from an rca session.  program
# output is always indented by one space, so we remove those lines
# before feeding what's left (i.e., the input) to rca, and comparing
# the result, which should match exactly.

ID=$$(./rca state q|sed -n 's/ *rca descriptor: *//p')
tests:  gentest optest tweaktest pi_approximations
	@echo Tests succeeded

pi_approximations:  # with and without rca_float
	test $$(PATH=:$$PATH bash -c ". ./rca_float; fe '22 / 7 - pi'") = 0.0013
	test $$(./rca "10 digits fixed ((355 / 113) - pi) q") = 0.0000002668


gentest:
	mkdir -p tests/tmp
	egrep -v '^ ' tests/mp30/gentests.txt | \
		( $(RUNVG) ./rca 1echo 2>&1 ) | \
		tee tests/tmp/gentests.txt | \
		diff -u tests/$(ID)/gentests.txt -

optest:
	mkdir -p tests/tmp
	egrep -v '^ ' tests/mp30/optests.txt | \
		( $(RUNVG) ./rca 1echo 2>&1 ) | \
		tee tests/tmp/optests.txt | \
		diff -u tests/$(ID)/optests.txt -

tweaktest:
	mkdir -p tests/tmp
	egrep -v '^ ' tests/mp30/tweaktests.txt | \
		( $(RUNVG) ./rca 1echo 2>&1 ) | \
		tee tests/tmp/tweaktests.txt | \
		diff -u tests/$(ID)/tweaktests.txt -

.PHONY: clean all gentest optest tweaktest html htmldiff htmlmv \
	release tag versioncheck pi_approximations tests

FORCE:
