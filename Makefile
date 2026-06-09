all: test readme

test: ALink.cpp Log.cpp test.cpp
	g++ -std=c++14 ALink.cpp Log.cpp test.cpp -o run_test
	./run_test

readme:
	@python3 extract_readme.py
