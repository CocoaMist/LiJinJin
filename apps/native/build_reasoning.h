#pragma once
#include <QJsonObject>
#include <QList>
#include <QString>
class Catalog;
struct GameState;

// Source-bound requirements and explanations. These never create numeric
// effects, observed triggers, legal purchases, or causal win probabilities.
QString heroEquipmentReasoningRevision(const Catalog &);
QJsonObject heroEquipmentBuildReasoning(const Catalog &,const GameState &,int championId,const QList<int> &items);
QJsonObject protectionEquipmentReference(const Catalog &,const GameState &,int championId,const QString &style);
