wayland_header:
	wayland-scanner
all: wayland_header
	gcc -o client ./xdg-private.c ./client.c -lwayland-client -lrt
