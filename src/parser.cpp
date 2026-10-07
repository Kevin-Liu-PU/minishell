#include "parser.hpp"

#include <cctype>
#include <utility>

namespace fsh {
namespace {

enum class Kind { word, pipe, input, output, append, error, error_append };
struct Token {
    Kind kind;
    std::string text;
};

std::vector<Token> tokenize(const std::string& line) {
    std::vector<Token> tokens;
    std::string word;
    bool started = false;
    char quote = 0;
    auto flush = [&] {
        if (started) {
            tokens.push_back({Kind::word, std::move(word)});
            word.clear();
            started = false;
        }
    };

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\0') throw ParseError("NUL bytes are not allowed");
        if (quote == '\'') {
            if (c == '\'') quote = 0;
            else word += c;
            continue;
        }
        if (c == '\\') {
            if (i + 1 == line.size()) throw ParseError("trailing backslash");
            const char next = line[i + 1];
            if (next == '\0') throw ParseError("NUL bytes are not allowed");
            if (!quote || next == '"' || next == '\\' || next == '$' || next == '`') {
                word += next;
                ++i;
            } else {
                word += c;
            }
            started = true;
            continue;
        }
        if (quote == '"') {
            if (c == '"') quote = 0;
            else word += c;
            continue;
        }
        if (!started && c >= '0' && c <= '9') {
            auto end = i;
            while (end < line.size() && line[end] >= '0' && line[end] <= '9') ++end;
            if (end < line.size() && (line[end] == '<' || line[end] == '>')) {
                const auto number = line.substr(i, end - i);
                const bool input = line[end] == '<';
                if ((input && number != "0") || (!input && number != "1" && number != "2"))
                    throw ParseError("unsupported redirection descriptor: " + number);
                i = end;
                const bool doubled = i + 1 < line.size() && line[i + 1] == line[i];
                if (input && doubled) throw ParseError("unsupported operator: <<");
                if (doubled) ++i;
                const auto kind = input ? Kind::input : number == "2"
                    ? (doubled ? Kind::error_append : Kind::error)
                    : (doubled ? Kind::append : Kind::output);
                tokens.push_back({kind, {}});
                continue;
            }
        }
        if (c == '\'' || c == '"') {
            quote = c;
            started = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            flush();
        } else if (c == '&' || c == ';' || c == '(' || c == ')' || c == '`') {
            throw ParseError(std::string("unsupported operator: ") + c);
        } else if (c == '<' || c == '>' || c == '|') {
            flush();
            Kind kind;
            if (c == '|') {
                if (i + 1 < line.size() && line[i + 1] == '|')
                    throw ParseError("unsupported operator: ||");
                kind = Kind::pipe;
            } else if (c == '<') {
                if (i + 1 < line.size() && line[i + 1] == '<')
                    throw ParseError("unsupported operator: <<");
                kind = Kind::input;
            } else {
                const bool append = i + 1 < line.size() && line[i + 1] == '>';
                if (append) ++i;
                kind = append ? Kind::append : Kind::output;
            }
            tokens.push_back({kind, {}});
        } else {
            word += c;
            started = true;
        }
    }
    if (quote) throw ParseError("unclosed quote");
    flush();
    return tokens;
}

} // namespace

Pipeline parse(const std::string& line) {
    if (line.size() > 65536) throw ParseError("line exceeds 65536 bytes");
    const auto tokens = tokenize(line);
    if (tokens.empty()) return {};
    Pipeline pipeline(1);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        auto& command = pipeline.back();
        if (token.kind == Kind::word) {
            if (command.args.size() == 256) throw ParseError("too many arguments (limit 256)");
            command.args.push_back(token.text);
        } else if (token.kind == Kind::pipe) {
            if (command.args.empty()) throw ParseError("empty command in pipeline");
            if (pipeline.size() == 32) throw ParseError("too many commands (limit 32)");
            pipeline.emplace_back();
        } else {
            if (++i == tokens.size() || tokens[i].kind != Kind::word)
                throw ParseError("redirection needs a filename");
            if (command.redirects.size() == 64) throw ParseError("too many redirections (limit 64)");
            const bool input = token.kind == Kind::input;
            const bool error = token.kind == Kind::error || token.kind == Kind::error_append;
            const bool append = token.kind == Kind::append || token.kind == Kind::error_append;
            command.redirects.push_back({input ? 0 : (error ? 2 : 1),
                input ? RedirectMode::read : (append ? RedirectMode::append : RedirectMode::truncate),
                tokens[i].text});
        }
    }
    if (pipeline.back().args.empty()) throw ParseError("command is missing");
    return pipeline;
}

} // namespace fsh
