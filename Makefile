

all:
	doit ca.c

test:
	egrep -v '^ ' ca_test.txt | ca | diff -u ca_test.txt -

newtest:
	egrep -v '^ ' ca_test.txt | ca > new_ca_test.txt
	
publish:
	( \
	echo $$(date +"// published %Y/%m/%d-%T -- ") $$(git rev-parse --short HEAD) ; \
	cat ca.c; \
	) > publish_me.c
	scp publish_me.c \
	  webcontent:/opt/very_public/www/projects.foxharp.net/software/ca.c.txt
	ssh webcontent web_publish projects
	echo Available here: https://projects.foxharp.net/software/ca.c.txt


	
