all:
	cargo build --release
	cp target/release/wayland-life client
