#!/bin/bash

gcc -c -Wall -Werror -fpic roeistimer.c &&
gcc -shared -o libroeistimer.so roeistimer.o &&
cp libroeistimer.so /home/zhen/.local/lib/python2.7/site-packages/torch/lib/
#cp libroeistimer.so /usr/local/lib/python2.7/dist-packages/torch/lib/
mkdir -p ../build/lib/
mkdir -p ../build/lib.linux-x86_64-2.7/torch/lib/
mkdir -p ../torch/lib/
mkdir -p ../torch/lib/tmp_install/lib/
cp libroeistimer.so ../build/lib/
cp libroeistimer.so ../build/lib.linux-x86_64-2.7/torch/lib/
cp libroeistimer.so ../torch/lib/
cp libroeistimer.so ../torch/lib/tmp_install/lib/
cp roeistimer.h ..
