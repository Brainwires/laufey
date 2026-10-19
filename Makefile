SHELL := /bin/bash

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
  HOST_OS := macos
else ifeq ($(UNAME_S),Linux)
  HOST_OS := linux
else
  HOST_OS := windows
endif

ifeq ($(filter arm64 aarch64,$(UNAME_M)),)
  HOST_ARCH := x86_64
else
  HOST_ARCH := aarch64
endif

ifeq ($(HOST_OS),macos)
  ifeq ($(HOST_ARCH),aarch64)
    CEF_ARCH := macosarm64
    PROJ_ARCH := arm64
  else
    CEF_ARCH := macosx64
    PROJ_ARCH := x86_64
  endif
else ifeq ($(HOST_OS),linux)
  ifeq ($(HOST_ARCH),aarch64)
    CEF_ARCH := linuxarm64
  else
    CEF_ARCH := linux64
  endif
  PROJ_ARCH := $(HOST_ARCH)
else ifeq ($(HOST_OS),windows)
  ifeq ($(HOST_ARCH),aarch64)
    CEF_ARCH := windowsarm64
  else
    CEF_ARCH := windows64
  endif
  PROJ_ARCH := $(HOST_ARCH)
endif

ifeq ($(HOST_OS),windows)
  LIB_EXT := .lib
else
  LIB_EXT := .a
endif

CEF_VERSION := 150.0.21
CEF_FULL_VERSION := 150.0.21+ga61e9a5+chromium-150.0.7871.255
CEF_URL_VERSION := $(subst +,%2B,$(CEF_FULL_VERSION))
CEF_DOWNLOAD_URL := https://cef-builds.spotifycdn.com/cef_binary_$(CEF_URL_VERSION)_$(CEF_ARCH)_minimal.tar.bz2
# SHA-256 of each platform's minimal distribution (cef/flake.nix pins the macOS
# ones too). Update them with CEF_FULL_VERSION.
CEF_SHA256_macosarm64 := c76962f68931a3a43bb0f98ec89bc50ee8861984cc33d1d1e16087ea6b2d44a5
CEF_SHA256_macosx64 := 91fd8e5fcacbc3270074d46e273a154e8aeb9d84e7a0a99e4a0c7f6374a8d457
CEF_SHA256_linuxarm64 := f6283c4c4bb59ebc0aeb55863e82e41dd0dd55a88bb4815d545b4982a1cf19fc
CEF_SHA256_linux64 := 3151a428c8f614ead64d764b7a11c6f2597606ff11742559ce1d51a1ff53b12d
CEF_SHA256_windowsarm64 := 5466f8216bead8a0dd4cdba3e879d55ea4ec35195f97b97eb395cf8616a80755
CEF_SHA256_windows64 := 2614d58de921f025b76f8c2970482cc4356278321a3712f579154738f9c06f51
CEF_SHA256 := $(CEF_SHA256_$(CEF_ARCH))
CEF_DIR := $(CURDIR)/vendor/cef/$(CEF_FULL_VERSION)/$(CEF_ARCH)
CEF_TARBALL := $(CEF_DIR)/cef-minimal.tar.bz2
CEF_SRC := $(CEF_DIR)/src
CEF_BUILD := $(CEF_DIR)/build
CEF_ROOT := $(CEF_DIR)/install

ifeq ($(HOST_OS),macos)
  BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
  LLVM_PREFIX := $(BREW_PREFIX)/opt/llvm
  export LIBCLANG_PATH := $(LLVM_PREFIX)/lib
endif

BUILD_DIR := $(CURDIR)/build

.PHONY: all clean check-deps help
.PHONY: runtimes hello-runtime ddcore-runtime
.PHONY: winit webview cef webauthn-linux
.PHONY: cef-deps
.PHONY: fmt fmt-check lint
.PHONY: native-e2e native-e2e-run

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'

all: runtimes winit webview cef ## Build all backends

check-deps: ## Check that required build tools are installed
	@echo "Checking build dependencies..."
	@command -v cargo >/dev/null || (echo "ERROR: cargo not found. Install Rust: https://rustup.rs" && exit 1)
	@command -v cmake >/dev/null || (echo "ERROR: cmake not found" && exit 1)
	@command -v ninja >/dev/null || (echo "ERROR: ninja not found" && exit 1)
ifeq ($(HOST_OS),macos)
	@test -d "$(LLVM_PREFIX)" || (echo "ERROR: llvm not found. Run: brew install llvm" && exit 1)
endif
	@echo "All base dependencies OK."

runtimes: hello-runtime ddcore-runtime ## Build all example runtimes

hello-runtime: check-deps ## Build hello_runtime
	cargo build --release -p hello_runtime

ddcore-runtime: check-deps ## Build ddcore_runtime
	cargo build --release -p ddcore_runtime

native-e2e: check-deps ## Build the native-chrome e2e runtime (+ Linux D-Bus driver)
	cargo build --release -p native_e2e_runtime
ifeq ($(HOST_OS),linux)
	cargo build --release -p native_e2e_driver
endif

RUNTIME_LIB := $(firstword $(wildcard target/release/libnative_e2e_runtime.dylib target/release/libnative_e2e_runtime.so target/release/native_e2e_runtime.dll))

native-e2e-run: native-e2e winit ## Run the e2e battery under the winit backend
	LAUFEY_RUNTIME_PATH="$(CURDIR)/$(RUNTIME_LIB)" ./target/release/laufey_winit

winit: check-deps ## Build the winit backend
	cd winit && cargo build --release

# Linux: liblaufey_webauthn.so (passkeys with security keys, through
# libwebauthn; webauthn-linux/), copied beside each backend's host, which
# dlopen()s it from there. It needs the libudev and libdbus-1 development
# files; without them it is skipped with a warning, and the backend reports
# no passkey capability (docs/passkeys.md).
WEBAUTHN_LIB := webauthn-linux/target/release/liblaufey_webauthn.so

webauthn-linux: ## Build liblaufey_webauthn.so (Linux; skipped without libudev / libdbus-1)
ifeq ($(HOST_OS),linux)
	@if pkg-config --exists libudev dbus-1; then \
		cargo build --release --locked --manifest-path webauthn-linux/Cargo.toml; \
	else \
		echo "warning: libudev / libdbus-1 development files missing: liblaufey_webauthn.so not built (no passkeys on Linux)" >&2; \
	fi
endif

# $(call install_webauthn,<dir>): the library beside a host, when built.
define install_webauthn
$(if $(filter linux,$(HOST_OS)),@if [ -f $(WEBAUTHN_LIB) ]; then cp $(WEBAUTHN_LIB) $(1)/; fi)
endef

webview: check-deps webauthn-linux ## Build the webview backend
	cmake -G Ninja -B webview/build -S webview -DCMAKE_BUILD_TYPE=Release \
		$(if $(WEBVIEW2_ROOT),-DWEBVIEW2_ROOT=$(WEBVIEW2_ROOT))
	ninja -C webview/build
	$(call install_webauthn,webview/build)

$(CEF_TARBALL):
	@echo "Downloading CEF $(CEF_VERSION) for $(CEF_ARCH)..."
	@mkdir -p $(CEF_DIR)
	curl -fL -o $(CEF_TARBALL).part "$(CEF_DOWNLOAD_URL)"
	@actual=$$( (command -v sha256sum >/dev/null && sha256sum $(CEF_TARBALL).part || shasum -a 256 $(CEF_TARBALL).part) | cut -d' ' -f1); \
	if [ "$$actual" != "$(CEF_SHA256)" ]; then \
		echo "CEF $(CEF_ARCH): SHA-256 $$actual, expected $(CEF_SHA256)" >&2; \
		rm -f $(CEF_TARBALL).part; exit 1; \
	fi
	mv $(CEF_TARBALL).part $(CEF_TARBALL)

$(CEF_SRC)/CMakeLists.txt: $(CEF_TARBALL)
	@echo "Extracting CEF..."
	@mkdir -p $(CEF_SRC)
	tar xf $(CEF_TARBALL) --strip-components=1 -C $(CEF_SRC)

$(CEF_ROOT)/libcef_dll_wrapper/libcef_dll_wrapper$(LIB_EXT): $(CEF_SRC)/CMakeLists.txt
	@echo "Building CEF dll_wrapper..."
	cmake -G Ninja -B $(CEF_BUILD) -S $(CEF_SRC) \
		-DCMAKE_BUILD_TYPE=Release \
		-DPROJECT_ARCH=$(PROJ_ARCH) \
		-DUSE_SANDBOX=OFF
	ninja -C $(CEF_BUILD) libcef_dll_wrapper
	@echo "Installing CEF..."
	@mkdir -p $(CEF_ROOT)/include $(CEF_ROOT)/cmake $(CEF_ROOT)/Release $(CEF_ROOT)/libcef_dll_wrapper
	cp -r $(CEF_SRC)/include/* $(CEF_ROOT)/include/
	cp -r $(CEF_SRC)/cmake/* $(CEF_ROOT)/cmake/
	cp -r $(CEF_SRC)/Release/* $(CEF_ROOT)/Release/
	@if [ -d "$(CEF_SRC)/Resources" ]; then \
		mkdir -p $(CEF_ROOT)/Resources && \
		cp -r $(CEF_SRC)/Resources/* $(CEF_ROOT)/Resources/; \
	fi
	cp $(CEF_BUILD)/libcef_dll_wrapper/libcef_dll_wrapper$(LIB_EXT) $(CEF_ROOT)/libcef_dll_wrapper/

cef-deps: $(CEF_ROOT)/libcef_dll_wrapper/libcef_dll_wrapper$(LIB_EXT) ## Download and build CEF dependencies

cef: check-deps cef-deps webauthn-linux ## Build the CEF backend
	cmake -G Ninja -B cef/build -S cef \
		-DCMAKE_BUILD_TYPE=Release \
		-DCEF_ROOT=$(CEF_ROOT) \
		-DPROJECT_ARCH=$(PROJ_ARCH)
	ninja -C cef/build
	$(call install_webauthn,cef/build/Release)

CPP_SOURCES := $(shell find capi cef/src webview/src -name '*.cc' -o -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.mm' 2>/dev/null)

fmt: ## Format all source files
	cargo fmt
	deno fmt
	@if command -v clang-format >/dev/null 2>&1 && [ -n "$(CPP_SOURCES)" ]; then \
		clang-format -i $(CPP_SOURCES); \
	fi

fmt-check: ## Check formatting without modifying files
	cargo fmt --check
	deno fmt --check
	@if command -v clang-format >/dev/null 2>&1 && [ -n "$(CPP_SOURCES)" ]; then \
		clang-format --dry-run --Werror $(CPP_SOURCES); \
	fi

lint: ## Run all linters
	cargo clippy --workspace -- -D warnings
	deno lint

clean: ## Remove all build artifacts
	cargo clean
	rm -rf webview/build cef/build webauthn-linux/target

clean-cef-vendor: ## Remove downloaded CEF files
	rm -rf vendor/cef
