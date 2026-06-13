/*
 * bench.c - throughput generator for measuring t3's per-line overhead.
 *
 * Writes COUNT lines of WIDTH printable characters (plus a newline) to stdout
 * as fast as possible, one write(2) per line and no artificial delays. With
 * COUNT large the fixed startup costs (fork, worker handshake, the parent's
 * one-time message hold) amortize away, so
 *
 *     (time through t3 - time of bench alone) / COUNT
 *
 * approximates the marginal cost t3 adds per line.
 *
 * Usage: bench [count] [width]   (defaults: 1000000 lines, 64 chars)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_all(int fd, const char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("write");
      _exit(EXIT_FAILURE);
    }
    off += (size_t)n;
  }
}

// Parse a positive integer argument, rejecting non-numeric input, trailing
// garbage, and overflow (anything outside [1, max]). Exits on error.
static long parse_positive(const char *s, const char *name, long max) {
  errno = 0;
  char *end;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0' || errno == ERANGE || v < 1 || v > max) {
    fprintf(stderr, "bench: invalid %s '%s' (expected 1..%ld)\n", name, s, max);
    exit(EXIT_FAILURE);
  }
  return v;
}

int main(int argc, char *argv[]) {
  // count never drives an allocation; width sizes the per-line buffer, so cap
  // it well within int/size_t range to keep the snprintf and indexing below
  // safe.
  long count = (argc > 1) ? parse_positive(argv[1], "count", 1000000000L) : 1000000;
  long width = (argc > 2) ? parse_positive(argv[2], "width", 16 * 1024 * 1024)
                          : 64;

  // Build one reusable line: "<seq> " padded with 'x' to WIDTH, then '\n'.
  char *line = malloc((size_t)width + 2);
  if (!line) {
    perror("malloc");
    return EXIT_FAILURE;
  }
  for (long i = 0; i < count; i++) {
    int prefix = snprintf(line, (size_t)width + 1, "%ld ", i);
    if (prefix < 0) {
      fprintf(stderr, "bench: snprintf failed\n");
      return EXIT_FAILURE;
    }
    if (prefix > width) {
      prefix = width;
    }
    memset(line + prefix, 'x', (size_t)(width - prefix));
    line[width] = '\n';
    write_all(STDOUT_FILENO, line, (size_t)width + 1);
  }

  free(line);
  return EXIT_SUCCESS;
}
