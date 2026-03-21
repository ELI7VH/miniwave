CC = clang
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
COMMON = ../common

FRAMEWORKS = -framework CoreMIDI -framework CoreAudio -framework AudioToolbox \
             -framework CoreFoundation
LDFLAGS = $(FRAMEWORKS) -lm -lpthread

TARGET = miniwave
SRC = miniwave.c $(COMMON)/emu2413.c
COMMON_HDRS = $(COMMON)/osc.h $(COMMON)/instruments.h $(COMMON)/presets.h \
              $(COMMON)/fm-synth.h $(COMMON)/ym2413.h $(COMMON)/sub-synth.h \
              $(COMMON)/platform.h $(COMMON)/rack.h $(COMMON)/server.h \
              $(COMMON)/emu2413.h $(COMMON)/emutypes.h

.PHONY: all clean

all: $(TARGET)
	@ln -sf $(COMMON)/web web 2>/dev/null || true

$(TARGET): $(SRC) $(COMMON_HDRS)
	$(CC) $(CFLAGS) -I$(COMMON) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)
