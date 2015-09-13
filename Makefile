include project/settings.mk
###############################################################################
##  SETTINGS                                                                 ##
###############################################################################

# Modules
COMMON 	:= $(COMMON)
MODULES := COMMON

# Override optimizations via: make O=n
O = 3

# Make-local Compiler Flags
CC_FLAGS = -std=gnu99 -g -Wall -fPIC -O$(O)
CC_FLAGS += -fno-common -fno-strict-aliasing -finline-functions
CC_FLAGS += -march=nocona -DMARCH_$(ARCH)
CC_FLAGS += -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_GNU_SOURCE $(EXT_CFLAGS)

PREPRO_SUFFIX = .cpp
ifeq ($(PREPRO),1)
  SUFFIX = $(PREPRO_SUFFIX)
  CC_FLAGS += -E -DPREPRO=$(PREPRO) -DGEN_TAG=$(GEN_TAG)"\
"
endif

# TODO (GeertJohan): Not sure what these are used for. Only Lua specific? Or would the Go module also benefit from this?
ifeq ($(OS),Darwin)
  CC_FLAGS += -D_DARWIN_UNLIMITED_SELECT
  CC_FLAGS += -DGO_DEBUG_HOOK
  GO_PLATFORM = macosx
else
  CC_FLAGS += -rdynamic
  GO_PLATFORM = linux
endif

ifneq ($(CF), )
  CC_FLAGS += -I$(CF)/include
endif

# Linker flags
LD_FLAGS = $(LDFLAGS)

ifeq ($(OS),Darwin)
  LD_FLAGS += -undefined dynamic_lookup
endif

# DEBUG Settings
ifdef DEBUG
  O = 0
  CC_FLAGS += -pg -fprofile-arcs -ftest-coverage -g2
  LD_FLAGS += -pg -fprofile-arcs -lgcov
endif

# Make-tree Compiler Flags
CFLAGS = -O$(O)

# Make-tree Linker Flags
# LDFLAGS =

# Make-tree Linker Flags
# LDFLAGS =

# Include Paths
INC_PATH += $(COMMON)/$(TARGET_INCL)

# Library Paths
# LIB_PATH +=

###############################################################################
##  OBJECTS                                                                  ##
###############################################################################

MOD_GO =
MOD_GO += mod_go.o

###############################################################################
##  MAIN TARGETS                                                             ##
###############################################################################

all: build prepare

.PHONY: prepare
prepare: $(TARGET_INCL)/aerospike/*.h

.PHONY: build
build: libmod_go

.PHONY: build-clean
build-clean:
	@rm -rf $(TARGET_BIN)
	@rm -rf $(TARGET_LIB)

.PHONY: libmod_go libmod_go.a libmod_go.$(DYNAMIC_SUFFIX)
libmod_go: libmod_go.a libmod_go.$(DYNAMIC_SUFFIX)
libmod_go.a: $(TARGET_LIB)/libmod_go.a
libmod_go.$(DYNAMIC_SUFFIX): $(TARGET_LIB)/libmod_go.$(DYNAMIC_SUFFIX)

###############################################################################
##  BUILD TARGETS                                                            ##
###############################################################################

$(TARGET_OBJ)/%.o: $(SOURCE_MAIN)/%.c | modules-prepare
	$(object)

$(TARGET_LIB)/libmod_go.a $(TARGET_LIB)/libmod_go.$(DYNAMIC_SUFFIX): $(MOD_GO:%=$(TARGET_OBJ)/%) | $(COMMON)/$(TARGET_INCL)/aerospike

$(TARGET_INCL)/aerospike: | $(TARGET_INCL)
	mkdir $@

$(TARGET_INCL)/aerospike/%.h:: $(SOURCE_INCL)/aerospike/%.h | $(TARGET_INCL)/aerospike
	cp -p $^ $(TARGET_INCL)/aerospike

# .PHONY: test
# test:
# 	@echo "No tests"

###############################################################################
include project/modules.mk project/test.mk project/rules.mk
