
# to tag a release, an annotated tag must be used.
#  git tag -a v17 -m "Release v17"  [ tags HEAD, or given commit ]

# build both the readline and no-readline versions by default
all: rca rca-norl rca.1 html

rca: rca.c
	v="$$(git describe --dirty=+ 2>/dev/null)"; \
	gcc -g -Wall -Wextra -o rca \
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

html: html/rca-man.html html/rca-help.html

html/rca-man.html: FORCE
	MAN_KEEP_FORMATTING=1 MANWIDTH=75 \
	    man --no-justification --no-hyphenation --local-file rca.1 | \
	     aha -w -t "rca(1) man page" --style 'font-size:125%' | \
	      sed -e 's/text-decoration: *underline;/font-style:italic;/g' \
		> html/rca-man.html.new

html/rca-help.html: FORCE
	PAGER= rca help q | \
	    aha -t "rca calculator help text" --style 'font-size:125%' \
		> html/rca-help.html.new

htmldiff:
	-diff -u html/rca-man.html html/rca-man.html.new
	-diff -u html/rca-help.html html/rca-help.html.new

htmlmv:
	mv html/rca-man.html.new html/rca-man.html
	mv html/rca-help.html.new html/rca-help.html

readme:
	@python3 -m markdown README.md

clean:
	rm -f rca rca-norl rca.1 html/rca-man.html.new html/rca-help.html.new

# test files are simply verbatim output from an rca session.  program
# output is always indented by one space, so we remove those lines
# before feeding what's left (i.e., the input) to rca, and comparing
# the result, which should match exactly.

test:
	egrep -v '^ ' rca_test.txt | ./rca | tee .test | diff -u rca_test.txt -
	@ echo test succeeded

optest:
	egrep -v '^ ' optests.txt | ./rca | tee .test | diff -u optests.txt -
	@ echo test succeeded

tweaktest:
	egrep -v '^ ' tweaktests.txt | ./rca | tee .test | diff -u tweaktests.txt -
	@ echo test succeeded

.PHONY: html clean all test optest tweaktest htmldiff htmlmv

FORCE:
