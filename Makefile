# AutoLink host build: every unit suite plus the protocol/integration suite,
# then the README example compile check. One failing suite fails the build.
CXX = g++
CXXFLAGS = -std=c++14 -Wall -Wextra

all: test readme

test: test_crc test_cobs test_blink test_framerx test_core

test_crc: UtilCrc.cpp UtilCrcTest.cpp
	$(CXX) $(CXXFLAGS) UtilCrc.cpp UtilCrcTest.cpp -o run_test_crc
	./run_test_crc

test_cobs: UtilCobs.cpp UtilCobsTest.cpp
	$(CXX) $(CXXFLAGS) UtilCobs.cpp UtilCobsTest.cpp -o run_test_cobs
	./run_test_cobs

test_blink: UtilBlink.h UtilBlinkTest.cpp
	$(CXX) $(CXXFLAGS) UtilBlinkTest.cpp -o run_test_blink
	./run_test_blink

test_framerx: UtilFrameRx.cpp UtilFrameRxTest.cpp
	$(CXX) $(CXXFLAGS) UtilFrameRx.cpp UtilCobs.cpp UtilCrc.cpp UtilFrameRxTest.cpp -o run_test_framerx
	./run_test_framerx

test_core: ALink.cpp Log.cpp test.cpp
	$(CXX) $(CXXFLAGS) ALink.cpp Log.cpp UtilCrc.cpp UtilCobs.cpp UtilFrameRx.cpp test.cpp -o run_test
	./run_test

readme:
	@python3 extract_readme.py

clean:
	rm -f run_test run_test_crc run_test_cobs run_test_blink run_test_framerx _readme_*.cpp

.PHONY: all test test_crc test_cobs test_blink test_framerx test_core readme clean
