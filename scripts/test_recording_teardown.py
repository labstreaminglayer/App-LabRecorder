#!/usr/bin/env python
"""Integration test for LabRecorderCLI teardown and XDF integrity.

Starts LSL outlets, records them with LabRecorderCLI, stops the recording and checks that

* the recorder exits within ``--max-stop`` seconds (1.0 s by default) and with status 0,
* every recorded stream has a stream footer whose sample count matches its data,
* pyxdf does not report the file as damaged, and
* nothing that was sent before the stop is missing from the file.

The cases cover a plain stop, a stop before any sample arrives, a stop while the recorder is
still subscribing to a source that has gone away, and repeated start/stop cycles.

Requires ``pylsl`` and ``pyxdf``.
"""

import argparse
import contextlib
import logging
import os
import subprocess
import sys
import tempfile
import threading
import time

import pylsl
import pyxdf

EEG_NAME = "TeardownTestEEG"
MARKER_NAME = "TeardownTestMarkers"
NAMES = (EEG_NAME, MARKER_NAME)
EEG_CHANNELS = 8
EEG_RATE = 100.0

# time given to LSL to make a new outlet discoverable, and to flush the last samples over TCP
SETTLE = 0.5


class TestFailure(AssertionError):
    """Raised when a case does not hold up."""


def check(condition, message):
    if not condition:
        raise TestFailure(message)


def make_outlets():
    """Create the EEG and marker outlets used by every case."""
    eeg_info = pylsl.StreamInfo(
        EEG_NAME, "EEG", EEG_CHANNELS, EEG_RATE, "float32", "teardown_test_eeg"
    )
    marker_info = pylsl.StreamInfo(
        MARKER_NAME, "Markers", 1, pylsl.IRREGULAR_RATE, "string", "teardown_test_markers"
    )
    return pylsl.StreamOutlet(eeg_info), pylsl.StreamOutlet(marker_info)


class Recorder:
    """A running LabRecorderCLI, with its output read as it appears.

    Reading the output as it appears is what lets a case wait for the recorder to actually be
    collecting before it sends anything: an outlet does not replay what it pushed before the
    recorder subscribed, so pushing too early silently loses samples.
    """

    def __init__(self, cli_path, xdf_path):
        self._proc = subprocess.Popen(
            [cli_path, xdf_path, f"name='{EEG_NAME}'", f"name='{MARKER_NAME}'"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self.lines = []
        self._reader = threading.Thread(target=self._read_output, daemon=True)
        self._reader.start()

    def _read_output(self):
        for line in self._proc.stdout:
            self.lines.append(line.rstrip())

    def wait_for(self, needles, timeout=20.0):
        """Block until every needle has shown up in the output."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            joined = "\n".join(self.lines)
            if all(needle in joined for needle in needles):
                return
            if self._proc.poll() is not None:
                raise TestFailure(
                    f"LabRecorderCLI exited (status {self._proc.returncode}) before it was ready"
                )
            time.sleep(0.02)
        raise TestFailure(f"LabRecorderCLI did not report {needles!r} within {timeout} s")

    def wait_until_collecting(self):
        self.wait_for([f"Started data collection for stream {name}." for name in NAMES])

    def stop(self):
        """Send the quit key and return how long the recorder took to exit."""
        started = time.perf_counter()
        self._proc.stdin.write("\n")
        self._proc.stdin.flush()
        self._proc.stdin.close()
        try:
            # generously above any bound asserted on, so a hang is reported as a hang rather
            # than as a timeout of this harness
            self._proc.wait(timeout=30.0)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            self._proc.wait()
            raise TestFailure("LabRecorderCLI did not exit within 30 s of the stop request")
        duration = time.perf_counter() - started
        self._reader.join(timeout=5.0)
        check(
            self._proc.returncode == 0,
            f"LabRecorderCLI exited with status {self._proc.returncode}",
        )
        return duration

    def terminate(self):
        if self._proc.poll() is None:
            self._proc.kill()
            self._proc.wait()


@contextlib.contextmanager
def recorder(cli_path, xdf_path):
    """Run LabRecorderCLI over both test streams, making sure it is gone afterwards."""
    rec = Recorder(cli_path, xdf_path)
    try:
        yield rec
    finally:
        rec.terminate()
        for line in rec.lines:
            print(f"    | {line}")


# pyxdf reports a damaged file through its logger rather than by raising, so these are the
# substrings that mark a load as failed. Other warnings (about jitter or clock offsets, say) say
# something about the data, not about the file being intact, and are only printed.
INTEGRITY_WARNINGS = ("footer", "truncat", "corrupt", "incomplete", "unexpected", "not parse")


def load_xdf_strict(xdf_path):
    """Load an XDF file and fail if pyxdf reports it as damaged."""
    records = []

    class Collector(logging.Handler):
        def emit(self, record):
            records.append(record)

    handler = Collector(level=logging.WARNING)
    logger = logging.getLogger("pyxdf")
    logger.addHandler(handler)
    try:
        streams, header = pyxdf.load_xdf(xdf_path)
    finally:
        logger.removeHandler(handler)

    problems = []
    for record in records:
        message = record.getMessage()
        if record.levelno >= logging.ERROR or any(
            marker in message.lower() for marker in INTEGRITY_WARNINGS
        ):
            problems.append(message)
        else:
            print(f"    (pyxdf) {message}")
    if problems:
        raise TestFailure(f"pyxdf reported a damaged file: {'; '.join(problems)}")
    return streams, header


def stream_by_name(streams, name):
    for stream in streams:
        if stream["info"]["name"][0] == name:
            return stream
    raise TestFailure(f"stream {name!r} is missing from the recording")


def check_footer(stream):
    """Check that a stream carries a footer consistent with its data."""
    name = stream["info"]["name"][0]
    footer = stream.get("footer")
    check(footer and footer.get("info"), f"stream {name!r} has no footer")

    info = footer["info"]
    recorded = len(stream["time_series"])
    reported = int(info["sample_count"][0])
    check(
        reported == recorded,
        f"stream {name!r} footer claims {reported} samples but holds {recorded}",
    )
    # these are written from the same footer and must be parseable for dejittering to work
    float(info["first_timestamp"][0])
    float(info["last_timestamp"][0])


def check_stop(duration, max_stop):
    print(f"    teardown took {duration:.3f} s (budget {max_stop:.3f} s)")
    check(
        duration <= max_stop,
        f"teardown took {duration:.3f} s, which is above the {max_stop:.3f} s budget",
    )


def push_eeg_for(outlet, seconds):
    """Push EEG samples at the nominal rate for the given duration."""
    deadline = time.time() + seconds
    value = 0.0
    while time.time() < deadline:
        outlet.push_sample([value] * EEG_CHANNELS)
        value += 1.0
        time.sleep(1.0 / EEG_RATE)


def case_normal_stop(cli_path, xdf_path, max_stop):
    """Record both streams for two seconds, then stop."""
    eeg, markers = make_outlets()
    time.sleep(SETTLE)
    with recorder(cli_path, xdf_path) as rec:
        rec.wait_until_collecting()
        push_eeg_for(eeg, 2.0)
        time.sleep(SETTLE)
        duration = rec.stop()

    check_stop(duration, max_stop)
    streams, _ = load_xdf_strict(xdf_path)
    check(len(streams) == 2, f"expected 2 streams in the recording, got {len(streams)}")

    eeg_stream = stream_by_name(streams, EEG_NAME)
    check(len(eeg_stream["time_series"]) > 0, "no EEG samples were recorded")
    # the marker stream stays silent on purpose: a stream that never sends must still be
    # closed out properly
    check_footer(eeg_stream)
    check_footer(stream_by_name(streams, MARKER_NAME))
    del eeg, markers


def case_stop_before_first_sample(cli_path, xdf_path, max_stop):
    """Stop once the recorder is collecting but before either stream has sent anything."""
    eeg, markers = make_outlets()
    time.sleep(SETTLE)
    with recorder(cli_path, xdf_path) as rec:
        rec.wait_until_collecting()
        duration = rec.stop()

    check_stop(duration, max_stop)
    streams, _ = load_xdf_strict(xdf_path)
    check(len(streams) == 2, f"expected 2 streams in the recording, got {len(streams)}")
    for name in NAMES:
        stream = stream_by_name(streams, name)
        check(
            len(stream["time_series"]) == 0,
            f"stream {name!r} recorded samples although none were sent",
        )
        check_footer(stream)
    del eeg, markers


def case_stop_while_subscribing(cli_path, xdf_path, max_stop):
    """Stop while the recorder is still subscribing, so it is blocked on the network.

    The sources are dropped as soon as the recorder has found them, which leaves it waiting on
    an endpoint that will never answer -- the case the data receiver alone cannot unblock.
    """
    eeg, markers = make_outlets()
    time.sleep(SETTLE)
    with recorder(cli_path, xdf_path) as rec:
        rec.wait_for([f"Found {name}" for name in NAMES])
        del eeg, markers
        duration = rec.stop()

    check_stop(duration, max_stop)
    # the file is checked for integrity, but not for content: how far the recorder got before
    # the sources went away is timing dependent
    load_xdf_strict(xdf_path)


def case_repeated_shutdown(cli_path, xdf_path, max_stop):
    """Run three start/stop cycles, so leaked threads or stale state show up."""
    for cycle in range(3):
        print(f"  cycle {cycle + 1}/3")
        case_normal_stop(cli_path, xdf_path, max_stop)


def case_no_buffered_samples_lost(cli_path, xdf_path, max_stop):
    """Every marker pushed before the stop must end up in the file."""
    marker_count = 40
    eeg, markers = make_outlets()
    time.sleep(SETTLE)
    with recorder(cli_path, xdf_path) as rec:
        rec.wait_until_collecting()
        push_eeg_for(eeg, 1.0)
        for i in range(marker_count):
            markers.push_sample([f"marker-{i}"])
        # let the markers reach the recorder; whatever is still sitting in its inlet at the
        # stop has to be drained rather than dropped
        time.sleep(SETTLE)
        duration = rec.stop()

    check_stop(duration, max_stop)
    streams, _ = load_xdf_strict(xdf_path)
    marker_stream = stream_by_name(streams, MARKER_NAME)
    recorded = len(marker_stream["time_series"])
    check(
        recorded == marker_count,
        f"{marker_count} markers were sent but {recorded} were recorded",
    )
    check_footer(marker_stream)
    del eeg, markers


CASES = [
    ("normal stop", case_normal_stop),
    ("stop before first sample", case_stop_before_first_sample),
    ("stop while subscribing", case_stop_while_subscribing),
    ("repeated shutdown", case_repeated_shutdown),
    ("no buffered samples lost", case_no_buffered_samples_lost),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bin", required=True, help="path to the LabRecorderCLI binary")
    parser.add_argument(
        "--max-stop",
        type=float,
        default=1.0,
        help="upper bound in seconds for how long teardown may take (default: %(default)s)",
    )
    args = parser.parse_args()

    if not os.path.exists(args.bin):
        print(f"LabRecorderCLI binary not found at {args.bin!r}")
        return 1
    # absolute, and with native separators: CreateProcess does not accept a relative path
    # spelled with forward slashes
    cli_path = os.path.abspath(args.bin)

    failures = []
    with tempfile.TemporaryDirectory() as workdir:
        for name, case in CASES:
            xdf_path = os.path.join(workdir, f"{name.replace(' ', '_')}.xdf")
            print(f"--- {name} ---")
            try:
                case(cli_path, xdf_path, args.max_stop)
            except TestFailure as exc:
                print(f"FAIL: {name}: {exc}")
                failures.append(name)
            else:
                print(f"PASS: {name}")

    print()
    if failures:
        print(f"{len(failures)}/{len(CASES)} cases failed: {', '.join(failures)}")
        return 1
    print(f"all {len(CASES)} cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
