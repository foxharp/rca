
# build both the readline and no-readline versions by default
all: rca rca-norl rca.1 copyrightcheck html 

# if hitting return on an empty line doesn't cause a newline
# on the screen, add this to the compile rule.  it's due to a bug
# in the readline library.
#    -D READLINE_NO_ECHO_BARE_NL
rca: rca.c
	v="$$(git describe --dirty=+ 2>/dev/null)"; \
	gcc -g -O -Wall -Wextra -o rca \
		-DVERSION=\"$${v}\" -D USE_READLINE \
		rca.c -lm -lreadline

rca-norl: rca.c
	v="$$(git describe --dirty=+ 2>/dev/null)"; \
	gcc -g -Wall -Wextra -o rca-norl \
		-DVERSION=\"$${v}\" \
		rca.c -lm

rca.1: rca.man
	v="$$(date +%Y-%m-%d)"; \
	sed -e "s/VERSIONSTRING/$${v}/g" rca.man > rca.1

copyrightcheck:
	@year=$$(date +%Y) ;\
	echo Checking for $$year in licenses; \
	grep -q "Copyright.*$$year" LICENSE && \
	grep -q "Copyright.*$$year" rca.c

html: docs/index.html docs/rca-man.html docs/rca-help.html

docs/index.html: FORCE
	python3 -m markdown README.md >docs/index.html.new

docs/rca-man.html: FORCE
	MAN_KEEP_FORMATTING=1 MANWIDTH=75 \
	    man --no-justification --no-hyphenation --local-file rca.1 | \
	     aha -w -t "rca(1) man page" --style 'font-size:125%' | \
	      sed -e 's/text-decoration: *underline;/font-style:italic;/g' \
		> docs/rca-man.html.new

docs/rca-help.html: FORCE
	PAGER= ./rca help q | \
	    aha -t "rca calculator help text" --style 'font-size:125%' \
		> docs/rca-help.html.new

htmldiff:
	-diff -u docs/index.html docs/index.html.new
	-diff -u docs/rca-man.html docs/rca-man.html.new
	-diff -u docs/rca-help.html docs/rca-help.html.new

htmlmv:
	mv docs/index.html.new docs/index.html
	mv docs/rca-man.html.new docs/rca-man.html
	mv docs/rca-help.html.new docs/rca-help.html

clean:
	rm -f rca rca-norl rca.1 .test docs/index.html.new \
		docs/rca-man.html.new docs/rca-help.html.new

# test files are simply verbatim output from an rca session.  program
# output is always indented by one space, so we remove those lines
# before feeding what's left (i.e., the input) to rca, and comparing
# the result, which should match exactly.

tests:  gentest optest tweaktest

gentest:
	egrep -v '^ ' tests/gentests.txt | \
		./rca 2>&1 | \
		tee .test | diff -u tests/gentests.txt -
	@ echo test succeeded

# valgrind messes with floating point.  only optests.txt avoids high
# precision FP, so only it works under valgrind.
optest:
	egrep -v '^ ' tests/optests.txt | \
		valgrind -q --leak-check=full ./rca 2>&1 | \
		tee .test | diff -u tests/optests.txt -
	@ echo test succeeded

tweaktest:
	egrep -v '^ ' tests/tweaktests.txt | \
		./rca 2>&1 | \
		tee .test | diff -u tests/tweaktests.txt -
	@ echo test succeeded

.PHONY: clean all test optest tweaktest html htmldiff htmlmv

FORCE:
