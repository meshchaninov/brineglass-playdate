PRODUCT = Brineglass.pdx
SDK ?= $(if $(PLAYDATE_SDK_PATH),$(PLAYDATE_SDK_PATH),$(shell sed -n 's/^SDKRoot[[:space:]]*//p' ~/.Playdate/config))
ifeq ($(strip $(SDK)),)
SDK = $(HOME)/Developer/PlaydateSDK
endif
HEAP_SIZE = 8388208
STACK_SIZE = 61800
HOSTCC ?= cc
SCENE = src/world.c src/render.c src/stone.c build/coral_atlas.c
SRC = src/main.c $(SCENE)
UINCDIR = src
ULIBS = -lm
include $(SDK)/C_API/buildsupport/common.mk
CLANGFLAGS += -O3 -ffp-contract=fast -fno-math-errno
OPT = -O3 -ffast-math -falign-functions=16 -fomit-frame-pointer
$(OBJDIR)/pdex.$(DYLIB_EXT): src/garden.h src/stone.h src/wreck.h

build/bake_coral: tools/bake_coral.c src/coral.h | MKOBJDIR
	$(HOSTCC) -O2 -std=c11 -Isrc $< -lm -o $@
build/coral_atlas.c: build/bake_coral
	$< $@.tmp
	mv $@.tmp $@
$(OBJDIR)/pdex.$(DYLIB_EXT): src/coral.h

.PHONY: check-wreck
check-wreck: build/check_wreck
	$<

build/check_wreck: tests/wreck_clearance.c $(SCENE) src/garden.h src/wreck.h src/stone.h src/coral.h
	$(HOSTCC) -O3 -std=c11 -Isrc $< $(filter-out src/render.c,$(SCENE)) -lm -o $@

.PHONY: check-swimmers
check-swimmers: build/check_swimmers
	$<

build/check_swimmers: tests/swimmer_clearance.c $(SCENE) src/garden.h src/wreck.h src/stone.h src/coral.h
	$(HOSTCC) -O3 -std=c11 -Isrc $< $(filter-out src/render.c,$(SCENE)) -lm -o $@
