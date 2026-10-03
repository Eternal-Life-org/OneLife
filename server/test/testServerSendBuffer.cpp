#include "../ServerSendBuffer.h"
#include "minorGems/util/SimpleVector.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstdlib>

// Fail one allocation deterministically without exhausting the test machine.
static int failAllocationAfter = -1;
void *operator new( size_t size ) {
    if( failAllocationAfter == 0 ) {
        failAllocationAfter = -1;
        throw std::bad_alloc();
        }
    if( failAllocationAfter > 0 ) --failAllocationAfter;
    void *memory = malloc( size == 0 ? 1 : size );
    if( memory == NULL ) throw std::bad_alloc();
    return memory;
    }
void operator delete( void *memory ) noexcept { free( memory ); }
void *operator new[]( size_t size ) { return ::operator new( size ); }
void operator delete[]( void *memory ) noexcept { free( memory ); }

static void enqueue( ServerSendBuffer &buffer, const std::string &message,
                     double now, double interval=0, size_t limit=1048576 ) {
    buffer.setMessageContext( (const unsigned char*)message.data(), message.size(),
                              "test", 123 );
    assert( buffer.enqueue( (const unsigned char*)message.data(), message.size(),
                           now, interval, limit ) );
    }

static std::string binaryFrame() {
    std::string message = "CM\n5 5\n#";
    message.append( "\0#\xff\n\0", 5 );
    return message + "FM\n#";
    }

static void testOrderingAndRetries() {
    ServerSendBuffer buffer;
    std::string first = binaryFrame(), second = "MX\n1 2 0 7233 -1\n#", last = "FM\n#";
    enqueue( buffer, first, 1 );
    enqueue( buffer, second, 1.1 );
    std::string received;
    int attempt = 0;
    auto send = [&]( const unsigned char *data, int size ) -> ServerWriteResult {
        ++attempt;
        if( attempt == 1 ) return { -2, EAGAIN };
        if( attempt == 3 ) return { -1, EINTR };
        int sent = attempt == 2 ? 7 : size;
        received.append( (const char*)data, sent );
        return { sent, 0 };
        };
    size_t original = buffer.pending();
    assert( buffer.drain( 2, 0, 15, 65536, send ) );
    assert( buffer.pending() == original );
    assert( buffer.drain( 2.1, 0, 15, 65536, send ) );
    assert( buffer.pending() == original - 7 );
    enqueue( buffer, last, 2.2 );
    assert( buffer.drain( 2.3, 0, 15, 65536, send ) );
    assert( buffer.pending() == original - 7 + last.size() );
    assert( buffer.drain( 2.4, 0, 15, 65536, send ) );
    assert( buffer.pending() == 0 );
    assert( received == first + second + last );
    assert( buffer.stats.wouldBlock == 1 && buffer.stats.interrupts == 1 );
    assert( buffer.stats.partialWrites == 1 && attempt == 4 );
    buffer.socketPrepared = true;
    buffer.reset();
    assert( buffer.stats.wouldBlock == 0 && buffer.stats.partialWrites == 0 &&
            buffer.stats.interrupts == 0 && buffer.stats.peakPendingBytes == 0 );
    assert( !buffer.socketPrepared && buffer.pending() == 0 && buffer.failure == NULL );
    }

static void testBatchingLimitsAgeAndReset() {
    ServerSendBuffer buffer;
    std::string message = "MX\n1 2 0 7233 -1\n#";
    enqueue( buffer, message, 1, .020 );
    enqueue( buffer, "FM\n#", 1.005, .020 );
    int calls = 0;
    auto send = [&]( const unsigned char*, int size ) -> ServerWriteResult {
        ++calls; return { size, 0 };
        };
    assert( buffer.drain( 1.019, .020, 15, 65536, send ) && calls == 0 );
    assert( buffer.drain( 1.021, .020, 15, 65536, send ) && calls == 1 );
    assert( buffer.pending() == 0 );

    buffer.reset();
    enqueue( buffer, "1234", 1, 0, 8 );
    buffer.setMessageContext( (const unsigned char*)"56789", 5, "overflow", 321 );
    assert( !buffer.enqueue( (const unsigned char*)"56789", 5, 1, 0, 8 ) );
    assert( buffer.pending() == 4 && buffer.failure != NULL );
    assert( !buffer.drain( 2, 0, 15, 65536, send ) && calls == 1 );

    buffer.reset();
    enqueue( buffer, "first", 1 );
    enqueue( buffer, "second", 2 );
    assert( buffer.drain( 3, 0, 15, 5, send ) );
    assert( buffer.oldestAge( 3 ) == 1 );
    assert( buffer.drain( 4, 0, 3, 65536, send ) );
    assert( buffer.failure == NULL );
    enqueue( buffer, "stalled", 5 );
    assert( !buffer.drain( 8, 0, 3, 65536, send ) );
    assert( buffer.lastError == 0 && buffer.lastSent == 0 );
    buffer.reset();
    assert( buffer.pending() == 0 && buffer.failure == NULL );
    enqueue( buffer, "new-session", 10 );
    assert( buffer.drain( 10.1, 0, 15, 65536, send ) );
    assert( buffer.pending() == 0 && buffer.failure == NULL );
    }

static void testFatalErrorAndValueCopies() {
    ServerSendBuffer buffer;
    enqueue( buffer, "MX\n1 2 0 7233 -1\n#FM\n#", 1 );
    auto broken = []( const unsigned char*, int ) -> ServerWriteResult {
        return { -1, ECONNRESET };
        };
    assert( !buffer.drain( 2, 0, 15, 65536, broken ) );
    assert( buffer.lastError == ECONNRESET );
    assert( strcmp( buffer.lastType, "MX" ) == 0 );
    assert( strcmp( buffer.lastFunction, "test" ) == 0 && buffer.lastLine == 123 );
    buffer.reset();
    enqueue( buffer, binaryFrame(), 1 );
    SimpleVector<ServerSendBuffer> players( 1 );
    players.push_back( buffer );
    players.push_back( buffer ); // force the same value-copy expansion as LiveObject
    buffer = ServerSendBuffer(); // drop temporary ownership after the transfer
    assert( players.getElement(0)->pending() == binaryFrame().size() );
    players.deleteElement( 0 );
    assert( players.getElement(0)->pending() == binaryFrame().size() );
    }

static void testMemoryFailuresAndIsolation() {
    assert( ServerSendBuffer::totalAllocated() == 0 );
    for( int failureAt=0; failureAt<3; ++failureAt ) {
        ServerSendBuffer buffer;
        buffer.setMessageContext( (const unsigned char*)"FM\n#", 4, "fault", 1 );
        failAllocationAfter = failureAt;
        assert( !buffer.enqueue( (const unsigned char*)"FM\n#", 4, 1, 0, 1048576 ) );
        assert( strcmp( buffer.failure, "Network output allocation failed" ) == 0 );
        assert( buffer.pending() == 0 );
        buffer.reset();
        assert( ServerSendBuffer::totalAllocated() == 0 );
        }
    ServerSendBuffer limited;
    limited.setMessageContext( (const unsigned char*)"FM\n#", 4, "quota", 1 );
    assert( !limited.enqueue( (const unsigned char*)"FM\n#", 4, 1, 0, 1048576, 32 ) );
    assert( strcmp( limited.failure, "Network output global memory limit" ) == 0 );
    limited.reset();

    ServerSendBuffer slow, healthy;
    enqueue( slow, "FM\n#", 1 );
    size_t beforeGrowth = slow.pending();
    std::string larger( 1000, 'X' );
    failAllocationAfter = 0;
    assert( !slow.enqueue( (const unsigned char*)larger.data(), larger.size(), 2, 0, 1048576 ) );
    assert( slow.pending() == beforeGrowth );
    slow.reset();
    const std::string marker = "FM\n#";
    for( int i=0; i<16384; ++i ) enqueue( slow, marker, 1 );
    assert( slow.pendingMessages() == 16384 );
    assert( !slow.enqueue( (const unsigned char*)marker.data(), marker.size(), 2, 0, 1048576 ) );
    assert( strcmp( slow.failure, "Network output message limit" ) == 0 );
    enqueue( healthy, marker, 2 );
    assert( healthy.drain( 3, 0, 15, 65536,
        []( const unsigned char*, int size ) -> ServerWriteResult { return { size, 0 }; } ) );
    assert( healthy.pending() == 0 ); // another player's failure does not block output
    slow.reset();
    assert( ServerSendBuffer::totalAllocated() == 0 );
    }

static void testNativeTCPBackpressure() {
    Socket::initSocketFramework();

    int listener = socket( AF_INET, SOCK_STREAM, 0 );
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    assert( bind( listener, (sockaddr*)&address, sizeof address ) == 0 );
    assert( listen( listener, 1 ) == 0 );
    socklen_t size = sizeof address;
    assert( getsockname( listener, (sockaddr*)&address, &size ) == 0 );
    int reader = socket( AF_INET, SOCK_STREAM, 0 );
    int small = 4096;
    setsockopt( reader, SOL_SOCKET, SO_RCVBUF, &small, sizeof small );
    assert( connect( reader, (sockaddr*)&address, sizeof address ) == 0 );

    Socket writer;
    assert( fcntl( listener, F_SETFL, O_NONBLOCK ) == 0 );
    pollfd incoming = { listener, POLLIN, 0 };
    assert( poll( &incoming, 1, 2000 ) == 1 );
    writer.mNativeSocketID = accept( listener, NULL, NULL );
    close( listener );
    assert( writer.mNativeSocketID >= 0 );
    setsockopt( writer.mNativeSocketID, SOL_SOCKET, SO_SNDBUF, &small, sizeof small );
    ServerSocketWriter::setNoDelay( &writer );
    int flags = fcntl( writer.mNativeSocketID, F_GETFL );
    std::string fill( 4096, 'X' );
    size_t filled = 0;
    for( int i=0; i<10000; i++ ) {
        ServerWriteResult result = ServerSocketWriter::send( &writer,
            (const unsigned char*)fill.data(), fill.size() );
        if( result.sent == -2 ) break;
        assert( result.sent > 0 );
        filled += result.sent;
        }
    assert( fcntl( writer.mNativeSocketID, F_GETFL ) == flags );

    std::string messages;
    for( int i=0; i<4000; i++ ) messages += binaryFrame();
    ServerSendBuffer buffer;
    enqueue( buffer, messages, 1 );
    auto send = [&]( const unsigned char *data, int count ) {
        return ServerSocketWriter::send( &writer, data, count );
        };
    for( int i=0; i<10 && buffer.stats.wouldBlock == 0; i++ ) {
        assert( buffer.drain( 1.1 + i*.01, 0, 15, 65536, send ) );
        }
    assert( buffer.stats.wouldBlock > 0 && buffer.pending() > 0 );
    assert( buffer.failure == NULL ); // previous server policy would disconnect here


    // Resume with a normal receive window; the small window above only forces
    // deterministic backpressure and would make recovery unnecessarily slow.
    int normal = 262144;
    setsockopt( reader, SOL_SOCKET, SO_RCVBUF, &normal, sizeof normal );
    timeval receiveTimeout = { 5, 0 };
    setsockopt( reader, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof receiveTimeout );
    std::string received;
    std::thread receiver( [&] {
        char data[8192];
        while( received.size() < filled + messages.size() ) {
            int count = recv( reader, data, sizeof data, 0 );
            assert( count > 0 );
            received.append( data, count );
            }
        } );
    auto started = std::chrono::steady_clock::now();
    while( buffer.pending() > 0 ) {
        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started ).count();
        assert( elapsed < 10 );
        assert( buffer.drain( 2 + elapsed, 0, 15, 65536, send ) );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    receiver.join();
    linger resetPeer = { 1, 0 };
    setsockopt( reader, SOL_SOCKET, SO_LINGER, &resetPeer, sizeof resetPeer );
    close( reader );
    assert( received == std::string( filled, 'X' ) + messages );
    printf( "native TCP: would_block=%llu partial=%llu bytes=%llu; recovered with exact stream\n",
            (unsigned long long)buffer.stats.wouldBlock,
            (unsigned long long)buffer.stats.partialWrites,
            (unsigned long long)messages.size() );
    ServerWriteResult closed = { 0, 0 };
    for( int i=0; i<100; ++i ) {
        closed = ServerSocketWriter::send( &writer,
            (const unsigned char*)"FM\n#", 4 );
        if( closed.sent == -1 ) break;
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    assert( closed.sent == -1 && closed.error != 0 );
    printf( "closed TCP peer: error=%d (%s); server process survived\n",
            closed.error, ServerSocketWriter::errorName( closed.error ) );
    }

int main() {
    setbuf( stdout, NULL );
    testOrderingAndRetries();
    testBatchingLimitsAgeAndReset();
    testFatalErrorAndValueCopies();
    testMemoryFailuresAndIsolation();
    testNativeTCPBackpressure();
    puts( "ServerSendBuffer tests passed" );
    }
