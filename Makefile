# Compiler and toolchain definitions
CC        := ntoarmv7-gcc
STRIP     := ntoarm-strip
ZIP       := zip

# Directory structure
SRC_DIR   := src
PKG_DIR   := pkg/init
ZIP_NAME  := volte.zip

# Output targets
TARGET_BIN := $(PKG_DIR)/ims_manager
TARGET_SO  := $(PKG_DIR)/devnp-volte-swcrypto.so

# Source files
SRCS_BIN  := ims_manager.c daemon_stats.c system_state.c carrier_apply.c ims_control.c ims_routes.c \
             ims_reconnect.c voice_flag.c pps_monitor.c self_test.c ims_patch.c \
             ims_carrier_config.c carrier_lookup.c carrier_profile.c \
             ims_carrier_db.c carrier_rows.c carrier_apns.c carrier_fetch.c \
             sys_utils.c log_utils.c file_utils.c proc_utils.c berry_monitor.c
SRCS_BIN  := $(addprefix $(SRC_DIR)/, $(SRCS_BIN))
SRCS_SO   := $(SRC_DIR)/devnp-volte-swcrypto.c

# Object and Dependency files
OBJS_BIN  := $(SRCS_BIN:.c=.o)
OBJS_SO   := $(SRCS_SO:.c=.o)
DEPS      := $(OBJS_BIN:.o=.d) $(OBJS_SO:.o=.d)

# Base CFlags and LDFLags
CFLAGS    := -std=gnu99 -MMD -MP
LDFLAGS   := -lxml2 -lcurl -lpps -lslog2 -lsocket -lz

# Build mode toggle: DEBUG=1 vs Release (default)
ifeq ($(DEBUG), 1)
    CFLAGS += -g -O0 -DDEBUG
    STRIP_CMD = @echo "[*] Debug mode enabled: Skipping strip for $@"
else
    CFLAGS += -O2 -DNDEBUG -fomit-frame-pointer
    STRIP_CMD = @echo "[*] Stripping $@..."; $(STRIP) $@
endif

.PHONY: all clean package help debug test

all: package

# Host-side unit tests for the portable helpers (native compiler + libxml2)
HOST_CC     ?= cc
HOST_XML2   ?= $(shell xml2-config --cflags --libs 2>/dev/null || echo -I/usr/include/libxml2 -lxml2)
TEST_BIN    := tests/test_carrier_config

test:
	$(HOST_CC) -std=gnu99 -Wall -Wextra -Isrc tests/test_carrier_config.c src/ims_carrier_config.c \
		$(HOST_XML2) -o $(TEST_BIN)
	./$(TEST_BIN)

# Helper target to invoke build with DEBUG=1
debug:
	@$(MAKE) DEBUG=1

# Ensure output directory exists
$(PKG_DIR):
	@mkdir -p $@

# Compile C source files into Object files and generate .d dependency files
%.o: %.c
	@echo "[*] Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Compile shared library object file with -fPIC
$(SRC_DIR)/devnp-volte-swcrypto.o: $(SRCS_SO)
	@echo "[*] Compiling $< (PIC)..."
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# Link and conditionally strip main executable
$(TARGET_BIN): $(OBJS_BIN) | $(PKG_DIR)
	@echo "[*] Linking $@..."
	$(CC) $^ -o $@ $(LDFLAGS)
	$(STRIP_CMD)

# Link and conditionally strip shared library
$(TARGET_SO): $(OBJS_SO) | $(PKG_DIR)
	@echo "[*] Linking shared library $@..."
	$(CC) -shared $^ -o $@
	$(STRIP_CMD)

# Include generated dependency files (.d)
-include $(DEPS)

# Pack everything inside pkg/ directory into archive
# The second copy is what an update installs while the old daemon is running; start.sh moves it in.
package: $(TARGET_BIN) $(TARGET_SO)
	cp $(TARGET_BIN) $(TARGET_BIN).new
	@echo "[*] Creating archive $(ZIP_NAME)..."
	@cd pkg && $(ZIP) -r -q ../$(ZIP_NAME) ./*
	@echo "[+] All done! Package created: $(ZIP_NAME)"

# Runs on the build host, not the phone: the carrier code has no QNX dependencies
test:
	@mkdir -p build
	cc -std=gnu99 -Isrc $$(xml2-config --cflags) tests/test_carrier.c \
		src/ims_carrier_config.c src/carrier_lookup.c src/carrier_profile.c \
		src/ims_carrier_db.c src/carrier_rows.c src/carrier_apns.c src/carrier_fetch.c \
		$$(xml2-config --libs) -lcurl -o build/test_carrier
	./build/test_carrier
	cc -std=gnu99 -Isrc tests/test_carrier_reload.c -o build/test_carrier_reload
	./build/test_carrier_reload
	cc -std=gnu99 -Isrc tests/test_radio_reset.c -o build/test_radio_reset
	./build/test_radio_reset
	cc -std=gnu99 -Isrc tests/test_stop_sender.c -o build/test_stop_sender
	./build/test_stop_sender
	cc -std=gnu99 -Isrc tests/test_routes.c -o build/test_routes
	./build/test_routes
	cc -std=gnu99 -Isrc tests/test_stop_signal.c -lpthread -o build/test_stop_signal
	./build/test_stop_signal
	cc -std=gnu99 -Isrc tests/test_ims_patch.c -lz -o build/test_ims_patch
	./build/test_ims_patch
	cc -std=gnu99 -Isrc -Itests/qnx tests/test_sim_carrier.c -o build/test_sim_carrier
	./build/test_sim_carrier
	sh tests/test_start.sh

# Clean object files, dependency files, binaries, and archive
clean:
	@echo "[*] Cleaning build artifacts..."
	rm -f $(OBJS_BIN) $(OBJS_SO) $(DEPS) $(TARGET_BIN) $(TARGET_SO) $(ZIP_NAME) $(TEST_BIN)

help:
	@echo "Usage:"
	@echo "  make               - Build release binaries into pkg/init/ and create $(ZIP_NAME)"
	@echo "  make DEBUG=1       - Build debug binaries (-g -O0, no strip)"
	@echo "  make debug         - Alias for 'make DEBUG=1'"
	@echo "  make -j4           - Parallel build using 4 core workers"
	@echo "  make clean         - Remove object files, generated dependencies, and archive"
	@echo "  make test          - Build and run host-side unit tests (native cc + libxml2)"
