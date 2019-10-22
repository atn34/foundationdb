#include <algorithm>
#include <random>

#include "flow/ActorCollection.h"
#include "flow/flow.h"

#include "flow/actorcompiler.h" // Must be the last include

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

struct FuzzRandom : Random {
	std::mt19937 rand_; // If you run out of bytes
	std::string_view bytes;
	double random01() override {
		if (bytes.size() >= 4) {
			double result = static_cast<double>(*reinterpret_cast<const uint32_t*>(bytes.begin())) /
			                static_cast<double>(std::numeric_limits<uint32_t>::max());
			bytes = bytes.substr(4);
			return result;
		}
		return std::uniform_real_distribution<double>{ 0, 1 }(rand_);
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

struct RandomSim : Simulator {
	explicit RandomSim(Random* rand) : rand_(rand) { max_buggified_delay = 0.2 * random01(); }
	Future<Void> delay(double seconds) override {
		if (random01() < 0.25) {
			double delta = max_buggified_delay * pow(random01(), 1000.0);
			/* if (delta > 0) { */
			/* 	printf("delay delta: %.16f\n", delta); */
			/* } */
			seconds += delta;
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
			Task task = std::move(tasks_.front());
			std::pop_heap(tasks_.begin(), tasks_.end(), std::greater<Task>{});
			tasks_.pop_back();
			now_ = task.t;
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
		auto copy = elements;
		std::sort(copy.begin(), copy.end());
		for (int i = 0; i < copy.size(); ++i) {
			assert(copy[i] == i);
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
		state int x = self->elements[i];
		state int y = self->elements[j];
		wait(self->sim->delay(0));
		self->elements[i] = y;
		self->elements[j] = x;
		return Void();
	}

	Simulator* sim;
	std::vector<int> elements;
};

ACTOR Future<Void> client(Simulator* sim, ExampleService* service) {
	state double lastTime = sim->now();
	loop {
		wait(poisson(sim, &lastTime, 5));
		if (sim->randomInt(0, 100) == 0) {
			wait(service->checkInvariant());
		} else {
			int i = sim->randomInt(0, ExampleService::kSize);
			int j = sim->randomInt(0, ExampleService::kSize);
			wait(service->swap(i, j));
		}
	}
}

ACTOR Future<Void> clients(Simulator* sim, ExampleService* service) {
	state ActorCollection actors(/*returnWhenEmptied*/ false);
	state double lastTime = sim->now();
	loop {
		choose {
			when(wait(poisson(sim, &lastTime, 5))) { actors.add(client(sim, service)); }
			when(wait(actors.getResult())) { throw internal_error(); }
		}
	}
}

ACTOR Future<Void> stopAfterSeconds(Simulator* sim, double seconds) {
	wait(sim->delay(seconds));
	sim->stop();
	return Void();
}

void runSimulation(Simulator* sim) {
	ExampleService service{ sim };
	std::vector<Future<Void>> futures;
	futures.push_back(clients(sim, &service));
	futures.push_back(stopAfterSeconds(sim, 100.0));
	sim->run();
}

#ifdef USE_LIBFUZZER

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size) {
	FuzzRandom rand;
	rand.bytes = { reinterpret_cast<const char*>(Data), Size };
	RandomSim sim{ &rand };
	runSimulation(&sim);
	return 0;
}

#else

int main() {
	int seed = 0;
	for (;;) {
		printf("Trying seed %d\n", seed);
		FairRandom rand{ seed++ };
		RandomSim sim{ &rand };
		runSimulation(&sim);
	}
}

#endif
