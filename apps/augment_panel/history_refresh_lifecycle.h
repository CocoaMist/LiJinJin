#pragma once
#include <string_view>
#include <string>
#include <cstdint>

// Phase observations only; missing/unknown observations never end a match.
class HistoryRefreshLifecycle {
public:
    enum class Event { None, EnterMatch, LeaveMatch };
    static bool insidePhase(std::string_view phase) {
        return phase=="GameStart"||phase=="LoadingScreen"||phase=="Loading"||phase=="PreStart"
            ||phase=="Start"||phase=="InProgress"||phase=="Reconnect";
    }
    static bool outsidePhase(std::string_view phase) {
        return phase=="None"||phase=="Lobby"||phase=="Matchmaking"||phase=="ReadyCheck"
            ||phase=="ChampSelect"||phase=="WaitingForStats"||phase=="PreEndOfGame"
            ||phase=="EndOfGame"||phase=="TerminatedInError";
    }
    Event observe(std::string_view phase) {
        if(!insidePhase(phase)&&!outsidePhase(phase))return Event::None;
        const bool next=insidePhase(phase);
        const auto event=next&&(!known||!inside)?Event::EnterMatch:
            known&&inside&&!next?Event::LeaveMatch:Event::None;
        known=true;inside=next;return event;
    }
    bool inMatch() const {return inside;}
private:
    bool known=false,inside=false;
};

// One retained event, including its sole optional leave followup. Time is
// injected so busy, login and deadline cases can be checked without a GUI.
class HistoryRefreshWork {
public:
    enum class State { Idle, Pending, Running, WaitingIdentity, WaitingFollowup, Finished, Expired };
    enum class Outcome { Ready, LoginRequired, RetryBeforeNetwork, SourceBusy, Failed };
    static constexpr std::int64_t LifetimeMs=180000, FollowupMs=15000;
    void begin(bool entering,std::int64_t now){enter=entering;phase=State::Pending;attemptNumber=0;retries=0;until=now+LifetimeMs;}
    void cancel(){phase=State::Idle;}
    void tick(std::int64_t now){
        if(active()&&now>=until){phase=State::Expired;return;}
        if(phase==State::WaitingFollowup&&now>=followupAt){phase=State::Pending;attemptNumber=1;retries=0;}
    }
    bool start(std::int64_t now){tick(now);if(phase!=State::Pending)return false;phase=State::Running;return true;}
    void complete(Outcome result,std::int64_t now){
        tick(now);if(phase!=State::Running)return;
        if(result==Outcome::LoginRequired&&retries<2){phase=State::WaitingIdentity;return;}
        if(result==Outcome::SourceBusy){phase=State::Pending;return;}
        if(result==Outcome::RetryBeforeNetwork&&retries<2){++retries;phase=State::Pending;return;}
        if(!enter&&attemptNumber==0){phase=State::WaitingFollowup;followupAt=now+FollowupMs;return;}
        phase=State::Finished;
    }
    void identityAvailable(std::int64_t now){tick(now);if(phase==State::WaitingIdentity&&retries<2){++retries;phase=State::Pending;}}
    bool active()const{return phase==State::Pending||phase==State::Running||phase==State::WaitingIdentity||phase==State::WaitingFollowup;}
    bool pending()const{return phase==State::Pending;}
    bool entering()const{return enter;}
    int attempt()const{return attemptNumber;}
    int retryCount()const{return retries;}
    State state()const{return phase;}
    std::int64_t remaining(std::int64_t now)const{return until>now?until-now:0;}
    std::int64_t followupRemaining(std::int64_t now)const{return followupAt>now?followupAt-now:0;}
private:
    State phase=State::Idle;
    bool enter=false;
    int attemptNumber=0,retries=0;
    std::int64_t until=0,followupAt=0;
};

class HistoryAccountScope {
public:
    enum class Change { None, First, Changed };
    Change observe(const std::string &next){if(next==key)return Change::None;const auto change=key.empty()?Change::First:Change::Changed;key=next;return change;}
    bool known()const{return !key.empty();}
    const std::string &value()const{return key;}
private:
    std::string key;
};

class HistoryPageReload {
public:
    void request(){queued=true;}
    void cancel(){queued=false;}
    bool pending()const{return queued;}
    bool take(bool busy,bool ownPage,bool visible,bool profileReady){
        if(!queued||busy)return false;
        if(!ownPage){queued=false;return false;}
        if(!visible||!profileReady)return false;
        queued=false;return true;
    }
private:
    bool queued=false;
};
