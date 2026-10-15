CXX      ?= clang++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

all: build_runs merge_runs

build_runs: build_runs.cpp dataset_reader.h tokenizer.h util.h ../index_format.h
	$(CXX) $(CXXFLAGS) -o $@ build_runs.cpp

merge_runs: merge_runs.cpp util.h ../config.h ../index_format.h ../varbyte.h
	$(CXX) $(CXXFLAGS) -o $@ merge_runs.cpp

clean:
	rm -f build_runs merge_runs
.PHONY: all clean
