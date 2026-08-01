# Host-native tests for the engine-agnostic present backend.
#
# These do NOT cross-compile. The converters and the frame hash are pure
# functions, so testing them on the build machine is faster than a device round
# trip and catches the arithmetic errors that are invisible on a test pattern.
# Anything touching /dev/mem is verified on hardware instead.
CFLAGS ?= -std=c99 -O1 -Wall -Wextra -g
BACKEND := ../tools/mame-frontend/mister-backend

nv_present_test: nv_present_test.c $(BACKEND)/nv_present.c $(BACKEND)/nv_present.h
	$(CC) $(CFLAGS) -DNV_HOST_TEST=1 -o $@ nv_present_test.c $(BACKEND)/nv_present.c

.PHONY: run clean
run: nv_present_test
	./nv_present_test

clean:
	rm -f nv_present_test
