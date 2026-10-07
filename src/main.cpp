#include "parser.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

volatile sig_atomic_t interrupted = 0;
volatile sig_atomic_t active_group = 0;
void on_interrupt(int) { interrupted = 1; }
void forward_signal(int signal) {
    const int saved = errno;
    if (active_group > 0) kill(-active_group, signal);
    errno = saved;
}

bool set_signal(int signal, void (*handler)(int)) {
    struct sigaction action {};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    return sigaction(signal, &action, nullptr) == 0;
}

bool print(int fd, const std::string& text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto n = write(fd, text.data() + offset, text.size() - offset);
        if (n > 0) offset += static_cast<std::size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

void error(const std::string& message) { print(STDERR_FILENO, "fsh: " + message + "\n"); }
void system_error(const std::string& operation) {
    const int saved = errno;
    error(operation + ": " + std::strerror(saved));
}

bool redirect(const fsh::Command& command) {
    for (const auto& item : command.redirects) {
        const int flags = item.mode == fsh::RedirectMode::read ? O_RDONLY
            : O_WRONLY | O_CREAT | (item.mode == fsh::RedirectMode::append ? O_APPEND : O_TRUNC);
        const int fd = open(item.path.c_str(), flags, 0666);
        if (fd < 0) { system_error(item.path); return false; }
        if (fd != item.fd) {
            if (dup2(fd, item.fd) < 0) {
                system_error("dup2");
                close(fd);
                return false;
            }
            close(fd);
        }
    }
    return true;
}

class SavedDescriptors {
public:
    bool capture(const fsh::Command& command) {
        for (const auto& item : command.redirects) {
            if (touched_[item.fd]) continue;
            saved_[item.fd] = fcntl(item.fd, F_DUPFD_CLOEXEC, 10);
            if (saved_[item.fd] < 0 && errno != EBADF) {
                system_error("save descriptor");
                return false;
            }
            touched_[item.fd] = true;
        }
        return true;
    }
    ~SavedDescriptors() {
        for (int fd = 0; fd != 3; ++fd) {
            if (!touched_[fd]) continue;
            if (saved_[fd] < 0) close(fd);
            else {
                if (dup2(saved_[fd], fd) < 0) system_error("restore descriptor");
                close(saved_[fd]);
            }
        }
    }
private:
    std::array<int, 3> saved_ {{-1, -1, -1}};
    std::array<bool, 3> touched_ {{false, false, false}};
};

class Terminal {
public:
    bool initialize() {
        if (!set_signal(SIGCHLD, SIG_DFL)) return false;
        interactive_ = isatty(STDIN_FILENO);
        if (interactive_) {
            if (!set_signal(SIGTTIN, SIG_DFL)) return false;
            while (true) {
                const pid_t foreground = tcgetpgrp(STDIN_FILENO);
                if (foreground < 0) return false;
                if (foreground == getpgrp()) break;
                if (kill(-getpgrp(), SIGTTIN) < 0) return false;
            }
            if (getpgrp() != getpid() && setpgid(0, 0) < 0) return false;
            group_ = getpgrp();
            if (!set_signal(SIGTTOU, SIG_IGN) || !set_signal(SIGTTIN, SIG_IGN) ||
                !set_signal(SIGTSTP, SIG_IGN)) return false;
            if (tcsetpgrp(STDIN_FILENO, group_) < 0 || tcgetattr(STDIN_FILENO, &settings_) < 0)
                return false;
        }
        return set_signal(SIGINT, on_interrupt) && set_signal(SIGQUIT, SIG_IGN) &&
               set_signal(SIGPIPE, SIG_IGN);
    }
    bool interactive() const { return interactive_; }
    bool give_to(pid_t group) const {
        return !interactive_ || tcsetpgrp(STDIN_FILENO, group) == 0;
    }
    void reclaim() const {
        if (!interactive_) return;
        if (tcsetpgrp(STDIN_FILENO, group_) < 0) system_error("restore foreground group");
        if (tcsetattr(STDIN_FILENO, TCSADRAIN, &settings_) < 0) system_error("restore terminal settings");
    }
private:
    bool interactive_ = false;
    pid_t group_ = 0;
    struct termios settings_ {};
};

bool is_builtin(const std::string& name) {
    return name == "cd" || name == "pwd" || name == "exit";
}

int builtin(const fsh::Command& command, int previous_status, bool& exit_requested) {
    SavedDescriptors descriptors;
    if (!descriptors.capture(command) || !redirect(command)) return 1;
    const auto& args = command.args;
    if (args[0] == "pwd") {
        if (args.size() != 1) { error("usage: pwd"); return 2; }
        std::vector<char> buffer(256);
        while (!getcwd(buffer.data(), buffer.size())) {
            if (errno != ERANGE) { system_error("pwd"); return 1; }
            buffer.resize(buffer.size() * 2);
        }
        if (!print(STDOUT_FILENO, std::string(buffer.data()) + "\n")) {
            system_error("pwd: write");
            return 1;
        }
        return 0;
    }
    if (args[0] == "cd") {
        if (args.size() > 2) { error("usage: cd [directory]"); return 2; }
        const char* path = args.size() == 2 ? args[1].c_str() : std::getenv("HOME");
        if (!path) { error("HOME is not set"); return 1; }
        if (chdir(path) < 0) { system_error("cd"); return 1; }
        return 0;
    }
    int status = previous_status;
    if (args.size() > 2) { error("usage: exit [0..255]"); return 2; }
    if (args.size() == 2) {
        if (args[1].empty() || args[1].find_first_not_of("0123456789") != std::string::npos) {
            error("exit status must be an integer from 0 to 255");
            return 2;
        }
        errno = 0;
        char* end = nullptr;
        const auto value = std::strtol(args[1].c_str(), &end, 10);
        if (errno || *end || value > 255) { error("exit status must be from 0 to 255"); return 2; }
        status = static_cast<int>(value);
    }
    exit_requested = true;
    return status;
}

using Pipe = std::array<int, 2>;
bool make_pipe(Pipe& pair) {
    if (pipe(pair.data()) < 0) return false;
    for (int& fd : pair) {
        if (fd >= 3) continue;
        const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (replacement < 0) {
            const int saved = errno;
            close(pair[0]); close(pair[1]);
            errno = saved;
            return false;
        }
        close(fd);
        fd = replacement;
    }
    return true;
}
void close_pipes(const std::vector<Pipe>& pipes) {
    for (const auto& pipe : pipes) { close(pipe[0]); close(pipe[1]); }
}

[[noreturn]] void child(const fsh::Pipeline& pipeline, std::size_t index,
                        const std::vector<Pipe>& pipes, Pipe gate, pid_t group,
                        const sigset_t& original_mask) {
    if (setpgid(0, group) < 0) { system_error("setpgid"); _exit(1); }
    for (int signal : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE}) {
        if (!set_signal(signal, SIG_DFL)) { system_error("sigaction"); _exit(1); }
    }
    close(gate[1]);
    char token;
    ssize_t count;
    do { count = read(gate[0], &token, 1); } while (count < 0 && errno == EINTR);
    close(gate[0]);
    if (count < 0) _exit(1);
    if (sigprocmask(SIG_SETMASK, &original_mask, nullptr) < 0) _exit(1);
    if (index > 0 && dup2(pipes[index - 1][0], STDIN_FILENO) < 0) {
        system_error("pipe input"); _exit(1);
    }
    if (index + 1 < pipeline.size() && dup2(pipes[index][1], STDOUT_FILENO) < 0) {
        system_error("pipe output"); _exit(1);
    }
    close_pipes(pipes);
    if (!redirect(pipeline[index])) _exit(1);
    std::vector<char*> args;
    for (const auto& arg : pipeline[index].args) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    const int saved = errno;
    system_error(pipeline[index].args[0]);
    _exit(saved == ENOENT ? 127 : 126);
}

void kill_children(pid_t group, const std::vector<pid_t>& children) {
    if (group > 0) kill(-group, SIGKILL);
    for (pid_t pid : children) if (pid > 0) kill(pid, SIGKILL);
}

void abort_children(pid_t group, const std::vector<pid_t>& children) {
    kill_children(group, children);
    for (pid_t pid : children) {
        if (pid <= 0) continue;
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
}

int execute(const fsh::Pipeline& pipeline, const Terminal& terminal) {
    std::vector<Pipe> pipes;
    pipes.reserve(pipeline.size() - 1);
    for (std::size_t i = 1; i < pipeline.size(); ++i) {
        Pipe pair;
        if (!make_pipe(pair)) { system_error("pipe"); close_pipes(pipes); return 1; }
        pipes.push_back(pair);
    }
    Pipe gate;
    if (!make_pipe(gate)) { system_error("startup pipe"); close_pipes(pipes); return 1; }
    std::vector<pid_t> children;
    children.reserve(pipeline.size());
    pid_t group = 0;
    sigset_t blocked, original_mask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGQUIT);
    if (sigprocmask(SIG_BLOCK, &blocked, &original_mask) < 0) {
        system_error("block launch signals"); close_pipes(pipes); close(gate[0]); close(gate[1]); return 1;
    }
    set_signal(SIGINT, forward_signal);
    set_signal(SIGQUIT, forward_signal);
    bool failed = false;
    for (std::size_t i = 0; i < pipeline.size(); ++i) {
        const pid_t pid = fork();
        if (pid == 0) child(pipeline, i, pipes, gate, group, original_mask);
        if (pid < 0) { system_error("fork"); failed = true; break; }
        if (!group) { group = pid; active_group = group; }
        children.push_back(pid);
        if (setpgid(pid, group) < 0) { system_error("setpgid"); failed = true; break; }
    }
    close_pipes(pipes);
    close(gate[0]);
    if (!failed && !terminal.give_to(group)) { system_error("foreground group"); failed = true; }
    if (failed) abort_children(group, children);
    close(gate[1]); // Release children only after the terminal belongs to their group.
    if (failed) active_group = 0;
    sigprocmask(SIG_SETMASK, &original_mask, nullptr);
    int last_status = 1;
    if (!failed) {
        const pid_t last_child = children.back();
        std::size_t remaining = children.size();
        bool stopped = false;
        while (remaining) {
            int status = 0;
            const pid_t pid = waitpid(-1, &status, WUNTRACED);
            if (pid < 0 && errno == EINTR) continue;
            if (pid < 0) { system_error("waitpid"); abort_children(group, children); break; }
            const auto live = std::find(children.begin(), children.end(), pid);
            if (live == children.end()) continue;
            if (WIFSTOPPED(status)) {
                if (!stopped) error("stopped pipelines are terminated; job control is not supported");
                stopped = true;
                kill_children(group, children);
                continue;
            }
            *live = 0;
            --remaining;
            if (pid == last_child) {
                last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
        }
    }
    terminal.reclaim();
    active_group = 0;
    interrupted = 0;
    set_signal(SIGINT, on_interrupt);
    set_signal(SIGQUIT, SIG_IGN);
    return last_status;
}

enum class LineResult { line, eof, interrupted, too_long, failed };
LineResult read_line(std::string& line) {
    line.clear();
    bool too_long = false;
    while (true) {
        if (interrupted) { interrupted = 0; return LineResult::interrupted; }
        char c;
        const auto n = read(STDIN_FILENO, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return LineResult::failed;
        }
        if (n == 0 || c == '\n') {
            if (too_long) return LineResult::too_long;
            return n == 0 && line.empty() ? LineResult::eof : LineResult::line;
        }
        if (line.size() == 65536) too_long = true;
        if (!too_long) line += c;
    }
}

int run_line(const std::string& line, const Terminal& terminal, int previous, bool& exit_requested) {
    try {
        const auto pipeline = fsh::parse(line);
        if (pipeline.empty()) return previous;
        for (const auto& command : pipeline) {
            if (is_builtin(command.args[0])) {
                if (pipeline.size() != 1) { error("builtins must be used outside pipelines"); return 2; }
                return builtin(command, previous, exit_requested);
            }
        }
        return execute(pipeline, terminal);
    } catch (const fsh::ParseError& e) {
        error(e.what());
        return 2;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: fsh [-c command]\nA foreground POSIX shell. See README.md for syntax and limits.\n";
        return 0;
    }
    if (argc != 1 && !(argc == 3 && std::string(argv[1]) == "-c")) {
        error("usage: fsh [-c command]");
        return 2;
    }
    Terminal terminal;
    if (!terminal.initialize()) { system_error("initialize terminal/signals"); return 1; }
    bool exit_requested = false;
    if (argc == 3) return run_line(argv[2], terminal, 0, exit_requested);
    int status = 0;
    while (!exit_requested) {
        if (terminal.interactive()) print(STDERR_FILENO, "fsh$ ");
        std::string line;
        const auto result = read_line(line);
        if (result == LineResult::eof) break;
        if (result == LineResult::failed) { system_error("read"); return 1; }
        if (result == LineResult::interrupted) {
            status = 130;
            if (terminal.interactive()) print(STDERR_FILENO, "\n");
            continue;
        }
        if (result == LineResult::too_long) { error("line exceeds 65536 bytes"); status = 2; continue; }
        status = run_line(line, terminal, status, exit_requested);
    }
    return status;
}
