#pragma once
#include "catalog.h"

// A bounded, anonymous conditional episode. This does not control the game,
// infer hidden events, or turn simulated transitions into observations.
QJsonObject runLifecycleSandbox(const Catalog &, const QJsonObject &request);
