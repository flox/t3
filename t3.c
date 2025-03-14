/*
 * t3.c
 *
 * Next generation tee with colorized output streams and precise time stamping.
 *
 * Author:  Michael Brantley, Flox
 *
 * The `t3` command parses the stdout and stderr streams of a command,
 * writing colorized precisely time-stamped versions of both streams to
 * the calling process's own stdout and stderr streams, as well as to the
 * provided filename.  In that respect `tee` is to `t3` what Perl's IPC::Open2()
 * function is to IPC::Open3(), preserving distinct handles for each of the
 * stdout and stderr streams.
 *
 * It works by creating pipes for parsing the stdout and stderr streams
 * before invoking the provided command with its output redirected to these
 * pipes.  It then forks independent processes that work in parallel to
 * timestamp the lines of output coming from both streams while the parent
 * process reassembles and writes colorized and timestamped renditions both
 * to the provided filename and to its own stdout and stderr streams.
 */

/*
 * Include <TargetConditionals.h> to address error:
 *   'TARGET_OS_IPHONE' is not defined
 * https://developer.apple.com/documentation/xcode/identifying-and-addressing-framework-module-issues
 */
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Size of the chunk a worker reads from the command at a time, and the
// initial capacity of its line-assembly buffer. Lines longer than this are
// handled by growing the buffer (see MAX_LINE_SIZE), not by splitting.
// https://stackoverflow.com/questions/3552095/sensible-line-buffer-size-in-c
#define BUFFER_SIZE 4096

// Upper bound on a single reassembled line. A worker's line buffer grows as
// needed up to this cap; a line longer than the cap is forwarded in cap-sized
// pieces so that pathological input (e.g. a command emitting megabytes with no
// newline) cannot drive unbounded memory growth.
#define MAX_LINE_SIZE (16 * 1024 * 1024)

// How long (in milliseconds) the parent holds a line in its queue before
// emitting it.  This grace period lets a slightly-later line from the *other*
// stream arrive so the two streams can be interleaved in timestamp order
// rather than purely in arrival order.
#define MESSAGE_HOLD_MS 100

// How long (in milliseconds) the parent blocks in poll() waiting for the next
// message from either worker before looping to flush any aged-out messages.
#define POLL_TIMEOUT_MS 1000

// A few ANSI color codes, see https://materialui.co/colors
#define ANSI_COLOR_RESET "\x1b[0m"
#define ANSI_COLOR_BOLD "\x1b[1m"
#define ANSI_COLOR_BLACK "\x1b[30m"
#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_BLUE "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN "\x1b[36m"
#define ANSI_COLOR_WHITE "\x1b[37m"

// Global variables
int color_to_tty = 1;
int debuglevel = 0;
int timestamp_enabled = 0;
int relative_timestamps = 0;
const char *ts_color = ANSI_COLOR_CYAN; // Timestamp color
const char *reset_color = ANSI_COLOR_RESET;
struct timespec start_timestamp;

// Diagnostic macros. Each expands to a single statement (wrapped in
// do/while(0)) so it behaves correctly when used as the body of an
// unbraced if/else.
#define _debug(dlevel, format, ...)                                            \
  do {                                                                         \
    if (debuglevel && debuglevel >= (dlevel))                                  \
      fprintf(stderr,                                                          \
              ANSI_COLOR_GREEN "DEBUG[%d]: " ANSI_COLOR_RESET format "\n",     \
              getpid(), ##__VA_ARGS__);                                        \
  } while (0)
#define _warn(format, ...)                                                     \
  fprintf(stderr,                                                              \
          ANSI_COLOR_YELLOW "WARNING[%d]: " ANSI_COLOR_RESET format "\n",      \
          getpid(), ##__VA_ARGS__)
#define _error(format, ...)                                                    \
  fprintf(stderr, ANSI_COLOR_RED "ERROR[%d]: " ANSI_COLOR_RESET format "\n",   \
          getpid(), ##__VA_ARGS__)

// Wire format of one message on a worker's message pipe: a fixed header
// followed immediately by `length` bytes of line text (no trailing NUL). Each
// message pipe has a single writer (its worker), so frames never interleave;
// write_full()/read_full() keep them aligned across partial transfers.
struct msg_header {
  struct timespec timestamp;
  uint32_t length;
};

// Largest legitimate frame on the wire: a full header plus a maximally long
// line. A worker never sends more than this, so the parent's read buffer
// never needs to grow beyond it.
#define MAX_FRAME_SIZE (sizeof(struct msg_header) + MAX_LINE_SIZE)

// In-memory message held on the parent's queues. The text is stored inline as
// a flexible array member sized to the actual line length (plus a NUL), so
// short lines no longer pay for a fixed multi-kilobyte buffer.
struct payload {
  struct timespec timestamp;
  uint32_t length;
  char text[];
};

struct message {
  struct payload *msg_payload;
  struct message *next;
};

// Head and tail pointers for the linked list
struct message *stdout_head = NULL;
struct message *stdout_tail = NULL;
int stdout_queuelen = 0;
struct message *stderr_head = NULL;
struct message *stderr_tail = NULL;
int stderr_queuelen = 0;

// malloc() that aborts on failure. Allocations here are small and frequent;
// there is nothing useful the program can do if they fail.
static void *xmalloc(size_t size) {
  void *ptr = malloc(size);
  if (!ptr) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  return ptr;
}

// realloc() that aborts on failure, with the same rationale as xmalloc().
static void *xrealloc(void *ptr, size_t size) {
  void *new_ptr = realloc(ptr, size);
  if (!new_ptr) {
    perror("realloc");
    exit(EXIT_FAILURE);
  }
  return new_ptr;
}

// Write exactly `count` bytes from `buf` to `fd`, resuming after partial
// writes and retrying when interrupted by a signal. Returns 0 on success or
// -1 on error (with errno set). A pipe write of more than PIPE_BUF bytes is
// not guaranteed to be atomic, so callers must never assume a single write()
// transfers a whole message.
static int write_full(int fd, const void *buf, size_t count) {
  const char *cursor = buf;
  while (count > 0) {
    ssize_t written = write(fd, cursor, count);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    cursor += written;
    count -= (size_t)written;
  }
  return 0;
}

// Read exactly `count` bytes from `fd` into `buf`, resuming after partial
// reads and retrying when interrupted by a signal. Returns 1 on success,
// 0 on a clean end-of-file that falls on a message boundary (nothing read),
// or -1 on error or a truncated message (errno is set to 0 to distinguish a
// short read at EOF from a genuine read() error).
static int read_full(int fd, void *buf, size_t count) {
  char *cursor = buf;
  size_t remaining = count;
  while (remaining > 0) {
    ssize_t bytes_read = read(fd, cursor, remaining);
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (bytes_read == 0) {
      if (remaining == count) {
        return 0; // EOF exactly on a message boundary
      }
      errno = 0; // truncated message: distinguish from a read() error
      return -1;
    }
    cursor += bytes_read;
    remaining -= (size_t)bytes_read;
  }
  return 1;
}

static void usage(const int rc) {
  printf("Usage: t3 [OPTION] FILE -- COMMAND ARGS ...\n");
  printf("Invoke provided command and write its colorized, "
         "precise time-stamped output both to the provided file "
         "and to stdout/err.\n\n");
  printf("  -l, --light       "
         "use color scheme suitable for light backgrounds\n");
  printf("  -d, --dark        "
         "use color scheme suitable for dark backgrounds\n");
  printf("  -b, --bold        "
         "highlight stderr in bold text (with no color)\n");
  printf("  -p, --plain       "
         "disable all timestamps, ANSI color and highlighting\n");
  printf("  -f, --forcecolor  "
         "enforce the use of color when not writing to a TTY\n");
  printf("  -o, --outcolor C  "
         "set the ANSI escape sequence used to color stdout\n");
  printf("  -e, --errcolor C  "
         "set the ANSI escape sequence used to color stderr\n");
  printf("  -t, --ts          "
         "enable timestamps in all outputs\n");
  printf("  -r, --relative    "
         "display timestamps as relative offsets from start time "
         "(implies --ts)\n");
  printf("  -h, --help        print this help message\n");
  printf("  -v, --version     print version string\n");
  printf("  --debug           enable debugging\n");
  exit(rc);
}

// Send one variable-length frame to the parent. `buf` has room for a
// struct msg_header reserved at the front (filled in here) followed by `len`
// bytes of line text, so the whole frame goes out in a single write_full()
// call - one syscall and no extra copy. Each worker owns its message pipe, so
// the single writer keeps frames from interleaving; write_full() keeps them
// aligned across partial writes. A write error means the parent has gone
// away, so there is nothing left to do but exit.
void send_line(int pipe_fd, char *buf, size_t len,
               const struct timespec *timestamp) {
  struct msg_header header;
  // Zero the whole struct first so its padding bytes are not sent as
  // uninitialized stack memory over the pipe.
  memset(&header, 0, sizeof(header));
  header.timestamp = *timestamp;
  header.length = (uint32_t)len;
  memcpy(buf, &header, sizeof(header));
  _debug(1, "Sending %zu-byte line to parent process, timestamp: %ld.%09ld",
         len, timestamp->tv_sec, timestamp->tv_nsec);
  if (write_full(pipe_fd, buf, sizeof(header) + len) == -1) {
    perror("Error writing message to pipe");
    exit(EXIT_FAILURE);
  }
}

// Worker process body: read the raw output of the command from `fd`, split it
// into lines, stamp each completed line with the time it was read, and forward
// it to the parent over the message pipe `pipe_fd`. The message pipe is left in
// its default blocking mode: if the parent falls behind, write_full() blocks
// here, which in turn applies natural back-pressure to the command rather than
// dropping or corrupting messages.
void timestamp_and_send(int pipe_fd, int fd, const char *prefix) {
  char buffer[BUFFER_SIZE];
  ssize_t bytes_read;

  // TODO: set argv[0] to incorporate prefix

  // Line-assembly buffer: a struct msg_header is reserved at the front and the
  // line text accumulates after it, so a completed line is sent with a single
  // write (see send_line). The buffer grows as needed up to MAX_FRAME_SIZE,
  // i.e. enough to hold the header plus a full MAX_LINE_SIZE bytes of text.
  const size_t header_size = sizeof(struct msg_header);
  size_t capacity = BUFFER_SIZE;
  char *line = xmalloc(capacity);
  size_t line_length = 0; // text bytes accumulated (excludes the header)
  struct timespec timestamp = {0, 0};

  // Send a zero-timestamped "<prefix> started" frame so the parent can confirm
  // the worker is online and the message pipe is wired up correctly.
  int started = snprintf(line + header_size, capacity - header_size,
                         "%s started", prefix);
  if (started < 0 || (size_t)started >= capacity - header_size) {
    _error("Message truncated in timestamp_and_send");
    exit(EXIT_FAILURE);
  }
  send_line(pipe_fd, line, (size_t)started, &timestamp);

  while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
    // Get the current time with nanosecond precision. Note that if a
    // line is split across multiple reads, the timestamp will be set
    // to the time that the _last_ read is completed.
    if (clock_gettime(CLOCK_REALTIME, &timestamp) == -1) {
      perror("clock_gettime");
      exit(EXIT_FAILURE);
    }

    for (ssize_t i = 0; i < bytes_read; i++) {
      if (buffer[i] == '\n') {
        send_line(pipe_fd, line, line_length, &timestamp);
        line_length = 0; // Reset for the next line
        continue;
      }
      // Ensure room for one more text byte, growing the buffer up to
      // MAX_FRAME_SIZE (header + MAX_LINE_SIZE bytes of text). A line whose
      // text reaches the MAX_LINE_SIZE cap is flushed in pieces rather than
      // truncated.
      if (header_size + line_length + 1 > capacity) {
        if (capacity >= MAX_FRAME_SIZE) {
          send_line(pipe_fd, line, line_length, &timestamp);
          line_length = 0;
        } else {
          capacity *= 2;
          if (capacity > MAX_FRAME_SIZE) {
            capacity = MAX_FRAME_SIZE;
          }
          line = xrealloc(line, capacity);
        }
      }
      line[header_size + line_length++] = buffer[i];
    }
  }

  if (bytes_read < 0) {
    fprintf(stderr, "Error reading file descriptor: %s\n", strerror(errno));
  }

  // Handle any remaining data in the buffer that doesn't end with a newline
  if (line_length > 0) {
    send_line(pipe_fd, line, line_length, &timestamp);
  }

  free(line);
}

int timespec_cmp(const struct timespec *a, const struct timespec *b) {
  if (a->tv_sec < b->tv_sec)
    return -1;
  if (a->tv_sec > b->tv_sec)
    return 1;
  if (a->tv_nsec < b->tv_nsec)
    return -1;
  if (a->tv_nsec > b->tv_nsec)
    return 1;
  return 0;
}

long timespec_ms_delta(const struct timespec *a, const struct timespec *b) {
  return (a->tv_sec - b->tv_sec) * 1000 + (a->tv_nsec - b->tv_nsec) / 1000000;
}

// Function to add a message to the end of a queue
void push(struct message **head, struct message **tail, struct message *msg,
          int *queuelen) {
  msg->next = NULL;
  if (*tail == NULL) {
    *tail = *head = msg;
  } else {
    (*tail)->next = msg;
    *tail = msg;
  }
  (*queuelen)++;
}

// Function to remove the message from the front of the queue
void shift(struct message **head, struct message **tail, int *queuelen) {
  if (*head == NULL) {
    return;
  }
  // Grab a pointer to the message to be deleted
  struct message *msg_to_free = *head;
  // Process the removal of the head message
  *head = (*head)->next;
  if (*head == NULL) {
    *tail = NULL;
  }
  (*queuelen)--;
  // Free all memory associated with the deleted message
  free(msg_to_free->msg_payload);
  free(msg_to_free);
}

// Read and validate a worker's startup handshake: a zero-timestamped
// "<prefix> started" frame. Returns 0 on success, -1 on any I/O error or
// mismatch (a diagnostic is printed in the mismatch case).
int await_worker(int fd, const char *prefix) {
  struct msg_header header;
  char text[64];
  char expected[64];

  int rc = read_full(fd, &header, sizeof(header));
  if (rc != 1) {
    // read_full signals a real read error with errno set, and a clean EOF or a
    // truncated frame with errno == 0; only perror() in the former case so we
    // never print a misleading "Success".
    if (rc < 0 && errno != 0) {
      perror("Error reading worker handshake");
    } else {
      fprintf(stderr, "Error: %s worker closed before completing handshake\n",
              prefix);
    }
    return -1;
  }
  if (header.length >= sizeof(text) ||
      read_full(fd, text, header.length) != 1) {
    fprintf(stderr, "Error: malformed handshake from %s worker\n", prefix);
    return -1;
  }
  text[header.length] = '\0';
  snprintf(expected, sizeof(expected), "%s started", prefix);
  if (header.timestamp.tv_sec != 0 || header.timestamp.tv_nsec != 0 ||
      strcmp(text, expected) != 0) {
    fprintf(stderr, "Error: Unexpected message from %s worker: %s\n", prefix,
            text);
    return -1;
  }
  return 0;
}

// A buffered reader over a worker's message pipe. Rather than issuing a
// separate read() for each frame's header and body, it pulls a large chunk
// per syscall into `buf` and parses as many whole frames as that chunk
// contains, so the per-line syscall cost is amortized across many lines.
// Unconsumed bytes live in buf[start:end]; a partial frame is simply carried
// over to the next read.
struct framereader {
  int fd;
  char *buf;
  size_t cap;
  size_t start; // offset of the first unconsumed byte
  size_t end;   // offset just past the last valid byte
};

#define FRAMEREADER_INITIAL_CAP (64 * 1024)

void framereader_init(struct framereader *fr, int fd) {
  fr->fd = fd;
  fr->cap = FRAMEREADER_INITIAL_CAP;
  fr->buf = xmalloc(fr->cap);
  fr->start = 0;
  fr->end = 0;
}

void framereader_free(struct framereader *fr) {
  free(fr->buf);
  fr->buf = NULL;
}

// Bytes received but not yet consumed as a whole frame. A nonzero value once
// the pipe has reached EOF means the worker stopped mid-frame.
size_t framereader_pending(const struct framereader *fr) {
  return fr->end - fr->start;
}

// Issue a single read() into the buffer, making room first by compacting
// consumed bytes and, if a frame larger than the buffer is being assembled,
// growing it. Returns 1 if bytes were read, 0 at end-of-file, -1 on error
// (errno set). Reads exactly once so it never blocks after a POLLIN.
int framereader_fill(struct framereader *fr) {
  if (fr->start > 0) {
    // Reclaim space consumed from the front.
    size_t remaining = fr->end - fr->start;
    memmove(fr->buf, fr->buf + fr->start, remaining);
    fr->start = 0;
    fr->end = remaining;
  }
  if (fr->end == fr->cap) {
    // Buffer full of one not-yet-complete frame: double it to make room, but
    // never past MAX_FRAME_SIZE. A complete frame always fits within that
    // bound (framereader_next() rejects any frame claiming a larger body), so
    // a buffer that is full at the cap means the stream is corrupt.
    if (fr->cap >= MAX_FRAME_SIZE) {
      fprintf(stderr, "Error: frame exceeds maximum size %zu; aborting\n",
              (size_t)MAX_FRAME_SIZE);
      exit(EXIT_FAILURE);
    }
    fr->cap *= 2;
    if (fr->cap > MAX_FRAME_SIZE) {
      fr->cap = MAX_FRAME_SIZE;
    }
    fr->buf = xrealloc(fr->buf, fr->cap);
  }
  ssize_t n;
  do {
    n = read(fr->fd, fr->buf + fr->end, fr->cap - fr->end);
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    return -1;
  }
  if (n == 0) {
    return 0;
  }
  fr->end += (size_t)n;
  return 1;
}

// Parse the next whole frame out of the buffer, if one is fully present.
// Returns a freshly allocated payload (caller frees) or NULL when more bytes
// are needed. memcpy is used for the header because buffered bytes are not
// suitably aligned for a struct access.
struct payload *framereader_next(struct framereader *fr) {
  size_t available = fr->end - fr->start;
  if (available < sizeof(struct msg_header)) {
    return NULL;
  }
  struct msg_header header;
  memcpy(&header, fr->buf + fr->start, sizeof(header));
  if (header.length > MAX_LINE_SIZE) {
    // No worker ever sends a frame larger than MAX_LINE_SIZE, so a length
    // beyond it means the frame stream has desynchronized or been corrupted.
    // Fail fast rather than attempt a huge allocation / unbounded growth.
    fprintf(stderr, "Error: frame length %u exceeds maximum %d; aborting\n",
            header.length, MAX_LINE_SIZE);
    exit(EXIT_FAILURE);
  }
  if (available < sizeof(header) + header.length) {
    return NULL; // body not fully buffered yet
  }
  struct payload *msg_payload = xmalloc(sizeof(*msg_payload) + header.length + 1);
  msg_payload->timestamp = header.timestamp;
  msg_payload->length = header.length;
  memcpy(msg_payload->text, fr->buf + fr->start + sizeof(header), header.length);
  msg_payload->text[header.length] = '\0';
  fr->start += sizeof(header) + header.length;
  return msg_payload;
}

void process_msg_payload(FILE *stream, FILE *logfile, const char *color,
                         struct payload *msg_payload) {
  // Write stderr message if only stderr is ready
  char timestamp[100];
  if (timestamp_enabled) {
    if (relative_timestamps) {
      // Write elapsed time since the start of the program as HH:MM:SS.MMMMMM.
      // First calculate the elapsed time in seconds and nanoseconds.
      long elapsed_sec = msg_payload->timestamp.tv_sec - start_timestamp.tv_sec;
      long elapsed_nsec =
          msg_payload->timestamp.tv_nsec - start_timestamp.tv_nsec;
      if (elapsed_nsec < 0) {
        elapsed_sec--;
        elapsed_nsec += 1000000000L;
      }
      // Then append the elapsed time to the timestamp string
      // in HH:MM:SS.MMMMMM format along with a trailing space.
      int hours = elapsed_sec / 3600;
      int minutes = (elapsed_sec % 3600) / 60;
      int seconds = elapsed_sec % 60;
      if (snprintf(timestamp, sizeof(timestamp), "%02d:%02d:%02d.%06ld ", hours,
                   minutes, seconds, // NOLINT
                   (elapsed_nsec / 1000)) >= sizeof(timestamp)) {
        _error("Timestamp truncated in process_msg_payload");
      }
    } else {
      struct tm *time_info = localtime(&msg_payload->timestamp.tv_sec);
      if (!time_info) {
        perror("localtime");
        exit(EXIT_FAILURE);
      }
      // Set timestamp to HH:MM:SS.NNNNNNNNN
      // First write the time in HH:MM:SS format
      strftime(timestamp, sizeof(timestamp), "%H:%M:%S", time_info);
      // Then append the nanoseconds and a space
      size_t current_len = strlen(timestamp);
      size_t remaining = sizeof(timestamp) - current_len;
      if (snprintf(timestamp + current_len, remaining, ".%06ld ", // NOLINT
                   msg_payload->timestamp.tv_nsec / 1000) >= remaining) {
        _error("Nanoseconds truncated in process_msg_payload");
      }
    }
  } else {
    // Make sure timestamp is empty
    timestamp[0] = '\0';
  }
  fprintf(logfile, "%s%s%s%s%s%s\n", ts_color, timestamp, reset_color, color,
          msg_payload->text, reset_color);
  if (color_to_tty) {
    fprintf(stream, "%s%s%s%s%s%s\n", ts_color, timestamp, reset_color, color,
            msg_payload->text, reset_color);
  } else {
    fprintf(stream, "%s%s\n", timestamp, msg_payload->text);
  }
  fflush(stream);
}

int main(int argc, char *argv[]) {
  int opt;
  int option_index = 0;
  const char *logfile_name = NULL;
  const char *out_color = ""; // No color for stdout
  const char *err_color =
      ANSI_COLOR_BOLD ANSI_COLOR_YELLOW; // Default for stderr
  int color_light = 0;
  int color_dark = 0;
  int color_bold = 0;
  int plain_mode = 0;
  int forcecolor_mode = 0;
  int timestamp_mode = 0;
  int debug_mode = 0;

  static struct option long_options[] = {
      {"bold", no_argument, 0, 'b'},
      {"dark", no_argument, 0, 'd'},
      {"errcolor", required_argument, 0, 'e'},
      {"forcecolor", no_argument, 0, 'f'},
      {"help", no_argument, 0, 'h'},
      {"light", no_argument, 0, 'l'},
      {"outcolor", required_argument, 0, 'o'},
      {"plain", no_argument, 0, 'p'},
      {"relative", no_argument, 0, 'r'},
      {"ts", no_argument, 0, 't'},
      {"version", no_argument, 0, 'v'},
      {"debug", no_argument, 0, 'x'},
      {0, 0, 0, 0}};

  while ((opt = getopt_long(argc, argv, "bde:flho:prtv", long_options,
                            &option_index)) != -1) {
    switch (opt) {
    case 'l':
      err_color = ANSI_COLOR_BOLD ANSI_COLOR_MAGENTA; // for light background
      ts_color = ANSI_COLOR_BLUE;                     // Timestamp color
      color_light = 1;
      break;
    case 'd':
      err_color = ANSI_COLOR_BOLD ANSI_COLOR_YELLOW; // for dark background
      ts_color = ANSI_COLOR_CYAN;                    // Timestamp color
      color_dark = 1;
      break;
    case 'b':
      err_color = ANSI_COLOR_BOLD; // ANSI bold for stderr
      ts_color = "";               // Timestamp color
      color_bold = 1;
      break;
    case 'f':
      forcecolor_mode = 1;
      break;
    case 'p':
      out_color = "";   // No color for stdout
      err_color = "";   // No color for stderr
      ts_color = "";    // No color for timestamp
      reset_color = ""; // Don't print ANSI reset characters
      timestamp_enabled = 0;
      plain_mode = 1;
      break;
    case 'o':
      out_color = optarg;
      break;
    case 'e':
      err_color = optarg;
      break;
    case 'r':
      relative_timestamps = 1;
      timestamp_enabled = 1;
      timestamp_mode = 1;
      break;
    case 't':
      timestamp_enabled = 1;
      timestamp_mode = 1;
      break;
    case 'h':
      usage(EXIT_SUCCESS);
      break;
    case 'v':
      puts(VERSION);
      exit(EXIT_SUCCESS);
    case 'x':
      debuglevel++;
      debug_mode = 1;
      break;
    default:
      usage(EXIT_FAILURE);
    }
  }

  if (color_light + color_dark + color_bold + plain_mode > 1) {
    fprintf(stderr, "Error: Options --light, --dark, --bold, and --plain are "
                    "mutually exclusive.\n");
    usage(EXIT_FAILURE);
  }

  if (forcecolor_mode + plain_mode > 1) {
    fprintf(
        stderr,
        "Error: Options --forcecolor and --plain are mutually exclusive.\n");
    usage(EXIT_FAILURE);
  }

  if (timestamp_mode + plain_mode > 1) {
    fprintf(stderr,
            "Error: Options --ts and --plain are mutually exclusive.\n");
    usage(EXIT_FAILURE);
  }

  if (debug_mode + plain_mode > 1) {
    fprintf(stderr,
            "Error: Options --debug and --plain are mutually exclusive.\n");
    usage(EXIT_FAILURE);
  }

  if (optind >= argc) {
    fprintf(stderr, "Expected logfile and command after options\n");
    usage(EXIT_FAILURE);
  }

  logfile_name = argv[optind++];
  if (optind >= argc) {
    fprintf(stderr, "Expected command after logfile\n");
    usage(EXIT_FAILURE);
  }

  const char *command = argv[optind];
  char **command_args = &argv[optind];

  // Determine if output is to a TTY
  if (!forcecolor_mode && (!isatty(STDOUT_FILENO) || !isatty(STDERR_FILENO))) {
    color_to_tty = 0;
  }

  int stdout_pipe[2], stderr_pipe[2], stdout_msg_pipe[2], stderr_msg_pipe[2];
  if (pipe(stdout_pipe) == -1 || pipe(stderr_pipe) == -1 ||
      pipe(stdout_msg_pipe) == -1 || pipe(stderr_msg_pipe) == -1) {
    perror("Error creating pipes");
    return EXIT_FAILURE;
  }

  FILE *logfile = fopen(logfile_name, "w");
  if (!logfile) {
    fprintf(stderr, "Error opening logfile %s %s\n", logfile_name,
            strerror(errno));
    return EXIT_FAILURE;
  }

  // Get the current time with nanosecond precision
  if (clock_gettime(CLOCK_REALTIME, &start_timestamp) == -1) {
    perror("clock_gettime");
    exit(EXIT_FAILURE);
  }

  pid_t stdout_worker = fork();
  if (stdout_worker == 0) {
    // Child process: handle stdout
    close(stdout_pipe[1]);     // Close write end of stdout pipe
    close(stderr_pipe[0]);     // Close unused read end of stderr pipe
    close(stderr_pipe[1]);     // Close unused write end of stderr pipe
    close(stdout_msg_pipe[0]); // Close read end of stdout message pipe
    close(stderr_msg_pipe[0]); // Close unused read end of stderr message pipe
    close(stderr_msg_pipe[1]); // Close unused write end of stderr message pipe
    timestamp_and_send(stdout_msg_pipe[1], stdout_pipe[0], "stdout");
    close(stdout_pipe[0]);
    close(stdout_msg_pipe[1]);
    exit(EXIT_SUCCESS);
  }

  // Verify that the stdout worker process is online and ready
  if (await_worker(stdout_msg_pipe[0], "stdout") != 0) {
    return EXIT_FAILURE;
  }
  _debug(2, "confirmed stdout worker process [%d] is online and ready",
         stdout_worker);

  pid_t stderr_worker = fork();
  if (stderr_worker == 0) {
    // Child process: handle stderr
    close(stderr_pipe[1]);     // Close write end of stderr pipe
    close(stdout_pipe[0]);     // Close unused read end of stdout pipe
    close(stdout_pipe[1]);     // Close unused write end of stdout pipe
    close(stderr_msg_pipe[0]); // Close read end of stderr message pipe
    close(stdout_msg_pipe[0]); // Close unused read end of stdout message pipe
    close(stdout_msg_pipe[1]); // Close unused write end of stdout message pipe
    timestamp_and_send(stderr_msg_pipe[1], stderr_pipe[0], "stderr");
    close(stderr_pipe[0]);
    close(stderr_msg_pipe[1]);
    exit(EXIT_SUCCESS);
  }

  // Verify that the stderr worker process is online and ready
  if (await_worker(stderr_msg_pipe[0], "stderr") != 0) {
    return EXIT_FAILURE;
  }
  _debug(2, "confirmed stderr worker process [%d] is online and ready",
         stderr_worker);

  pid_t pid = fork();
  if (pid == -1) {
    perror("Error forking process");
    return EXIT_FAILURE;
  }

  if (pid == 0) {
    // Child process: execute the command
    close(stdout_pipe[0]);     // Close read end of stdout pipe
    close(stderr_pipe[0]);     // Close read end of stderr pipe
    close(stdout_msg_pipe[0]); // Close read end of stdout message pipe
    close(stderr_msg_pipe[0]); // Close read end of stderr message pipe
    close(stdout_msg_pipe[1]); // Close write end of stdout message pipe
    close(stderr_msg_pipe[1]); // Close write end of stderr message pipe

    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);

    close(stdout_pipe[1]); // Close write end of stdout pipe
    close(stderr_pipe[1]); // Close write end of stderr pipe

    execvp(command, command_args);

    // If execvp fails
    perror("Error executing command");
    exit(EXIT_FAILURE);
  }

  // Parent process
  close(stdout_pipe[1]);     // Close write end of stdout pipe
  close(stderr_pipe[1]);     // Close write end of stderr pipe
  close(stdout_msg_pipe[1]); // Close write end of stdout message pipe
  close(stderr_msg_pipe[1]); // Close write end of stderr message pipe

  struct pollfd pfds[2];
  nfds_t num_open_fds = 2; // We start with two open file descriptors
  pfds[0].fd = stdout_msg_pipe[0];
  pfds[0].events = POLLIN | POLLHUP;
  pfds[1].fd = stderr_msg_pipe[0];
  pfds[1].events = POLLIN | POLLHUP;

  struct framereader stdout_reader, stderr_reader;
  framereader_init(&stdout_reader, stdout_msg_pipe[0]);
  framereader_init(&stderr_reader, stderr_msg_pipe[0]);

  int loopcount = 0;
  long ms_delta = 0;
  while (stdout_head || stderr_head || (num_open_fds > 0)) {
    _debug(2, "loop %d", loopcount++);

    // Check for new input on the message pipes
    if (num_open_fds > 0) {
      int poll_result = poll(pfds, 2, POLL_TIMEOUT_MS); // Wait for the next
                                                        // message, or time out
                                                        // to flush aged lines
      if (poll_result == -1) {
        if (errno == EINTR)
          continue;
        perror("Error polling message pipes");
        break;
      }
      if (poll_result > 0) {
        _debug(2, "poll result 0x%08x", poll_result);
        _debug(2,
               "stdout POLLIN=%d, POLLPRI=%d, POLLOUT=%d, POLLERR=%d, "
               "POLLHUP=%d, POLLNVAL=%d",
               pfds[0].revents & POLLIN, pfds[0].revents & POLLPRI,
               pfds[0].revents & POLLOUT, pfds[0].revents & POLLERR,
               pfds[0].revents & POLLHUP, pfds[0].revents & POLLNVAL);
        _debug(2,
               "stderr POLLIN=%d, POLLPRI=%d, POLLOUT=%d, POLLERR=%d, "
               "POLLHUP=%d, POLLNVAL=%d",
               pfds[1].revents & POLLIN, pfds[1].revents & POLLPRI,
               pfds[1].revents & POLLOUT, pfds[1].revents & POLLERR,
               pfds[1].revents & POLLHUP, pfds[1].revents & POLLNVAL);
        if (pfds[0].revents & POLLIN) {
          _debug(2, "detected input on stdout_msg_pipe[0]");
          // One read() per poll iteration on purpose: it keeps the two streams
          // serviced fairly and never blocks. Do not "optimize" this into a
          // loop that drains the pipe, which could starve the other stream.
          int rc = framereader_fill(&stdout_reader);
          // Enqueue every whole frame the read made available.
          struct payload *msg_payload;
          while ((msg_payload = framereader_next(&stdout_reader)) != NULL) {
            struct message *msg = xmalloc(sizeof(struct message));
            msg->msg_payload = msg_payload;
            push(&stdout_head, &stdout_tail, msg, &stdout_queuelen);
          }
          if (rc <= 0) {
            // EOF or error: stop watching for input. The POLLHUP branch
            // closes the pipe (and reports any truncated final frame) on a
            // later poll.
            if (rc < 0) {
              perror("read(stdout_msg_pipe[0])");
            }
            pfds[0].events = POLLHUP;
          }
        } else if (pfds[0].revents & POLLHUP) {
          _debug(2, "closing stdout_msg_pipe[0]");
          // Leftover unconsumed bytes mean the worker died mid-frame. EOF can
          // surface as POLLHUP with no final POLLIN, so check here - the one
          // branch every pipe passes through exactly once - rather than only
          // on an EOF seen during a read.
          if (framereader_pending(&stdout_reader) > 0) {
            _warn("stdout worker ended mid-frame; %zu trailing byte(s) "
                  "discarded",
                  framereader_pending(&stdout_reader));
          }
          close(stdout_msg_pipe[0]);
          pfds[0].fd = -1; // Ignore this file descriptor in future polls
          num_open_fds--;
          waitpid(stdout_worker, NULL, WNOHANG);
        }
        if (pfds[1].revents & POLLIN) {
          _debug(2, "detected input on stderr_msg_pipe[0]");
          int rc = framereader_fill(&stderr_reader);
          // Enqueue every whole frame the read made available.
          struct payload *msg_payload;
          while ((msg_payload = framereader_next(&stderr_reader)) != NULL) {
            struct message *msg = xmalloc(sizeof(struct message));
            msg->msg_payload = msg_payload;
            push(&stderr_head, &stderr_tail, msg, &stderr_queuelen);
          }
          if (rc <= 0) {
            // EOF or error: stop watching for input. The POLLHUP branch
            // closes the pipe (and reports any truncated final frame) on a
            // later poll.
            if (rc < 0) {
              perror("read(stderr_msg_pipe[0])");
            }
            pfds[1].events = POLLHUP;
          }
        } else if (pfds[1].revents & POLLHUP) {
          _debug(2, "closing stderr_msg_pipe[0]");
          // Leftover unconsumed bytes mean the worker died mid-frame. EOF can
          // surface as POLLHUP with no final POLLIN, so check here - the one
          // branch every pipe passes through exactly once - rather than only
          // on an EOF seen during a read.
          if (framereader_pending(&stderr_reader) > 0) {
            _warn("stderr worker ended mid-frame; %zu trailing byte(s) "
                  "discarded",
                  framereader_pending(&stderr_reader));
          }
          close(stderr_msg_pipe[0]);
          pfds[1].fd = -1; // Ignore this file descriptor in future polls
          num_open_fds--;
          waitpid(stderr_worker, NULL, WNOHANG);
        }
      }
    }

    // Get the current time as close as possible to receiving messages
    struct timespec current_time;
    if (clock_gettime(CLOCK_REALTIME, &current_time) == -1) {
      perror("clock_gettime");
      continue;
    }

    // Drain message queues
    while (stdout_head || stderr_head) {
      _debug(
          1,
          "stdout/stderr queuelen = %d/%d, stdout_head = %p stderr_head = %p",
          stdout_queuelen, stderr_queuelen, stdout_head, stderr_head);
      struct message *stdout_ready = NULL;
      struct message *stderr_ready = NULL;

      if (num_open_fds > 0) {
        // Create pointers to the head of each of the queues, but only
        // if they are not too new.
        if (stdout_head) {
          ms_delta = timespec_ms_delta(&current_time,
                                       &stdout_head->msg_payload->timestamp);
          if (ms_delta >= MESSAGE_HOLD_MS) {
            stdout_ready = stdout_head;
          } else {
            _debug(2, "message on stdout not ready to send after only %ldms",
                   ms_delta);
          }
        }
        if (stderr_head) {
          ms_delta = timespec_ms_delta(&current_time,
                                       &stderr_head->msg_payload->timestamp);
          if (ms_delta >= MESSAGE_HOLD_MS) {
            stderr_ready = stderr_head;
          } else {
            _debug(2, "message on stderr not ready to send after only %ldms",
                   ms_delta);
          }
        }
      } else {
        // If the message pipes are closed, go ahead and process the
        // remaining messages irrespective of their age.
        stdout_ready = stdout_head;
        stderr_ready = stderr_head;
      }

      // Process whichever message is older.
      if (stdout_ready && stderr_ready) {
        // Compare timestamps to determine which to write first
        if (timespec_cmp(&stdout_ready->msg_payload->timestamp,
                         &stderr_ready->msg_payload->timestamp) <= 0) {
          process_msg_payload(stdout, logfile, out_color,
                              stdout_ready->msg_payload);
          shift(&stdout_head, &stdout_tail, &stdout_queuelen);
        } else {
          process_msg_payload(stderr, logfile, err_color,
                              stderr_ready->msg_payload);
          shift(&stderr_head, &stderr_tail, &stderr_queuelen);
        }
      } else if (stdout_ready) {
        // Write stdout message if only stdout is ready
        process_msg_payload(stdout, logfile, out_color,
                            stdout_ready->msg_payload);
        shift(&stdout_head, &stdout_tail, &stdout_queuelen);
      } else if (stderr_ready) {
        // Write stderr message if only stderr is ready
        process_msg_payload(stderr, logfile, err_color,
                            stderr_ready->msg_payload);
        shift(&stderr_head, &stderr_tail, &stderr_queuelen);
      } else {
        break;
      }
    }
  }

  framereader_free(&stdout_reader);
  framereader_free(&stderr_reader);

  // Reap the timestamp workers. They have closed their pipes (POLLHUP) by the
  // time we get here; a blocking wait collects them so they do not linger as
  // zombies. ECHILD (already reaped via the WNOHANG calls above) is harmless.
  waitpid(stdout_worker, NULL, 0);
  waitpid(stderr_worker, NULL, 0);

  // Wait for child command process to complete
  int status;
  waitpid(pid, &status, 0);

  fclose(logfile);

  return WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
}
