#include "../ticketAuthRequests.h"

#include <cassert>
#include <chrono>
#include <thread>

std::atomic<int> WebRequest::destroyed( 0 );

int main() {
    WebRequest *pending[64];
    for( int i=0; i<64; ++i ) {
        pending[i] = TicketAuthRequests::start( "http://test.invalid/" );
        assert( pending[i] != NULL );
    }
    assert( TicketAuthRequests::start( "http://test.invalid/" ) == NULL );

    const auto started = std::chrono::steady_clock::now();
    for( int i=0; i<64; ++i ) TicketAuthRequests::retire( pending[i] );
    const auto elapsed = std::chrono::steady_clock::now() - started;
    // Deleting the requests here would block for at least 1.28 seconds.
    assert( elapsed < std::chrono::milliseconds( 500 ) );

    const auto deadline = started + std::chrono::seconds( 5 );
    while( WebRequest::destroyed.load() != 64 &&
           std::chrono::steady_clock::now() < deadline ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    assert( WebRequest::destroyed.load() == 64 );
    // The fixture counts destruction just before the reaper releases its slot.
    WebRequest *recovered = NULL;
    while( recovered == NULL && std::chrono::steady_clock::now() < deadline ) {
        recovered = TicketAuthRequests::start( "http://test.invalid/" );
        if( recovered == NULL )
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    assert( recovered != NULL );
    TicketAuthRequests::finish( recovered );
    assert( WebRequest::destroyed.load() == 65 );
}
