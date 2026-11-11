#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QString>

class Catalog;
struct GameState;

// Anonymous observations only. No network, account identity, or inferred purchases.
class LiveEquipmentCues {
public:
    QJsonObject update(const Catalog &,const GameState &);
    QJsonObject report() const;
private:
    struct Inventory {int champion=0;QMap<int,int> counts;};
    QMap<QString,Inventory> inventories;
    QString contextId,patch,ownTeam,latestActor,reason=QStringLiteral("inactive");
    QJsonObject latest;
    QJsonArray timeline,changes;
    QJsonValue valueDifference=QJsonValue(QJsonValue::Null);
    double lastGameTime=-1;
    bool available=false;
    void clear(const QString &);
};
