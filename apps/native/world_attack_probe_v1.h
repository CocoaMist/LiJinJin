#pragma once
#include "catalog.h"

// Given-contact, noncritical Garen -> Lux base-attack impact. Only pre-contact
// panel facts enter the native prediction; measured outcomes remain external.
QJsonObject runWorldAttackProbeV1(const Catalog &, const QJsonObject &);

// Used by the continuous entry point as well as the adapter. A probe request
// cannot unlock the two-player roster or observed-panel path for other actions.
QString validateWorldAttackProbeContinuousV1(const QJsonObject &);
