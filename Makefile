# nasmount — conventional `make` / `make install` front end for the CMake build.
#
# `install`/`uninstall` delegate to install.sh/uninstall.sh rather than
# reimplementing them: those scripts gate installation on the test suite
# passing, detect and remove a previous org.kde.nasmount (transient-design)
# install, refresh Dolphin's and System Settings' KCM cache, and enable the
# session supervisor. A bare `cmake --install` would skip all of that.

BUILD_DIR := build
PREFIX ?= /usr
JOBS ?= $(shell nproc)

.PHONY: all build configure test install uninstall clean

all: build

configure:
	cmake -S . -B $(BUILD_DIR) \
	      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	      -DCMAKE_INSTALL_PREFIX=$(PREFIX) \
	      -DNASMOUNT_PACKAGE_FAMILY=source \
	      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# Not `sudo make install`: install.sh itself elevates only for the `cmake
# --install` step, and refuses that step as root (see the script). Run it
# plain; it will prompt for a password only when it actually needs one.
install:
	./install.sh

uninstall:
	./uninstall.sh

clean:
	rm -rf $(BUILD_DIR)
