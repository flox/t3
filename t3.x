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
By default the maximum line length is 4096 characters,
and lines longer than this produced by the invoked command
will be processed as multiple distinct messages.
This does not cause any loss of data,
but it does mean that timestamps and colorization
may be inserted between messages
that were originally part of a single line.
If this is a problem for you,
you can work around it by setting the
.BR --nocolor
option and not using the
.BR --ts
option.

This program is specifically designed for processing
line-buffered text terminal output, so unlike
.BR tee
is not suitable for processing binary data streams.
[AUTHOR]
Written by Michael Brantley.
[SEE ALSO]
tee(1)
