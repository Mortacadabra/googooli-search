CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

all: build dump query

%: %.cpp common.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f build dump query

.PHONY: all clean
