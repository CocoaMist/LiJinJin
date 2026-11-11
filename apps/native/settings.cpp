#include "settings.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {
bool readSettingsObject(const QString &path, QJsonObject &object, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("现有本机设置无法读取，未覆盖原文件");
        return false;
    }
    if (file.size() > 65536) {
        if (error) *error = QStringLiteral("现有本机设置超过大小限制，未覆盖原文件");
        return false;
    }
    const auto previous = file.read(65537);
    if (file.error() != QFileDevice::NoError) {
        if (error) *error = QStringLiteral("现有本机设置读取失败，未覆盖原文件");
        return false;
    }
    if (previous.size() > 65536 || !file.atEnd()) {
        if (error) *error = QStringLiteral("现有本机设置超过大小限制，未覆盖原文件");
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(previous, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("现有本机设置格式无效，未覆盖原文件");
        return false;
    }
    object = document.object();
    return true;
}

LocalSettings settingsFromObject(const QJsonObject &object) {
    LocalSettings settings;
    settings.apiKey = object.value(QStringLiteral("deepseek_api_key")).toString();
    const auto model = object.value(QStringLiteral("deepseek_model")).toString().trimmed();
    if (!model.isEmpty()) settings.model = model;
    if (object.contains(QStringLiteral("active_model_slot"))) settings.activeModelSlot = object.value(QStringLiteral("active_model_slot")).toString();
    if (object.contains(QStringLiteral("local_model_base_url"))) settings.localBaseUrl = object.value(QStringLiteral("local_model_base_url")).toString().trimmed();
    settings.localModel = object.value(QStringLiteral("local_model_name")).toString().trimmed();
    settings.localApiKey = object.value(QStringLiteral("local_model_api_key")).toString();
    return settings;
}

bool writeSettingsObject(const QString &root, const QJsonObject &object, QString *error) {
    const auto bytes = QJsonDocument(object).toJson();
    if (bytes.size() > 65536) {
        if(error) *error = QStringLiteral("本机设置超过大小限制，未覆盖原文件");
        return false;
    }
    if (!QDir().mkpath(root + QStringLiteral("/.local"))) {
        if(error) *error = QStringLiteral("本机设置目录创建失败");
        return false;
    }
    QSaveFile file(root + QStringLiteral("/.local/settings.json"));
    if (!file.open(QIODevice::WriteOnly)) {
        if(error) *error = QStringLiteral("本机设置无法写入");
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if(error) *error = QStringLiteral("本机设置保存失败");
        return false;
    }
    return true;
}
}

LocalSettings loadLocalSettings(const QString &root, QString *error) {
    const auto path = root + QStringLiteral("/.local/settings.json");
    if (!QFile::exists(path)) return {};
    QJsonObject object;
    if (!readSettingsObject(path, object, error)) return {};
    auto settings = settingsFromObject(object);
    if (settings.activeModelSlot != QStringLiteral("deepseek") && settings.activeModelSlot != QStringLiteral("local_openai")) {
        if (error) *error = QStringLiteral("未识别的模型位，请重新选择后保存");
    } else if (settings.activeModelSlot == QStringLiteral("local_openai") && !settings.modelConnection().isConfigured()) {
        if (error) *error = QStringLiteral("本地模型配置不完整或地址无效");
    }
    return settings;
}

bool saveLocalSettings(const QString &root, const LocalSettings &settings, QString *error) {
    if (settings.activeModelSlot != QStringLiteral("deepseek") && settings.activeModelSlot != QStringLiteral("local_openai")) {
        if (error) *error = QStringLiteral("未识别的模型位");
        return false;
    }
    auto selected = settings.modelConnection();
    selected.model = selected.model.trimmed(); selected.apiKey = selected.apiKey.trimmed();
    if (settings.activeModelSlot == QStringLiteral("deepseek")) {
        if (selected.model.isEmpty()) selected.model = QStringLiteral("deepseek-flash");
        selected.apiKey = QStringLiteral("configuration-validation");
        if (!selected.isConfigured()) {
            if (error) *error = QStringLiteral("模型名称格式无效");
            return false;
        }
    }
    if (settings.activeModelSlot == QStringLiteral("local_openai") && !selected.isConfigured()) {
        if (error) *error = QStringLiteral("请填写有效的本地模型名及本机服务地址");
        return false;
    }
    if (root.isEmpty()) {
        if(error) *error = QStringLiteral("本机设置目录创建失败");
        return false;
    }
    const auto path = root + QStringLiteral("/.local/settings.json");
    QJsonObject object;
    if (QFile::exists(path) && !readSettingsObject(path, object, error)) return false;
    object.insert(QStringLiteral("deepseek_api_key"), settings.apiKey.trimmed());
    object.insert(QStringLiteral("deepseek_model"), settings.model.trimmed().isEmpty()
        ? QStringLiteral("deepseek-flash") : settings.model.trimmed());
    object.insert(QStringLiteral("active_model_slot"), settings.activeModelSlot);
    object.insert(QStringLiteral("local_model_base_url"), settings.localBaseUrl.trimmed());
    object.insert(QStringLiteral("local_model_name"), settings.localModel.trimmed());
    object.insert(QStringLiteral("local_model_api_key"), settings.localApiKey.trimmed());
    return writeSettingsObject(root, object, error);
}

bool activateLocalModelSlot(const QString &rootPath, const QString &slotId, QString *error) {
    if (slotId != QStringLiteral("deepseek") && slotId != QStringLiteral("local_openai")) {
        if (error) *error = QStringLiteral("未识别的模型位");
        return false;
    }
    if (rootPath.isEmpty()) {
        if (error) *error = QStringLiteral("本机设置目录无效");
        return false;
    }
    QJsonObject object;
    if (!readSettingsObject(rootPath + QStringLiteral("/.local/settings.json"), object, error)) return false;
    auto saved = settingsFromObject(object);
    const auto previousSlot = saved.activeModelSlot;
    saved.activeModelSlot = slotId;
    const auto connection = saved.modelConnection();
    if (!connection.isConfigured()) {
        if (error) {
            if (!connection.hasValidEndpoint()) *error = QStringLiteral("此模型位的本机服务地址无效，请填写后保存启用");
            else if (connection.model.isEmpty() || (connection.isDeepSeek() && connection.apiKey.trimmed().isEmpty()))
                *error = QStringLiteral("此模型位尚未配置，请填写后保存启用");
            else *error = QStringLiteral("此模型位的模型名称格式无效，请填写后保存启用");
        }
        return false;
    }
    if (previousSlot == slotId) return true;
    object.insert(QStringLiteral("active_model_slot"), slotId);
    return writeSettingsObject(rootPath, object, error);
}
