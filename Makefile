

all: rca rca-norl

rca: rca.c
	gcc -g -Wall -Wextra -o rca -D USE_READLINE rca.c -lm -lreadline

rca-norl: rca.c
	gcc -g -Wall -Wextra -o rca-norl rca.c -lm

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

publish_prepare:
	( \
	echo $$(date +"// published %Y/%m/%d-%T -- ") $$(git rev-parse --short HEAD) ; \
	cat rca.c; \
	) > publish_me.c

publish: publish_prepare
	scp publish_me.c \
	  webcontent:/opt/very_public/www/projects.foxharp.net/software/rca.c.txt
	ssh webcontent web_publish projects
	@echo Available here: https://projects.foxharp.net/software/rca.c.txt


install:
	wake_host chive clover; sleep 3
	for x in grass hemlock chive flax colo basil clover lumber; \
	do \
		$$x put rca rca_float bin ; \
		$$x run mkdir -p man/man1 ; \
		$$x put rca.1 man/man1 ; \
	done
	# redo lumber
	lumber put rca.c .
	lumber run doit rca.c
	lumber run mv rca bin
	lumber put rca_float bin

clean:
	rm -f rca rca-norl
