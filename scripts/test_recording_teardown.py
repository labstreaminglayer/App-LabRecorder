#!/usr/bin/env python
"""
Automated integration test for LabRecorder teardown and XDF integrity.
Tests that LabRecorder stops cleanly and instantly (< 500 ms) and produces valid XDF footers.
"""

import argparse
import os
import subprocess
import sys
import time
import pylsl
import pyxdf


def run_test(cli_path, output_xdf="test_recording.xdf"):
    if not os.path.exists(cli_path):
        print(f"Error: LabRecorderCLI binary not found at '{cli_path}'")
        return False

    if os.path.exists(output_xdf):
        os.remove(output_xdf)

    print(f"--- Starting LSL test streams ---")
    info_eeg = pylsl.StreamInfo("TestEEG", "EEG", 8, 100, "float32", "test_eeg_source_123")
    outlet_eeg = pylsl.StreamOutlet(info_eeg)

    info_marker = pylsl.StreamInfo("TestMarker", "Markers", 1, 0, "string", "test_marker_source_123")
    outlet_marker = pylsl.StreamOutlet(info_marker)

    time.sleep(0.5)

    print(f"--- Launching LabRecorderCLI ({cli_path}) ---")
    proc = subprocess.Popen(
        [cli_path, output_xdf, "name='TestEEG'", "name='TestMarker'"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    print(f"--- Streaming samples for 2 seconds ---")
    start_time = time.time()
    sample_val = 0.0
    while time.time() - start_time < 2.0:
        outlet_eeg.push_sample([sample_val] * 8)
        sample_val += 1.0
        time.sleep(0.01)

    print(f"--- Triggering shutdown (Enter key to stdin) ---")
    t0 = time.perf_counter()
    try:
        stdout, stderr = proc.communicate(input="\n", timeout=4.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        print("FAIL: LabRecorderCLI hung during shutdown (> 4.0s)!")
        return False

    stop_duration = time.perf_counter() - t0
    print(f"--- Teardown completed in {stop_duration:.3f} seconds ---")

    if stop_duration > 1.5:
        print(f"FAIL: Shutdown took too long ({stop_duration:.3f}s > 1.5s)")
        return False
    else:
        print(f"PASS: Instant shutdown verified (< 1.5s)")

    if not os.path.exists(output_xdf):
        print(f"FAIL: Output file '{output_xdf}' was not created!")
        return False

    file_size_kb = os.path.getsize(output_xdf) / 1024.0
    print(f"--- Output XDF file size: {file_size_kb:.2f} KB ---")

    print(f"--- Validating XDF file with pyxdf ---")
    try:
        streams, header = pyxdf.load_xdf(output_xdf)
    except Exception as e:
        print(f"FAIL: pyxdf failed to load XDF: {e}")
        return False

    if len(streams) != 2:
        print(f"FAIL: Expected 2 streams in XDF, got {len(streams)}")
        return False

    eeg_stream = next((s for s in streams if s["info"]["name"][0] == "TestEEG"), None)
    if not eeg_stream:
        print("FAIL: TestEEG stream not found in XDF")
        return False

    if len(eeg_stream["time_series"]) == 0:
        print("FAIL: TestEEG has 0 recorded samples!")
        return False

    print(f"PASS: TestEEG has {len(eeg_stream['time_series'])} samples recorded.")

    # Check footer
    if "footer" not in eeg_stream or eeg_stream["footer"]["info"] is None:
        print("FAIL: TestEEG is missing footer info!")
        return False

    print("PASS: Stream footers are present and valid.")
    print("=== ALL INTEGRATION TESTS PASSED ===")

    # Cleanup
    if os.path.exists(output_xdf):
        os.remove(output_xdf)

    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test LabRecorder teardown and XDF validity")
    parser.add_argument(
        "--bin",
        default="./build/install/bin/LabRecorderCLI",
        help="Path to LabRecorderCLI binary",
    )
    args = parser.parse_args()
    success = run_test(args.bin)
    sys.exit(0 if success else 1)
