

# build both the readline and no-readline versions by default
all: rca rca-norl rca-man.html rca-help.html

rca: rca.c
	gcc -g -Wall -Wextra -o rca -D USE_READLINE rca.c -lm -lreadline

rca-norl: rca.c
	gcc -g -Wall -Wextra -o rca-norl rca.c -lm

rca-man.html: Makefile
	man2html rca.1 | \
	    sed -e '/Content-type:/d' \
		-e 's/.*TITLE.*/&<style type="text\/css">.mywidth { max-width:40em } <\/style> <div class="mywidth">/' \
		-e 's/Section: User.*/<A HREF="#index">Index<\/A>/' \
		-e '/HREF.*man2html/d' \
		-e '/This document was created by/,$$ d' \
		    > rca-man.html

rca-help.html:
	PAGER= rca help q | \
	    ./txt2html.sh "rca calculator help text" \
		>rca-help.html

clean:
	rm -f rca rca-norl rca-man.html rca-help.html

# test files are simply verbatim output from an rca session.  program
# output is always indented by one space, so we remove those lines before
# feeding what's left to rca, and comparing the result, which should match
# exactly.

test:
	egrep -v '^ ' rca_test.txt | ./rca | diff -u rca_test.txt -
	@ echo test succeeded

newtest:
	egrep -v '^ ' rca_test.txt | ./rca > new_rca_test.txt

optest:
	sed -e 's/^#.*//' optests.txt > .tests.txt
	sed -e '/^ /d' .tests.txt | rca | diff -u .tests.txt -

tweaktest:
	sed -e 's/^#.*//' tweaktests.txt > .tests.txt
	sed -e '/^ /d' .tests.txt | rca | diff -u .tests.txt -
