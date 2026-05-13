# Makefile for ffmpeg_frame_viewer
#
# Dependencies (Ubuntu/Debian):
#   sudo apt install libavformat-dev libavcodec-dev libswscale-dev \
#                    libavutil-dev libsdl2-dev build-essential
#
# Build:    make
# Clean:    make clean
# Run:      ./ffmpeg_frame_viewer <video_file> [start_frame]

CC      = gcc
TARGET  = ffmpeg_frame_viewer
SRC     = ffmpeg_frame_viewer.c

CFLAGS  = -Wall -Wextra -O2 -g \
          $(shell pkg-config --cflags libavformat libavcodec libswscale libavutil sdl2)

LDFLAGS = $(shell pkg-config --libs   libavformat libavcodec libswscale libavutil sdl2) \
          -lm

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo ""
	@echo "Build OK → ./$(TARGET) <video_file> [start_frame]"

clean:
	rm -f $(TARGET)

# Quick test run – override VIDEO= on the command line
#   make run VIDEO=myvideo.mp4
VIDEO ?= test.mp4
run: $(TARGET)
	./$(TARGET) $(VIDEO)