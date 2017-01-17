#include <cstdint>
#include <thread>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <chrono>

const int nthreads = 4;

uint64_t global_epoch; // epoch for new read-side critical sections; existing ones may be up to 1 epoch behind
uint64_t thread_epochs[nthreads];
uint64_t nreads[nthreads];
uint64_t nupdates[nthreads];
uint64_t ndeletes[nthreads];
thread_local int thread_id;

inline void compiler_fence() {
	asm volatile("":::"memory");
}

inline void acquire_fence() {
	compiler_fence();
}

inline void release_fence() {
	compiler_fence();
}

inline void nop_pause() {
	asm volatile("pause":::"memory");
}

void try_epoch_advance() {
	auto static start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();

	compiler_fence();
	uint64_t old_global_epoch = global_epoch;
	for (int i = 0; i < nthreads; i++) {
		if (thread_epochs[i] < old_global_epoch) {
			return;
		}
	}

	// allows concurrent calls to try_epoch_advance
	if (!__sync_bool_compare_and_swap(&global_epoch, old_global_epoch, old_global_epoch + 1)) {
		return;
	}

	if (old_global_epoch > 0 && old_global_epoch % (1 << 20) == 0) {
		uint64_t reads = 0, updates = 0, deletes = 0;
		for (int i = 0; i < nthreads; i++) {
			reads += nreads[i];
			updates += nupdates[i];
			deletes += ndeletes[i];
		}
		auto ts = std::chrono::high_resolution_clock::now().time_since_epoch();
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(ts).count() - start_ms;
		std::cout << "epoch " << old_global_epoch << " reads " << reads
			<< " updates " << updates << " deletes " << deletes
			<< " ms " << ms << "\n";
		if (old_global_epoch == (1 << 22)) {
			std::cout << "exiting\n";
			exit(0);
		}
	}
}

void rcu_load_epoch() {
	compiler_fence();
	thread_epochs[thread_id] = global_epoch;
	compiler_fence();
}

void rcu_read_lock() {
	rcu_load_epoch();

	// ensure epoch load happens before further loads occur
	// it's okay if the store to thread_epochs is behind (will only slow down epoch advance)
	acquire_fence();
}

void rcu_read_unlock() {
	// ensure previous loads are complete before updating local epoch
	acquire_fence();

	rcu_load_epoch();
}

void rcu_synchronize() {
	// if the first store was at local epoch E, then another at local epoch
	// E + 1 might read it. We are guaranteed to have no more readers when
	// all local epochs are E + 2 or higher.
	uint64_t last_overlapping_epoch = thread_epochs[thread_id] + 2;

	// ensure all stores are complete before updating local epoch
	release_fence();

	// ensure epochs continue to advance
	thread_epochs[thread_id] = last_overlapping_epoch;

	// spin until the global epoch is above E + 2 and all local epochs are at least E + 2
	compiler_fence();
	while (global_epoch <= last_overlapping_epoch) {
		try_epoch_advance();
	}
}

template <typename T>
T *rcu_replace_ptr(T *newval, T **dst) {
	T* ret;
	do {
		ret = *dst;
	} while (!__sync_bool_compare_and_swap(dst, ret, newval));
	return ret;
}

template <typename T>
T *rcu_get_ptr(T *ptr) {
	T* ret = ptr;
	acquire_fence();
	return ret;
}

const int thingsz = 32;
struct thing {
	int arr[thingsz];
};

thing *thingptr;

void run_thread() {
	thing *p;
	while (1) {
		switch (rand() % 10) {
		case 0:
			nupdates[thread_id]++;
			p = new thing();
			for (int i = 0; i < thingsz; i++) {
				p->arr[i] = i;
			}
			p = rcu_replace_ptr(p, &thingptr);
			rcu_synchronize();
			delete p;
			break;
		case 1:
			ndeletes[thread_id]++;
			rcu_read_lock();
			p = rcu_replace_ptr((thing *) nullptr, &thingptr);
			rcu_synchronize();
			delete p;
			break;
		default:
			nreads[thread_id]++;
			rcu_read_lock();
			p = rcu_get_ptr(thingptr);
			if (p != nullptr) {
				for (int i = 0; i < thingsz; i++) {
					if (p->arr[i] != i) {
						std::cout << "FAIL\n";
					}
				}
			}
			rcu_read_unlock();
			break;
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
