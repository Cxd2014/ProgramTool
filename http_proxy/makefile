
INCLUDE_PATH += -I/usr/local/include/

LIBRARY_PATH = /usr/local/lib/libevent.a

LIB = -lpthread

.PHONY: clean 

clean:
	rm -rf http_proxy

http_proxy: main.cpp
	g++ $? -g -o http_proxy $(INCLUDE_PATH) $(LIBRARY_PATH) $(LIB)

build: clean http_proxy

.DEFAULT_GOAL := build
