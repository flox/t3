/*
 * command that performs write system call to stdout then calls
 * flush then writes a second line with a newline.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>

#define hello(fd) fprintf(fd, "%s: Hello, %s! ", argv[1], fd == stdout ? "stdout" : "stderr")
#define goodbye(fd) fprintf(fd, "%s: Goodbye, %s!\n", argv[1], fd == stdout ? "stdout" : "stderr")

int main(int argc, char *argv[]) {
    hello(stdout);
    hello(stderr);
    fflush(stdout);
    fflush(stderr);
    goodbye(stderr);
    goodbye(stdout);
    fflush(stderr);
    usleep(10000); // Give t3 10ms to process and serialize the stderr line
                   // before stdout arrives; 10µs was too tight under Nix
                   // sandbox scheduling pressure.
    fflush(stdout);
    usleep(10000); // Give t3 10ms to finish writing stdout before the next
                   // iteration starts.
    return 0;
}
