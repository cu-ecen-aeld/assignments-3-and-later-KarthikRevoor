CROSS_COMPILE ?= aarch64-linux-gnu-
CC      := $(CROSS_COMPILE)gcc
CFLAGS  := -Wall -Werror -g -pthread
LDLIBS  :=

PROGRAM := aesdsocket
SOURCES := aesdsocket.c
OBJECTS := $(SOURCES:.c=.o)

all: $(PROGRAM)

$(PROGRAM): $(OBJECTS)
	$(CC) $(OBJECTS) $(CFLAGS) $(LDLIBS) -o $@

$(OBJECTS): %.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	@echo "Cleaning build files..."
	@rm -f $(PROGRAM) $(OBJECTS)

