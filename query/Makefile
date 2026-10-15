CXX      ?= clang++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

all: query

query: query.cpp ../indexer/tokenizer.h ../indexer/util.h ../config.h ../index_format.h ../varbyte.h
	$(CXX) $(CXXFLAGS) -o $@ query.cpp

clean:
	rm -f query
.PHONY: all clean
