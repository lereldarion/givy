.PHONY: all clean disassemble

CPPFLAGS = -std=c++14 -O3 -Wall

FILES = superpage_tracker.cpp assert_level.cpp

all: test_main test_spt

test_main: region.cpp $(FILES) $(wildcard *.h)
	g++ $(CPPFLAGS) -o $@ $< $(FILES)

test_spt: superpage_tracker.t.cpp $(FILES) $(wildcard *.h)
	g++ $(CPPFLAGS) -o $@ $< $(FILES) -pthread

clean:
	$(RM) $(wildcard test_*)

disassemble: test_main
	gdb -batch -ex 'disassemble main' $<
