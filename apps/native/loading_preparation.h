#pragma once
#include "analysis.h"

// Loading-screen preparation is an explicit hypothetical empty-loadout branch.
// Its cached result must never substitute for a fresh in-game recommendation.
QString loadingPreparationBasis(const Catalog &,const GameState &,const QString &style);
QJsonObject runLoadingPreparation(const Catalog &,const GameState &,const QString &style);
