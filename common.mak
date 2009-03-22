#
# common bits used by all libraries
#

all: # make "all" default target

ifndef SUBDIR
vpath %.c $(SRC_DIR)
vpath %.h $(SRC_DIR)
vpath %.S $(SRC_DIR)
vpath %.asm $(SRC_DIR)

ifeq ($(SRC_DIR),$(SRC_PATH_BARE))
BUILD_ROOT_REL = .
else
BUILD_ROOT_REL = ..
endif

ALLFFLIBS = avcodec avdevice avfilter avformat avutil postproc swscale

CFLAGS := -DHAVE_AV_CONFIG_H -I$(BUILD_ROOT_REL) -I$(SRC_PATH) $(OPTFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(LIBOBJFLAGS) -c -o $@ $<

%.o: %.S
	$(AS) $(CFLAGS) $(LIBOBJFLAGS) -c -o $@ $<

%.ho: %.h
	$(CC) $(CFLAGS) $(LIBOBJFLAGS) -Wno-unused -c -o $@ -x c $<

%.d: %.c
	$(DEPEND_CMD) > $@

%.d: %.S
	$(DEPEND_CMD) > $@

%.d: %.cpp
	$(DEPEND_CMD) > $@

%.o: %.d

%$(EXESUF): %.c

SVN_ENTRIES = $(SRC_PATH_BARE)/.svn/entries
ifeq ($(wildcard $(SVN_ENTRIES)),$(SVN_ENTRIES))
$(BUILD_ROOT_REL)/version.h: $(SVN_ENTRIES)
endif

$(BUILD_ROOT_REL)/version.h: $(SRC_PATH_BARE)/version.sh config.mak
	$< $(SRC_PATH) $@ $(EXTRA_VERSION)

install: install-libs install-headers

uninstall: uninstall-libs uninstall-headers

.PHONY: all depend dep *clean install* uninstall* examples tests
endif

CFLAGS   += $(CFLAGS-yes)
OBJS     += $(OBJS-yes)
FFLIBS   := $(FFLIBS-yes) $(FFLIBS)
TESTS    += $(TESTS-yes)

FFEXTRALIBS := $(addprefix -l,$(addsuffix $(BUILDSUF),$(FFLIBS))) $(EXTRALIBS)
FFLDFLAGS   := $(addprefix -L$(BUILD_ROOT)/lib,$(FFLIBS)) $(LDFLAGS)

EXAMPLES := $(addprefix $(SUBDIR),$(EXAMPLES))
OBJS  := $(addprefix $(SUBDIR),$(OBJS))
TESTS := $(addprefix $(SUBDIR),$(TESTS))

DEP_LIBS:=$(foreach NAME,$(FFLIBS),lib$(NAME)/$($(BUILD_SHARED:yes=S)LIBNAME))

ALLHEADERS := $(subst $(SRC_DIR)/,$(SUBDIR),$(wildcard $(SRC_DIR)/*.h $(SRC_DIR)/$(ARCH)/*.h))
checkheaders: $(filter-out %_template.ho,$(ALLHEADERS:.h=.ho))

DEPS := $(OBJS:.o=.d)
depend dep: $(DEPS)

CLEANSUFFIXES = *.o *~ *.ho
LIBSUFFIXES   = *.a *.lib *.so *.so.* *.dylib *.dll *.def *.dll.a *.exp *.map
DISTCLEANSUFFIXES = *.d *.pc

define RULES
$(SUBDIR)%$(EXESUF): $(SUBDIR)%.o
	$(CC) $(FFLDFLAGS) -o $$@ $$^ $(SUBDIR)$(LIBNAME) $(FFEXTRALIBS)

$(SUBDIR)%-test.o: $(SUBDIR)%.c
	$(CC) $(CFLAGS) -DTEST -c -o $$@ $$^

$(SUBDIR)%-test.o: $(SUBDIR)%-test.c
	$(CC) $(CFLAGS) -DTEST -c -o $$@ $$^

$(SUBDIR)x86/%.o: $(SUBDIR)x86/%.asm
	$(YASM) $(YASMFLAGS) -I $$(<D)/ -o $$@ $$<

$(SUBDIR)x86/%.d: $(SUBDIR)x86/%.asm
	$(YASM) $(YASMFLAGS) -I $$(<D)/ -M -o $$(@:%.d=%.o) $$< > $$@

clean::
	rm -f $(EXAMPLES) $(TESTS) $(addprefix $(SUBDIR),$(CLEANFILES) $(CLEANSUFFIXES) $(LIBSUFFIXES)) \
	    $(addprefix $(SUBDIR), $(foreach suffix,$(CLEANSUFFIXES),$(addsuffix /$(suffix),$(DIRS))))

distclean:: clean
	rm -f  $(addprefix $(SUBDIR),$(DISTCLEANSUFFIXES)) \
            $(addprefix $(SUBDIR), $(foreach suffix,$(DISTCLEANSUFFIXES),$(addsuffix /$(suffix),$(DIRS))))
endef

$(eval $(RULES))

examples: $(EXAMPLES)
tests: $(TESTS)

-include $(DEPS)
