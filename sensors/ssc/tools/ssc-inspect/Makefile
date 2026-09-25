CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Werror $(shell pkg-config --cflags libssc libprotobuf-c)
LDLIBS += $(shell pkg-config --libs libssc libprotobuf-c)

.PHONY: all clean

all: ssc-inspect

ssc-inspect: ssc-inspect.c ssc-sensor-accelerometer.pb-c.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f ssc-inspect
