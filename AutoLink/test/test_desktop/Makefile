# AutoLink desktop (host) test build.
#
# Runs the pure-logic unit suites and the protocol/integration suite on your
# PC — no ESP32 required. The hardware abstraction (ILink) is mocked in
# test.cpp, so the entire protocol stack compiles and runs natively.
#
#   make            build and run every suite
#   make clean      remove the built binaries
#
# Sources live in ../../src (core) and ../../src/util (utilities); both are on
# the include path so headers resolve by the same names the firmware uses.

CXX      = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -DAUTOLINK_HOST_TEST -I../../src -I../../src/util
SRC      = ../../src
UTIL     = ../../src/util
LINK_SRC = $(SRC)/ALink.cpp $(SRC)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp $(UTIL)/UtilFrameRx.cpp $(UTIL)/UtilBaudSweep.cpp

all: test_crc test_cobs test_blink test_framerx test_baudsweep test_log test_core test_autolink

test_crc: $(UTIL)/UtilCrc.cpp UtilCrcTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCrc.cpp UtilCrcTest.cpp -o run_test_crc
	./run_test_crc

test_cobs: $(UTIL)/UtilCobs.cpp UtilCobsTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCobs.cpp UtilCobsTest.cpp -o run_test_cobs
	./run_test_cobs

test_blink: $(UTIL)/UtilBlink.h UtilBlinkTest.cpp
	$(CXX) $(CXXFLAGS) UtilBlinkTest.cpp -o run_test_blink
	./run_test_blink

test_framerx: $(UTIL)/UtilFrameRx.cpp UtilFrameRxTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilFrameRx.cpp $(UTIL)/UtilCobs.cpp $(UTIL)/UtilCrc.cpp UtilFrameRxTest.cpp -o run_test_framerx
	./run_test_framerx

test_baudsweep: $(UTIL)/UtilBaudSweep.cpp UtilBaudSweepTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilBaudSweep.cpp UtilBaudSweepTest.cpp -o run_test_baudsweep
	./run_test_baudsweep

test_log: $(SRC)/Log.cpp LogTest.cpp
	$(CXX) $(CXXFLAGS) $(SRC)/Log.cpp LogTest.cpp -o run_test_log
	./run_test_log

test_core: $(LINK_SRC) test.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) test.cpp -o run_test
	./run_test

test_autolink: $(LINK_SRC) AutoLinkTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) AutoLinkTest.cpp -o run_test_autolink
	./run_test_autolink

clean:
	rm -f run_test run_test_crc run_test_cobs run_test_blink run_test_framerx run_test_baudsweep run_test_log run_test_autolink

.PHONY: all test_crc test_cobs test_blink test_framerx test_baudsweep test_log test_core test_autolink clean
