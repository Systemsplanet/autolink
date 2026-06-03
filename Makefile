all: test

test: ALink.cpp test.cpp
	g++ -std=c++11 ALink.cpp test.cpp -o run_test
	./run_test
