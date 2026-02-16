
# override with make PREFIX=/xyzzy install
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

# build both the readline and no-readline versions by default
all: rca rca-norl rca.1 copyrightcheck html 

# if hitting return on an empty line doesn't cause a newline
# on the screen, add this to the compile rule.  it's due to a bug
# in the readline library.
#    -D READLINE_NO_ECHO_BARE_NL
rca: rca.c
	v="$$(git describe --dirty=+ 2>/dev/null)"; \
	gcc -g -O -Wall -Wextra -o rca \
		-DCCVERSION=\"$${v}\" -D USE_READLINE \
		rca.c -lm -lreadline

rca-norl: rca.c
	v="$$(git describe --dirty=+ 2>/dev/null)"; \
	gcc -g -Wall -Wextra -o rca-norl \
		-DCCVERSION=\"$${v}\" \
		rca.c -lm

rca.1: rca.man
	v="\"$$(rca version q)\""; \
	sed -e "s/VERSIONSTRING/$${v}/g" rca.man > rca.1

copyrightcheck:
	@year=$$(date +%Y) ;\
	echo Checking for $$year in licenses; \
	grep -q "Copyright.*$$year" LICENSE && \
	grep -q "Copyright.*$$year" rca.c

# we build the docs/*.html.new files regularly, and only occasionally,
# usually when doing releases, do we move and commit them to docs/*.html
# this keeps the git tree clean(er) most of the time.
html: docs/index.html.new docs/rca-man.html.new docs/rca-help.html.new

docs/index.html.new: README.md
	echo '<div style="max-width: 700px; width: 100%;">' >docs/index.html.new
	python3 -m markdown README.md >>docs/index.html.new

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

# to release a version:
#   vi rca.c (bump version)
#   git commit	    # "bumped to vNN"
#   make tag clean all versioncheck
#   make htmldiff   # check html files
#   make htmlmv
#   update CHANGES file
#   git commit      # "new man/help html files for vNN"

release: tag clean all versioncheck

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

clean:
	rm -f rca rca-norl rca.1 .test docs/index.html.new \
		docs/rca-man.html.new docs/rca-help.html.new

install: all
	@echo "INSTALL bin/rca"
	install -D -m 755 -o root -g root rca \
		$(PREFIX)/bin/rca
	@echo "INSTALL rca.1"
	install -D -m 644 -o root -g root rca.1 \
		$(MANPREFIX)/man1/rca.1

uninstall:
	@echo "REMOVE installed bin/rca"
	rm -f $(PREFIX)/bin/rca
	@echo "REMOVE installed rca.1"
	rm -f $(MANPREFIX)/man1/rca.1



# test files are simply verbatim output from an rca session.  program
# output is always indented by one space, so we remove those lines
# before feeding what's left (i.e., the input) to rca, and comparing
# the result, which should match exactly.

tests:  gentest optest tweaktest pi_approximations

pi_approximations:  # with and without rca_float
	test $$(bash -c "source ./rca_float; fe '22 / 7 - pi'") = 0.001
	test $$(./rca "10 digits fixed ((355 / 113) - pi) q") = 0.0000002668
	@echo Tests succeeded

gentest:
	egrep -v '^ ' tests/gentests.txt | \
		./rca 2>&1 | \
		tee .test | diff -u tests/gentests.txt -
	@ echo test succeeded

# valgrind messes with floating point.  only optests.txt avoids high
# precision FP, so only it works under valgrind.
optest:
	egrep -v '^ ' tests/optests.txt | \
		( RUNVG="valgrind -q --leak-check=full"; \
		  which valgrind >/dev/null || RUNVG=; \
		  $$RUNVG ./rca 2>&1 \
		) | \
		tee .test | diff -u tests/optests.txt -
	@ echo test succeeded

tweaktest:
	egrep -v '^ ' tests/tweaktests.txt | \
		./rca 2>&1 | \
		tee .test | diff -u tests/tweaktests.txt -
	@ echo test succeeded

.PHONY: clean all gentest optest tweaktest html htmldiff htmlmv \
	release tag versioncheck pi_approximations

FORCE:
