# ptx-opt Makefile  (make / make test / make clean)
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CPPFLAGS += -Iinclude
LIBSRC   := $(filter-out src/main.cpp,$(wildcard src/*.cpp))
LIBOBJ   := $(LIBSRC:src/%.cpp=build/%.o)
BIN      := build/ptx-opt
TESTBIN  := build/ptx-opt-tests

all: $(BIN)

build/%.o: src/%.cpp $(wildcard include/ptxopt/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

$(BIN): $(LIBOBJ) build/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

build/test_main.o: tests/test_main.cpp $(wildcard include/ptxopt/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

$(TESTBIN): $(LIBOBJ) build/test_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(TESTBIN) $(BIN)
	./$(TESTBIN) samples

# Optional: needs the CUDA toolkit (driver API). Runs original vs optimised PTX on a real GPU.
gpu-verify: verify/run_ptx.cpp
	@mkdir -p build
	$(CXX) -std=c++17 -O2 $< -o build/run_ptx -I$(CUDA_HOME)/include -L$(CUDA_HOME)/lib64 -lcuda

clean:
	rm -rf build

.PHONY: all test clean gpu-verify
