#include "../loginFailure.h"

#include <cassert>
#include <cstring>
#include <string>

int main() {
    char frame[64];
    int length = formatLoginReject( LoginRejectReason::AuthBlocked,
                                    frame, sizeof frame );
    assert( length == (int)strlen( "REJECTED\nERR1 AUTH_BLOCKED\n#" ) );
    assert( strcmp( frame, "REJECTED\nERR1 AUTH_BLOCKED\n#" ) == 0 );
    assert( formatLoginReject( LoginRejectReason::AuthBlocked, frame, 8 ) == 0 );
    LoginRejectSend sender;
    sender.reset();
    assert( sender.prepare( LoginRejectReason::AuthBlocked ) );
    std::string delivered;
    while( sender.pending() ) {
        int count = sender.remaining() < 4 ? sender.remaining() : 4;
        delivered.append( (const char *)sender.tail(), count );
        sender.advance( count );
        sender.advance( -2 ); // would-block leaves the unsent tail intact
    }
    assert( delivered == "REJECTED\nERR1 AUTH_BLOCKED\n#" );

    const unsigned char valid[] = "VALID";
    const unsigned char paddedValid[] = "VALID\n";
    const unsigned char invalid[] = "INVALID";
    const unsigned char blocked[] = "INVALID\nACCOUNT_BLOCKED";
    const unsigned char misleading[] = "ERROR: NOT_VALID";
    const unsigned char embeddedNul[] = { 'V', 'A', 'L', 'I', 'D', 0, 'X' };
    assert( parseTicketCheckResult( 200, valid, sizeof valid - 1 ) ==
            TicketCheckResult::Valid );
    assert( parseTicketCheckResult( 200, paddedValid,
                                   sizeof paddedValid - 1 ) ==
            TicketCheckResult::Unavailable );
    assert( parseTicketCheckResult( 200, invalid, sizeof invalid - 1 ) ==
            TicketCheckResult::Invalid );
    assert( parseTicketCheckResult( 200, blocked, sizeof blocked - 1 ) ==
            TicketCheckResult::Blocked );
    assert( parseTicketCheckResult( 500, valid, sizeof valid - 1 ) ==
            TicketCheckResult::Unavailable );
    assert( parseTicketCheckResult( 200, misleading, sizeof misleading - 1 ) ==
            TicketCheckResult::Unavailable );
    assert( parseTicketCheckResult( 200, embeddedNul, sizeof embeddedNul ) ==
            TicketCheckResult::Unavailable );
    assert( parseTicketCheckResult( 200, NULL, 0 ) ==
            TicketCheckResult::Unavailable );
    assert( parseTicketCheckResult( 200, valid, 4096 ) ==
            TicketCheckResult::Unavailable );

    const char *hash = "0123456789ABCDEF0123456789ABCDEF01234567";
    const char *normal[] = { "LOGIN", "64@123456|spawn", hash, hash, "0" };
    ParsedLoginHeader parsed;
    assert( parseLoginHeader( normal, 5, 64, 64, 4, parsed ) );
    assert( strcmp( parsed.email, "123456|spawn" ) == 0 );
    assert( parsed.mapD == 64 && parsed.tutorialNumber == 0 && !parsed.live );

    const char *family[] = { "LOGIN", "123456:Family", hash, hash };
    assert( parseLoginHeader( family, 4, 64, 64, 4, parsed ) );
    assert( strcmp( parsed.email, "123456:Family" ) == 0 );
    const char *twins[] = { "LOGIN", "123456", hash, hash,
                            "0", hash, "9" };
    assert( parseLoginHeader( twins, 7, 64, 64, 4, parsed ) &&
            parsed.twinCount == 4 );
    const char *live[] = { "LVOLOGIN", "123456", hash, hash, "42" };
    assert( parseLoginHeader( live, 5, 64, 64, 4, parsed ) &&
            parsed.live && parsed.liveTargetID == 42 );

    const char *badMap[] = { "LOGIN", "x@123456", hash, hash };
    const char *zeroMap[] = { "LOGIN", "0@123456", hash, hash };
    const char *badHash[] = { "LOGIN", "123456", "invalid", hash };
    const char *badLive[] = { "LVOLOGIN", "123456", hash, hash, "-1" };
    const char *badNumber[] = { "LOGIN", "123456", hash, hash,
                                "999999999999999999999999" };
    assert( !parseLoginHeader( badMap, 4, 64, 64, 4, parsed ) );
    assert( !parseLoginHeader( zeroMap, 4, 64, 64, 4, parsed ) );
    assert( !parseLoginHeader( badHash, 4, 64, 64, 4, parsed ) );
    assert( !parseLoginHeader( badLive, 5, 64, 64, 4, parsed ) );
    assert( !parseLoginHeader( badNumber, 5, 64, 64, 4, parsed ) );
}
