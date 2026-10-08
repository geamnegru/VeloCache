CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
SOURCES = main.cpp storage.cpp client_handler.cpp event_loop.cpp server.cpp resp.cpp background_worker.cpp
HEADERS = storage.h client_handler.h event_loop.h server.h resp.h background_worker.h

.PHONY: all test clean

all: velocache

velocache: $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@

storage_test: tests/storage_test.cpp storage.cpp storage.h
	$(CXX) $(CXXFLAGS) -I. tests/storage_test.cpp storage.cpp -o $@

resp_test: tests/resp_test.cpp resp.cpp resp.h client_handler.cpp client_handler.h storage.cpp storage.h
	$(CXX) $(CXXFLAGS) -I. tests/resp_test.cpp resp.cpp client_handler.cpp storage.cpp -o $@

slow_fsync.dylib: tests/slow_fsync.cpp
	$(CXX) -std=c++17 -Wall -Wextra -Wpedantic -dynamiclib $< -o $@

background_worker_test: tests/background_worker_test.cpp background_worker.cpp background_worker.h
	$(CXX) $(CXXFLAGS) -I. tests/background_worker_test.cpp background_worker.cpp -o $@

test: velocache storage_test resp_test background_worker_test slow_fsync.dylib
	./storage_test
	./resp_test
	./background_worker_test
	python3 tests/integration.py ./velocache
	python3 tests/resp_integration.py ./velocache
	python3 tests/background_io.py ./velocache ./slow_fsync.dylib

clean:
	rm -f velocache storage_test resp_test background_worker_test slow_fsync.dylib
