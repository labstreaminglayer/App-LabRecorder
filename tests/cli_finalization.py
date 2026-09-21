"""Exercise process exit with the real CLI/recorder and a controlled LSL inlet."""
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time


def run_case(binary, directory, stalled):
    path = Path(directory) / ("stalled.xdf" if stalled else "complete.xdf")
    env = dict(os.environ)
    if stalled:
        env["LSL_TEST_STALL"] = "1"
    else:
        env.pop("LSL_TEST_STALL", None)
    proc = subprocess.Popen(
        [binary, "--stop-timeout", "0.5" if stalled else "5", str(path), "test"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, env=env,
    )
    lines = queue.Queue()
    output = []

    def read():
        for line in proc.stdout:
            output.append(line)
            lines.put(line)

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    try:
        needle = "TEST: offset query stalled" if stalled else "Started data collection"
        deadline = time.monotonic() + 12
        while True:
            line = lines.get(timeout=max(0.01, deadline - time.monotonic()))
            if needle in line:
                break
        start = time.monotonic()
        proc.stdin.write("\n")
        proc.stdin.flush()
        proc.wait(timeout=5)
        reader.join(timeout=1)
        elapsed = time.monotonic() - start
        assert proc.returncode == (3 if stalled else 0), "".join(output)
        assert elapsed < (1.5 if stalled else 4), f"CLI did not honor its timeout: {elapsed}"
        data = path.read_bytes()
        assert data.startswith(b"XDF:"), "no recoverable XDF header was flushed"
        if stalled:
            assert "Finalization timed out" in "".join(output)
            # A permanently stalled worker cannot close the writer. Periodic checkpoints
            # must nevertheless have preserved complete header and sample chunks.
            tags = []
            pos = 4
            while pos < len(data):
                width = data[pos]
                assert width in (1, 4, 8)
                length = int.from_bytes(data[pos + 1:pos + 1 + width], "little")
                pos += 1 + width
                assert pos + length <= len(data), "checkpoint ended inside an XDF chunk"
                tags.append(int.from_bytes(data[pos:pos + 2], "little"))
                pos += length
            assert 2 in tags and 3 in tags, "recorded samples were not checkpointed"
        else:
            assert b"</clock_offsets></info>" in data, "success reported before footer flush"
        print(f"{'stalled' if stalled else 'normal'}: exit={proc.returncode}, {elapsed:.3f}s")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


with tempfile.TemporaryDirectory() as directory:
    run_case(sys.argv[1], directory, False)
    run_case(sys.argv[1], directory, True)
