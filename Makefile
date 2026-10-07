CXX ?= c++
CPPFLAGS ?= -Iinclude
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
PYTHON ?= python3

.PHONY: all test demo clean
all: build/fsh build/parser_tests

build:
	mkdir -p build

build/fsh: src/main.cpp src/parser.cpp include/parser.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/main.cpp src/parser.cpp $(LDFLAGS) -o $@

build/parser_tests: tests/parser_tests.cpp src/parser.cpp include/parser.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/parser_tests.cpp src/parser.cpp $(LDFLAGS) -o $@

test: all
	./build/parser_tests
	$(PYTHON) tests/integration.py ./build/fsh

demo: build/fsh
	$(PYTHON) examples/demo.py ./build/fsh

clean:
	rm -f build/fsh build/parser_tests
