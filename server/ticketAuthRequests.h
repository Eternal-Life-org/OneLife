#ifndef ONE_LIFE_TICKET_AUTH_REQUESTS_H
#define ONE_LIFE_TICKET_AUTH_REQUESTS_H

#include "minorGems/network/web/WebRequest.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// WebRequest owns a DNS LookupThread whose destructor joins.  A timed-out
// lookup must never be destroyed on the game loop.  This process-lifetime
// worker owns retired requests and never accesses game-server state.
class TicketAuthRequests {
public:
    static WebRequest *start( const char *url ) {
        TicketAuthRequests &state = instance();
        int count = state.mCount.load();
        do {
            if( count >= 64 ) return NULL;
        } while( !state.mCount.compare_exchange_weak( count, count + 1 ) );
        try {
            return new WebRequest( "GET", url, NULL, NULL, 8, 4096 );
        }
        catch( ... ) {
            state.mCount--;
            return NULL;
        }
    }

    // Only use this for a completed request: DNS has already finished.
    static void finish( WebRequest *request ) {
        if( request == NULL ) return;
        delete request;
        instance().mCount--;
    }

    static void retire( WebRequest *request ) {
        if( request == NULL ) return;
        TicketAuthRequests &state = instance();
        try {
            std::lock_guard<std::mutex> lock( state.mMutex );
            if( !state.mWorker.joinable() ) {
                TicketAuthRequests *owner = &state;
                state.mWorker = std::thread( [owner]() { owner->reap(); } );
            }
            state.mRetired.push_back( request );
            state.mReady.notify_one();
        }
        catch( ... ) {
            // Resource exhaustion must not make the game loop join a stalled
            // DNS lookup.  The reservation remains, so leaks are bounded.
        }
    }

private:
    TicketAuthRequests() : mCount( 0 ) {}
    TicketAuthRequests( const TicketAuthRequests & ) = delete;
    TicketAuthRequests &operator=( const TicketAuthRequests & ) = delete;

    static TicketAuthRequests &instance() {
        static TicketAuthRequests *state = new TicketAuthRequests();
        return *state;
    }

    void reap() {
        for( ;; ) {
            WebRequest *request = NULL;
            {
                std::unique_lock<std::mutex> lock( mMutex );
                mReady.wait( lock, [this]() { return !mRetired.empty(); } );
                request = mRetired.front();
                mRetired.pop_front();
            }
            delete request;
            mCount--;
        }
    }

    std::atomic<int> mCount;
    std::mutex mMutex;
    std::condition_variable mReady;
    std::deque<WebRequest *> mRetired;
    // instance() deliberately outlives server shutdown; no join on the game loop.
    std::thread mWorker;
};

#endif
