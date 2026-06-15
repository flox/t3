[NAME]
t3 \- next generation tee with colorized output streams and precise time stamping
[DESCRIPTION]
The
.BR t3
command parses the \fIstdout\fR and \fIstderr\fR streams of a command,
writing colorized precisely time-stamped versions of both streams
to the calling process's own \fIstdout\fR and \fIstderr\fR streams,
as well as to the provided filename.
In that respect
.BR tee
is to
.BR t3
what perl's
.BR IPC::Open2()
function is to
.BR IPC::Open3(),
preserving distinct handles for each of the \fIstdout\fR and \fIstderr\fR streams.
.P
It works by creating pipes for parsing the \fIstdout\fR and \fIstderr\fR streams
before invoking the provided command with its output redirected to these pipes.
It then forks independent processes that work in parallel to timestamp the lines of
output coming from both streams while the parent process reassembles and writes
colorized and timestamped renditions both to the provided filename and to its own
\fIstdout\fR and \fIstderr\fR streams.
.SH OPTIONS
[TEE COMPATIBILITY]
The \fB\-\-append\fR, \fB\-\-ignore\-interrupts\fR, and \fB\-\-output\-error\fR
options take their names and broad meaning from \fBtee\fR(1).
.PP
\fB\-\-append\fR opens the log file for appending instead of truncating it, so
output accumulates across successive invocations against the same file.
.PP
\fB\-\-ignore\-interrupts\fR makes
.BR t3
and its timestamp worker processes ignore \fISIGINT\fR; the wrapped command
keeps the default disposition, so a \fISIGINT\fR (e.g. \fBCtrl\-C\fR) still
interrupts the command while
.BR t3
drains and flushes its final output before exiting, rather than being torn
down mid-flush.
.PP
Note that \fBt3\fR's \fB\-p\fR is \fB\-\-plain\fR, not \fBtee\fR's pipe-mode
flag; the pipe-aware write-error behavior is reached through
\fB\-\-output\-error=\fR...\fB\-nopipe\fR.
[WRITE ERRORS]
The
.BR \-\-output\-error
option mirrors \fBtee\fR(1) and controls what
.BR t3
does when a write to one of its outputs fails.
.BR warn
diagnoses every write error and continues;
.BR warn\-nopipe
diagnoses errors except on broken pipes;
.BR exit
exits on any write error; and
.BR exit\-nopipe
exits on any write error except a broken pipe.
When
.BR \-\-output\-error
is not given, the default is to exit immediately on a broken-pipe error and
to diagnose other write errors.
Once a particular output has failed,
.BR t3
stops writing to it but keeps writing to the remaining outputs, so
.BR \-\-output\-error=warn\-nopipe
preserves the complete log file even when a downstream consumer of
\fBt3\fR's own \fIstdout\fR or \fIstderr\fR closes early.
.PP
When a write error is fatal (the
.BR exit
or
.BR exit\-nopipe
modes, or the default broken-pipe case),
.BR t3
closes the log file cleanly and exits with a failure status. That failure
status \fBreplaces the wrapped command's own exit status\fR, so a non-zero exit
from
.BR t3
may indicate either that the command failed or that writing its output did.
[BUGS]
Lines are reassembled in full regardless of length, growing the
internal buffer as needed up to a generous cap (16 MiB). A single
line longer than that cap is forwarded in cap-sized pieces. No data
is lost, but each piece is emitted as its own output line \(em with
its own newline, timestamp, and color reset \(em so one input line
longer than the cap appears as several lines in the output.

This program is specifically designed for processing
line-buffered text terminal output, so unlike
.BR tee
is not suitable for processing binary data streams.
[AUTHOR]
Written by Michael Brantley.
[SEE ALSO]
tee(1)
