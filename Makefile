all: waspserver wasp

update:
	git submodule update --remote --merge

AJL/ajl.o: AJL/ajl.c
	make -C AJL
AXL/axl.o: AXL/axl.c
	make -C AXL
websocket/websocket.o: websocket/websocket.c
	make -C websocket

waspserver: waspserver.c AXL/axl.o AJL/ajl.o websocket/websocket.o
	cc -D_GNU_SOURCE -g -Wall -Wextra -O -o waspserver waspserver.c -pthread -IAXL -IAJL -Iwebsocket AXL/axl.o AJL/ajl.o websocket/websocket.o -lpopt -lcurl -lssl -lcrypto

wasp: wasp.c Makefile AXL/axl.o
	cc -D_GNU_SOURCE -g -Wall -Wextra -O -o wasp wasp.c -IAXL AXL/axl.o -lpopt -lcurl -lssl -lcrypto
