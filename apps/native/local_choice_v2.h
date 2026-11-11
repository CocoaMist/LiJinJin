#pragma once

#include <QJsonObject>

// Round6's native-stat interventions. The caller applies these to one current
// baseline and measures the realized stat and component changes.
QJsonObject localChoiceV2ProbeOffsets();

// CPU inference over the supplied native response packet. The caller owns
// snapshot, candidate identity, and purchase/offer admission checks.
QJsonObject evaluateLocalChoiceV2(const QJsonObject &packet);
