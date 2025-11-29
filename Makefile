

all:
	doit ca.c

test:
	egrep -v '^ ' ca_test.txt | ca | diff -u ca_test.txt -
	@ echo test succeeded

newtest:
	egrep -v '^ ' ca_test.txt | ca > new_ca_test.txt
	
publish_prepare:
	( \
	echo $$(date +"// published %Y/%m/%d-%T -- ") $$(git rev-parse --short HEAD) ; \
	cat ca.c; \
	) > publish_me.c

publish: publish_prepare
	scp publish_me.c \
	  webcontent:/opt/very_public/www/projects.foxharp.net/software/ca.c.txt
	ssh webcontent web_publish projects
	@echo Available here: https://projects.foxharp.net/software/ca.c.txt


install:
	wake_host chive clover; sleep 3
	for x in grass hemlock chive flax colo basil clover; \
	do \
	 	$$x put ca bin ; \
	done
	lumber put ca.c .
	lumber run doit ca.c
	lumber run mv ca bin

