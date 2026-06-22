CC      = gcc
CFLAGS  = -O2 -Wall -Wextra $(shell pkg-config --cflags gtk+-3.0 json-c)
LDFLAGS = $(shell pkg-config --libs gtk+-3.0 json-c) -lcurl
TARGET  = cxm-leads

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
