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

struct FairRandomSim : Simulator {
	explicit FairRandomSim(int seed) : rand_(seed) { max_buggified_delay = 0.2 * random01(); }
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
	int randomInt(int min, int maxPlusOne) override {
		return std::uniform_int_distribution{ min, maxPlusOne - 1 }(rand_);
	}
	double random01() override { return std::uniform_real_distribution{ 0.0, 1.0 }(rand_); }
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
	~FairRandomSim() override = default;

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
	std::mt19937 rand_;
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

	constexpr static int kSize = 100;

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
		if (sim->randomInt(0, 100) == 0) {
			wait(service->checkInvariant());
		} else {
			int i = sim->randomInt(0, ExampleService::kSize);
			int j = sim->randomInt(0, ExampleService::kSize);
			wait(service->swap(i, j));
		}
		wait(poisson(sim, &lastTime, 5));
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

int main() {
	int seed = 0;
	for (;;) {
		printf("Trying seed %d\n", seed);
		FairRandomSim sim{ seed++ };
		ExampleService service{ &sim };
		std::vector<Future<Void>> futures;
		futures.push_back(clients(&sim, &service));
		futures.push_back(stopAfterSeconds(&sim, 100.0));
		sim.run();
	}
}
