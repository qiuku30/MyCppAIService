#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys

failures = 0
for script in ('helper_extended.py', 'handlers.py', 'curl_stream.py'):
    result = subprocess.run([sys.executable, str(Path(__file__).with_name(script))])
    failures += result.returncode != 0
raise SystemExit(1 if failures else 0)
