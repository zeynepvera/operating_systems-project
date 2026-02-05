CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -lrt
TARGET = procx

all: $(TARGET)

$(TARGET): procx.c
	$(CC) $(CFLAGS) -o $(TARGET) procx.c $(LDFLAGS)

clean:
	rm -f $(TARGET)
