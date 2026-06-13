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
