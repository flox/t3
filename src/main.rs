use std::cell::RefCell;
use std::collections::VecDeque;
use std::fs::OpenOptions;
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use std::rc::Rc;

use anyhow::{Context, Result};
use clap::Parser;
use colored::Colorize;

const DEFAULT_COLOR: colored::Color = colored::Color::White;
const ERROR_COLOR: colored::Color = colored::Color::Red;
const TIME_COLOR: colored::Color = colored::Color::BrightBlack;

#[derive(Debug, Parser)]
struct Args {
    #[arg(long("ts"), default_value("off"))]
    timestamps: TimestampSpec,
    #[arg(long, default_value("auto"))]
    color: ColorSpec,

    #[arg(long, conflicts_with("timestamps"), conflicts_with("color"))]
    plain: bool,

    log_file: PathBuf,

    command: String,
    args: Vec<String>,
}

#[derive(Debug, Clone, Copy, clap::ValueEnum)]
enum TimestampSpec {
    Off,
    Absolute,
    Relative,
}

#[derive(Debug, Clone, Copy, clap::ValueEnum)]
enum ColorSpec {
    Auto,
    Always,
    Never,
}

fn main() -> Result<ExitCode> {
    let mut args = Args::try_parse()?;

    if args.plain {
        args.color = ColorSpec::Never;
        args.timestamps = TimestampSpec::Off;
    }

    match args.color {
        ColorSpec::Auto => {}
        ColorSpec::Always => colored::control::set_override(true),
        ColorSpec::Never => colored::control::set_override(false),
    };

    // configure outputs based on cli arguments
    let (stderr_outputs, stdout_outputs) = setup_outputs(&args);

    let child = Command::new(&args.command)
        .args(&args.args)
        .stderr(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .spawn()
        .context("Failed to spawn child process")?;

    let status = run(child, stderr_outputs, stdout_outputs)?;

    Ok(ExitCode::from(status.code().unwrap_or(126) as u8))
}

/// Attach to a [`std::process::Child`] process
/// and write its output to `stderr_outputs` and `stdout_outputs` respectively.
/// Each channel is read by a separate thread which send the messages
/// via an [`std::sync::mpsc::Sender`] back to the main thread
/// where the messages are processed in order.
fn run(
    mut child: std::process::Child,
    mut stderr_outputs: Outputs,
    mut stdout_outputs: Outputs,
) -> Result<std::process::ExitStatus, anyhow::Error> {
    let stderr = BufReader::new(child.stderr.take().expect("Stderr is piped"));
    let stdout = BufReader::new(child.stdout.take().expect("Stdout is piped"));

    let status = std::thread::scope(|scope| {
        let (receiver, stderr_thread, stdout_thread) = setup_output_channels(scope, stderr, stdout);

        let (stdout_remaining, stderr_remaining) =
            read_lines_with_backoff(receiver, 100, &mut stderr_outputs, &mut stdout_outputs)?;

        // Drain the remaining messages
        drain_remaining_messages(
            stdout_remaining,
            stderr_remaining,
            &mut stderr_outputs,
            &mut stdout_outputs,
        )?;

        // Clean up resources for child process to avoid zombies.
        // Also, in case of errors in the reader channels,
        // ensure that the process is finished regardless.
        let status = child.wait().context("Failed to wait for child process")?;

        // Check for io errors in channels.
        // If channels panicked likewise panic here as there is not much we can say in that case.
        stderr_thread
            .join()
            .expect("panic occured in stderr writer thread")
            .context("Failed to write stderr output")?;
        stdout_thread
            .join()
            .expect("panic occured in stdout writer thread")
            .context("Failed to write stdout output")?;

        anyhow::Ok(status)
    })?;

    Ok(status)
}

/// Create sets of [`OutputSpec`]s for each channel (stdout/stderr).
/// Each will have write to the respective stdout/err
/// of this executable as well as a common log file.
///
/// Log files are always colorized except with [`Args::color`]
/// set to [`ColorSpec::Never`].
fn setup_outputs(args: &Args) -> (Outputs, Outputs) {
    let start_timestamp = time::UtcDateTime::now();

    let log_file = open_log_file(&args.log_file).expect("Failed to open log file");
    let share_log_file = SharedOutput::new(BufWriter::new(log_file));

    let logfile_color_spec = match args.color {
        ColorSpec::Auto => ColorSpec::Always,
        ColorSpec::Always | ColorSpec::Never => args.color,
    };

    let stderr_outputs = Outputs(vec![
        // terminal output
        OutputSpec {
            output: Box::new(std::io::stderr()),
            start_timestamp,
            print_timestamp: args.timestamps,
            color: OutputColor(args.color, ERROR_COLOR),
        },
        // log file output
        OutputSpec {
            output: Box::new(share_log_file.clone()),
            start_timestamp,
            print_timestamp: args.timestamps,
            color: OutputColor(logfile_color_spec, ERROR_COLOR),
        },
    ]);

    let stdout_outputs = Outputs(vec![
        // terminal output
        OutputSpec {
            output: Box::new(std::io::stdout()),
            print_timestamp: args.timestamps,
            start_timestamp,
            color: OutputColor(args.color, DEFAULT_COLOR),
        },
        // log file output
        OutputSpec {
            output: Box::new(share_log_file.clone()),
            start_timestamp,
            print_timestamp: args.timestamps,
            color: OutputColor(logfile_color_spec, DEFAULT_COLOR),
        },
    ]);
    (stderr_outputs, stdout_outputs)
}

#[must_use]
fn read_lines_with_backoff(
    receiver: std::sync::mpsc::Receiver<(Source, Message)>,
    backoff_ms: usize,
    stderr_outputs: &mut Outputs,
    stdout_outputs: &mut Outputs,
) -> Result<(VecDeque<Message>, VecDeque<Message>), anyhow::Error> {
    let mut stdout_queue = VecDeque::new();
    let mut stderr_queue = VecDeque::new();

    for line in receiver.iter() {
        match line {
            (Source::Stdout, message) => stdout_queue.push_back(message),
            (Source::Stderr, message) => stderr_queue.push_back(message),
        }

        // Drain the messages if they are older than 100ms to allow for delay.
        // Question: is this even necessary? MPSC channels are already ordered,
        // and the delay between taking the time and sending the message
        // is minimal.
        loop {
            match (stdout_queue.front(), stderr_queue.front()) {
                (None, None) => {
                    break;
                }
                (Some(stdout), _)
                    if (time::UtcDateTime::now() - stdout.timestamp).whole_milliseconds()
                        < backoff_ms as i128 =>
                {
                    break;
                }
                (_, Some(stderr))
                    if (time::UtcDateTime::now() - stderr.timestamp).whole_milliseconds()
                        < backoff_ms as i128 =>
                {
                    break;
                }
                (Some(_), None) => {
                    stdout_outputs.write_message(&stdout_queue.pop_front().unwrap())?
                }
                (None, Some(_)) => {
                    stderr_outputs.write_message(&stderr_queue.pop_front().unwrap())?
                }
                (Some(stdout), Some(stderr)) => {
                    if stdout.timestamp < stderr.timestamp {
                        stdout_outputs.write_message(&stdout_queue.pop_front().unwrap())?;
                    } else {
                        stderr_outputs.write_message(&stderr_queue.pop_front().unwrap())?;
                    }
                }
            };
        }
    }

    Ok((stdout_queue, stderr_queue))
}

fn drain_remaining_messages(
    mut stdout_remaining: VecDeque<Message>,
    mut stderr_remaining: VecDeque<Message>,
    stderr_outputs: &mut Outputs,
    stdout_outputs: &mut Outputs,
) -> Result<(), anyhow::Error> {
    Ok(loop {
        match (stdout_remaining.front(), stderr_remaining.front()) {
            (None, None) => break,
            (Some(_), None) => {
                stdout_outputs.write_message(&stdout_remaining.pop_front().unwrap())?;
            }
            (None, Some(_)) => {
                stderr_outputs.write_message(&stderr_remaining.pop_front().unwrap())?;
            }
            (Some(stdout), Some(stderr)) => {
                if stdout.timestamp < stderr.timestamp {
                    stdout_outputs.write_message(&stdout_remaining.pop_front().unwrap())?;
                } else {
                    stderr_outputs.write_message(&stderr_remaining.pop_front().unwrap())?;
                }
            }
        }
    })
}

struct Message {
    // [sic] System time is not monotonic
    timestamp: time::UtcDateTime,
    line: String,
}

impl Message {
    fn record(line: String) -> Self {
        Self {
            timestamp: time::UtcDateTime::now(),
            line,
        }
    }
}

enum Source {
    Stderr,
    Stdout,
}

fn setup_output_channels<'scope>(
    scope: &'scope std::thread::Scope<'scope, '_>,
    stderr: impl BufRead + Send + 'scope,
    stdout: impl BufRead + Send + 'scope,
) -> (
    std::sync::mpsc::Receiver<(Source, Message)>,
    std::thread::ScopedJoinHandle<'scope, std::result::Result<(), anyhow::Error>>,
    std::thread::ScopedJoinHandle<'scope, std::result::Result<(), anyhow::Error>>,
) {
    let (sender, receiver) = std::sync::mpsc::channel();
    let stderr_sender = sender.clone();
    let stderr_thread = scope.spawn(move || {
        redirect(stderr, |message| {
            stderr_sender.send((Source::Stderr, message))
        })
    });

    let stdout_sender = sender;
    let stdout_thread = scope.spawn(move || {
        redirect(stdout, |message| {
            stdout_sender.send((Source::Stdout, message))
        })
    });
    (receiver, stderr_thread, stdout_thread)
}

struct OutputColor(ColorSpec, colored::Color);

trait WriteMessage {
    fn write_message(&mut self, message: &Message) -> Result<()>;
}

struct OutputSpec {
    output: Box<dyn Write>,
    start_timestamp: time::UtcDateTime,
    print_timestamp: TimestampSpec,
    color: OutputColor,
}

impl WriteMessage for OutputSpec {
    /// Write messages according to the spec.
    fn write_message(&mut self, message: &Message) -> Result<()> {
        let line = {
            let OutputColor(spec, color) = self.color;

            // Enable or disable color based on the associated color spec.
            // If `Auto` uses `colored`s defaults to decide whether to colorize or not.
            match spec {
                ColorSpec::Auto => {}
                ColorSpec::Always => colored::control::set_override(true),
                ColorSpec::Never => colored::control::set_override(false),
            }

            message.line.color(color).to_string()
        };

        let timestamp = match self.print_timestamp {
            TimestampSpec::Relative => {
                let duration = message.timestamp - self.start_timestamp;

                let timestamp = format!(
                    "{hours:02}:{minutes:02}:{seconds:02}.{subsec:06}",
                    hours = duration.whole_hours(),
                    minutes = duration.whole_minutes(),
                    seconds = duration.whole_seconds(),
                    subsec = duration.subsec_microseconds(),
                );

                Some(timestamp)
            }
            TimestampSpec::Absolute => Some(message.timestamp.to_string()),
            _ => None,
        };

        if let Some(timestamp_str) = timestamp {
            let timestamp_str = timestamp_str.color(TIME_COLOR).to_string();

            writeln!(self.output, "{timestamp_str}: {line}",)?;
        } else {
            writeln!(self.output, "{line}")?;
        };

        colored::control::unset_override();
        Ok(())
    }
}

/// Multiple [`OutputSpec`]s that aare associated with the same message stream.
struct Outputs(Vec<OutputSpec>);

impl WriteMessage for Outputs {
    fn write_message(&mut self, message: &Message) -> Result<()> {
        for output in &mut self.0 {
            output.write_message(message)?;
        }
        Ok(())
    }
}

/// A wrapper around the same output sink,
/// to allow multiple [OutputSpec]s to write to the same destination.
#[derive(Debug)]
struct SharedOutput<W> {
    writer: Rc<RefCell<W>>,
}

impl<W> SharedOutput<W>
where
    W: Write,
{
    fn new(writer: W) -> Self {
        SharedOutput {
            writer: Rc::new(RefCell::new(writer)),
        }
    }
}

/// Allow using [`SharedOutput`] in multiple [`OutputSpecs`]
///
/// SAFETY:
/// Requires to be written to **from a single thread**.
/// [`RC`] should prevent this being moved to another thread,
/// but in either case concurrent writes
/// will cause the [`RefCell::borrow_mut`] to panic.
impl<W> Clone for SharedOutput<W>
where
    W: Write,
{
    fn clone(&self) -> Self {
        SharedOutput {
            writer: self.writer.clone(),
        }
    }
}

/// Allow using [`SharedOutput`] as a [`Write`] implementation with [`OutputSpec`]s
impl<W> Write for SharedOutput<W>
where
    W: Write,
{
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.writer.borrow_mut().write(buf)
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.writer.borrow_mut().flush()
    }
}

/// Open the log file at the provided path.
/// The containing directory must exists,
/// any existing file at the given path with be truncated.
fn open_log_file<'a>(path: impl AsRef<Path>) -> Result<impl Write + 'a> {
    OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(path)
        .context("Failed to open log file")
}

/// Read lines from a [BufRead] instance,
/// create a [Message] from each read line
/// and then call the supplied closure with this message.
fn redirect<E: Into<anyhow::Error>>(
    reader: impl BufRead,
    message_handler: impl Fn(Message) -> Result<(), E>,
) -> Result<()> {
    for line in reader.lines() {
        let line = line.context("Failed to read line")?;
        let message = Message::record(line);
        message_handler(message)
            .map_err(Into::into)
            .context("Failed to write line to output")?;
    }
    Ok(())
}
