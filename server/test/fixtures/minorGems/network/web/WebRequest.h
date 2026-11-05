#ifndef TEST_BLOCKING_WEB_REQUEST_H
#define TEST_BLOCKING_WEB_REQUEST_H

#include <atomic>
#include <chrono>
#include <thread>

// Models LookupThread::join in WebRequest destruction without DNS timing.
class WebRequest {
public:
    static std::atomic<int> destroyed;
    WebRequest( const char *, const char *, const char *, const char *,
                double, int ) {}
    ~WebRequest() {
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
        destroyed++;
    }
};

#endif
