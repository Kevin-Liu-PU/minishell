"""Run a deterministic demo in a disposable directory."""
from pathlib import Path
import subprocess
import sys
import tempfile

shell = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="fsh-demo-") as directory:
    Path(directory, "notes.txt").write_text("pipes\nquotes\nsignals\n")
    cases = [
        ("Quoted and empty arguments", "printf '<%s>\\n' pre\"two words\"post ''"),
        ("Pipeline with file input", "cat <notes.txt | tr a-z A-Z"),
        ("Output redirection", "printf saved >result.txt"),
        ("Read redirected output", "cat result.txt"),
        ("Last command determines status", "false | true"),
        ("Syntax failure before execution", "printf never >untouched |"),
    ]
    for title, command in cases:
        result = subprocess.run([shell, "-c", command], cwd=directory,
                                text=True, capture_output=True, timeout=5)
        print(f"{title}\n$ {command}")
        if result.stdout:
            print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
        if result.stderr:
            print(result.stderr, end="")
        print(f"status: {result.returncode}\n")
    if Path(directory, "untouched").exists():
        raise SystemExit("invalid syntax created a file")
