# C++ port of the part-1 run builder.
CXX      ?= clang++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

build_runs: build_runs.cpp dataset_reader.h tokenizer.h
	$(CXX) $(CXXFLAGS) -o $@ build_runs.cpp

clean:
	rm -f build_runs
.PHONY: clean
