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

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// The maximum line length supported by our line buffering
// https://stackoverflow.com/questions/3552095/sensible-line-buffer-size-in-c
#define BUFFER_SIZE 4096

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

#define _debug(dlevel, format, ...)                                            \
  if (debuglevel && debuglevel >= dlevel)                                      \
  fprintf(stderr, ANSI_COLOR_GREEN "DEBUG[%d]: " ANSI_COLOR_RESET format "\n", \
          getpid(), ##__VA_ARGS__)
#define _warn(format, ...)                                                     \
  fprintf(stderr,                                                              \
          ANSI_COLOR_YELLOW "WARNING[%d]: " ANSI_COLOR_RESET format "\n",      \
          getpid(), ##__VA_ARGS__)
#define _error(format, ...)                                                    \
  fprintf(stderr, ANSI_COLOR_RED "ERROR[%d]: " ANSI_COLOR_RESET format "\n",   \
          getpid(), ##__VA_ARGS__)

struct payload {
  struct timespec timestamp;
  char text[BUFFER_SIZE];
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

static void usage(const int rc) {
  printf("Usage: t3 [OPTION] FILE -- COMMAND ARGS ...\n");
  printf("Invoke provided command and write its colorized, "
         "precise time-stamped output both to the provided file"
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
  printf("  -e, --errcolor    color\n");
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

void send_msg_payload(int pipe_fd, struct payload *msg_payload) {
  // Send message payload to parent process being careful to
  // ensure that the entire message is sent.
  _debug(1, "Sending msg_payload '%s' to parent process, timestamp: %ld.%09ld",
         msg_payload->text, msg_payload->timestamp.tv_sec,
         msg_payload->timestamp.tv_nsec);
  ssize_t written = write(pipe_fd, msg_payload, sizeof(*msg_payload));
  while (written < sizeof(*msg_payload)) {
    if (written == -1) {
      if (errno == EAGAIN) {
        // If the pipe is full, keep trying to write
        // until it is available
        usleep(1000);
        written = write(pipe_fd, msg_payload, sizeof(*msg_payload));
      } else {
        perror("Error writing to pipe");
        break;
      }
    } else {
      // If only part of the message was written,
      // try again to write the rest of it
      ssize_t more_written =
          write(pipe_fd, msg_payload + written, sizeof(*msg_payload) - written);
      if (more_written == -1) {
        if (errno == EAGAIN) {
          // If the pipe is full, keep trying to write
          // until it is available
          while (more_written == -1 && errno == EAGAIN) {
            usleep(1000);
            more_written = write(pipe_fd, msg_payload + written,
                                 sizeof(*msg_payload) - written);
          }
          break;
        } else {
          perror("Error writing to pipe");
          break;
        }
      }
      written += more_written;
    }
  }
}

void timestamp_and_send(int pipe_fd, int fd, const char *prefix) {
  char buffer[BUFFER_SIZE];
  ssize_t bytes_read;
  size_t line_length = 0;

  // TODO: set argv[0] to incorporate prefix

  // Set the read-side file descriptor to line-buffered mode using setvbuf
  FILE *stream = fdopen(fd, "r");
  if (!stream) {
    perror("fdopen failed");
    exit(EXIT_FAILURE);
  }
  setvbuf(stream, NULL, _IOLBF, 0); // Line buffering

  struct payload msg_payload;

  // Set pipe_fd to non-blocking mode
  int flags = fcntl(pipe_fd, F_GETFL, 0);
  if (flags == -1 || fcntl(pipe_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    perror("Error setting pipe to non-blocking mode");
    exit(EXIT_FAILURE);
  }

  // Send a message to the parent process to indicate that the child
  // process has started
  if (snprintf(msg_payload.text, BUFFER_SIZE, "%s started", prefix) >= BUFFER_SIZE) { // NOLINT
      _error("Message truncated in timestamp_and_send");
  }
  msg_payload.timestamp.tv_sec = 0;
  msg_payload.timestamp.tv_nsec = 0;
  ssize_t written = write(pipe_fd, &msg_payload, sizeof(msg_payload));
  if (written < sizeof(msg_payload)) {
    _error("Error writing to pipe from %s", prefix);
    exit(EXIT_FAILURE);
  }

  while ((bytes_read = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
    // Get the current time with nanosecond precision. Note that if a
    // line is split across multiple reads, the timestamp will be set
    // to the time that the _last_ read is completed.
    if (clock_gettime(CLOCK_REALTIME, &msg_payload.timestamp) == -1) {
      perror("clock_gettime");
      exit(EXIT_FAILURE);
    }

    buffer[bytes_read] = '\0'; // Null-terminate the buffer
    _debug(1, "Read %ld bytes from fd: '%s' timestamp: %ld.%09ld", bytes_read,
           buffer, msg_payload.timestamp.tv_sec, msg_payload.timestamp.tv_nsec);

    size_t i = 0;
    while (i < bytes_read) {
      if (line_length >= BUFFER_SIZE - 1) {
        fprintf(stderr, "Line too long, truncating.\n");
        msg_payload.text[BUFFER_SIZE - 1] = '\0';
        send_msg_payload(pipe_fd, &msg_payload);
        line_length = 0;
      }

      if (buffer[i] == '\n') {
        msg_payload.text[line_length] = '\0';
        send_msg_payload(pipe_fd, &msg_payload);
        line_length = 0; // Reset for the next line
      } else {
        msg_payload.text[line_length++] = buffer[i];
      }
      i++;
    }
  }

  if (bytes_read < 0) {
    fprintf(stderr, "Error reading file descriptor: %s\n", strerror(errno));
  }

  // Handle any remaining data in the buffer that doesn't end with a newline
  if (line_length > 0) {
    msg_payload.text[line_length] = '\0';
    send_msg_payload(pipe_fd, &msg_payload);
  }
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

int timespec_ms_delta(const struct timespec *a, const struct timespec *b) {
  long diff_in_ms =
      (a->tv_sec - b->tv_sec) * 1000 + (a->tv_nsec - b->tv_nsec) / 1000000;
  return diff_in_ms;
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
      if (snprintf(timestamp, sizeof(timestamp), "%02d:%02d:%02d.%06ld ", hours, minutes, seconds, // NOLINT
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
      printf("t3 version 1.0\n");
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
    perror("Error opening logfile");
    return EXIT_FAILURE;
  }

  // Get the current time with nanosecond precision
  if (clock_gettime(CLOCK_REALTIME, &start_timestamp) == -1) {
    perror("clock_gettime");
    exit(EXIT_FAILURE);
  }

  // Test message payload for verifying stdout and stderr workers
  struct payload test_msg_payload;

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
  if (read(stdout_msg_pipe[0], &test_msg_payload, sizeof(struct payload)) ==
      -1) {
    perror("Error reading from stdout pipe");
    return EXIT_FAILURE;
  }
  if (test_msg_payload.timestamp.tv_sec != 0 ||
      test_msg_payload.timestamp.tv_nsec != 0) {
    fprintf(stderr, "Error: Unexpected message from stdout worker: %s",
            test_msg_payload.text);
    return EXIT_FAILURE;
  }
  if (strcmp(test_msg_payload.text, "stdout started") != 0) {
    fprintf(stderr, "Error: Unexpected message from stdout worker: %s",
            test_msg_payload.text);
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
  if (read(stderr_msg_pipe[0], &test_msg_payload, sizeof(struct payload)) ==
      -1) {
    perror("Error reading from stderr pipe");
    return EXIT_FAILURE;
  }
  if (test_msg_payload.timestamp.tv_sec != 0 ||
      test_msg_payload.timestamp.tv_nsec != 0) {
    fprintf(stderr, "Error: Unexpected message from stderr worker: %s",
            test_msg_payload.text);
    return EXIT_FAILURE;
  }
  if (strcmp(test_msg_payload.text, "stderr started") != 0) {
    fprintf(stderr, "Error: Unexpected message from stderr worker: %s",
            test_msg_payload.text);
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
  pfds[0].events = POLLIN;
  pfds[1].fd = stderr_msg_pipe[0];
  pfds[1].events = POLLIN;

  int loopcount = 0;
  int ms_delta = 0;
  while (stdout_head || stderr_head || (num_open_fds > 0)) {
    _debug(2, "loop %d", loopcount++);

    // Check for new input on the message pipes
    if (num_open_fds > 0) {
      int poll_result = poll(pfds, 2, 1000); // Block for a second waiting
                                             // for something to happen
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
               pfds[0].revents & POLLIN, pfds[0].revents & POLLPRI,
               pfds[0].revents & POLLOUT, pfds[0].revents & POLLERR,
               pfds[0].revents & POLLHUP, pfds[0].revents & POLLNVAL);
        if ((pfds[0].revents & POLLIN) && !(pfds[0].revents & POLLHUP)) {
          _debug(2, "detected input on stdout_msg_pipe[0]");
          struct payload *msg_payload = malloc(sizeof(struct payload));
          if (read(stdout_msg_pipe[0], msg_payload, sizeof(struct payload)) >
              0) {
            // Add received message to stdout queue
            struct message *msg = malloc(sizeof(struct message));
            msg->msg_payload = msg_payload;
            push(&stdout_head, &stdout_tail, msg, &stdout_queuelen);
          } else {
            perror("read(stdout_msg_pipe[0])");
          }
        } else if (pfds[0].revents & POLLHUP) {
          _debug(2, "closing stdout_msg_pipe[0]");
          close(stdout_msg_pipe[0]);
          pfds[0].fd = -1; // Ignore this file descriptor in future polls
          num_open_fds--;
          waitpid(stdout_worker, NULL, WNOHANG);
        }
        if ((pfds[1].revents & POLLIN) && !(pfds[1].revents & POLLHUP)) {
          _debug(2, "detected input on stderr_msg_pipe[0]");
          struct payload *msg_payload = malloc(sizeof(struct payload));
          if (read(stderr_msg_pipe[0], msg_payload, sizeof(struct payload)) >
              0) {
            // Add received message to stderr queue
            struct message *msg = malloc(sizeof(struct message));
            msg->msg_payload = msg_payload;
            push(&stderr_head, &stderr_tail, msg, &stderr_queuelen);
          } else {
            perror("read(stderr_msg_pipe[0])");
          }
        } else if (pfds[1].revents & POLLHUP) {
          _debug(2, "closing stderr_msg_pipe[0]");
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
          if (ms_delta >= 100) {
            stdout_ready = stdout_head;
          } else {
            _debug(2, "message on stdout not ready to send after only %dms",
                   ms_delta);
          }
        }
        if (stderr_head) {
          ms_delta = timespec_ms_delta(&current_time,
                                       &stderr_head->msg_payload->timestamp);
          if (ms_delta >= 100) {
            stderr_ready = stderr_head;
          } else {
            _debug(2, "message on stderr not ready to send after only %dms",
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

  // Wait for child command process to complete
  int status;
  waitpid(pid, &status, 0);

  fclose(logfile);

  return WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
}
