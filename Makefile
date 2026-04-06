CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -Iinclude -Ivendor
LDFLAGS = -lcurl -lreadline

SRCS    = src/main.c src/llm.c src/router.c src/builtin.c src/exec.c \
          src/history.c src/safety.c src/json_helpers.c vendor/cJSON.c
OBJS    = $(SRCS:.c=.o)
TARGET  = llmsh

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
