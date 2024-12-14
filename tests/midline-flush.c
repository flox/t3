/*
 * command that performs write system call to stdout then calls
 * flush then writes a second line with a newline.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>

#define hello(fd) fprintf(fd, "Hello, %s! ", fd == stdout ? "stdout" : "stderr")
#define goodbye(fd) fprintf(fd, "Goodbye, %s!\n", fd == stdout ? "stdout" : "stderr")

int main() {
    hello(stdout);
    hello(stderr);
    fflush(stdout);
    fflush(stderr);
    goodbye(stderr);
    usleep(3); // It only takes a millisecond to give stderr a head start
               // but we'll give it three to avoid false positives where
	       // the first line is written to stdout.
    goodbye(stdout);
    fflush(stderr);
    fflush(stdout);
    return 0;
}