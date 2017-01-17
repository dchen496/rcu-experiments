#include <cstdint>
#include <thread>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <chrono>

const int nthreads = 4;

uint64_t nreads[nthreads];
uint64_t nupdates[nthreads];
uint64_t ndeletes[nthreads];
thread_local int thread_id;

inline void compiler_fence() {
	asm volatile("":::"memory");
}

const int thingsz = 32;
struct thing {
	int arr[thingsz];
};

struct mutex {
	int locked;

	void lock() {
		while (locked || !__sync_bool_compare_and_swap(&locked, 0, -1))
			compiler_fence();
	}

	void unlock() {
		locked = 0;
	}

	void lock_shared() {
		lock();
	}

	void unlock_shared() {
		unlock();
	}
};

struct shared_mutex {
	int locked;

	void lock() {
		while (locked || !__sync_bool_compare_and_swap(&locked, 0, -1))
			compiler_fence();
	}

	void unlock() {
		locked = 0;
	}

	void lock_shared() {
		int lock_val;
		do {
			compiler_fence();
			lock_val = locked;
		} while (lock_val < 0 || !__sync_bool_compare_and_swap(&locked, lock_val, lock_val + 1));
	}

	void unlock_shared() {
		__sync_fetch_and_add(&locked, -1);
	}
};

shared_mutex mu;
thing *thingptr;

void run_thread() {
	auto static start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
	thing *p, *q;
	while (1) {
		switch (rand() % 10) {
		case 0:
			nupdates[thread_id]++;

			p = new thing();
			for (int i = 0; i < thingsz; i++) {
				p->arr[i] = i;
			}
			mu.lock();
			q = thingptr;
			thingptr = p;
			mu.unlock();
			delete q;
			break;
		case 1:
			ndeletes[thread_id]++;

			mu.lock();
			p = thingptr;
			thingptr = nullptr;
			mu.unlock();
			delete p;
			break;
		default:
			nreads[thread_id]++;
			mu.lock_shared();
			p = thingptr;
			if (p != nullptr) {
				for (int i = 0; i < thingsz; i++) {
					if (p->arr[i] != i) {
						std::cout << "FAIL\n";
					}
				}
			}
			mu.unlock_shared();
			break;
		}

		if (thread_id == 0 && nreads[thread_id] % (1 << 19) == 0) {
			uint64_t reads = 0, updates = 0, deletes = 0;
			for (int i = 0; i < nthreads; i++) {
				reads += nreads[i];
				updates += nupdates[i];
				deletes += ndeletes[i];
			}
			auto ts = std::chrono::high_resolution_clock::now().time_since_epoch();
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(ts).count() - start_ms;
			std::cout << "reads " << reads
				<< " updates " << updates << " deletes " << deletes
				<< " ms " << ms << "\n";
			if (reads > (1 << 23)) {
				std::cout << "exiting\n";
				exit(0);
			}
		}
	}
}

int main() {
	std::thread *threads[nthreads];
	for (int i = 0; i < nthreads; i++) {
		threads[i] = new std::thread([=] {
			thread_id = i;
			run_thread();
		});
	}
	threads[0]->join();
	return 0;
}
