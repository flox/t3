#!/bin/sh
./t3 "$1" -- sh -c 'for i in `seq 1 100`; do echo line $i to stdout; sleep 0; echo line $i to stderr 1>&2; sleep 0; done'
