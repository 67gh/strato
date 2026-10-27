#!/usr/bin/env python3
"""Real NVDEC/VIC decode and conversion tests, replacing unsupported-engine assertions."""
from pathlib import Path
import subprocess,sys
root=Path(__file__).resolve().parents[2]
subprocess.run([str(root/'tools/video_tests/run.sh'),*sys.argv[1:]],check=True)
