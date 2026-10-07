# minishell

A small C++17 shell for studying command parsing, Unix file descriptors, and
foreground process control.

## What it does

The handwritten tokenizer and parser support quoted arguments, empty arguments,
backslash escapes, pipelines, and input/output redirection (`<`, `>`, `>>`,
`2>`, `2>>`). `cd`, `pwd`, and `exit` run as builtins outside pipelines.

The executor uses POSIX `fork`, `execvp`, pipes, and process groups. It hands the
terminal to each foreground pipeline, forwards interrupts when run without a
terminal, waits for its children, and restores terminal settings afterward. The
whole command is parsed before execution, so invalid syntax cannot partially run.

## Build and try it

On Linux or Ubuntu WSL, install a C++17 compiler, Make, and Python 3. The tests
also use standard Unix utilities such as `cat`, `tr`, `wc`, `sleep`, and `stty`.
There are no downloaded project dependencies.

```sh
make -j1
make -j1 test
make -j1 demo
./build/fsh
```

Run a single pipeline:

```sh
./build/fsh -c 'printf "alpha\nbeta\n" | tr a-z A-Z'
```

```text
ALPHA
BETA
```

The [demo](examples/demo.py) creates and removes its own temporary fixture files.
[Recorded output](examples/demo-output.txt) shows quoting, pipelines, redirection,
exit status, and syntax rejection. A video will be added when it is available.

CMake 3.16+ is also supported:

```sh
cmake -S . -B build-cmake
cmake --build build-cmake --parallel 1
ctest --test-dir build-cmake --output-on-failure
```

Native Windows builds run only the portable parser tests. Use Linux/WSL for the
executor. With a multi-configuration CMake generator, add `--config Debug` when
building and `-C Debug` when running CTest.

## Behavior and limits

- Each input line is one foreground pipeline. There is no background execution,
  job management, history, completion, or multiline input.
- Variables, wildcards, tilde, command substitution, and arithmetic are not
  expanded. Unsupported operators such as `;`, `&`, `&&`, `||`, `<<`, and `2>&1`
  are rejected when unquoted. Quoted operator characters are literal.
- Redirections run left to right, after pipeline connections. `printf kept >saved | cat` writes to `saved` and leaves the next command with empty input.
- Only stdin input and stdout/stderr output redirections are supported. Builtins
  cannot appear in a pipeline. A stopped pipeline is terminated because there is
  no facility to resume it.
- A pipeline returns its last command's status. Signal exits use `128 + signal`;
  parse/usage errors return 2, setup errors 1, missing commands 127, and other
  execution failures 126. There is no `pipefail` option.
- Input is limited to 65,536 bytes per line, 32 commands per pipeline, 256
  arguments per command, and 64 redirections per command.

This is a focused systems exercise, not a POSIX-conformance shell.

## Tests

The suite has 39 parser checks and 19 POSIX integration tests, including
pseudo-terminal tests for Ctrl-C, foreground input, terminal restoration, and
stopped-command cleanup. It also checks redirection order, exit status, SIGPIPE,
repeated pipelines, closed standard descriptors, inherited signal state, and
noninteractive interrupt forwarding.

Validated with Ubuntu WSL, GCC 13.3, and Python 3.12: all 39
parser checks and all 19 integration tests passed; the demo completed. Native
Windows parser checks also passed with GCC 15.2. Other POSIX platforms and forced
`fork` failure paths have not been tested.
