#pragma once
#include <QJsonObject>
#include <QString>

class Catalog;
struct GameState;

// Reviewed, source-bound conditions only. Does not grant numeric ranking,
// predict procs, infer missing selections, or issue a purchase instruction.
QString augmentContextAdviceVersion();
QJsonObject buildAugmentContextAdvice(const Catalog &, const GameState &, int offeredId);
