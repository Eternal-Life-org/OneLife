#ifndef ONE_LIFE_LOGIN_FAILURE_H
#define ONE_LIFE_LOGIN_FAILURE_H

#include <climits>
#include <cstdio>
#include <cstring>

enum class LoginRejectReason {
    None, AuthInvalid, AuthBlocked, WhitelistDenied,
    AuthServiceUnavailable, ClientIncompatible, LoginTimeout,
    FamilyUnavailable, LiveUnavailable, JoinUnavailable, ServerError
};

inline const char *loginRejectCode( LoginRejectReason reason ) {
    switch( reason ) {
        case LoginRejectReason::AuthInvalid: return "AUTH_INVALID";
        case LoginRejectReason::AuthBlocked: return "AUTH_BLOCKED";
        case LoginRejectReason::WhitelistDenied: return "WHITELIST_DENIED";
        case LoginRejectReason::AuthServiceUnavailable: return "AUTH_SERVICE_UNAVAILABLE";
        case LoginRejectReason::ClientIncompatible: return "CLIENT_INCOMPATIBLE";
        case LoginRejectReason::LoginTimeout: return "LOGIN_TIMEOUT";
        case LoginRejectReason::FamilyUnavailable: return "FAMILY_UNAVAILABLE";
        case LoginRejectReason::LiveUnavailable: return "LIVE_UNAVAILABLE";
        case LoginRejectReason::JoinUnavailable: return "JOIN_UNAVAILABLE";
        default: return "SERVER_ERROR";
    }
}

// The first line and the # terminator are unchanged for old clients.
inline int formatLoginReject( LoginRejectReason reason, char *out, int capacity ) {
    if( out == NULL || capacity <= 0 ) return 0;
    int length = std::snprintf( out, capacity, "REJECTED\nERR1 %s\n#",
                                loginRejectCode( reason ) );
    return length > 0 && length < capacity ? length : 0;
}

struct LoginRejectSend {
    char frame[64];
    int length;
    int sent;

    void reset() { length = 0; sent = 0; }
    bool prepare( LoginRejectReason reason ) {
        sent = 0;
        length = formatLoginReject( reason, frame, sizeof frame );
        return length > 0;
    }
    bool pending() const { return sent < length; }
    unsigned char *tail() {
        return (unsigned char *)frame + sent;
    }
    int remaining() const { return length - sent; }
    void advance( int count ) {
        if( count > 0 && count <= remaining() ) sent += count;
    }
};

enum class TicketCheckResult { Valid, Invalid, Blocked, Unavailable };

inline TicketCheckResult parseTicketCheckResult( int status,
                                                 const unsigned char *body,
                                                 int length ) {
    if( status != 200 || body == NULL )
        return TicketCheckResult::Unavailable;
    if( length == 5 && memcmp( body, "VALID", 5 ) == 0 )
        return TicketCheckResult::Valid;
    if( length == 7 && memcmp( body, "INVALID", 7 ) == 0 )
        return TicketCheckResult::Invalid;
    if( length == 23 && memcmp( body,
                             "INVALID\nACCOUNT_BLOCKED", 23 ) == 0 )
        return TicketCheckResult::Blocked;
    return TicketCheckResult::Unavailable;
}

inline bool parseLoginDecimal( const char *text, int &value ) {
    if( text == NULL || text[0] == '\0' ) return false;
    unsigned int result = 0;
    for( const unsigned char *p = (const unsigned char *)text; *p; ++p ) {
        if( *p < '0' || *p > '9' ) return false;
        unsigned int digit = *p - '0';
        if( result > ( (unsigned int)INT_MAX - digit ) / 10 ) return false;
        result = result * 10 + digit;
    }
    value = (int)result;
    return true;
}

inline bool isLoginHash( const char *text ) {
    if( text == NULL || strlen( text ) != 40 ) return false;
    for( const char *p = text; *p; ++p ) {
        if( !( *p >= '0' && *p <= '9' ) &&
            !( *p >= 'a' && *p <= 'f' ) &&
            !( *p >= 'A' && *p <= 'F' ) ) return false;
    }
    return true;
}

struct ParsedLoginHeader {
    const char *email;
    int mapD;
    int tutorialNumber;
    int liveTargetID;
    int twinCount;
    bool live;
};

// Only parses the fixed LOGIN fields.  Seed/family/twin handling remains in
// the existing login flow, after this validation succeeds.
inline bool parseLoginHeader( const char *const *tokens, int count,
                              int defaultMapD, int maxMapD, int maxTwinCount,
                              ParsedLoginHeader &out ) {
    if( tokens == NULL || ( count != 4 && count != 5 && count != 7 ) ||
        tokens[0] == NULL || tokens[1] == NULL ||
        tokens[2] == NULL || tokens[3] == NULL ) return false;
    bool live = strcmp( tokens[0], "LVOLOGIN" ) == 0;
    if( !live && strcmp( tokens[0], "LOGIN" ) != 0 ) return false;
    if( live && count < 5 ) return false;
    if( !isLoginHash( tokens[2] ) || !isLoginHash( tokens[3] ) ) return false;

    const char *email = tokens[1];
    size_t emailLength = strlen( email );
    if( emailLength == 0 || emailLength > 512 ) return false;
    int mapD = defaultMapD;
    const char *at = strchr( email, '@' );
    if( at != NULL && at - email <= 3 ) {
        char prefix[4];
        size_t prefixLength = at - email;
        if( prefixLength == 0 || at[1] == '\0' ) return false;
        memcpy( prefix, email, prefixLength );
        prefix[prefixLength] = '\0';
        if( !parseLoginDecimal( prefix, mapD ) || mapD < 1 ) return false;
        if( mapD > maxMapD ) mapD = defaultMapD;
        email = at + 1;
    }

    int option = 0;
    if( count >= 5 &&
        ( !parseLoginDecimal( tokens[4], option ) || ( live && option < 1 ) ) )
        return false;
    int twinCount = 0;
    if( count == 7 ) {
        if( tokens[5] == NULL || tokens[5][0] == '\0' ||
            strlen( tokens[5] ) > 128 || tokens[6] == NULL ||
            !parseLoginDecimal( tokens[6], twinCount ) ) return false;
        if( maxTwinCount < 0 ) maxTwinCount = 0;
        if( twinCount > maxTwinCount ) twinCount = maxTwinCount;
    }
    out.email = email;
    out.mapD = mapD;
    out.tutorialNumber = live ? 0 : option;
    out.liveTargetID = live ? option : -1;
    out.twinCount = twinCount;
    out.live = live;
    return true;
}

#endif
