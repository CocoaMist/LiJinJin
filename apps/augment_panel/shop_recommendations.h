#pragma once

#include <QList>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <functional>

// Native shop presentation only. This module never discovers installations,
// calls LCU, chooses a build, purchases an item, or asserts hot-reload support.
namespace ShopRecommendations {

struct Context {
    QString sessionId;
    int championId = 0;
    QString championKey;
    QString sourceVersion;
    bool operator==(const Context &) const = default;
};

struct Item { int id = 0; int count = 1; };
struct Group { QString title; QList<Item> items; QJsonObject provenance; };
struct Request {
    Context context;
    QString sourceId;
    QString evidenceId;
    bool confirmed = false;
    // Caller must derive this allowlist from its already verified catalog.
    // A positive integer alone is not evidence that an item exists.
    QSet<int> verifiedItemIds;
    QList<Group> groups;
};
struct Result {
    bool ok = false;
    bool changed = false;
    QString code;
    QString message;
    QString filePath;
};

class Store {
public:
    // registrationFile must be an absolute, module-dedicated path in app state.
    // Its exact registered filename+digest determine what cleanup may remove.
    explicit Store(QString registrationFile);
    // Owner-thread API. Only a caller-supplied existing Game directory with a
    // regular League of Legends.exe is accepted. No directory scan or LCU.
    Result configureGameDirectory(const QString &gameDirectory);
    void setActiveContext(const Context &context);
    Context activeContext() const { return active; }
    // Perform on the owner thread after model freshness/quality gating. The
    // optional guard is rechecked immediately before the atomic page commit;
    // use it to verify the caller's current result revision/freshness as well.
    Result publish(const Request &, const std::function<bool()> &commitGuard = {});
    Result cleanup(const std::function<bool()> &commitGuard = {});
    QString gameDirectory() const { return gameDir; }
    // Per-user temporary module lock, identified by canonical Game+page, so
    // even Stores with different registration files serialize the same page.
    QString lockFilePath() const;
    static QString pageFileName();

private:
    QString registryFile, gameDir;
    Context active;
    quintptr ownerThread = 0;
    bool owner() const;
};

} // namespace ShopRecommendations
