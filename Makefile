all: test

test: ALink.cpp test.cpp
	g++ -std=c++14 ALink.cpp test.cpp -o run_test
	./run_test
