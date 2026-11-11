#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <optional>
struct CombatContext;
// Absent legacy metadata is disclosed; malformed/new metadata is rejected.
std::optional<QJsonObject> validatedCombatModeRules(const QJsonValue &, const QString &patch);
void applyCombatModeRules(CombatContext &, const QJsonObject &validated);
