#pragma once
#include <QString>
#include <QRegularExpression>
#include <QUrl>

struct ModelConnection {
    QString slotId = QStringLiteral("deepseek");
    QString apiKey;
    QString model = QStringLiteral("deepseek-flash");
    QString baseUrl = QStringLiteral("https://api.deepseek.com");

    bool isDeepSeek() const {
        const QUrl url(baseUrl.trimmed(), QUrl::StrictMode);
        return slotId == QStringLiteral("deepseek") && url.isValid()
            && url.scheme() == QStringLiteral("https") && url.host() == QStringLiteral("api.deepseek.com")
            && url.port(-1) == -1 && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment()
            && (url.path().isEmpty() || url.path() == QStringLiteral("/"));
    }
    bool hasValidEndpoint() const {
        if (isDeepSeek()) return true;
        if (slotId != QStringLiteral("local_openai")) return false;
        const QUrl url(baseUrl.trimmed(), QUrl::StrictMode);
        const auto host = url.host().toLower();
        return url.isValid() && !url.isRelative()
            && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))
            && (host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1"))
            && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment()
            && url.port(-1) != 0;
    }
    bool isConfigured() const {
        static const QRegularExpression syntax(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._:/-]{0,95}\\z"));
        return hasValidEndpoint() && syntax.match(model).hasMatch()
            && (slotId == QStringLiteral("local_openai") || !apiKey.trimmed().isEmpty());
    }
    QString completionUrl() const {
        if (!hasValidEndpoint()) return {};
        QUrl url(baseUrl.trimmed(), QUrl::StrictMode);
        auto path = url.path();
        while (path.endsWith(QChar(u'/'))) path.chop(1);
        if (!path.endsWith(QStringLiteral("/chat/completions"))) path += QStringLiteral("/chat/completions");
        url.setPath(path);
        return url.toString(QUrl::FullyEncoded);
    }
    bool operator==(const ModelConnection &other) const {
        return slotId == other.slotId && apiKey == other.apiKey && model == other.model && baseUrl == other.baseUrl;
    }
    bool operator!=(const ModelConnection &other) const { return !(*this == other); }
};

struct LocalSettings {
    QString apiKey;
    QString model = QStringLiteral("deepseek-flash");
    QString activeModelSlot = QStringLiteral("deepseek");
    QString localBaseUrl = QStringLiteral("http://127.0.0.1:1234/v1");
    QString localModel;
    QString localApiKey;
    ModelConnection modelConnection() const {
        if (activeModelSlot == QStringLiteral("deepseek")) return {activeModelSlot, apiKey, model, QStringLiteral("https://api.deepseek.com")};
        if (activeModelSlot == QStringLiteral("local_openai")) return {activeModelSlot, localApiKey, localModel, localBaseUrl};
        return {activeModelSlot, {}, {}, {}};
    }
};

LocalSettings loadLocalSettings(const QString &root, QString *error = nullptr);
bool saveLocalSettings(const QString &root, const LocalSettings &, QString *error = nullptr);
bool activateLocalModelSlot(const QString &rootPath, const QString &slotId, QString *error = nullptr);
