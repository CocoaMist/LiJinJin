#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QtGlobal>

class Catalog;
struct GameState;

// The unranked mechanism/advisory pair used by the three-card HUD. This helper
// also provides the uncached control for deterministic replay/benchmark checks.
QJsonObject buildCardReferencesUncached(const Catalog &, const GameState &);

// One-entry, owner-thread cache. It does not cache a recommendation score,
// purchase decision, affordability, or any live observation. Call get() again
// for every render so freshness and offer admission are checked before reuse.
// offerBindingRevision must change when the caller rebinds an observed offer
// window, including a reroll that happens to show the same ordered three IDs.
// External reference revisions identify immutable snapshots, as enforced by
// the installed content-addressed loader; available unversioned packs bypass
// reuse. The complete external bundle is never hashed per render.
class CardReferenceCache {
public:
    QJsonObject get(const Catalog &, const GameState &, const QString &offerBindingRevision = {});
    void clear();
    QJsonObject statistics() const;

private:
    QMap<int, QJsonObject> identitySource_;
    QJsonArray augmentIdentities_;
    QJsonObject key_, result_;
    bool valid_ = false;
    quint64 calls_ = 0, hits_ = 0, misses_ = 0, builds_ = 0, rejected_ = 0, unversioned_ = 0;
    qint64 elapsedNs_ = 0, buildNs_ = 0;
};
