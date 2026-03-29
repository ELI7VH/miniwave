CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
         -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard -DNO_JACK
COMMON = ../common

LDFLAGS = -lasound -lm -lpthread -lrt

TARGET = miniwave
UI = pocketwave-ui
SRC = miniwave.c $(COMMON)/emu2413.c
COMMON_HDRS = $(COMMON)/osc.h $(COMMON)/instruments.h $(COMMON)/presets.h \
              $(COMMON)/fm-synth.h $(COMMON)/ym2413.h $(COMMON)/sub-synth.h \
              $(COMMON)/platform.h $(COMMON)/rack.h $(COMMON)/server.h

.PHONY: all clean

all: $(TARGET) $(UI)
	@ln -sf $(COMMON)/web web 2>/dev/null || true

$(TARGET): $(SRC) $(COMMON_HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

$(UI): pocketwave-ui.c
	$(CC) $(CFLAGS) -o $@ $< -lasound

clean:
	rm -f $(TARGET) $(UI)
