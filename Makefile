all: waspserver wasp

update:
	git submodule update --remote --merge

AXL/axl.o: AXL/axl.c
	make -C AXL
websocket/websocketxml.o: websocket/websocket.c
	make -C websocket

waspserver: waspserver.c AXL/axl.o websocket/websocketxml.o
	cc -D_GNU_SOURCE -g -Wall -Wextra -O -o waspserver waspserver.c -pthread -IAXL -Iwebsocket AXL/axl.o websocket/websocketxml.o -lpopt -lcurl -lssl -lcrypto

wasp: wasp.c Makefile AXL/axl.o
	cc -D_GNU_SOURCE -g -Wall -Wextra -O -o wasp wasp.c -IAXL AXL/axl.o -lpopt -lcurl -lssl -lcrypto
