

# build both the readline and no-readline versions by default
all: rca rca-norl html

rca: rca.c
	gcc -g -Wall -Wextra -o rca -D USE_READLINE rca.c -lm -lreadline

rca-norl: rca.c
	gcc -g -Wall -Wextra -o rca-norl rca.c -lm

html: html/rca-man.html html/rca-help.html

html/rca-man.html: FORCE
	man2html rca.1 | \
	    sed -e '/Content-type:/d' \
		-e 's/.*TITLE.*/&<style type="text\/css">.mywidth { max-width:40em } <\/style> <div class="mywidth">/' \
		-e 's/Section: User.*/<A HREF="#index">Index<\/A>/' \
		-e '/HREF.*man2html/d' \
		-e '/This document was created by/,$$ d' \
		    > html/rca-man.html.new

html/rca-help.html: FORCE
	PAGER= rca help q | \
	    ./txt2html.sh "rca calculator help text" \
		> html/rca-help.html.new

htmldiff:
	-diff -u html/rca-man.html html/rca-man.html.new
	-diff -u html/rca-help.html html/rca-help.html.new

htmlmv:
	mv html/rca-man.html.new html/rca-man.html
	mv html/rca-help.html.new html/rca-help.html

clean:
	rm -f rca rca-norl html/rca-man.html.new html/rca-help.html.new

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
