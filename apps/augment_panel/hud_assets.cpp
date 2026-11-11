#include "hud_assets.h"
#include "../native/catalog.h"
#include <QBuffer>
#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonDocument>
#include <QMap>
#include <QPromise>
#include <QRegularExpression>
#include <QSet>
#include <QThreadPool>

namespace {
constexpr qint64 MaxImageBytes = 512 * 1024;
struct IconEntry { QString relativePath, sha256; QSize size; qint64 bytes = 0; };
struct LoadedIcon { QImage image; QString error; };
QString hash(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
LoadedIcon loadLocal(const QString &base, const IconEntry &entry) {
    const QString absolute = QFileInfo(QDir(base).filePath(entry.relativePath)).canonicalFilePath();
    if (absolute.isEmpty() || !absolute.startsWith(base + QChar(u'/'), Qt::CaseInsensitive)) return {{}, QStringLiteral("path_unavailable")};
    QFile file(absolute);
    if (!file.open(QIODevice::ReadOnly) || file.size() != entry.bytes || file.size() > MaxImageBytes) return {{}, QStringLiteral("file_size_or_read_error")};
    auto bytes = file.read(MaxImageBytes + 1);
    if (bytes.size() != entry.bytes || hash(bytes) != entry.sha256) return {{}, QStringLiteral("image_hash_mismatch")};
    QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png");
    if (reader.size() != entry.size) return {{}, QStringLiteral("image_dimensions_mismatch")};
    const auto image = reader.read();
    if (image.isNull()) return {{}, QStringLiteral("image_decode_failed")};
    return {image, {}};
}
}

struct HudAssets::State {
    QString patch, base, status = QStringLiteral("manifest_missing"), manifestHash;
    QMap<QString, IconEntry> entries;
    QSet<int> itemIds, championIds;
    QSet<QString> pending;
    QMap<QString, QString> failed;
    QCache<QString, QPixmap> pixmaps{128};
    int itemReady = 0, championReady = 0, loaded = 0;
};

HudAssets::HudAssets(const Catalog &catalog, QObject *parent) : QObject(parent), state(std::make_unique<State>()) {
    state->patch = catalog.patch;
    for (auto it = catalog.items.cbegin(); it != catalog.items.cend(); ++it) state->itemIds.insert(it.key());
    for (auto it = catalog.champions.cbegin(); it != catalog.champions.cend(); ++it) state->championIds.insert(it.key());
    if (!QRegularExpression(QStringLiteral("^[0-9]{1,3}\\.[0-9]{1,3}$")).match(catalog.patch).hasMatch()) {
        state->status = QStringLiteral("invalid_catalog_patch"); return;
    }
    const QString root = QFileInfo(catalog.root).canonicalFilePath();
    state->base = QFileInfo(QDir(catalog.root).filePath(QStringLiteral(".local/hud-assets/") + catalog.patch)).canonicalFilePath();
    if (root.isEmpty() || state->base.isEmpty()) return;
    if (!state->base.startsWith(root + QChar(u'/'), Qt::CaseInsensitive)) { state->status = QStringLiteral("cache_path_outside_workspace"); return; }
    QFile file(QDir(state->base).filePath(QStringLiteral("manifest.json")));
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 2 * 1024 * 1024) return;
    const auto bytes = file.read(2 * 1024 * 1024 + 1);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) { state->status = QStringLiteral("invalid_manifest"); return; }
    const auto manifest = document.object();
    state->manifestHash = hash(bytes);
    if (manifest.value(QStringLiteral("schema_version")).toInt() != 1 || manifest.value(QStringLiteral("patch")).toString() != catalog.patch) {
        state->status = QStringLiteral("manifest_patch_mismatch"); return;
    }
    const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
    auto index = [&](const QString &kind, const QSet<int> &identities, int &ready) {
        const auto rows = manifest.value(kind).toObject();
        if (rows.size() != identities.size()) return false;
        for (int id : identities) {
            const auto key = QString::number(id);
            if (!rows.value(key).isObject()) return false;
            const auto row = rows.value(key).toObject();
            if (row.value(QStringLiteral("status")) != QStringLiteral("ready")) continue;
            IconEntry entry;
            entry.relativePath = row.value(QStringLiteral("path")).toString();
            entry.sha256 = row.value(QStringLiteral("sha256")).toString();
            entry.bytes = row.value(QStringLiteral("bytes")).toInteger();
            entry.size = QSize(row.value(QStringLiteral("width")).toInt(), row.value(QStringLiteral("height")).toInt());
            if (entry.relativePath != kind + QChar(u'/') + key + QStringLiteral(".png") || !digest.match(entry.sha256).hasMatch()
                || entry.bytes <= 0 || entry.bytes > MaxImageBytes || entry.size.width() < 16 || entry.size.width() > 512
                || entry.size.height() < 16 || entry.size.height() > 512) return false;
            state->entries.insert(kind + QChar(u':') + key, entry); ++ready;
        }
        return true;
    };
    if (!index(QStringLiteral("items"), state->itemIds, state->itemReady)
        || !index(QStringLiteral("champions"), state->championIds, state->championReady)) {
        state->entries.clear(); state->itemReady = state->championReady = 0;
        state->status = QStringLiteral("manifest_identity_or_entry_mismatch"); return;
    }
    state->status = state->itemReady == state->itemIds.size() && state->championReady == state->championIds.size()
        ? QStringLiteral("ready") : QStringLiteral("partial_artwork");
    QFile augmentFile(QDir(state->base).filePath("augments-manifest.json"));
    if(augmentFile.open(QIODevice::ReadOnly)&&augmentFile.size()<=2*1024*1024){
        const auto manifest=QJsonDocument::fromJson(augmentFile.readAll()).object();
        if(manifest.value("patch").toString()==catalog.patch&&manifest.value("schema_version").toInt()==1){
            const auto rows=manifest.value("augments").toObject();
            for(auto it=rows.begin();it!=rows.end();++it){bool ok=false;const int id=it.key().toInt(&ok);const auto row=it.value().toObject();
                IconEntry entry{row.value("path").toString(),row.value("sha256").toString(),QSize(row.value("width").toInt(),row.value("height").toInt()),row.value("bytes").toInteger()};
                if(ok&&id>0&&entry.relativePath=="augments/"+QString::number(id)+".png"&&digest.match(entry.sha256).hasMatch()&&entry.bytes>0&&entry.bytes<=MaxImageBytes&&entry.size.width()>=16&&entry.size.width()<=512&&entry.size.height()>=16&&entry.size.height()<=512)state->entries.insert("augments:"+QString::number(id),entry);
            }
        }
    }
}
HudAssets::~HudAssets() = default;
QPixmap HudAssets::item(int id) { return request(QStringLiteral("items"), id); }
QPixmap HudAssets::champion(int id) { return request(QStringLiteral("champions"), id); }
QPixmap HudAssets::augment(int id) { return request(QStringLiteral("augments"), id); }
QPixmap HudAssets::request(const QString &kind, int id) {
    const QString key = kind + QChar(u':') + QString::number(id);
    if (const auto pixmap = state->pixmaps.object(key)) return *pixmap;
    if (!state->entries.contains(key) || state->pending.contains(key) || state->failed.contains(key) || state->pending.size() >= 32) return {};
    state->pending.insert(key);
    const auto entry = state->entries.value(key);
    const auto base = state->base;
    auto promise = std::make_shared<QPromise<LoadedIcon>>();
    promise->start();
    auto watcher = new QFutureWatcher<LoadedIcon>(this);
    connect(watcher, &QFutureWatcher<LoadedIcon>::finished, this, [this, watcher, key] {
        const auto result = watcher->result(); watcher->deleteLater();
        state->pending.remove(key);
        if (result.image.isNull()) state->failed.insert(key, result.error);
        else { state->pixmaps.insert(key, new QPixmap(QPixmap::fromImage(result.image))); ++state->loaded; }
        if (onChanged) onChanged();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, base, entry] {
        promise->addResult(loadLocal(base, entry)); promise->finish();
    });
    return {};
}
QJsonObject HudAssets::report() const {
    QJsonObject reasons;
    for (auto it = state->failed.cbegin(); it != state->failed.cend(); ++it)
        reasons[it.value()] = reasons.value(it.value()).toInt() + 1;
    return {{QStringLiteral("patch"), state->patch}, {QStringLiteral("status"), state->status},
        {QStringLiteral("runtime_network"), false}, {QStringLiteral("asynchronous_local_decode"), true},
        {QStringLiteral("manifest_sha256"), state->manifestHash},
        {QStringLiteral("item_total"), state->itemIds.size()}, {QStringLiteral("item_ready"), state->itemReady},
        {QStringLiteral("champion_total"), state->championIds.size()}, {QStringLiteral("champion_ready"), state->championReady},
        {QStringLiteral("loaded_total"), state->loaded}, {QStringLiteral("cached"), state->pixmaps.size()},
        {QStringLiteral("pending"), state->pending.size()}, {QStringLiteral("failed"), state->failed.size()},
        {QStringLiteral("failure_reasons"), reasons}};
}
