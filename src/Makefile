CXX := g++
CC := gcc
TARGET := MagnusChess

BUILD_PROFILE ?= auto
PGO ?= 0
MNUEV2_TELEMETRY ?= 0
BUILD_DIR := build
TARGET_TRIPLE := $(shell $(CXX) -dumpmachine)
X86_64_TARGET := $(filter x86_64% amd64%,$(TARGET_TRIPLE))

ifeq ($(X86_64_TARGET),)
  $(error MagnusChess 64-bit profiles require an x86_64 compiler target; got '$(TARGET_TRIPLE)')
endif

ifeq ($(BUILD_PROFILE),auto)
  EFFECTIVE_PROFILE := native64
else ifeq ($(BUILD_PROFILE),portable64)
  EFFECTIVE_PROFILE := portable64
else ifeq ($(BUILD_PROFILE),native64)
  EFFECTIVE_PROFILE := native64
else
  $(error Unknown BUILD_PROFILE '$(BUILD_PROFILE)'; use auto, portable64, or native64)
endif

ifneq ($(PGO),0)
  ifneq ($(PGO),1)
    $(error Unknown PGO value '$(PGO)'; use PGO=0 or PGO=1)
  endif
endif

ifneq ($(MNUEV2_TELEMETRY),0)
  ifneq ($(MNUEV2_TELEMETRY),1)
    $(error Unknown MNUEV2_TELEMETRY value '$(MNUEV2_TELEMETRY)'; use 0 or 1)
  endif
endif

OBJ_VARIANT ?= normal
OBJ_DIR := $(BUILD_DIR)/obj/$(EFFECTIVE_PROFILE)/$(OBJ_VARIANT)
PGO_DIR := $(BUILD_DIR)/pgo/$(EFFECTIVE_PROFILE)
PGO_BENCH_LOG := $(PGO_DIR)/bench.log
PGO_BENCH_ARGS ?= bench

SRC := \
	board/Attack.cpp \
	Bench.cpp \
	Evaluation.cpp \
	History.cpp \
	Lmr.cpp \
	Main.cpp \
	MovePicker.cpp \
	board/MoveGen.cpp \
	Nmp.cpp \
	Nnue.cpp \
	PerftMoveGen.cpp \
	Perft.cpp \
	PerftPosition.cpp \
	board/Position.cpp \
	Search.cpp \
	See.cpp \
	syzygy/Syzygy.cpp \
	Time.cpp \
	TT.cpp \
	Uci.cpp \
	mnue/Mnue.cpp \
	mnue/MnueX1Features.cpp \
	mnue/MnueX1Network.cpp \
	mnue/MnueV2Features.cpp \
	mnue/MnueV2Network.cpp \
	mnue/MnueV2Telemetry.cpp

FATHOM_DIR := ../third_party/fathom
FATHOM_OBJ := $(OBJ_DIR)/FathomTbProbe.o

# Embedded network object — compiled separately without LTO / PGO to keep
# the 21 MB incbin isolated from instrumentation and whole-program analysis.
MNUE_EMBEDDED_OBJ := $(OBJ_DIR)/mnue/MnueEmbedded.o

OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SRC)) $(FATHOM_OBJ) $(MNUE_EMBEDDED_OBJ)
DEP := $(OBJ:.o=.d)
MODULE_OBJ_DIRS := \
	$(OBJ_DIR)/board \
	$(OBJ_DIR)/mnue \
	$(OBJ_DIR)/syzygy
PERFT_CORE_OBJ := \
	$(OBJ_DIR)/PerftMoveGen.o \
	$(OBJ_DIR)/PerftPosition.o

PORTABLE64_FLAGS := \
	-m64 \
	-march=x86-64 \
	-mtune=generic

NATIVE64_FLAGS := \
	-m64 \
	-march=native \
	-mtune=native

ifeq ($(EFFECTIVE_PROFILE),portable64)
	ARCH_FLAGS := $(PORTABLE64_FLAGS)
	PROFILE_CPPFLAGS := -DMAGNUS_DISABLE_PEXT=1
	FATHOM_PROFILE_FLAGS := -DTB_NO_HW_POP_COUNT=1
else
	ARCH_FLAGS := $(NATIVE64_FLAGS)
	PROFILE_CPPFLAGS :=
	FATHOM_PROFILE_FLAGS :=
endif

OPT_FLAGS := \
	-Ofast \
	-DNDEBUG \
	-flto=auto \
	-fuse-linker-plugin \
	-fomit-frame-pointer \
	-funroll-loops \
	-fstrict-aliasing \
	-ffast-math \
	-fno-math-errno \
	-fno-signed-zeros \
	-fno-trapping-math \
	-fdevirtualize \
	-fdevirtualize-at-ltrans \
	-ftree-vectorize \
	-fipa-pta \
	-fipa-cp \
	-fipa-cp-clone \
	-fipa-icf \
	-fipa-ra \
	-falign-functions=32 \
	-falign-loops=32 \
	-falign-jumps=32 \
	-falign-labels=32 \
	-fweb \
	-fprefetch-loop-arrays \
	-frename-registers

LOWLEVEL_FLAGS := \
	-fno-exceptions \
	-fno-rtti

ENCODING_FLAGS := \
	-finput-charset=UTF-8 \
	-fexec-charset=UTF-8

WARN_FLAGS := \
	-Wall \
	-Wextra \
	-Wshadow \
	-Wconversion \
	-Wcast-qual \
	-Wno-unused-parameter \
	-Wno-sign-conversion

CPPFLAGS := -MMD -MP -iquote . -I$(FATHOM_DIR) $(PROFILE_CPPFLAGS) \
	-DMAGNUS_MNUEV2_TELEMETRY=$(MNUEV2_TELEMETRY)

BASE_CXXFLAGS := \
	-std=c++20 \
	$(ARCH_FLAGS) \
	$(OPT_FLAGS) \
	$(LOWLEVEL_FLAGS) \
	$(ENCODING_FLAGS) \
	$(WARN_FLAGS) \
	$(CPPFLAGS)

FATHOM_CFLAGS := \
	-std=gnu99 \
	$(ARCH_FLAGS) \
	-O3 \
	-DNDEBUG \
	$(FATHOM_PROFILE_FLAGS) \
	-I$(FATHOM_DIR)

LINK_OPT_LEVEL ?= 3

# Stack reserve size.
# 1 GiB = 1024^3 = 1073741824 bytes.
# Do not set this to tens of TiB: each created thread may reserve stack address space,
# which can make the executable fail at runtime or make std::thread throw std::system_error.
STACK_SIZE_BYTES ?= 1073741824

ifeq ($(OS),Windows_NT)
	# MinGW / GNU ld PE stack reserve.
	STACK_LDFLAGS := -Wl,--stack,$(STACK_SIZE_BYTES)
else
	# ELF GNU ld stack-size note.
	STACK_LDFLAGS := -Wl,-z,stack-size=$(STACK_SIZE_BYTES)
endif

BASE_LDFLAGS := \
	$(ARCH_FLAGS) \
	-Ofast \
	-flto=auto \
	-Wl,-O$(LINK_OPT_LEVEL) \
	$(STACK_LDFLAGS)

PGO_GENERATE_CXXFLAGS := -fprofile-generate=$(PGO_DIR)
PGO_GENERATE_LDFLAGS := -fprofile-generate=$(PGO_DIR)
PGO_USE_CXXFLAGS := \
	-fprofile-use=$(PGO_DIR) \
	-fprofile-correction \
	-fno-peel-loops \
	-fno-tracer
PGO_USE_LDFLAGS := \
	-fprofile-use=$(PGO_DIR) \
	-fprofile-correction

LDLIBS :=

ifeq ($(OS),Windows_NT)
	EXEEXT := .exe

	fixpath = $(subst /,\,$(1))

	MKDIR_P = if not exist "$(call fixpath,$(1))" mkdir "$(call fixpath,$(1))"
	RM_F    = if exist "$(call fixpath,$(1))" del /q "$(call fixpath,$(1))"
	RMDIR_R = if exist "$(call fixpath,$(1))" rmdir /s /q "$(call fixpath,$(1))"
else
	EXEEXT :=
	MKDIR_P = mkdir -p "$(1)"
	RM_F    = rm -f "$(1)"
	RMDIR_R = rm -rf "$(1)"
endif

ifeq ($(BUILD_PROFILE),auto)
	OUTPUT_BASENAME := $(TARGET)
else
	OUTPUT_BASENAME := $(TARGET)-$(EFFECTIVE_PROFILE)
endif

OUTPUT := $(BUILD_DIR)/$(OUTPUT_BASENAME)$(EXEEXT)
PROFGEN_OUTPUT := $(BUILD_DIR)/$(TARGET)-$(EFFECTIVE_PROFILE)-profgen$(EXEEXT)
PGO_OUTPUT := $(BUILD_DIR)/$(TARGET)-$(EFFECTIVE_PROFILE)-pgo$(EXEEXT)

ifeq ($(PGO),1)
	AUTO_BUILD_TARGET := profile-build
	FINAL_OUTPUT := $(PGO_OUTPUT)
else
	AUTO_BUILD_TARGET := build
	FINAL_OUTPUT := $(OUTPUT)
endif

ifeq ($(OS),Windows_NT)
	RUN_CMD = .\$(call fixpath,$(FINAL_OUTPUT))
	RUN_PROFGEN_CMD = .\$(call fixpath,$(PROFGEN_OUTPUT))
	RUN_PGO_CMD = .\$(call fixpath,$(PGO_OUTPUT))
	PGO_BENCH_LOG_CMD = $(call fixpath,$(PGO_BENCH_LOG))
else
	RUN_CMD = ./$(FINAL_OUTPUT)
	RUN_PROFGEN_CMD = ./$(PROFGEN_OUTPUT)
	RUN_PGO_CMD = ./$(PGO_OUTPUT)
	PGO_BENCH_LOG_CMD = $(PGO_BENCH_LOG)
endif

.PHONY: all auto portable64 native64 variants detect build release releases \
	pgo profile-build pgo-step1 pgo-step2 pgo-step3 \
	pgo-generate-build pgo-use-build run \
	clean objclean pgo-clean distclean

all: auto
auto: $(AUTO_BUILD_TARGET)

portable64:
	@$(MAKE) --no-print-directory BUILD_PROFILE=portable64 auto

native64:
	@$(MAKE) --no-print-directory BUILD_PROFILE=native64 auto

variants: portable64 native64

detect:
	@echo Compiler target : $(TARGET_TRIPLE)
	@echo Requested profile: $(BUILD_PROFILE)
	@echo Selected profile : $(EFFECTIVE_PROFILE)
	@echo PGO enabled      : $(PGO)
	@echo CPU flags        : $(ARCH_FLAGS)
	@echo Output           : $(FINAL_OUTPUT)

build: $(OUTPUT)
release: auto
releases: variants

$(OUTPUT): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(BASE_LDFLAGS) $(PGO_LDFLAGS) $(LDLIBS)

$(PROFGEN_OUTPUT): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(BASE_LDFLAGS) $(PGO_LDFLAGS) $(LDLIBS)

$(PGO_OUTPUT): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(BASE_LDFLAGS) $(PGO_LDFLAGS) $(LDLIBS)

$(OBJ): | $(MODULE_OBJ_DIRS)

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(BASE_CXXFLAGS) $(PGO_CXXFLAGS) -c $< -o $@

# Keep the large perft-only implementation opaque to whole-program LTO so
# adding or tuning it cannot repartition the search code during LTRANS.
$(PERFT_CORE_OBJ): $(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(BASE_CXXFLAGS) -fno-lto -c $< -o $@

$(FATHOM_OBJ): \
	$(FATHOM_DIR)/tbprobe.c \
	$(FATHOM_DIR)/tbprobe.h \
	$(FATHOM_DIR)/tbconfig.h \
	$(FATHOM_DIR)/tbchess.c \
	$(FATHOM_DIR)/stdendian.h | $(OBJ_DIR)
	$(CC) $(FATHOM_CFLAGS) -c $(FATHOM_DIR)/tbprobe.c -o $@

# === Embedded network (separate .S — no LTO, no PGO) ===
# The incbin source is a raw 21 MB MNUE file.  We compile it as a standalone
# assembly object so that -fprofile-generate, -flto, and whole-program analysis
# never touch this blob.
$(MNUE_EMBEDDED_OBJ): mnue/MnueEmbedded.S build/mm-b421bfeb0.MNUE | $(OBJ_DIR)/mnue
	$(CC) -x assembler-with-cpp -std=gnu99 -m64 -O2 \
		-I. \
		-c $< -o $@

$(OBJ_DIR)/mnue/Mnue.o: build/mm-b421bfeb0.MNUE

$(OBJ_DIR):
	$(call MKDIR_P,$@)

$(MODULE_OBJ_DIRS): | $(OBJ_DIR)
	$(call MKDIR_P,$@)

$(PGO_DIR):
	$(call MKDIR_P,$@)

pgo-generate-build: PGO_CXXFLAGS = $(PGO_GENERATE_CXXFLAGS)
pgo-generate-build: PGO_LDFLAGS = $(PGO_GENERATE_LDFLAGS)
pgo-generate-build: $(PROFGEN_OUTPUT)

pgo-use-build: PGO_CXXFLAGS = $(PGO_USE_CXXFLAGS)
pgo-use-build: PGO_LDFLAGS = $(PGO_USE_LDFLAGS)
pgo-use-build: $(PGO_OUTPUT)

pgo: profile-build

profile-build:
	@echo Step 1/3. Building instrumented executable ($(PROFGEN_OUTPUT)) ...
	@$(MAKE) --no-print-directory pgo-step1
	@echo Step 2/3. Running bench to collect profile data ...
	@$(MAKE) --no-print-directory pgo-step2
	@echo Step 3/3. Rebuilding optimized executable with PGO ($(PGO_OUTPUT)) ...
	@$(MAKE) --no-print-directory pgo-step3

pgo-step1:
	@$(MAKE) --no-print-directory OBJ_VARIANT=pgo objclean
	@$(MAKE) --no-print-directory pgo-clean
	$(call RM_F,$(PROFGEN_OUTPUT))
	$(call RM_F,$(PGO_OUTPUT))
	@$(MAKE) --no-print-directory OBJ_VARIANT=pgo pgo-generate-build

pgo-step2: | $(PGO_DIR)
	@$(MAKE) --no-print-directory OBJ_VARIANT=pgo pgo-generate-build
	$(RUN_PROFGEN_CMD) $(PGO_BENCH_ARGS) > "$(PGO_BENCH_LOG_CMD)" 2>&1
	@echo PGO bench log written to $(PGO_BENCH_LOG)

pgo-step3:
	@$(MAKE) --no-print-directory OBJ_VARIANT=pgo objclean
	$(call RM_F,$(PGO_OUTPUT))
	@$(MAKE) --no-print-directory OBJ_VARIANT=pgo pgo-use-build

.PHONY: run
run: all
	$(RUN_CMD)

objclean:
	$(call RMDIR_R,$(OBJ_DIR))

pgo-clean:
	$(call RMDIR_R,$(PGO_DIR))

clean:
	$(call RMDIR_R,$(BUILD_DIR)/obj)
	$(call RM_F,$(BUILD_DIR)/$(TARGET)$(EXEEXT))
	$(call RM_F,$(BUILD_DIR)/$(TARGET)-portable64$(EXEEXT))
	$(call RM_F,$(BUILD_DIR)/$(TARGET)-native64$(EXEEXT))
	$(call RM_F,$(BUILD_DIR)/$(TARGET)-portable64-profgen$(EXEEXT))
	$(call RM_F,$(BUILD_DIR)/$(TARGET)-native64-profgen$(EXEEXT))
	$(call RM_F,$(BUILD_DIR)/$(TARGET)-portable64-pgo$(EXEEXT))
	$(call RM_F,$(BUILD_DIR)/$(TARGET)-native64-pgo$(EXEEXT))

distclean: clean
	$(call RMDIR_R,$(BUILD_DIR)/pgo)

-include $(DEP)
