#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>

// Holds only anonymous observations. Raw session IDs and account identities
// never appear in json(); callers fence asynchronous reads with generation().
class MatchContextTracker {
public:
    quint64 generation() const { return epoch; }
    QString contextId() const { return id; }
    // Local bridge only. The real game ID remains absent from public json().
    QString privateGameId() const { return gameId; }
    bool active() const { return !id.isEmpty(); }
    static bool isMatchPhase(const QString &);
    static bool isLoadingPhase(const QString &);
    void clear();
    void observePhase(const QString &);
    bool observeChampionSelect(const QJsonObject &,quint64 expectedGeneration);
    bool observeGameflow(const QJsonObject &,const QString &selfPuuid,quint64 expectedGeneration);
    bool observeLive(const QJsonArray &anonymousPlayers,double gameTime,quint64 expectedGeneration);
    QJsonObject json() const;
private:
    quint64 epoch=0,revision=0;
    QString id,gameId,phase,source;
    QJsonArray roster;
    bool alliesObserved=false,enemiesObserved=false,ownTeamKnown=false,loadingRosterMissing=false;
    double lastLiveTime=-1;
    qint64 startedAtMs=0;
    void begin(const QString &);
    void setRoster(QJsonArray,const QString &,bool,bool,bool);
};

QJsonObject analyzeMatchComposition(const QJsonObject &context,const QMap<int,QJsonObject> &champions);
QString parseObservedGamePatch(const QJsonValue &version);
// Anonymous binding proof scoped to one random local match context.
QString rosterSlotToken(const QString &contextId,const QString &puuid);
