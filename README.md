# t3
Next generation `tee` with colorized output streams and precise time stamping.

## Overview

The `t3` command parses the stdout and stderr streams of a command,
writing colorized precisely time-stamped versions of both streams
to the calling process's own stdout and stderr streams,
as well as to the provided filename.
In that respect `tee` is to `t3` what
Perl's `IPC::Open2()` function is to `IPC::Open3()`,
preserving distinct handles for each of the stdout and stderr streams.

It works by creating pipes for parsing the stdout and stderr streams
before invoking the provided command with output redirected to these pipes.
It then forks independent processes that work in parallel
to timestamp the lines of output coming from both streams
while the parent process reassembles and writes colorized and timestamped renditions
both to the provided filename and to its own stdout and stderr streams.

## Motivation

When parsing the output of a build invocation
it is often extremely useful to be able
to differentiate output written to the `stdout` and `stderr` streams,
and occasionally to be able to view the
precise timings of each of those lines in the logs.

Traditionally, when recording the output of a build to a log
the convention is to first merge the `stdout` and `stderr` streams,
and then use a tool like `tee` to multiplex the resulting stream
to both a file and back to the controlling terminal.
This presents a few problems, but most importantly
destroys the ability to identify lines sent to `stderr` in the first place.
Build output from the two streams can also be interleaved,
occasionally resulting in garbled output in the logs.

A depiction of the use of legacy `tee` is shown below:

```mermaid
flowchart LR
 subgraph s1["Legacy tee Invocation"]
    direction LR
        n15["cmd args"]
        n16["<br>"]
        n17["tee &lt;file&gt;"]
        n18["&lt;file&gt;"]
        n19["stdout"]
  end
    n15 -- stdout --> n16
    n16 --> n17
    n15 -- stderr --> n16
    n17 --> n18 & n19

    n16@{ shape: junction}
    n17@{ shape: lin-proc}
    n18@{ shape: doc}
    n19@{ shape: terminal}
```

## A better way

A much better result would be
to preserve the original `stdout` and `stderr` streams
being sent to the controlling terminal
while clearly identifying lines sent to `stderr` in the output file.
That is the role of `t3`, and it does this by way of
a pair of worker processes that apply high-precision timestamps
to lines of output from the invoked command
before forwarding those timestamped messages back to the parent process
where it briefly buffers the messages before sorting, formatting, and collating
the lines back to each of the output file and original `stdout`/`stderr` streams.

A diagram depicting the internal operation of `t3` is shown below:

```mermaid
flowchart LR
 subgraph s2["New t3 Invocation"]
        n20["t3 &lt;file&gt; -- cmd args"]
        s3["Worker 1"]:::subprocess
        s4["Command"]:::subprocess
        s5["Worker 2"]:::subprocess
        n24["Buffer, Sort &amp; Collate"]
        n25["&lt;file&gt;"]
        n26["stdout"]
        n27["stderr"]
  end
 subgraph s4["Command"]
        n23["exec cmd args"]
  end
 subgraph s3["Worker 1"]
        n21["add<br>timestamp"]
  end
 subgraph s5["Worker 2"]
        n28["add<br>timestamp"]
  end
    n20 -. fork .-> s3 & s4 & s5
    n23 -- stdout pipe --> n21
    n21 -- stdout message pipe --> n24
    n24 --> n25 & n26 & n27
    n28 -- stderr message pipe --> n24
    n23 -- stderr pipe --> n28

    classDef subprocess fill:#ace
    n25@{ shape: doc}
    n26@{ shape: terminal}
    n27@{ shape: terminal}
    n21@{ shape: tag-proc}
    n28@{ shape: tag-proc}
    n24@{ shape: collate}
```

## Usage

```
Usage: t3 [OPTION] FILE -- COMMAND ARGS ...
Invoke provided command and write its colorized, precise time-stamped output both to the provided file and to stdout/err.

  -l, --light       use color scheme suitable for light backgrounds
  -d, --dark        use color scheme suitable for dark backgrounds
  -b, --bold        highlight stderr in bold text (with no color)
  -p, --plain       disable all timestamps, ANSI color and highlighting
  -f, --forcecolor  enforce the use of color when not writing to a TTY
  -o, --outcolor C  set the ANSI escape sequence used to color stdout
  -e, --errcolor C  set the ANSI escape sequence used to color stderr
  -t, --ts          enable timestamps in all outputs
  -r, --relative    display timestamps as relative offsets from start time (implies --ts)
  -a, --append      append to the log file instead of overwriting it
  -i, --ignore-interrupts  ignore interrupt signals (finish flushing output on Ctrl-C)
  --output-error[=MODE]    set behavior on a write error; MODE is one of
                    warn, warn-nopipe, exit, exit-nopipe (a bare --output-error
                    means warn; with no --output-error, t3 exits on a broken
                    pipe and warns on other write errors)
  -h, --help        print this help message
  -v, --version     print version string
  --debug           enable debugging
```

The `-a`/`--append`, `-i`/`--ignore-interrupts`, and `--output-error` options
take their names and broad meaning from [`tee(1)`](https://www.gnu.org/software/coreutils/tee),
with a few deliberate differences noted below.

- **`-i`/`--ignore-interrupts`** does more than `tee`'s: it makes `t3` *and* its
  timestamp worker processes ignore `SIGINT`, while the wrapped command keeps
  the default disposition (so a `Ctrl-C` still interrupts the command). `t3`
  then drains and flushes the command's final output before exiting, rather
  than being torn down mid-flush.
- **`--output-error`** — by default, matching `tee`, `t3` **exits** when a write
  to its own stdout or stderr fails with a broken pipe, so the log file can be
  left truncated if a downstream consumer (e.g. `t3 log -- cmd | head`) closes
  early. On such a fatal write error `t3` exits with a failure status, which
  **replaces the wrapped command's own exit status**. Pass
  **`--output-error=warn-nopipe`** to instead ignore broken-pipe errors and keep
  the log file (and the surviving stream) complete.
- **`-p`** remains `t3`'s `--plain`, *not* `tee`'s pipe-mode flag; reach the
  pipe-aware behavior through `--output-error=…-nopipe`.

## Installing

The easiest way to get `t3` is using Flox:

1. [install Flox](https://flox.dev)
2. invoke `flox install t3`

## Contributing

This project is developed and maintained with Flox.

1. `flox activate`
2. `make`

## Continuous Integration

The project uses GitHub Actions for CI with the following checks:
- Building with multiple GCC versions
- Running the test suite
- Static analysis with clang-tidy
- Building in Flox environment

All checks must pass before merging pull requests.
