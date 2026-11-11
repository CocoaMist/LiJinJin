#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// Plans recipients for effects already admitted by ProgressionLedger. This is
// pure preflight: the caller commits the staged progression ledger only after
// accepted=true, then applies deliveries through its own HP/defense ledger.
// players: unique {player_key,team,alive,current_health,maximum_health} rows.
// Delayed recipient effects require verified_source_death_policy=true and a
// source_death_policy of "persist" or "cancel" in the effect or observation
// facts. The caller must recheck recipient life and defenses at delivery time.
// No AoE recipient expansion, future hit, cast, cooldown refund or summon is
// manufactured. Rejection always returns empty deliveries/state_only_ops.
QJsonObject planMechanicDeliveryV4(const QJsonArray &newEffects,
                                  const QJsonObject &observation,
                                  const QString &actorKey,
                                  const QJsonArray &players,
                                  double at);
