all:
	gcc -o client ./xdg-private.c ./client.c -lwayland-client -lrt
