#pragma once
#include "catalog.h"
#include <QStringList>

// Shared input guard for ordinary QWER growth and reviewed free-transform R.
// Unreviewed special schedules keep catalog bounds and disclose their gap.
bool sandboxSkillRanksLegal(const CombatDatabase &, int champion, int level,
    const QMap<QString, int> &ranks, QStringList *unknowns = nullptr);

// Conditional ten-player numeric combat, with one shared clock and HP pool.
// This is an abstract decision sandbox; it does not report a real win rate.
QJsonObject runTeamfightSandbox(const Catalog &, const QJsonObject &request);
