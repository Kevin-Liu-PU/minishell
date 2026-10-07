#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace fsh {

enum class RedirectMode { read, truncate, append };

struct Redirect {
    int fd;
    RedirectMode mode;
    std::string path;
};

struct Command {
    std::vector<std::string> args;
    std::vector<Redirect> redirects;
};

using Pipeline = std::vector<Command>;

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// An empty or whitespace-only line produces an empty pipeline.
Pipeline parse(const std::string& line);

} // namespace fsh
