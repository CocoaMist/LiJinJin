#pragma once
#include <QJsonObject>
#include <QSet>
class Catalog;

// Language evidence explains candidate mechanics. It never supplies observed
// state, numerical effects, purchase legality or recommendation scores.
QJsonObject candidateSemanticEvidence(const Catalog &,int championId,
    const QSet<int> &itemIds,const QSet<int> &augmentIds);
QJsonObject filterCandidateSemanticEvidence(const QJsonObject &,int championId,
    const QSet<int> &itemIds,const QSet<int> &augmentIds);
