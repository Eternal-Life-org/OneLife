#include "minorGems/util/SimpleVector.h"
#include "../../gameSource/GridPos.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

struct PlayerNameRecord { char* name; char* email; double timeCreated; int lineageEveID; };
struct CurseRecord {
    bool alive=false, bornCursed=false;
    double aliveStartTimeSinceTokenSpent=0, aliveStartTimeSinceScoreDecrement=0;
    double livedTimeSinceTokenSpent=0, livedTimeSinceScoreDecrement=0;
    double lastBirthTime=0, deathTime=0;
    GridPos deathPos={0,0};
};
struct CurseStatus { int curseLevel; };
struct RemoteUpdateRecord { char* email; char* action; int sequence; void* request; };
static SimpleVector<PlayerNameRecord> playerNames;
static SimpleVector<RemoteUpdateRecord> serverPendingUpdates;
static bool useCurseServer=false, usePersonalCurses=false;
static CurseRecord record;
class Time {
public:
    static double getCurrentTime() { return 100; }
};
static void stepCurses() {}
static CurseRecord* findCurseRecord(char*) { return &record; }
static CurseStatus getCurseLevel(CurseRecord*) { return {0}; }
char* stringDuplicate(const char* text) {
    char* copy=new char[std::strlen(text)+1];
    std::strcpy(copy,text);
    return copy;
}
static char* autoSprintf(const char* format,double seconds) {
    char text[256];
    std::snprintf(text,sizeof text,format,seconds);
    return stringDuplicate(text);
}

#include "playerNameCache.inc"

int main() {
    char email[]="eve@example", other[]="other@example", name[]="EVE 张", otherName[]="EVE 李";
    const GridPos position={1,2};
    for (int life=0; life<10000; ++life) {
        cursesLogBirth(email);
        logPlayerNameForCurses(email,name,life);
        assert(!isNameDuplicateForCurses(name) && playerNames.size()==0);
        cursesLogDeath(email,14,position);
    }
    assert(serverPendingUpdates.size()==0);
    usePersonalCurses=true;
    logPlayerNameForCurses(email,name,1);
    logPlayerNameForCurses(other,otherName,2);
    assert(isNameDuplicateForCurses(name) && playerNames.size()==2);
    cursesLogDeath(email,14,position);
    assert(!isNameDuplicateForCurses(name) && isNameDuplicateForCurses(otherName));
    for (int life=0; life<10000; ++life) {
        cursesLogBirth(email);
        logPlayerNameForCurses(email,name,life);
        assert(playerNames.size()==2 && isNameDuplicateForCurses(name));
        logPlayerNameForCurses(email,name,life);
        assert(playerNames.size()==2);
        cursesLogDeath(email,14,position);
        assert(playerNames.size()==1 && !isNameDuplicateForCurses(name));
    }
    logPlayerNameForCurses(email,name,1);
    cursesLogBirth(email);
    assert(!isNameDuplicateForCurses(name));
    forgetPlayerNames(other);
    assert(playerNames.size()==0);
    std::cout << "PASS: disabled cache, death cleanup, birth cleanup and repeated-name lifetimes\n";
}
