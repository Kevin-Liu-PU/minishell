#include "parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << label << '\n'; std::exit(1); }
}
void rejects(const std::string& input) {
    try { fsh::parse(input); }
    catch (const fsh::ParseError&) { ++checks; return; }
    check(false, "should reject: " + input);
}
}

int main() {
    check(fsh::parse(" \t").empty(), "blank line");
    const auto words = fsh::parse(R"(echo plain 'two words' "" pre"mid"'post' a\ b 'x|y' \|)");
    check(words.size() == 1, "one command");
    check(words[0].args == std::vector<std::string>{"echo", "plain", "two words", "", "premidpost", "a b", "x|y", "|"}, "quote joining and empty argument");
    check(fsh::parse(R"(echo "a\qb\"c\\d\$e" '$HOME' *.cpp)")[0].args ==
          std::vector<std::string>{"echo", "a\\qb\"c\\d$e", "$HOME", "*.cpp"}, "double quote escapes and literal expansion characters");
    const auto pipeline = fsh::parse("<in cat | tr a-z A-Z >first >>second 2>error 2>>more");
    check(pipeline.size() == 2, "pipeline size");
    check(pipeline[0].redirects[0].fd == 0, "input descriptor");
    check(pipeline[1].redirects.size() == 4, "redirection count");
    check(pipeline[1].redirects[0].path == "first" && pipeline[1].redirects[1].path == "second", "redirection order");
    check(pipeline[1].redirects[1].mode == fsh::RedirectMode::append, "append mode");
    check(pipeline[1].redirects[2].fd == 2 && pipeline[1].redirects[3].mode == fsh::RedirectMode::append, "stderr modes");
    check(fsh::parse("echo a2>out")[0].args == std::vector<std::string>{"echo", "a2"}, "embedded digit is a word");
    check(fsh::parse("echo '2'>out")[0].args == std::vector<std::string>{"echo", "2"}, "quoted digit is a word");
    check(fsh::parse("cat 0<in 1>out 1>>more")[0].redirects.size() == 3, "explicit stdin and stdout descriptors");
    rejects("echo x 3>file");
    rejects("echo x 22>file");
    rejects("echo x 2<file");
    for (const std::string input : {"|cat", "cat|", "cat||cat", "cat | | cat", "echo >", "cat < | cat", ">out", "echo 'x", "echo \"x", "echo \\", "a && b", "a &", "a;b", "cat <<x", "echo `pwd`", "echo $(pwd)", "cat 2>&1"}) rejects(input);
    rejects(std::string("echo x\0y", 8));
    rejects(std::string(65537, 'a'));
    std::string many = "echo";
    for (int i = 0; i < 256; ++i) many += " x";
    rejects(many);
    many = "cat";
    for (int i = 0; i < 32; ++i) many += " | cat";
    rejects(many);
    many = "echo";
    for (int i = 0; i < 65; ++i) many += " >x";
    rejects(many);
    check(fsh::parse(std::string(65536, 'a'))[0].args[0].size() == 65536, "line length boundary");
    std::cout << checks << " parser checks passed\n";
}
