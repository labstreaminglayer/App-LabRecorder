"""A GUI with permanently pending completion must honor confirmed Force quit."""
import subprocess
import sys

result = subprocess.run([sys.argv[1], "--force-quit"], timeout=20)
assert result.returncode == 3, f"expected forced-exit status 3, got {result.returncode}"
