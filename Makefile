# Trial build: two programs sharing the header-only common.h.
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

all: build query

build: build.cpp common.h
	$(CXX) $(CXXFLAGS) -o $@ build.cpp

query: query.cpp common.h
	$(CXX) $(CXXFLAGS) -o $@ query.cpp

clean:
	rm -f build query

.PHONY: all clean
