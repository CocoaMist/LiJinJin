#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <memory>
class CombatDatabase;

class Catalog {
public:
    bool load(const QString &root, QString *error);
    QMap<int, QJsonObject> champions, augments, items;
    QJsonArray mechanics;
    std::shared_ptr<CombatDatabase> combat;
    QString root, patch, catalogPath;
    QJsonObject runtimeManifest;
    // Optional immutable public strategy references, separate from numerical
    // combat rules. Runtime consumers use currentExternalStrategyEvidence for
    // bounded background refresh; this initial verified snapshot stays intact.
    QJsonObject strategyEvidence;
    bool ordinaryOfferEligible(int id) const;
    bool rewardOnlyAugment(int id) const;
    QString offerEligibilityReason(int id) const;
    int championId(const QString &name) const;
    QString championName(int id) const;
    QString itemName(int id) const;
    QJsonObject champion(int id) const { return champions.value(id); }
};

QJsonDocument loadJson(const QString &file, QString *error = nullptr);
QString plainText(QString html);
