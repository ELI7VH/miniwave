CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
COMMON = ../common

# Link against PipeWire's JACK library so we connect to PipeWire natively
PW_JACK_DIR = /usr/lib/x86_64-linux-gnu/pipewire-0.3/jack
ifneq ($(wildcard $(PW_JACK_DIR)/libjack.so),)
  JACK_LDFLAGS = -L$(PW_JACK_DIR) -Wl,-rpath,$(PW_JACK_DIR) -ljack
else
  JACK_LDFLAGS = -ljack
endif

LDFLAGS = -lasound $(JACK_LDFLAGS) -lm -lpthread -lrt

TARGET = miniwave
SRC = miniwave.c $(COMMON)/emu2413.c
COMMON_HDRS = $(COMMON)/osc.h $(COMMON)/instruments.h $(COMMON)/presets.h \
              $(COMMON)/fm-synth.h $(COMMON)/ym2413.h $(COMMON)/sub-synth.h \
              $(COMMON)/platform.h $(COMMON)/rack.h $(COMMON)/server.h \
              $(COMMON)/jack_backend.h

.PHONY: all clean

all: $(TARGET)
	@ln -sf $(COMMON)/web web 2>/dev/null || true

$(TARGET): $(SRC) $(COMMON_HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)
