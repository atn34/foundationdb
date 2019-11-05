#include <algorithm>
#include <random>

#include <string_view>
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

int bytesRequired(int64_t denom) {
	ASSERT(denom > 0);
	int result = 0;
	while (denom > 0) {
		result++;
		denom >>= 8;
	}
	return result;
}

struct RecordRandomBytes : Random {
	Random* src; // Not owned
	std::string bytes;
	double random01() override {
		auto result = src->random01();
		auto u = static_cast<uint32_t>(result * static_cast<double>(std::numeric_limits<uint32_t>::max()));
		bytes += std::string_view(reinterpret_cast<char*>(&u), sizeof(u));
		return result;
	}
	int32_t randomInt(int32_t min, int32_t max_plus_one) override {
		int64_t result = src->randomInt(min, max_plus_one);
		result -= min;
		bytes +=
		    std::string_view(reinterpret_cast<char*>(&result), bytesRequired(int64_t{ max_plus_one } - int64_t{ min }));
		result += min;
		return result;
	}
	~RecordRandomBytes() = default;
};

struct ReplayRandomBytes : Random {
	std::string_view bytes;
	double random01() override {
		auto bytes4 = consumeBytes(4);
		uint32_t u;
		memcpy(&u, bytes4.begin(), 4);
		return static_cast<double>(u) / static_cast<double>(std::numeric_limits<uint32_t>::max());
	}
	int32_t randomInt(int32_t min, int32_t max_plus_one) override {
		int64_t min64 = min;
		int64_t max_plus_one64 = max_plus_one;
		auto numeratorBytes = consumeBytes(bytesRequired(max_plus_one64 - min64));
		int64_t numerator = 0;
		memcpy(&numerator, numeratorBytes.begin(), numeratorBytes.size());
		return std::clamp(min64 + numerator, min64, max_plus_one64 - 1);
	}
	std::string_view consumeBytes(int size) {
		if (bytes.size() >= size) {
			auto result = bytes.substr(0, size);
			bytes = bytes.substr(size);
			return result;
		}
		throw EndSimulation{};
	}
	~ReplayRandomBytes() override = default;
};

struct FairRandom : Random {
	explicit FairRandom(int seed) : rand_(seed) {}
	double random01() override { return std::uniform_real_distribution{ 0.0, 1.0 }(rand_); }
	int32_t randomInt(int32_t min, int32_t maxPlusOne) override {
		return std::uniform_int_distribution<int64_t>{ min, maxPlusOne - 1 }(rand_);
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
	  : rand_(rand), scheduling_strategy_(s),
	    max_buggified_delay(scheduling_strategy_ == SchedulingStrategy::InOrder ? 0.2 * rand->random01() : 0) {}

	Future<Void> delay(double seconds) override {
		if (max_buggified_delay > 0 && random01() < 0.25) {
			seconds += max_buggified_delay * pow(random01(), 1000.0);
		}
		Promise<Void> task;
		switch (scheduling_strategy_) {
		case SchedulingStrategy::InOrder:
			tasks_.push_back({ task, now_ + seconds, stable++ });
			std::push_heap(tasks_.begin(), tasks_.end(), std::greater<Task>{});
			break;
		case SchedulingStrategy::RandomOrder:
			tasks_.push_back({ task, now_ + seconds, stable++ });
			break;
		}
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
		int stable; // For determinism with SchedulingStrategy::InOrder
		friend bool operator>(const Task& lhs, const Task& rhs) {
			return std::make_pair(lhs.t, lhs.stable) > std::make_pair(rhs.t, rhs.stable);
		}
	};
	double now_ = 0;
	std::vector<Task> tasks_;
	Random* rand_; // Not owned
	const SchedulingStrategy scheduling_strategy_;
	int stable = 0;
	bool running = true;
	const double max_buggified_delay;
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

	constexpr static int kSize = 10000;

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
	ReplayRandomBytes rand;
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
