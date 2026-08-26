PREFIX ?= $(HOME)/.local
BUILD := build
CXXFLAGS := $(shell pkg-config --cflags gtk4 gio-2.0) -O2 -std=c++17
LDLIBS := $(shell pkg-config --libs gtk4 gio-2.0) -lm

all: $(BUILD)/extmon

$(BUILD)/extmon: src/extmon.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

install: all
	install -Dm755 $(BUILD)/extmon $(PREFIX)/bin/extmon
	install -Dm755 src/extmon.py $(PREFIX)/bin/extmon-cli
	install -Dm644 data/io.github.Aihuman.ExtMon.desktop \
		$(PREFIX)/share/applications/io.github.Aihuman.ExtMon.desktop
	install -Dm644 data/icons/extmon.svg \
		$(PREFIX)/share/icons/hicolor/scalable/apps/io.github.Aihuman.ExtMon.svg
	@for s in 16 32 48 64 128 256 512; do \
		install -Dm644 data/icons/hicolor/$${s}x$${s}/apps/io.github.Aihuman.ExtMon.png \
			$(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/io.github.Aihuman.ExtMon.png; \
	done

uninstall:
	rm -f $(PREFIX)/bin/extmon $(PREFIX)/bin/extmon-cli
	rm -f $(PREFIX)/share/applications/io.github.Aihuman.ExtMon.desktop
	rm -f $(PREFIX)/share/icons/hicolor/scalable/apps/io.github.Aihuman.ExtMon.svg
	@for s in 16 32 48 64 128 256 512; do \
		rm -f $(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/io.github.Aihuman.ExtMon.png; \
	done

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall clean
