#pragma once
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>

class Catalog;
struct GameState;

// Verify one immutable public bundle without changing the pinned combat data.
QJsonObject loadExternalStrategyEvidence(const QString &root,const QString &expectedPatch);

// Return the latest verified same-patch bundle. Pointer probes and validation
// run in the background, at most once every two seconds; a rejected update
// preserves the last verified bundle. Explicitly injected, cleared or rejected
// snapshots stay pinned; a loader-produced not_installed result can recover.
QJsonObject currentExternalStrategyEvidence(const Catalog &);

// Provider statistics and dated claims are descriptive references only. The
// actual offered cards, legal purchase search and numerical model stay local.
QJsonObject externalStrategyReference(const Catalog &,const GameState &,
    const QSet<int> &allowedItems={},const QSet<int> &allowedAugments={},
    int offeredAugmentId=0,bool candidateOnly=false);

// Equipment-only continuity: preserve the public source's original patch,
// exclude changed items, and never project stale augment statistics or scores.
QJsonObject externalEquipmentReference(const Catalog &,const GameState &);

// Project at most three requested current offers from one immutable evidence
// snapshot. Values are identical to the scalar candidate-only lookup for each
// valid offer. Duplicate requests share one result; more than three requests
// reject the whole batch. Zero/unknown/not-offered IDs never mean "all cards".
// This is work-local preparation only, with no cross-call result cache.
// Comparisons can request the batch keys of omitted or conflicting statistics.
QJsonObject externalAugmentStrategyReferences(const Catalog &,const GameState &,
    const QList<int> &offeredAugmentIds,QSet<QString> *incompleteStatisticsBatches=nullptr);
QString augmentStatisticsBatchKey(const QJsonObject &row);
