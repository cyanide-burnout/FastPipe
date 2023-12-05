ARCHITECTURE ?= $(shell $(CC) -dumpmachine | grep -o -E '^[^-]+')

EXECUTABLE   := test
DIRECTORIES  := ../Pipe
LIBRARIES    := pthread
DEPENDENCIES :=

OBJECTS := \
	../Pipe/FastPipe.o \
	Test.o

ifeq ($(ARCHITECTURE), x86_64)
	FLAGS += -fPIC
endif

ifeq ($(ARCHITECTURE), aarch64)
	FLAGS += -march=armv8-a+crc+crypto
endif

FLAGS += \
	-Wno-unused-result -Wno-format-truncation -Wno-format-overflow \
	-rdynamic -fno-omit-frame-pointer -O2 -MMD -gdwarf \
	$(foreach directory, $(DIRECTORIES), -I$(directory)) \
	$(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(DEPENDENCIES))

LIBS := \
	$(foreach library, $(LIBRARIES), -l$(library)) \
	$(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(DEPENDENCIES))

CFLAGS   += $(FLAGS) -std=gnu11
CXXFLAGS += $(FLAGS) -std=gnu++2a -fnon-call-exceptions

all: build

build: $(OBJECTS)
	$(CC) $(OBJECTS) $(FLAGS) $(LIBS) -o $(EXECUTABLE)

clean:
	rm -f $(OBJECTS) $(patsubst %.o,%.d,$(OBJECTS)) $(EXECUTABLE) $(EXECUTABLE).d

.PHONY: all build clean install package