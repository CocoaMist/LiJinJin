#pragma once
#include "catalog.h"

// Replays one caller-supplied continuous scenario against explicit alternative
// inventories. This is a conditional local comparison, never a future hit
// predictor or an observed win rate. Event admission changes disable ranking.
QJsonObject compareChainEquipment(const Catalog &, const QJsonObject &request);
