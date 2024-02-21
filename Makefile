

all:
	doit ca.c

test:
	doit -test ca.c

newtest:
	doit -newtest ca.c
	
publish:
	( \
	echo $$(date +"// published %Y/%m/%d-%T -- ") $$(git rev-parse --short HEAD) ; \
	cat ca.c; \
	) > publish_me.c
	scp publish_me.c \
	  webcontent:/opt/very_public/www/projects.foxharp.net/software/ca.c.txt
	ssh webcontent web_publish projects


	
