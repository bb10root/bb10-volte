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
SRCS_BIN  := ims_manager.c ims_carrier_config.c sys_utils.c berry_monitor.c
SRCS_BIN  := $(addprefix $(SRC_DIR)/, $(SRCS_BIN))
SRCS_SO   := $(SRC_DIR)/devnp-volte-swcrypto.c

# Object and Dependency files
OBJS_BIN  := $(SRCS_BIN:.c=.o)
OBJS_SO   := $(SRCS_SO:.c=.o)
DEPS      := $(OBJS_BIN:.o=.d) $(OBJS_SO:.o=.d)

# Base CFlags and LDFLags
CFLAGS    := -std=gnu99 -Isrc/include -MMD -MP
LDFLAGS   := -lxml2 -lpps -lslog2

# Build mode toggle: DEBUG=1 vs Release (default)
ifeq ($(DEBUG), 1)
    CFLAGS += -g -O0 -DDEBUG
    STRIP_CMD = @echo "[*] Debug mode enabled: Skipping strip for $@"
else
    CFLAGS += -O2 -DNDEBUG -fomit-frame-pointer
    STRIP_CMD = @echo "[*] Stripping $@..."; $(STRIP) $@
endif

.PHONY: all clean package help debug

all: package

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
package: $(TARGET_BIN) $(TARGET_SO)
	@echo "[*] Creating archive $(ZIP_NAME)..."
	@cd pkg && $(ZIP) -r -q ../$(ZIP_NAME) ./*
	@echo "[+] All done! Package created: $(ZIP_NAME)"

# Clean object files, dependency files, binaries, and archive
clean:
	@echo "[*] Cleaning build artifacts..."
	rm -f $(OBJS_BIN) $(OBJS_SO) $(DEPS) $(TARGET_BIN) $(TARGET_SO) $(ZIP_NAME)

help:
	@echo "Usage:"
	@echo "  make               - Build release binaries into pkg/init/ and create $(ZIP_NAME)"
	@echo "  make DEBUG=1       - Build debug binaries (-g -O0, no strip)"
	@echo "  make debug         - Alias for 'make DEBUG=1'"
	@echo "  make -j4           - Parallel build using 4 core workers"
	@echo "  make clean         - Remove object files, generated dependencies, and archive"