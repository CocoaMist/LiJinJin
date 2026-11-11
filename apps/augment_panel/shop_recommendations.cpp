#include "shop_recommendations.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QThread>
#include <utility>

namespace ShopRecommendations {
namespace {
const QString moduleId = QStringLiteral("lixin_shop_recommendations_v1");
constexpr qint64 maximumFileBytes = 256 * 1024;

Result result(bool ok, const char *code, const QString &message, const QString &file = {}, bool changed = false) {
    return {ok, changed, QString::fromLatin1(code), message, file};
}
Result failure(const char *code, const QString &message) { return result(false, code, message); }
bool linked(const QFileInfo &info) { return info.isSymbolicLink() || info.isJunction(); }
bool text(const QString &value, int maximum) {
    if (value.trimmed().isEmpty() || value.size() > maximum) return false;
    for (QChar character : value) if (character.category() == QChar::Other_Control) return false;
    return true;
}
bool validContext(const Context &c) {
    static const QRegularExpression key(QStringLiteral("^[A-Za-z][A-Za-z0-9]{0,63}$"));
    return text(c.sessionId, 256) && c.championId > 0 && c.championId <= 100000
        && key.match(c.championKey).hasMatch() && text(c.sourceVersion, 80);
}
QString digest(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
bool readBytes(const QString &file, QByteArray &bytes) {
    const QFileInfo info(file);
    if (!info.isFile() || linked(info) || info.size() > maximumFileBytes) return false;
    QFile source(file);
    if (!source.open(QIODevice::ReadOnly)) return false;
    bytes = source.read(maximumFileBytes + 1);
    return source.error() == QFileDevice::NoError && bytes.size() <= maximumFileBytes;
}
bool atomicWrite(const QString &file, const QByteArray &bytes) {
    QSaveFile target(file);
    target.setDirectWriteFallback(false);
    return target.open(QIODevice::WriteOnly) && target.write(bytes) == bytes.size() && target.commit();
}

// Every existing path segment must remain an ordinary directory in the
// explicitly configured root. Reject links/junctions rather than following
// Config/Global/Recommended to some unrelated location.
bool gameRootValid(const QString &root) {
    const QFileInfo directory(root), executable(QDir(root).filePath(QStringLiteral("League of Legends.exe")));
    return directory.isAbsolute() && directory.isDir() && !linked(directory)
        && directory.canonicalFilePath() == root
        && directory.fileName().compare(QStringLiteral("Game"), Qt::CaseInsensitive) == 0
        && executable.isFile() && !linked(executable);
}
bool recommendedPath(const QString &root, bool create, QString &out) {
    if (!gameRootValid(root)) return false;
    QString current = root;
    for (const QString &part : {QStringLiteral("Config"), QStringLiteral("Global"), QStringLiteral("Recommended")}) {
        const QString next = QDir(current).filePath(part);
        QFileInfo info(next);
        if (linked(info)) return false;
        if (!info.exists()) {
            if (!create) { current = next; continue; }
            if (!QDir(current).mkdir(part)) return false;
            info.refresh();
        }
        if (info.exists() && (!info.isDir() || info.canonicalFilePath() != next)) return false;
        current = next;
    }
    out = current;
    return true;
}

struct Registration { bool present = false; QStringList hashes; QByteArray bytes; };
bool readRegistration(const QString &file, const QString &root, Registration &registration) {
    const QFileInfo info(file);
    if (linked(info)) return false;
    if (!info.exists()) return true;
    QByteArray bytes;
    if (!readBytes(file, bytes)) return false;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    const auto value = doc.object();
    if (error.error != QJsonParseError::NoError || !doc.isObject()
        || value.value("schema_version").toInt() != 1 || value.value("module").toString() != moduleId
        || value.value("game_directory").toString() != root
        || value.value("file_name").toString() != Store::pageFileName()
        || !value.value("sha256").isArray()) return false;
    static const QRegularExpression hash(QStringLiteral("^[0-9a-f]{64}$"));
    const auto hashes = value.value("sha256").toArray();
    if (hashes.isEmpty() || hashes.size() > 2) return false;
    for (const auto &entry : hashes) {
        if (!entry.isString() || !hash.match(entry.toString()).hasMatch()) return false;
        registration.hashes.append(entry.toString());
    }
    registration.present = true;
    registration.bytes = bytes;
    return true;
}
bool registrationUnchanged(const QString &file, const Registration &snapshot) {
    const QFileInfo info(file);
    if (linked(info) || info.exists() != snapshot.present) return false;
    if (!snapshot.present) return true;
    QByteArray bytes;
    return readBytes(file, bytes) && bytes == snapshot.bytes;
}
QByteArray registrationBytes(const QString &root, const QStringList &hashes) {
    QJsonArray values; for (const auto &hash : hashes) values.append(hash);
    return QJsonDocument(QJsonObject{{"schema_version", 1}, {"module", moduleId},
        {"game_directory", root}, {"file_name", Store::pageFileName()}, {"sha256", values}}).toJson(QJsonDocument::Compact);
}
bool ownedPage(const QByteArray &bytes, const Registration &registration) {
    if (!registration.present || !registration.hashes.contains(digest(bytes))) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        && document.object().value("_lixin").toObject().value("module").toString() == moduleId;
}
bool registryPathReady(const QString &path) {
    if (!QFileInfo(path).isAbsolute() || linked(QFileInfo(path))) return false;
    // Do not use mkpath on an unchecked state-directory chain.
    QString missing = QFileInfo(path).absolutePath();
    QStringList parts;
    while (!QFileInfo(missing).exists()) {
        const QFileInfo info(missing);
        if (linked(info) || info.fileName().isEmpty()) return false;
        parts.prepend(info.fileName()); missing = info.absolutePath();
    }
    QFileInfo parent(missing);
    if (!parent.isDir() || linked(parent) || parent.canonicalFilePath() != missing) return false;
    for (const auto &part : parts) {
        if (!QDir(missing).mkdir(part)) return false;
        missing = QDir(missing).filePath(part);
    }
    return true;
}
}

Store::Store(QString registrationFile)
    : registryFile(QDir::cleanPath(std::move(registrationFile))),
      ownerThread(reinterpret_cast<quintptr>(QThread::currentThreadId())) {}
bool Store::owner() const { return ownerThread == reinterpret_cast<quintptr>(QThread::currentThreadId()); }
QString Store::pageFileName() { return QStringLiteral("!!!lixin-recommended.json"); }
QString Store::lockFilePath() const {
    if (gameDir.isEmpty()) return {};
    QString identity = gameDir;
#ifdef Q_OS_WIN
    identity = identity.toCaseFolded();
#endif
    return QDir::tempPath() + QStringLiteral("/lixin-shop-recommendations-v1/!!!lixin-shop-")
        + digest((identity + '\n' + pageFileName()).toUtf8()) + QStringLiteral(".lock");
}
Result Store::configureGameDirectory(const QString &directory) {
    if (!owner()) return failure("wrong_thread", QStringLiteral("请在所属线程配置商店推荐"));
    const QFileInfo input(directory);
    if (!input.isAbsolute() || linked(input)) return failure("invalid_game_directory", QStringLiteral("请选择明确的 Game 目录"));
    const QString candidate = input.canonicalFilePath();
    QString recommended;
    if (candidate.isEmpty() || !recommendedPath(candidate, false, recommended))
        return failure("invalid_game_directory", QStringLiteral("Game 目录或推荐目录未通过校验"));
    if (!QFileInfo(registryFile).isAbsolute()
        || registryFile.compare(candidate, Qt::CaseInsensitive) == 0
        || registryFile.startsWith(candidate + '/', Qt::CaseInsensitive))
        return failure("invalid_registration", QStringLiteral("推荐登记必须使用 Game 目录以外的独立绝对路径"));
    Registration registration;
    if (!readRegistration(registryFile, candidate, registration))
        return failure("invalid_registration", QStringLiteral("推荐登记不匹配，请先处理原目录的推荐文件"));
    if (gameDir != candidate) active = {};
    gameDir = candidate;
    return result(true, "configured", QStringLiteral("已核对商店推荐目录"));
}
void Store::setActiveContext(const Context &context) { if (owner()) active = context; }

Result Store::publish(const Request &request, const std::function<bool()> &guard) {
    if (!owner()) return failure("wrong_thread", QStringLiteral("请在所属线程写入商店推荐"));
    if (!request.confirmed || !validContext(request.context) || !text(request.sourceId, 256)
        || !text(request.evidenceId, 256) || request.verifiedItemIds.isEmpty())
        return failure("unconfirmed_input", QStringLiteral("英雄、来源版本或装备资料尚未确认"));
    const auto current = [&] { return active == request.context && (!guard || guard()) && active == request.context; };
    if (!current()) return failure("stale_context", QStringLiteral("推荐已不属于当前英雄或对局"));
    if (request.groups.isEmpty() || request.groups.size() > 8)
        return failure("invalid_groups", QStringLiteral("推荐分组为空或过多"));
    QJsonArray blocks, provenance; int total = 0;
    for (const auto &group : request.groups) {
        if (!text(group.title, 96) || group.items.isEmpty() || group.items.size() > 32)
            return failure("invalid_groups", QStringLiteral("推荐分组内容无效"));
        QSet<int> seen; QJsonArray items;
        for (const auto &item : group.items) {
            if (item.id <= 0 || item.id > 1000000 || item.count < 1 || item.count > 9
                || seen.contains(item.id) || !request.verifiedItemIds.contains(item.id) || ++total > 128)
                return failure("unverified_item", QStringLiteral("包含未知、重复或未核实装备"));
            seen.insert(item.id);
            items.append(QJsonObject{{"id", QString::number(item.id)}, {"count", item.count}});
        }
        blocks.append(QJsonObject{{"type", group.title}, {"items", items}, {"recMath", false},
            {"minSummonerLevel", -1}, {"maxSummonerLevel", -1}, {"showIfSummonerSpell", ""}, {"hideIfSummonerSpell", ""}});
        if (QJsonDocument(group.provenance).toJson(QJsonDocument::Compact).size() > 8192)
            return failure("invalid_provenance", QStringLiteral("推荐来源记录过长"));
        provenance.append(group.provenance);
    }
    const QJsonObject page{{"title", QStringLiteral("李金金 · 当前出装 · %1").arg(request.context.sourceVersion)},
        {"championKey", request.context.championKey}, {"map", "any"}, {"mode", "any"},
        {"type", "custom"}, {"priority", true}, {"sortrank", 900000000}, {"blocks", blocks},
        {"_lixin", QJsonObject{{"module", moduleId}, {"schema_version", 1},
            {"champion_id", request.context.championId}, {"source_version", request.context.sourceVersion},
            {"source_id", request.sourceId}, {"evidence_id", request.evidenceId},
            {"groups", provenance}, {"automatic_purchase", false}, {"hot_reload_verified", false}}}};
    const QByteArray bytes = QJsonDocument(page).toJson(QJsonDocument::Compact);
    QString directory;
    if (gameDir.isEmpty() || !recommendedPath(gameDir, false, directory))
        return failure("invalid_game_directory", QStringLiteral("商店推荐目录已变化，停止写入"));
    const QString file = QDir(directory).filePath(pageFileName());
    if (!registryPathReady(registryFile))
        return failure("registration_failed", QStringLiteral("无法准备推荐登记目录，保留原文件"));
    if (!registryPathReady(lockFilePath())) return failure("lock_failed", QStringLiteral("无法准备商店推荐专用锁"));
    QLockFile moduleLock(lockFilePath());
    if (!moduleLock.tryLock(0)) return failure("busy", QStringLiteral("商店推荐正在更新，请稍后重试"));
    Registration registration;
    if (!readRegistration(registryFile, gameDir, registration))
        return failure("invalid_registration", QStringLiteral("商店推荐登记无法核对"));
    QByteArray previous;
    const QFileInfo old(file);
    if (linked(old) || (old.exists() && (!readBytes(file, previous) || !ownedPage(previous, registration))))
        return failure("ownership_conflict", QStringLiteral("同名文件未登记或已被修改，保留原文件"));
    if (old.exists() && previous == bytes) {
        if (!current()) return failure("stale_context", QStringLiteral("推荐已失效"));
        return result(true, "unchanged", QStringLiteral("商店推荐已是当前内容"), file);
    }
    if (!current()) return failure("stale_context", QStringLiteral("推荐已失效"));
    if (!recommendedPath(gameDir, true, directory))
        return failure("write_failed", QStringLiteral("无法准备推荐目录，保留原文件"));
    QSaveFile pending(file); pending.setDirectWriteFallback(false);
    if (!pending.open(QIODevice::WriteOnly) || pending.write(bytes) != bytes.size())
        return failure("write_failed", QStringLiteral("无法暂存推荐，保留原文件"));
    // Register old+new digests BEFORE replacing the page. A crash at either
    // boundary leaves a known digest; failed page commits retain the old page.
    QStringList hashes;
    if (!previous.isEmpty()) hashes.append(digest(previous));
    if (!hashes.contains(digest(bytes))) hashes.append(digest(bytes));
    if (!registrationUnchanged(registryFile, registration))
        return failure("registration_changed", QStringLiteral("推荐登记已变化，保留原文件与登记"));
    const QByteArray nextRegistration = registrationBytes(gameDir, hashes);
    if (!atomicWrite(registryFile, nextRegistration))
        return failure("registration_failed", QStringLiteral("无法登记推荐，保留原文件"));
    Registration pendingRegistration; pendingRegistration.present = true; pendingRegistration.bytes = nextRegistration;
    QString finalDirectory;
    if (!current() || !recommendedPath(gameDir, false, finalDirectory) || finalDirectory != directory) {
        pending.cancelWriting();
        return failure("stale_context", QStringLiteral("提交前推荐或目录已变化，保留原文件"));
    }
    // An external edit between preparation and commit must not be overwritten.
    QByteArray latest;
    if (linked(QFileInfo(file)) || QFileInfo(file).exists() != old.exists()
        || (old.exists() && (!readBytes(file, latest) || latest != previous))) {
        pending.cancelWriting();
        return failure("ownership_conflict", QStringLiteral("推荐文件已变化，保留原文件"));
    }
    if (!registrationUnchanged(registryFile, pendingRegistration)) {
        pending.cancelWriting();
        return failure("registration_changed", QStringLiteral("提交前推荐登记已变化，保留原文件与登记"));
    }
    if (!pending.commit()) return failure("write_failed", QStringLiteral("无法替换推荐，原文件保持不变"));
    return result(true, "written", QStringLiteral("已写入商店推荐；游戏是否实时刷新仍待验证"), file, true);
}

Result Store::cleanup(const std::function<bool()> &guard) {
    if (!owner()) return failure("wrong_thread", QStringLiteral("请在所属线程清理商店推荐"));
    QString directory;
    if (gameDir.isEmpty() || !recommendedPath(gameDir, false, directory))
        return failure("invalid_game_directory", QStringLiteral("商店推荐目录未通过校验"));
    if (!registryPathReady(registryFile)) return failure("registration_failed", QStringLiteral("无法准备推荐登记目录"));
    if (!registryPathReady(lockFilePath())) return failure("lock_failed", QStringLiteral("无法准备商店推荐专用锁"));
    QLockFile moduleLock(lockFilePath());
    if (!moduleLock.tryLock(0)) return failure("busy", QStringLiteral("商店推荐正在更新，请稍后重试"));
    Registration registration;
    if (!readRegistration(registryFile, gameDir, registration))
        return failure("invalid_registration", QStringLiteral("商店推荐登记无法核对"));
    if (!registration.present) return result(true, "nothing_registered", QStringLiteral("没有本模块登记的推荐"));
    const QString file = QDir(directory).filePath(pageFileName());
    const QFileInfo info(file); bool removed = false;
    if (linked(info)) return failure("ownership_conflict", QStringLiteral("推荐文件归属已变化，保留文件"));
    if (info.exists()) {
        QByteArray bytes;
        if (!readBytes(file, bytes) || !ownedPage(bytes, registration))
            return failure("ownership_conflict", QStringLiteral("推荐文件已被修改，保留文件"));
        if (guard && !guard()) return failure("cleanup_cancelled", QStringLiteral("清理已取消，保留文件"));
        QByteArray current;
        if (!registrationUnchanged(registryFile, registration))
            return failure("registration_changed", QStringLiteral("清理前推荐登记已变化，保留文件与登记"));
        if (!readBytes(file, current) || current != bytes)
            return failure("ownership_conflict", QStringLiteral("清理前推荐文件已变化，保留文件"));
        if (!QFile::remove(file)) return failure("cleanup_failed", QStringLiteral("无法清理已登记推荐"));
        removed = true;
    }
    if (!registrationUnchanged(registryFile, registration))
        return failure("registration_changed", QStringLiteral("推荐登记已变化，保留新登记"));
    if (!QFile::remove(registryFile)) return failure("cleanup_failed", QStringLiteral("推荐已清理，登记文件尚未删除"));
    return result(true, "cleaned", QStringLiteral("已清理本模块登记的推荐"), file, removed);
}
} // namespace ShopRecommendations
