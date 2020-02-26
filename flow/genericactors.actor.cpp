/*
 * genericactors.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flow/flow.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

ACTOR Future<bool> allTrue( std::vector<Future<bool>> all ) {
	state int i=0;
	while (i != all.size()) {
		bool r = wait( all[i] );
		if (!r) return false;
		i++;
	}
	return true;
}

ACTOR Future<Void> anyTrue( std::vector<Reference<AsyncVar<bool>>> input, Reference<AsyncVar<bool>> output ) {
	loop {
		bool oneTrue = false;
		std::vector<Future<Void>> changes;
		for(auto it : input) {
			if( it->get() ) oneTrue = true;
			changes.push_back( it->onChange() );
		}
		output->set( oneTrue );
		wait( waitForAny(changes) );
	}
}

ACTOR Future<Void> cancelOnly( std::vector<Future<Void>> futures ) {
	// We don't do anything with futures except hold them, we never return, but if we are cancelled we (naturally) drop the futures
	wait( Never() );
	return Void();
}

ACTOR Future<Void> timeoutWarningCollector( FutureStream<Void> input, double logDelay, const char* context, UID id ) {
	state uint64_t counter = 0;
	state Future<Void> end = delay( logDelay );
	loop choose {
		when ( waitNext( input ) ) {
			counter++;
		}
		when ( wait( end ) ) {
			if( counter )
				TraceEvent(SevWarn, context, id).detail("LateProcessCount", counter).detail("LoggingDelay", logDelay);
			end = delay( logDelay );
			counter = 0;
		}
	}
}

ACTOR Future<bool> quorumEqualsTrue( std::vector<Future<bool>> futures, int required ) {
	state std::vector< Future<Void> > true_futures;
	state std::vector< Future<Void> > false_futures;
	for(int i=0; i<futures.size(); i++) {
		true_futures.push_back( onEqual( futures[i], true ) );
		false_futures.push_back( onEqual( futures[i], false ) );
	}

	choose {
		when( wait( quorum( true_futures, required ) ) ) {
			return true;
		}
		when( wait( quorum( false_futures, futures.size() - required + 1 ) ) ) {
			return false;
		}
	}
}

ACTOR Future<bool> shortCircuitAny( std::vector<Future<bool>> f )
{
	std::vector<Future<Void>> sc;
	for(Future<bool> fut : f) {
		sc.push_back(returnIfTrue(fut));
	}

	choose {
		when( wait( waitForAll( f ) ) ) {
			// Handle a possible race condition? If the _last_ term to
			// be evaluated triggers the waitForAll before bubbling
			// out of the returnIfTrue quorum
			for ( auto fut : f ) {
				if ( fut.get() ) {
					return true;
				}
			}
			return false;
		}
		when( wait( waitForAny( sc ) ) ) {
			return true;
		}
	}
}

Future<Void> orYield( Future<Void> f ) {
	if(f.isReady()) {
		if(f.isError())
			return tagError<Void>(yield(), f.getError());
		else
			return yield();
	}
	else
		return f;
}

ACTOR Future<Void> returnIfTrue( Future<bool> f )
{
	bool b = wait( f );
	if ( b ) {
		return Void();
	}
	wait( Never() );
	throw internal_error();
}

ACTOR Future<Void> lowPriorityDelay( double waitTime ) {
	state int loopCount = 0;
	while(loopCount < FLOW_KNOBS->LOW_PRIORITY_DELAY_COUNT) {
		wait(delay(waitTime/FLOW_KNOBS->LOW_PRIORITY_DELAY_COUNT, TaskPriority::Low));
		loopCount++;
	}
	return Void();
}

ACTOR Future<Void> FlowLock::takeActor(FlowLock* lock, TaskPriority taskID, int64_t amount) {
	state std::list<std::pair<Promise<Void>, int64_t>>::iterator it =
	    lock->takers.insert(lock->takers.end(), std::make_pair(Promise<Void>(), amount));

	try {
		wait(it->first.getFuture());
	} catch (Error& e) {
		if (e.code() == error_code_actor_cancelled) {
			lock->takers.erase(it);
			lock->release(0);
		}
		throw;
	}
	try {
		double duration = BUGGIFY_WITH_PROB(.001)
		                      ? deterministicRandom()->random01() * FLOW_KNOBS->BUGGIFY_FLOW_LOCK_RELEASE_DELAY
		                      : 0.0;
		choose {
			when(wait(delay(duration, taskID))) {
			} // So release()ing the lock doesn't cause arbitrary code to run on the stack
			when(wait(lock->broken_on_destruct.getFuture())) {}
		}
		return Void();
	} catch (...) {
		TEST(true); // If we get cancelled here, we are holding the lock but the caller doesn't know, so release it
		lock->release(amount);
		throw;
	}
}

ACTOR Future<Void> FlowLock::takeMoreActor(FlowLock* lock, int64_t* amount) {
	wait(lock->take());
	int64_t extra = std::min(lock->available(), *amount - 1);
	lock->active += extra;
	*amount = 1 + extra;
	return Void();
}

ACTOR Future<Void> FlowLock::safeYieldActor(FlowLock* lock, TaskPriority taskID, int64_t amount) {
	try {
		choose {
			when(wait(yield(taskID))) {}
			when(wait(lock->broken_on_destruct.getFuture())) {}
		}
		return Void();
	} catch (Error& e) {
		lock->release(amount);
		throw;
	}
}

ACTOR Future<Void> FlowLock::releaseWhenActor(FlowLock* self, Future<Void> signal, int64_t amount) {
	wait(signal);
	self->release(amount);
	return Void();
}
