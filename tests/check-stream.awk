# check-stream.awk - verify a stream produced by tests/stress via t3.
#
# Reads lines of the form "<PREFIX> <tid> <seq>" and confirms that t3 relayed
# the generator's output perfectly: every expected line present exactly once,
# none garbled, and each thread's sequence numbers still in ascending order.
#
# Variables (set with -v):
#   prefixes  space-separated stream labels to expect, e.g. "OUT" or "OUT ERR"
#   threads   number of generator threads
#   lines     lines each thread wrote per stream
#   label     human-readable name for this stream, used in messages
#
# Exits non-zero and prints the problems to stderr if anything is amiss.

BEGIN {
  count_prefixes = split(prefixes, prefix_list, " ")
  for (i = 1; i <= count_prefixes; i++) {
    allowed[prefix_list[i]] = 1
  }
}

{
  total++
  # Anchored field check: a garbled, truncated, or concatenated line will not
  # match the exact "<PREFIX> <int> <int>" shape and is flagged immediately.
  if (split($0, field, " ") != 3 || !(field[1] in allowed) ||
      field[2] !~ /^[0-9]+$/ || field[3] !~ /^[0-9]+$/) {
    report("garbled line " NR ": <" $0 ">")
    next
  }
  prefix = field[1]
  tid = field[2] + 0
  seq = field[3] + 0
  if (tid < 0 || tid >= threads) {
    report("thread id " tid " out of range at line " NR)
    next
  }
  if (seq < 0 || seq >= lines) {
    report("sequence " seq " out of range at line " NR)
    next
  }
  key = prefix SUBSEP tid SUBSEP seq
  if (key in seen) {
    report("duplicate " prefix " " tid " " seq)
    next
  }
  seen[key] = 1
  group = prefix SUBSEP tid
  count[group]++
  if (group in last && seq <= last[group]) {
    report("out-of-order " prefix " thread " tid ": " seq " after " last[group])
  }
  last[group] = seq
}

function report(message) {
  printf("  FAIL: %s\n", message) > "/dev/stderr"
  problems++
}

END {
  expected = count_prefixes * threads * lines
  if (total != expected) {
    report("line count " total " != expected " expected)
  }
  for (i = 1; i <= count_prefixes; i++) {
    for (t = 0; t < threads; t++) {
      got = count[prefix_list[i] SUBSEP t] + 0
      if (got != lines) {
        report(prefix_list[i] " thread " t " produced " got " lines, expected " lines)
      }
    }
  }
  if (problems) {
    printf("%s: FAILED (%d problem(s))\n", label, problems) > "/dev/stderr"
    exit 1
  }
  printf("%s: OK (%d lines)\n", label, total)
}
