all: waspserver wasp

update:
	git submodule update --remote --merge

AJL/ajl.o: AJL/ajl.c
	make -C AJL
websocket/websocket.o: websocket/websocket.c
	make -C websocket

waspserver: waspserver.c AJL/ajl.o websocket/websocket.o
	cc -D_GNU_SOURCE -g -Wall -Wextra -O -o waspserver waspserver.c -pthread -IAJL -Iwebsocket AJL/ajl.o websocket/websocket.o -lpopt -lcurl -lssl -lcrypto

wasp: wasp.c Makefile AJL/ajl.o
	cc -D_GNU_SOURCE -g -Wall -Wextra -O -o wasp wasp.c -IAJL AJL/ajl.o -lpopt -lcurl -lssl -lcrypto
