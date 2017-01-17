CXX=g++
CFLAGS := -O3 -std=c++14

all: rcu mutex

rcu:
	$(CXX) $(CFLAGS) rcu.cc -o rcu

mutex:
	$(CXX) $(CFLAGS) mutex.cc -o mutex

clean:
	rm -f rcu mutex
