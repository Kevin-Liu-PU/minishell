"""POSIX execution checks; every subprocess and terminal read has a deadline."""
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest

SHELL = str(Path(sys.argv.pop(1)).resolve())
PROBE = Path(__file__).with_name("probe.py").resolve()


def quote(text):
    return "'" + str(text).replace("'", "'\\''") + "'"


class ExecutionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fsh-test-")
        self.cwd = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def run_command(self, command, *, script=False):
        return subprocess.run([SHELL] if script else [SHELL, "-c", command],
                              input=command if script else "", text=True,
                              capture_output=True, cwd=self.cwd, timeout=8)

    def probe(self, mode):
        return f"{quote(sys.executable)} {quote(PROBE)} {mode}"

    def test_quoted_arguments(self):
        result = self.run_command(self.probe("args") + " '' pre\"two words\"post a\\ b '$HOME' '*.txt'")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), ["", "pretwo wordspost", "a b", "$HOME", "*.txt"])

    def test_pipeline_and_input(self):
        (self.cwd / "input file").write_text("alpha\nbeta\n")
        result = self.run_command("cat < 'input file' | tr a-z A-Z | wc -l")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "2")

    def test_last_pipeline_status(self):
        self.assertEqual(self.run_command("false | true").returncode, 0)
        self.assertEqual(self.run_command("true | false").returncode, 1)
        self.assertEqual(self.run_command(self.probe("exit") + " 23").returncode, 23)

    def test_waits_for_child_that_changes_process_group(self):
        result = self.run_command("true | " + self.probe("new-session"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")

    def test_redirections_are_ordered(self):
        result = self.run_command("printf one >first >second\nprintf two >>second\n", script=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.cwd / "first").read_text(), "")
        self.assertEqual((self.cwd / "second").read_text(), "onetwo")
        result = self.run_command(self.probe("streams") + " >out 2>err\n" + self.probe("streams") + " >out 2>>err\n", script=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.cwd / "out").read_text(), "out\n")
        self.assertEqual((self.cwd / "err").read_text(), "err\nerr\n")

    def test_redirection_overrides_pipe(self):
        result = self.run_command("printf kept >saved | cat")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual((self.cwd / "saved").read_text(), "kept")

    def test_builtins_persist_and_restore_descriptors(self):
        (self.cwd / "child").mkdir()
        result = self.run_command("pwd >before\ncd child\npwd\ncd ..\npwd\nexit 9\nprintf unreachable\n", script=True)
        self.assertEqual(result.returncode, 9, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [str(self.cwd / "child"), str(self.cwd)])
        self.assertEqual((self.cwd / "before").read_text().strip(), str(self.cwd))
        result = self.run_command("pwd >missing/out\nprintf recovered\n", script=True)
        self.assertEqual(result.stdout, "recovered")
        self.assertIn("missing/out", result.stderr)

    def test_bad_builtin_does_not_exit(self):
        result = self.run_command("exit nope\nprintf recovered\nexit\n", script=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "recovered")
        self.assertEqual(self.run_command("pwd | cat").returncode, 2)

    @unittest.skipUnless(Path("/dev/full").exists(), "/dev/full unavailable")
    def test_builtin_write_failure(self):
        result = self.run_command("pwd >/dev/full")
        self.assertEqual(result.returncode, 1)
        self.assertIn("pwd: write", result.stderr)

    def test_syntax_validated_before_effects(self):
        result = self.run_command("printf bad >should-not-exist |")
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.cwd / "should-not-exist").exists())
        self.assertEqual(self.run_command("printf bad && true").returncode, 2)

    def test_failures_have_status(self):
        self.assertEqual(self.run_command("command-that-does-not-exist-fsh").returncode, 127)
        (self.cwd / "not-executable").write_text("hello")
        self.assertEqual(self.run_command("./not-executable").returncode, 126)
        self.assertEqual(self.run_command("cat <missing").returncode, 1)
        self.assertEqual(self.run_command("false\nexit\n", script=True).returncode, 1)

    def test_sigpipe_and_repeated_pipelines(self):
        result = self.run_command("yes | head -n 1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "y\n")
        result = self.run_command("printf x | cat\n" * 80, script=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "x" * 80)

    def test_oversize_line_recovers(self):
        result = self.run_command("x" * 65537 + "\nprintf recovered\n", script=True)
        self.assertEqual(result.stdout, "recovered")
        self.assertIn("65536", result.stderr)

    def test_closed_inherited_standard_descriptors(self):
        for fd in (0, 1, 2):
            with self.subTest(fd=fd):
                result = subprocess.run([SHELL, "-c", "printf kept | cat >saved"],
                                        stdin=subprocess.DEVNULL, capture_output=True,
                                        cwd=self.cwd, timeout=8,
                                        preexec_fn=lambda: os.close(fd))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((self.cwd / "saved").read_text(), "kept")

    def test_noninteractive_signal_forwarding(self):
        process = subprocess.Popen([SHELL, "-c", self.probe("wait")],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, cwd=self.cwd, text=True)
        try:
            self.assertTrue(select.select([process.stdout], [], [], 5)[0], "child not ready")
            self.assertEqual(process.stdout.readline(), "ready\n")
            os.kill(process.pid, signal.SIGINT)
            process.communicate(timeout=5)
            self.assertEqual(process.returncode, 130)
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)

    def test_inherited_ignored_sigchld(self):
        result = subprocess.run(
            [SHELL, "-c", "printf ready | cat"],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            cwd=self.cwd, timeout=8,
            preexec_fn=lambda: signal.signal(signal.SIGCHLD, signal.SIG_IGN),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "ready")
        self.assertEqual(result.stderr, "")


@unittest.skipUnless(os.name == "posix", "POSIX terminal required")
class TerminalTests(unittest.TestCase):
    def setUp(self):
        import pty
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(SHELL, [SHELL])
        self.buffer = b""
        self.read_until(b"fsh$ ")

    def tearDown(self):
        try:
            foreground = os.tcgetpgrp(self.fd)
            if foreground > 0 and foreground != self.pid:
                os.killpg(foreground, signal.SIGKILL)
            os.write(self.fd, b"exit\n")
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                if os.waitpid(self.pid, os.WNOHANG)[0]:
                    return
                time.sleep(0.02)
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except (ProcessLookupError, ChildProcessError, OSError):
            pass
        finally:
            os.close(self.fd)

    def read_until(self, marker):
        deadline = time.monotonic() + 5
        while marker not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], max(0, remaining))[0]:
                self.fail(f"terminal timed out waiting for {marker!r}: {self.buffer!r}")
            chunk = os.read(self.fd, 4096)
            if not chunk:
                self.fail("terminal closed")
            self.buffer += chunk
        end = self.buffer.index(marker) + len(marker)
        result, self.buffer = self.buffer[:end], self.buffer[end:]
        return result

    def wait_for_child_foreground(self):
        deadline = time.monotonic() + 5
        while os.tcgetpgrp(self.fd) == self.pid:
            if time.monotonic() >= deadline:
                self.fail("pipeline did not take foreground terminal ownership")
            time.sleep(0.01)

    def test_ctrl_c_pipeline_and_prompt_recovery(self):
        os.write(self.fd, b"sleep 30 | cat\n")
        self.wait_for_child_foreground()
        os.write(self.fd, b"\x03")
        self.read_until(b"fsh$ ")
        self.assertEqual(os.tcgetpgrp(self.fd), self.pid)
        os.write(self.fd, b"printf ready\n")
        output = self.read_until(b"fsh$ ")
        self.assertIn(b"\r\nreadyfsh$ ", output)
        os.write(self.fd, b"\x03")
        self.read_until(b"fsh$ ")

    def test_foreground_stdin_and_terminal_restore(self):
        import termios
        before = termios.tcgetattr(self.fd)
        os.write(self.fd, b"cat\n")
        self.wait_for_child_foreground()
        os.write(self.fd, b"read-from-terminal\n")
        self.read_until(b"read-from-terminal\r\nread-from-terminal\r\n")
        os.write(self.fd, b"\x04")
        self.read_until(b"fsh$ ")
        os.write(self.fd, b"stty -echo\n")
        self.read_until(b"fsh$ ")
        self.assertEqual(termios.tcgetattr(self.fd), before)

    def test_stopped_command_does_not_strand_terminal(self):
        os.write(self.fd, b"sleep 30\n")
        self.wait_for_child_foreground()
        os.write(self.fd, b"\x1a")
        output = self.read_until(b"fsh$ ")
        self.assertIn(b"stopped pipelines are terminated", output)
        self.assertEqual(os.tcgetpgrp(self.fd), self.pid)


if __name__ == "__main__":
    unittest.main(verbosity=2)
