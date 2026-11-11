#pragma once
#include "catalog.h"

// Explicit absolute-time actions share one resource/cooldown/control ledger.
// Automatic actions, when requested, are synthetic policy assumptions.
QJsonObject runContinuousSandbox(const Catalog &, const QJsonObject &request);
