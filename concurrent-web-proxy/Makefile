# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -lpthread

# Executable and object files
TARGET = proxy
OBJS = proxy.o csapp.o

# Default rule: Build proxy
all: $(TARGET)

# Compile proxy.c
proxy.o: proxy.c csapp.h
	$(CC) $(CFLAGS) -c proxy.c -o proxy.o

# Compile csapp.c
csapp.o: csapp.c csapp.h
	$(CC) $(CFLAGS) -c csapp.c -o csapp.o

# Link everything together and create the 'proxy' executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

# Clean compiled files
clean:
	rm -f $(TARGET) $(OBJS)
