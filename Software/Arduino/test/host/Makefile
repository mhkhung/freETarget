# Host unit tests for the Arduino firmware, on a computer with a C++
# compiler: make
#
#   make                       build and run the tests
#   make SKETCH=<folder>       test another copy of the sketch
#   make clean                 remove the test binaries

CXX    ?= c++
SKETCH ?= ../../freETarget
CXXFLAGS := -std=c++11 -g -I. -Istub -I$(SKETCH) -fsanitize=address,undefined -w

.PHONY: test clean

test: test_json test_compute_hit
	./test_json
	./test_compute_hit

test_json: test_json.cpp arduino_stub.h extract.py $(wildcard $(SKETCH)/*.ino $(SKETCH)/*.h)
	python3 extract.py $(SKETCH) > json_host.inc
	$(CXX) $(CXXFLAGS) -o $@ test_json.cpp

test_compute_hit: test_compute_hit.cpp arduino_stub.h extract.py $(wildcard $(SKETCH)/*.ino $(SKETCH)/*.h)
	python3 extract.py $(SKETCH) compute_hit > compute_hit_host.inc
	$(CXX) $(CXXFLAGS) -o $@ test_compute_hit.cpp

clean:
	rm -rf test_json test_json.dSYM json_host.inc test_compute_hit test_compute_hit.dSYM compute_hit_host.inc
