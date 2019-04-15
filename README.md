# WASP
WASP is a platform to make it easy to run a web socket server.

The issue with any websocket server is it needs to be some program that sits there on the open connection sending and receiving messages as necessary. This is not so easy to script. The WASP system allows simple scripts in whatever language prefer, even shell scripts, to easily handle connections, and for events/actions to cause messages to be easily sent from scripts to the connected sessions.

This makes it easy to create websocket based applications.

The waspserver accepts connections, running a script locally for new connection, incoming message, and closing connection.
The waspserver can easily be run indirectly through apache

The wasp command allows messages to be sent to the connected sessions.

See manual for full details

(c) Copyright 2018 Andrews & Arnold Ltd, Adrian Kennard. See LICENSE file (GPL)
