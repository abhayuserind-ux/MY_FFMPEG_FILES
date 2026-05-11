#!/bin/bash

INC_PATH="/home/abhay-zstch1561/ffmpeg_8.1/ffmpeg/include"
LIB_PATH="/home/abhay-zstch1561/ffmpeg_8.1/ffmpeg/lib"

gcc -g \
    -I"$INC_PATH" "$1" -o "${1%.*}" \
    -L"$LIB_PATH" \
    -lavformat -lavcodec -lavfilter -lavutil -lswresample -lswscale -lm -lz \
    -Wl,-rpath,"$LIB_PATH"

if [ $? -eq 0 ]; then
    echo "--- Compilation Successful ---"
    ./"${1%.*}"
fi