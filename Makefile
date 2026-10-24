ifeq ($(OS),Windows_NT)
EXE := .exe
endif

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

BIN := cpp/harvest$(EXE)
TESTBIN := cpp/test_harvest$(EXE)

.PHONY: all test python-test cpp-test demo gui clean

all: $(BIN)

$(BIN): cpp/harvest.cpp
	$(CXX) $(CXXFLAGS) -o $@ cpp/harvest.cpp

$(TESTBIN): cpp/harvest.cpp cpp/test_harvest.cpp
	$(CXX) $(CXXFLAGS) -o $@ cpp/test_harvest.cpp

python-test:
	python -m unittest discover -s python -p "test_*.py"

cpp-test: $(TESTBIN)
	$(TESTBIN)

test: python-test cpp-test

demo:
	python python/harvest.py --recipe recipes/authors.json --out results/authors.txt

gui:
	python python/harvest_gui.py

clean:
	-rm -f $(BIN) $(TESTBIN)
