#ifndef SERVER_SEND_BUFFER_INCLUDED
#define SERVER_SEND_BUFFER_INCLUDED

#include "minorGems/network/Socket.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>
#include <cerrno>
#include <cstring>

#ifdef WIN_32
#include <winsock.h>
#else
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#ifdef __linux__
#include <linux/sockios.h>
#endif
#endif

struct ServerWriteResult {
    int sent;
    int error;
};

// Capture the send syscall's error before another socket operation changes it.
// POSIX MSG_DONTWAIT also leaves the descriptor's receive flags untouched.
class ServerSocketWriter {
    public:
        static ServerWriteResult send( Socket *sock, const unsigned char *data,
                                       int size ) {
#ifdef WIN_32
            u_long mode = 1;
            if( ioctlsocket( sock->mNativeSocketID, FIONBIO, &mode ) != 0 ) {
                return { -1, WSAGetLastError() };
                }
            int sent = ::send( sock->mNativeSocketID,
                               reinterpret_cast<const char*>( data ), size, 0 );
            int error = sent < 0 ? WSAGetLastError() : 0;
            mode = 0;
            ioctlsocket( sock->mNativeSocketID, FIONBIO, &mode );
            if( sent < 0 && error == WSAEWOULDBLOCK ) sent = -2;
#else
            int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
#endif
            int sent = ::send( sock->mNativeSocketID, data, size, flags );
            int error = sent < 0 ? errno : 0;
            if( sent < 0 && ( error == EAGAIN || error == EWOULDBLOCK ) ) sent = -2;
#endif
            return { sent, error };
            }

        static void setNoDelay( Socket *sock ) {
            int flag = 1;
            setsockopt( sock->mNativeSocketID, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>( &flag ), sizeof flag );
            }

        static bool interrupted( int error ) {
#ifdef WIN_32
            return error == WSAEINTR;
#else
            return error == EINTR;
#endif
            }

        static const char *errorName( int error ) {
#ifdef WIN_32
            switch( error ) {
                case 0: return "none";
                case WSAEWOULDBLOCK: return "WSAEWOULDBLOCK";
                case WSAEINTR: return "WSAEINTR";
                case WSAECONNRESET: return "WSAECONNRESET";
                case WSAECONNABORTED: return "WSAECONNABORTED";
                case WSAENOTCONN: return "WSAENOTCONN";
                default: return "Winsock error";
                }
#else
            return error == 0 ? "none" : strerror( error );
#endif
            }
};

struct ServerSendStats {
    uint64_t messages = 0, requestedBytes = 0, sendCalls = 0, sentBytes = 0;
    uint64_t wouldBlock = 0, partialWrites = 0, interrupts = 0;
    uint64_t mxMessages = 0, mxCells = 0, mxPlainBytes = 0, mxWireBytes = 0;
    size_t peakPendingBytes = 0;
    int maxMXCells = 0;
};

// SimpleVector copies LiveObject during growth and tutorial transfers. Only the
// queue storage is shared by these copies, so copying a player never allocates
// or duplicates its backlog. Each newly connected player starts with no queue.
class ServerSendBuffer {
    private:
        struct Message {
            size_t end;
            double time;
            const char *function;
            int line;
            char type[3];
            int size;
        };

        static size_t &allocatedBytes() {
            // Server output runs exclusively on the main server thread.
            static size_t value = 0;
            return value;
            }

        struct Queue {
            std::vector<unsigned char> bytes;
            std::vector<Message> messages;
            size_t offset = 0, firstMessage = 0, accounted = 0;
            double nextAttempt = 0;
            void releaseStorage() {
                size_t released = bytes.capacity() + messages.capacity() * sizeof(Message);
                std::vector<unsigned char>().swap( bytes );
                std::vector<Message>().swap( messages );
                allocatedBytes() -= released;
                accounted -= released;
                offset = firstMessage = 0;
                }
            ~Queue() { allocatedBytes() -= accounted; }
        };
        std::shared_ptr<Queue> queue;
        static const size_t maxMessages = 16384;

        static size_t nextCapacity( size_t current, size_t required, size_t limit ) {
            return (std::min)( limit, (std::max)( required,
                current == 0 ? (std::min)( limit, size_t(32) ) : current * 2 ) );
            }

        template<class T>
        bool reserve( std::vector<T> &vector, size_t required, size_t limit,
                      size_t totalLimit ) {
            if( required <= vector.capacity() ) return true;
            size_t capacity = nextCapacity( vector.capacity(), required, limit );
            // Include the temporary old/new allocations during vector growth,
            // and the message metadata, in the global memory budget.
            size_t newBytes = capacity * sizeof(T);
            if( allocatedBytes() > totalLimit || newBytes > totalLimit - allocatedBytes() ) {
                failure = "Network output global memory limit";
                return false;
                }
            size_t previous = vector.capacity() * sizeof(T);
            vector.reserve( capacity );
            size_t added = vector.capacity() * sizeof(T) - previous;
            allocatedBytes() += added;
            queue->accounted += added;
            return true;
            }

        bool fail( const char *reason ) {
            failure = reason;
            lastSent = lastError = 0; // no send syscall was attempted
            return false;
            }

        void compact() {
            Queue &q = *queue;
            if( q.offset > 0 ) {
                q.bytes.erase( q.bytes.begin(), q.bytes.begin() + q.offset );
                for( size_t i=q.firstMessage; i<q.messages.size(); ++i ) {
                    q.messages[i].end -= q.offset;
                    }
                q.offset = 0;
                }
            if( q.firstMessage > 0 ) {
                q.messages.erase( q.messages.begin(), q.messages.begin() + q.firstMessage );
                q.firstMessage = 0;
                }
            }

    public:
        ServerSendStats stats;
        double reportStart = 0;
        bool socketPrepared = false;
        int lastRequested = 0, lastSent = 0, lastError = 0;
        const char *lastFunction = "none";
        int lastLine = 0;
        char lastType[3] = { '?', '?', 0 };
        const char *failure = NULL;

        static size_t totalAllocated() { return allocatedBytes(); }

        void reset() {
            // SimpleVector leaves a copy in unused slots after deletion.
            // Release its shared storage as well when this connection ends.
            if( queue ) queue->releaseStorage();
            queue.reset(); // releases storage without allocating
            stats = ServerSendStats();
            reportStart = 0;
            socketPrepared = false;
            lastRequested = lastSent = lastError = lastLine = 0;
            lastFunction = "none";
            lastType[0] = lastType[1] = '?';
            failure = NULL;
            }

        size_t pending() const { return queue ? queue->bytes.size() - queue->offset : 0; }
        size_t pendingMessages() const {
            return queue ? queue->messages.size() - queue->firstMessage : 0;
            }
        double due() const { return queue ? queue->nextAttempt : 0; }
        double oldestAge( double now ) const {
            return pendingMessages() == 0 ? 0 : (std::max)( 0.0,
                now - queue->messages[queue->firstMessage].time );
            }

        void observeMessage( const unsigned char *data, int size, double now,
                             const char *function, int line ) {
            if( reportStart == 0 ) reportStart = now;
            stats.messages++;
            stats.requestedBytes += size;
            lastRequested = size;
            lastFunction = function;
            lastLine = line;
            lastType[0] = size > 0 ? data[0] : '?';
            lastType[1] = size > 1 ? data[1] : '?';
            }

        void observeWrite( ServerWriteResult result, int requested ) {
            stats.sendCalls++;
            lastSent = result.sent;
            lastError = result.error;
            lastRequested = requested;
            if( result.sent > 0 ) {
                stats.sentBytes += result.sent;
                if( result.sent < requested ) stats.partialWrites++;
                }
            else if( result.sent == -2 ) stats.wouldBlock++;
            else if( ServerSocketWriter::interrupted( result.error ) ) stats.interrupts++;
            }

        void observeMX( int cells, int plainBytes, int wireBytes ) {
            stats.mxMessages++;
            stats.mxCells += cells;
            stats.mxPlainBytes += plainBytes;
            stats.mxWireBytes += wireBytes;
            stats.maxMXCells = (std::max)( stats.maxMXCells, cells );
            }

        bool enqueue( const unsigned char *data, int size, double now,
                      double interval, size_t limit, size_t totalLimit=67108864 ) {
            if( failure != NULL ) return false;
            if( size <= 0 || data == NULL ) return fail( "Invalid network output message" );
            if( pending() > limit || static_cast<size_t>(size) > limit - pending() ) {
                return fail( "Network output queue full" );
                }
            if( pendingMessages() >= maxMessages ) {
                return fail( "Network output message limit" );
                }
            try {
                if( !queue ) {
                    size_t overhead = sizeof(Queue) + 64;
                    if( allocatedBytes() > totalLimit || overhead > totalLimit - allocatedBytes() ) {
                        return fail( "Network output global memory limit" );
                        }
                    queue = std::make_shared<Queue>();
                    queue->accounted = overhead;
                    allocatedBytes() += overhead;
                    queue->nextAttempt = now + interval;
                    }
                Queue &q = *queue;
                if( q.offset > 0 && q.bytes.size() + size > limit ) compact();
                if( q.firstMessage > 0 && q.messages.size() >= maxMessages ) compact();
                if( !reserve( q.messages, q.messages.size() + 1, maxMessages, totalLimit ) ||
                    !reserve( q.bytes, q.bytes.size() + size, limit, totalLimit ) ) {
                    return fail( failure );
                    }
                // Both vectors have capacity now; these trivial-type inserts
                // cannot allocate. A failed reservation leaves the stream intact.
                q.bytes.insert( q.bytes.end(), data, data + size );
                Message message = { q.bytes.size(), now, lastFunction, lastLine,
                                    { lastType[0], lastType[1], 0 }, size };
                q.messages.push_back( message );
                stats.peakPendingBytes = (std::max)( stats.peakPendingBytes, pending() );
                return true;
                }
            catch( const std::bad_alloc & ) {
                return fail( "Network output allocation failed" );
                }
            }

        // One bounded, nonblocking syscall per player per drain. On -2/EINTR
        // keep all bytes; on partial writes advance only by the actual count.
        template<class Sender>
        bool drain( double now, double interval, double maxAge,
                    size_t budget, Sender sender ) {
            if( failure != NULL ) return false;
            if( pending() == 0 ) return true;
            Queue &q = *queue;
            const Message &head = q.messages[q.firstMessage];
            if( oldestAge( now ) >= maxAge ) {
                setHeadContext( head );
                lastRequested = head.size;
                return fail( "Network output queue timed out" );
                }
            if( now < q.nextAttempt || budget == 0 ) return true;
            setHeadContext( head );
            int requested = static_cast<int>( (std::min)( size_t(65536),
                (std::min)( budget, pending() ) ) );
            ServerWriteResult result = sender( q.bytes.data() + q.offset, requested );
            observeWrite( result, requested );
            q.nextAttempt = now + (std::max)( 0.001, interval );
            if( result.sent == -2 || ServerSocketWriter::interrupted( result.error ) ) {
                return true;
                }
            if( result.sent <= 0 || result.sent > requested ) {
                failure = "Socket write failed";
                return false;
                }
            q.offset += result.sent;
            while( q.firstMessage < q.messages.size() &&
                   q.messages[q.firstMessage].end <= q.offset ) ++q.firstMessage;
            if( q.offset == q.bytes.size() ) {
                q.releaseStorage();
                queue.reset(); // release memory also held by unused vector slots
                }
            else if( q.offset >= 65536 || q.offset >= q.bytes.size() / 2 ||
                     q.firstMessage >= 256 ) {
                compact();
                }
            return true;
            }

        void restartReport( double now ) {
            stats = ServerSendStats();
            stats.peakPendingBytes = pending();
            reportStart = now;
            }

    private:
        void setHeadContext( const Message &head ) {
            lastFunction = head.function;
            lastLine = head.line;
            lastType[0] = head.type[0];
            lastType[1] = head.type[1];
            }
};

#endif
