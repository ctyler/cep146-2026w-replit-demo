CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -O2
LDFLAGS = -lncurses
TARGET  = timetracker
SRC     = timetracker.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o
