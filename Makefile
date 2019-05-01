CC = gcc
CFLAGS = -O2 -Wall -Wextra
LDFLAGS = -lssl -lcrypto -lresolv

# Detect platform and add necessary libraries
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S), Darwin)
    LDFLAGS += -lutil
endif

TARGET = krush
SRC = krush.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
