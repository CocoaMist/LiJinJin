#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>
class QUrl;

// SUMMARY is an end-of-game record. Only the caller's in-memory identity is
// used for matching; no identifiers or server-provided text leave this class.
class SgpHistory {
public:
    struct Page { int received=0,added=0,invalid=0; };
    Page append(const QJsonArray &,const QString &targetPuuid);
    QJsonObject summary(const QString &server,bool exhausted,const QString &stopReason={}) const;
    QJsonArray records() const { return normalized; }
    int size() const { return seen.size(); }
private:
    QSet<QString> seen;
    QJsonArray normalized;
    QMap<QString,int> versions;
    int invalidGames=0,invalidKiwiGames=0,otherGames=0;
    qint64 first=0,last=0;
};

// Exact full Riot ID matching, shared by the interactive client and checks.
QJsonArray exactRiotAliases(const QJsonValue &,const QString &gameName,const QString &tagLine);
QJsonValue sgpJsonValue(const QByteArray &);
QMap<QString,QString> sgpServers();
bool allowedSgpUrl(const QUrl &,const QString &server);
