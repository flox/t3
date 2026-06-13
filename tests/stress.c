/*
 * stress.c - highly concurrent output generator for exercising t3.
 *
 * Spawns THREADS worker threads, each of which writes LINES lines to stdout
 * and LINES lines to stderr.  Every line is uniquely identified by its stream,
 * thread id, and sequence number:
 *
 *     OUT <tid> <seq>
 *     ERR <tid> <seq>
 *
 * Each line is emitted with a single write(2) call.  Because a pipe write of
 * no more than PIPE_BUF bytes is guaranteed atomic, the lines arriving at t3
 * are never interleaved at the byte level by the kernel.  Any dropped,
 * duplicated, truncated, or garbled line observed in t3's output is therefore
 * t3's own doing - which is exactly what tests/check-stream.awk checks for.
 *
 * Usage: stress [threads] [lines]   (defaults: 16 threads, 1000 lines)
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long lines_per_stream = 1000;

struct worker {
  long id;
};

// Emit one line with a single, atomic write. Lines are well under PIPE_BUF, so
// a pipe write must transfer all-or-nothing; this harness relies on that to
// keep concurrent threads' lines from interleaving at the byte level. A short
// write would break that guarantee, so treat it (like any write error) as a
// fatal harness fault rather than resuming and risking a false garble. A
// signal before any byte is written (EINTR) cannot interleave, so it is
// retried.
static void write_line(int fd, const char *buf, size_t len) {
  for (;;) {
    ssize_t written = write(fd, buf, len);
    if (written == (ssize_t)len) {
      return;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0) {
      perror("write");
    } else {
      fprintf(stderr, "stress: short write (%zd of %zu); atomicity violated\n",
              written, len);
    }
    _exit(EXIT_FAILURE);
  }
}

// Format one line and write it, failing fast if formatting errors or would
// truncate - either would corrupt the very output this harness exists to
// validate, so it must never pass silently.
static void emit(int fd, char *line, size_t cap, const char *fmt, long id,
                 long seq) {
  int len = snprintf(line, cap, fmt, id, seq);
  if (len < 0 || (size_t)len >= cap) {
    fprintf(stderr, "stress: snprintf failed or truncated\n");
    _exit(EXIT_FAILURE);
  }
  write_line(fd, line, (size_t)len);
}

static void *worker_main(void *arg) {
  const struct worker *w = arg;
  char line[64];
  for (long seq = 0; seq < lines_per_stream; seq++) {
    emit(STDOUT_FILENO, line, sizeof(line), "OUT %ld %ld\n", w->id, seq);
    emit(STDERR_FILENO, line, sizeof(line), "ERR %ld %ld\n", w->id, seq);
  }
  return NULL;
}

// Parse a positive integer argument, rejecting non-numeric input, trailing
// garbage, and overflow (anything outside [1, max]). Exits on error.
static long parse_positive(const char *s, const char *name, long max) {
  errno = 0;
  char *end;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0' || errno == ERANGE || v < 1 || v > max) {
    fprintf(stderr, "stress: invalid %s '%s' (expected 1..%ld)\n", name, s, max);
    exit(EXIT_FAILURE);
  }
  return v;
}

int main(int argc, char *argv[]) {
  // threads sizes the calloc()s below, so cap it; lines is only a loop bound.
  long threads = (argc > 1) ? parse_positive(argv[1], "threads", 65536) : 16;
  if (argc > 2) {
    lines_per_stream = parse_positive(argv[2], "lines", 1000000000L);
  }

  pthread_t *tids = calloc((size_t)threads, sizeof(*tids));
  struct worker *workers = calloc((size_t)threads, sizeof(*workers));
  if (!tids || !workers) {
    perror("calloc");
    return EXIT_FAILURE;
  }

  for (long i = 0; i < threads; i++) {
    workers[i].id = i;
    int rc = pthread_create(&tids[i], NULL, worker_main, &workers[i]);
    if (rc != 0) {
      fprintf(stderr, "pthread_create: %s\n", strerror(rc));
      return EXIT_FAILURE;
    }
  }
  for (long i = 0; i < threads; i++) {
    pthread_join(tids[i], NULL);
  }

  free(tids);
  free(workers);
  return EXIT_SUCCESS;
}
