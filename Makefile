CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra
CORE = src/storage.cpp src/btree.cpp src/database.cpp src/torture.cpp
WASI_SDK ?= /opt/wasi-sdk

ifeq ($(OS),Windows_NT)
  EXE = .exe
endif

all: granite$(EXE)

granite$(EXE): $(CORE) src/main.cpp src/*.h
	$(CXX) $(CXXFLAGS) $(CORE) src/main.cpp -o $@

run-tests$(EXE): $(CORE) tests/tests.cpp src/*.h
	$(CXX) $(CXXFLAGS) $(CORE) tests/tests.cpp -o $@

test: run-tests$(EXE)
	./run-tests$(EXE)

diff-runner$(EXE): $(CORE) tests/diff_runner.cpp src/*.h
	$(CXX) $(CXXFLAGS) $(CORE) tests/diff_runner.cpp -o $@

# compare thousands of random queries against SQLite (needs python3)
differential: diff-runner$(EXE)
	python3 tests/differential.py 1 100

torture: granite$(EXE)
	./granite$(EXE) --torture 2000

# The browser build: WebAssembly via the WASI SDK (https://github.com/WebAssembly/wasi-sdk)
wasm: web/granite.wasm
web/granite.wasm: $(CORE) src/wasm.cpp src/*.h
	$(WASI_SDK)/bin/clang++ --target=wasm32-wasip1 -std=c++20 -O2 -fno-exceptions -mexec-model=reactor \
	  $(CORE) src/wasm.cpp -o $@ -Wl,--strip-all

clean:
	rm -f granite granite.exe run-tests run-tests.exe diff-runner diff-runner.exe web/granite.wasm

.PHONY: all test differential torture wasm clean
