#Makefile for Offline Judge System
CC = gcc
CFLAGS = -Wall
LIBS = -lpthread -lseccomp
TARGET = judge
SRCS = judge.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) -o $(TARGET) $(SRCS) $(LIBS) $(CFLAGS)

clean:
	rm -f $(TARGET) temp_bin compile_error.log