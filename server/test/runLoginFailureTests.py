#!/usr/bin/env python3
"""Build the isolated login protocol and blocking-request cleanup tests."""
from pathlib import Path
import shutil
import subprocess
import tempfile

server = Path(__file__).resolve().parents[1]
compiler = shutil.which("clang++") or shutil.which("g++")
if compiler is None:
    raise SystemExit("C++ compiler required")

with tempfile.TemporaryDirectory(prefix="login-failure-tests-") as scratch:
    for source, flags in (
        ("testLoginFailure.cpp", []),
        ("testTicketAuthRequests.cpp", ["-pthread", "-I", str(server / "test/fixtures")]),
    ):
        executable = str(Path(scratch) / Path(source).stem)
        subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        *flags, str(server / "test" / source), "-o", executable],
                       check=True)
        subprocess.run([executable], check=True, timeout=10)
        print("PASS", source)
