# Default: build native on host
CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
CFLAGS  := -Wall -Werror -g -pthread


PROGRAM := aesdsocket
SOURCES := aesdsocket.c
OBJECTS := $(SOURCES:.c=.o)

all: $(PROGRAM)

$(PROGRAM): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) $(LDLIBS) -o $@

$(OBJECTS): %.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	@echo "Cleaning build files..."
	@rm -f $(PROGRAM) $(OBJECTS)

