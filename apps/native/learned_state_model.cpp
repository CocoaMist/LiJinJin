#include "learned_state_model.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

namespace {
bool numeric(const QJsonValue &v) { return v.isDouble() && std::isfinite(v.toDouble()); }
bool whole(const QJsonValue &v, int low, int high) { return numeric(v) && v.toDouble() >= low && v.toDouble() <= high && std::floor(v.toDouble()) == v.toDouble(); }
double clip(double n, double limit) { return std::clamp(n, -limit, limit); }
double logit(double p) { p = std::clamp(p, 1e-8, 1-1e-8); return std::log(p/(1-p)); }
qint64 steadyMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct ArtifactStamp {
    bool exists = false, regular = false, readable = true;
    qint64 size = -1, modified = 0, changed = 0, born = 0;
    quint64 device = 0, identity = 0;
    QString target;
    bool operator==(const ArtifactStamp &) const = default;
};
ArtifactStamp artifactStamp(const QString &path) {
    const QFileInfo info(path);
    ArtifactStamp stamp;
    stamp.exists = info.exists();
    if (!stamp.exists) return stamp;
    stamp.regular = info.isFile();
    stamp.size = info.size();
    stamp.modified = info.lastModified().toMSecsSinceEpoch();
    stamp.born = info.birthTime().toMSecsSinceEpoch();
    stamp.target = info.canonicalFilePath();
    if (!stamp.regular) return stamp;
#ifdef Q_OS_WIN
    const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { stamp.readable = false; return stamp; }
    BY_HANDLE_FILE_INFORMATION identity{};
    FILE_BASIC_INFO basic{};
    stamp.readable = GetFileInformationByHandle(handle, &identity) &&
        GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic));
    if (stamp.readable) {
        stamp.device = identity.dwVolumeSerialNumber;
        stamp.identity = (quint64(identity.nFileIndexHigh) << 32) | identity.nFileIndexLow;
        stamp.size = qint64((quint64(identity.nFileSizeHigh) << 32) | identity.nFileSizeLow);
        stamp.modified = basic.LastWriteTime.QuadPart;
        stamp.changed = basic.ChangeTime.QuadPart;
        stamp.born = basic.CreationTime.QuadPart;
    }
    CloseHandle(handle);
#elif defined(Q_OS_UNIX)
    struct stat native{};
    stamp.readable = ::stat(QFile::encodeName(path).constData(), &native) == 0;
    if (stamp.readable) {
        stamp.regular = S_ISREG(native.st_mode);
        stamp.device = quint64(native.st_dev);
        stamp.identity = quint64(native.st_ino);
        stamp.size = native.st_size;
#ifdef Q_OS_MACOS
        stamp.modified = qint64(native.st_mtimespec.tv_sec) * 1000000000 + native.st_mtimespec.tv_nsec;
        stamp.changed = qint64(native.st_ctimespec.tv_sec) * 1000000000 + native.st_ctimespec.tv_nsec;
#else
        stamp.modified = qint64(native.st_mtim.tv_sec) * 1000000000 + native.st_mtim.tv_nsec;
        stamp.changed = qint64(native.st_ctim.tv_sec) * 1000000000 + native.st_ctim.tv_nsec;
#endif
    }
#endif
    return stamp;
}
QJsonObject abstain(const QString &reason) { return {{"available",false},{"reason",reason},{"causal_choice_value",false}}; }
LearnedStateArtifactCache &artifactCache() { static LearnedStateArtifactCache cache; return cache; }
}

struct LearnedStateArtifactCache::Impl {
    struct Entry {
        ArtifactStamp stamp;
        QJsonObject object;
        QByteArray digest;
        qint64 checkedAt = 0, contentCheckedAt = 0;
        quint64 touched = 0;
        bool initialized = false, retryRead = false;
    };
    explicit Impl(Clock source) : clock(source ? std::move(source) : Clock(steadyMilliseconds)) {}
    mutable QMutex mutex;
    Clock clock;
    QHash<QString, Entry> entries;
    quint64 touches = 0, hits = 0, probes = 0, reads = 0, parses = 0, failures = 0;
};

LearnedStateArtifactCache::LearnedStateArtifactCache(Clock clock) : impl(std::make_unique<Impl>(std::move(clock))) {}
LearnedStateArtifactCache::~LearnedStateArtifactCache() = default;

QJsonObject LearnedStateArtifactCache::read(const QString &root, const QString &relativePath) {
    if (root.trimmed().isEmpty() || relativePath.isEmpty()) return {};
    const auto normalizedRoot = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    const auto path = QDir::cleanPath(QDir(normalizedRoot).absoluteFilePath(relativePath));
    auto key = normalizedRoot + QChar(u'\n') + path;
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    QMutexLocker lock(&impl->mutex);
    if (!impl->entries.contains(key) && impl->entries.size() >= maximumEntries) {
        auto oldest = impl->entries.begin();
        for (auto it = impl->entries.begin(); it != impl->entries.end(); ++it)
            if (it->touched < oldest->touched) oldest = it;
        impl->entries.erase(oldest);
    }
    auto &entry = impl->entries[key];
    entry.touched = ++impl->touches;
    const qint64 now = impl->clock();
    if (entry.initialized && now >= entry.checkedAt && now - entry.checkedAt < probeIntervalMs) {
        ++impl->hits;
        return entry.object;
    }
    const auto stamp = artifactStamp(path);
    ++impl->probes;
    const bool recheckContent = entry.initialized &&
        (now < entry.contentCheckedAt || now - entry.contentCheckedAt >= fullRecheckIntervalMs);
    const bool changed = !entry.initialized || !(entry.stamp == stamp);
    entry.initialized = true;
    entry.checkedAt = now;
    if (!changed && !entry.retryRead && !recheckContent) { ++impl->hits; return entry.object; }
    entry.stamp = stamp;
    entry.retryRead = false;
    entry.contentCheckedAt = now;
    const auto failClosed = [&] { entry.object = {}; entry.digest.clear(); ++impl->failures; };
    if (!stamp.exists || !stamp.regular || !stamp.readable || stamp.size < 0 || stamp.size > maximumArtifactBytes) {
        failClosed();
        entry.retryRead = stamp.exists && stamp.regular && !stamp.readable;
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { failClosed(); entry.retryRead = true; return {}; }
    ++impl->reads;
    const auto bytes = file.read(maximumArtifactBytes + 1);
    const bool readOkay = file.error() == QFileDevice::NoError && bytes.size() <= maximumArtifactBytes && file.atEnd();
    file.close();
    const auto after = artifactStamp(path);
    ++impl->probes;
    // Never publish a read spanning an atomic replacement or an in-place write.
    if (!readOkay || !(after == stamp)) { failClosed(); entry.retryRead = true; return {}; }
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    if (digest == entry.digest) { ++impl->hits; return entry.object; }
    entry.digest = digest;
    ++impl->parses;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        entry.object = {};
        ++impl->failures;
        return {};
    }
    entry.object = document.object();
    return entry.object;
}

QJsonObject LearnedStateArtifactCache::report() const {
    QMutexLocker lock(&impl->mutex);
    return {{"entries", impl->entries.size()}, {"maximum_entries", maximumEntries},
        {"cache_hits", qint64(impl->hits)}, {"metadata_probes", qint64(impl->probes)},
        {"disk_reads", qint64(impl->reads)}, {"parse_attempts", qint64(impl->parses)},
        {"failures", qint64(impl->failures)}, {"probe_interval_ms", probeIntervalMs},
        {"full_recheck_interval_ms", fullRecheckIntervalMs}, {"cached_predictions", false}};
}

QJsonObject evaluateLearnedState(const QJsonObject &a, const GameState &s) {
    if (a.value("schema_version").toInt() != 1 || a.value("kind").toString() != "lixin_observable_state_logistic" || !a.value("eligible").toBool()) return abstain("model_not_eligible");
    const auto gates = a.value("deployment").toObject().value("checks").toObject();
    if (gates.isEmpty()) return abstain("missing_validation_gates");
    for (const auto &g : gates) if (!g.isBool() || !g.toBool()) return abstain("validation_gate_failed");
    if (s.source != GameState::Source::Live || !s.fresh()) return abstain("stale_or_nonlive_observation");
    if (s.gameMode != "KIWI" || s.queueId != 2400 || a.value("mode").toString() != "KIWI" || a.value("queue").toInt() != 2400 || s.observedPatch.isEmpty() || s.observedPatch != a.value("patch").toString()) return abstain("mode_queue_or_observed_patch_mismatch");
    double distance = 1e20;
    for (double t : {180.,360.,600.,900.}) distance = std::min(distance, std::abs(s.gameTime-t));
    if (!std::isfinite(s.gameTime) || distance > 45) return abstain("outside_validated_checkpoints");
    const QStringList expected{"champion_prior_logit_difference","kill_difference_div10","death_difference_div10","assist_difference_div20","mean_level_difference_div3"};
    const auto names = a.value("feature_names").toArray();
    if (names.size() != 5) return abstain("feature_contract_mismatch");
    for (int i=0;i<5;++i) if (names[i].toString() != expected[i]) return abstain("feature_contract_mismatch");
    const auto fit = a.value("fit").toObject();
    const auto coefficients = fit.value("coefficients").toArray(), scales = fit.value("rms_scales").toArray(), maxima = fit.value("observed_feature_abs_max").toArray();
    if (coefficients.size()!=5 || scales.size()!=5 || maxima.size()!=5 || !numeric(fit.value("temperature")) || fit.value("temperature").toDouble()<.25 || fit.value("temperature").toDouble()>10 || fit.value("intercept").toDouble(1)!=0 || fit.value("normalized_feature_clip").toDouble()!=6) return abstain("invalid_model_parameters");
    const auto priors = a.value("champion_prior").toObject();
    if (s.players.size()!=10) return abstain("incomplete_roster");
    int counts[2]{}, selfCount=0, perspective=-1; double totals[2][5]{};
    for (const auto &v : s.players) {
        const auto p = v.toObject(), scores = p.value("scores").toObject(); const auto team = p.value("team").toString();
        if (team!="ORDER" && team!="CHAOS") return abstain("unknown_team");
        const int side=team=="ORDER"?0:1; ++counts[side];
        if (p.value("self").toBool()) { ++selfCount; perspective=side; }
        if (!whole(p.value("champion_id"),1,10000000) || !whole(p.value("level"),1,18)) return abstain("required_public_field_missing");
        for (const auto &key : {"kills","deaths","assists"}) if (!whole(scores.value(key),0,100000)) return abstain("required_public_field_missing");
        const auto prior = priors.value(QString::number(p.value("champion_id").toInt()));
        double probability=.5;
        if (!prior.isUndefined()) { if (!numeric(prior) || prior.toDouble()<=0 || prior.toDouble()>=1) return abstain("invalid_champion_prior"); probability=prior.toDouble(); }
        totals[side][0]+=logit(probability); totals[side][1]+=scores.value("kills").toDouble(); totals[side][2]+=scores.value("deaths").toDouble(); totals[side][3]+=scores.value("assists").toDouble(); totals[side][4]+=p.value("level").toDouble();
    }
    if (counts[0]!=5 || counts[1]!=5 || selfCount!=1) return abstain("incomplete_roster_or_self");
    const std::array<double,5> features{clip((totals[0][0]-totals[1][0])/5,4),clip((totals[0][1]-totals[1][1])/10,10),clip((totals[0][2]-totals[1][2])/10,10),clip((totals[0][3]-totals[1][3])/20,10),clip((totals[0][4]-totals[1][4])/15,10)};
    double z=0;
    for (int i=0;i<5;++i) {
        if (!numeric(coefficients[i]) || std::abs(coefficients[i].toDouble())>1e6 || !numeric(scales[i]) || scales[i].toDouble()<=0 || !numeric(maxima[i]) || maxima[i].toDouble()<0) return abstain("invalid_model_parameters");
        if (std::abs(features[i])>maxima[i].toDouble()*1.25+1e-6) return abstain("outside_training_feature_support");
        z+=coefficients[i].toDouble()*clip(features[i]/scales[i].toDouble(),6);
    }
    double probability=1/(1+std::exp(-clip(z/fit.value("temperature").toDouble(),40)));
    if (perspective==1) probability=1-probability;
    return {{"available",true},{"probability",probability},{"patch",s.observedPatch},{"kind","validated_observed_state_association"},{"causal_choice_value",false},{"uses_augment_or_item_outcome_leakage",false}};
}

QJsonObject learnedStatePrediction(const QString &root,const GameState &state) {
    return evaluateLearnedState(artifactCache().read(root,QStringLiteral("data/history-state-model.v1.json")),state);
}

QJsonObject historicalPurchaseReference(const QString &root,const GameState &state) {
    if (!state.fresh() || state.gameMode!="KIWI" || state.observedPatch.isEmpty()) return abstain("waiting_for_observed_patch_and_game");
    const auto data=artifactCache().read(root,QStringLiteral("data/history-purchase-references.v1.json"));
    if (data.value("schema_version").toInt()!=1 || data.value("patch").toString()!=state.observedPatch || data.value("causal_recommendation").toBool(true) || data.value("complete_inventory_reconstructed").toBool(true)) return abstain("history_reference_unavailable");
    for (const auto &v:data.value("groups").toArray()) { auto group=v.toObject();
        if (group.value("champion_id").toInt()==state.championId && group.value("patch").toString()==state.observedPatch && group.value("unique_matches").toInt()>0) { group["available"]=true;group["causal_recommendation"]=false;return group; }
    }
    return abstain("no_same_patch_champion_routes");
}
