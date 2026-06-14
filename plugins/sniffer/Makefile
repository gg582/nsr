TARGET = sniffer

all: $(TARGET)

$(TARGET): src/main.rs Cargo.toml
	cargo build --release
	cp target/release/$(TARGET) .

install: $(TARGET)
	@mkdir -p $(HOME)/.nsr/plugins
	@rm -f $(HOME)/.nsr/plugins/*.so
	cp $(TARGET) $(HOME)/.nsr/plugins/

clean:
	cargo clean
	rm -f $(TARGET) *.so

.PHONY: all install clean

