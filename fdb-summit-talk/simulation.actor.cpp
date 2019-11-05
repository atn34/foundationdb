#include <algorithm>
#include <random>

#include "flow/ActorCollection.h"
#include "flow/flow.h"

#include "flow/actorcompiler.h" // Must be the last include

#ifdef DO_TRACE
#define TRACE(...)                                                                                                     \
	do {                                                                                                               \
		printf(__VA_ARGS__);                                                                                           \
	} while (0)
#else
#define TRACE(...)                                                                                                     \
	do {                                                                                                               \
	} while (0)
#endif

struct Simulator {
	virtual Future<Void> delay(double seconds) = 0;
	virtual double now() = 0;
	virtual int randomInt(int min, int maxPlusOne) = 0;
	virtual double random01() = 0;
	virtual void run() = 0;
	virtual void stop() = 0;
	virtual ~Simulator() = default;
};

struct Random {
	virtual double random01() = 0;
	virtual int32_t randomInt(int32_t min, int32_t max_plus_one) = 0;
	virtual ~Random() = default;
};

struct EndSimulation {};

struct FuzzRandom : Random {
	std::string owned_bytes;
	std::string_view bytes;
	double random01() override {
		if (bytes.size() >= 4) {
			double result = static_cast<double>(*reinterpret_cast<const uint32_t*>(bytes.begin())) /
			                static_cast<double>(std::numeric_limits<uint32_t>::max());
			bytes = bytes.substr(4);
			return result;
		}
		throw EndSimulation{};
	}
	int32_t randomInt(int32_t min, int32_t max_plus_one) override {
		int32_t result = min + random01() * (max_plus_one - min);
		return std::min(std::max(result, min), max_plus_one - 1);
	}
	~FuzzRandom() override = default;
};

struct FairRandom : Random {
	explicit FairRandom(int seed) : rand_(seed) {}
	double random01() override { return std::uniform_real_distribution{ 0.0, 1.0 }(rand_); }
	int randomInt(int min, int maxPlusOne) override {
		return std::uniform_int_distribution{ min, maxPlusOne - 1 }(rand_);
	}
	~FairRandom() override = default;

private:
	std::mt19937 rand_;
};

enum class SchedulingStrategy {
	InOrder,
	RandomOrder,
};

struct RandomSim : Simulator {
	explicit RandomSim(Random* rand, SchedulingStrategy s = SchedulingStrategy::InOrder)
	  : rand_(rand), scheduling_strategy_(s) {
		max_buggified_delay = 0.2 * random01();
	}
	Future<Void> delay(double seconds) override {
		if (random01() < 0.25) {
			seconds += max_buggified_delay * pow(random01(), 1000.0);
		}
		Promise<Void> task;
		tasks_.push_back({ task, now_ + seconds, stable++ });
		std::push_heap(tasks_.begin(), tasks_.end(), std::greater<Task>{});
		return task.getFuture();
	}
	double now() override { return now_; }
	int randomInt(int min, int maxPlusOne) override { return rand_->randomInt(min, maxPlusOne); }
	double random01() override { return rand_->random01(); }
	void run() override {
		while (running && !tasks_.empty()) {
			Task task;
			switch (scheduling_strategy_) {
			case SchedulingStrategy::InOrder:
				task = std::move(tasks_.front());
				std::pop_heap(tasks_.begin(), tasks_.end(), std::greater<Task>{});
				tasks_.pop_back();
				break;
			case SchedulingStrategy::RandomOrder:
				std::swap(tasks_[randomInt(0, tasks_.size())], tasks_.back());
				task = std::move(tasks_.back());
				tasks_.pop_back();
				break;
			}
			now_ = std::max(task.t, now_);
			task.p.send(Void());
		}
	}
	void stop() override { running = false; }
	~RandomSim() override = default;

private:
	struct Task {
		Promise<Void> p;
		double t;
		int stable; // For determinism
		friend bool operator>(const Task& lhs, const Task& rhs) {
			return std::make_pair(lhs.t, lhs.stable) > std::make_pair(rhs.t, rhs.stable);
		}
	};
	double now_ = 0;
	std::vector<Task> tasks_;
	Random* rand_; // Not owned
	SchedulingStrategy scheduling_strategy_;
	int stable = 0;
	bool running = true;
	double max_buggified_delay;
};

ACTOR Future<Void> poisson(Simulator* sim, double* last, double meanInterval) {
	*last += meanInterval * -log(sim->random01());
	wait(sim->delay(*last - sim->now()));
	return Void();
}

struct ExampleService {
	Future<Void> swap(int i, int j) { return swap_(this, i, j); }
	Future<Void> checkInvariant() {
		TRACE("%f\t\t\tcheckInvariant()\n", sim->now());
		auto copy = elements;
		std::sort(copy.begin(), copy.end());
		for (int i = 0; i < copy.size(); ++i) {
			ASSERT_ABORT(copy[i] == i);
		}
		return Void();
	}

	explicit ExampleService(Simulator* sim) : sim(sim) {
		elements.resize(kSize);
		for (int i = 0; i < elements.size(); ++i) {
			elements[i] = i;
		}
	}

	constexpr static int kSize = 1000;

private:
	ACTOR static Future<Void> swap_(ExampleService* self, int i, int j) {
		static int freshSwapId = 0;
		state [[maybe_unused]] int swapId = freshSwapId++;
		TRACE("%f\t%d\tBegin\tswap(%d, %d)\n", self->sim->now(), swapId, i, j);
		state int x = self->elements[i];
		state int y = self->elements[j];
		wait(self->sim->delay(0)); // This wait is the bug!
		self->elements[i] = y;
		self->elements[j] = x;
		TRACE("%f\t%d\tEnd\tswap(%d, %d)\n", self->sim->now(), swapId, i, j);
		return Void();
	}

	Simulator* sim;
	std::vector<int> elements;
};

std::pair<int, int> sampleDistinctOrderedPair(Simulator* sim, int size) {
	int i = sim->randomInt(0, size - 1);
	int j = sim->randomInt(i + 1, size);
	return { i, j };
}

ACTOR Future<Void> client(Simulator* sim, ExampleService* service) {
	state double lastTime = sim->now();
	loop {
		wait(poisson(sim, &lastTime, 1));
		if (sim->randomInt(0, 100) == 0) {
			wait(service->checkInvariant());
		} else {
			auto pair = sampleDistinctOrderedPair(sim, ExampleService::kSize);
			wait(service->swap(pair.first, pair.second));
		}
	}
}

ACTOR Future<Void> clients(Simulator* sim, ExampleService* service) {
	state ActorCollection actors(/*returnWhenEmptied*/ false);
	for (int i = 0; i < 5; ++i) {
		actors.add(client(sim, service));
	}
	wait(actors.getResult());
	throw internal_error();
}

ACTOR Future<Void> stopAfterSeconds(Simulator* sim, double seconds) {
	wait(sim->delay(seconds));
	sim->stop();
	return Void();
}

void runSimulation(Random* random) {
	TRACE("Time\t\tOpId\tPhase\tOp\n");
	try {
		RandomSim sim{ random, SchedulingStrategy::RandomOrder };
		ExampleService service{ &sim };
		std::vector<Future<Void>> futures;
		futures.push_back(clients(&sim, &service));
		futures.push_back(stopAfterSeconds(&sim, 100.0));
		sim.run();
	} catch (EndSimulation&) {
	}
}

#ifdef USE_LIBFUZZER

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size) {
	FuzzRandom rand;
	rand.bytes = { reinterpret_cast<const char*>(Data), Size };
	runSimulation(&rand);
	return 0;
}

#else

int main(int argc, char** argv) {
#ifdef DO_TRACE
	ASSERT(argc > 1);
	FairRandom rand{ std::atoi(argv[1]) };
	runSimulation(&rand);
#else
	int seed = 0;
	for (;;) {
		printf("Trying seed %d\n", seed);
		FairRandom rand{ seed++ };
		runSimulation(&rand);
	}
#endif
}

#endif
