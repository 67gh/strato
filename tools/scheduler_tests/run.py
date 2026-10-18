#!/usr/bin/env python3
"""Host regression harness: real scheduler methods, simulated guest signals/ticks.

Requires Python 3 and g++ (C++20). Does not build or execute the Android emulator.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "app/src/main/cpp/skyline"
HERE = Path(__file__).resolve().parent

def function(source, name):
    match = re.search(r"^    [^\n]*Scheduler::" + name + r"\(", source, re.M)
    if not match:
        raise RuntimeError("Missing production method: " + name)
    end = source.index("\n    }", match.start()) + len("\n    }")
    return source[match.start():end]

def main():
    source = (NATIVE / "kernel/scheduler.cpp").read_text()
    header = (NATIVE / "kernel/types/KThread.h").read_text()
    condition_type = re.search(r"(\S+) scheduleCondition;", header).group(1)
    spin = (NATIVE / "common/spin_lock.h").read_text()
    begin = spin.index("    class SpinLock {")
    end = spin.index("\n    };", begin) + len("\n    };")
    methods = ["GetOptimalCoreForThread", "YieldThread", "InsertThread", "MigrateToCore",
               "WaitSchedule", "TimedWaitSchedule", "Rotate", "RemoveThread"]
    template = (HERE / "scheduler_host.cpp").read_text()
    generated = template.replace("// @PRODUCTION_SPIN_LOCK@", spin[begin:end])
    generated = generated.replace("@SCHEDULE_CONDITION_TYPE@", condition_type)
    generated = generated.replace("// @PRODUCTION_METHODS@", "\n".join(function(source, n) for n in methods))
    with tempfile.TemporaryDirectory(prefix="strato-scheduler-") as directory:
        directory = Path(directory)
        cpp = directory / "scheduler_host.cpp"
        exe = directory / "scheduler_host"
        cpp.write_text(generated)
        subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-O1", "-g", "-pthread",
                        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-fsanitize=undefined", "-fno-sanitize-recover=all",
                        "-I", str(NATIVE), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)

if __name__ == "__main__":
    main()
