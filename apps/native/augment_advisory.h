#pragma once
#include <QJsonObject>

class Catalog;
struct GameState;

// An actionable, explicitly conditional suggestion for the current three
// offers. Public-reference preference is separate from combat-model ranking:
// no win rate, gain percentage, complete selection or trigger count is inferred.
// The caller must additionally bind the visible card slots to this match/round.
// opening_reference_prior describes only a confirmed empty selected list. It
// prefers a source-bound champion_augment_slot_1 cohort when available, otherwise
// labels champion_augment_reference as slot_specific=false. Its route anchor is
// a candidate only: it never changes selected augments or locks later purchases.
QJsonObject buildAugmentAdvisory(const Catalog &,const GameState &);
// offeredReferences receives the same per-card source snapshot used by the advisory.
QJsonObject buildAugmentAdvisory(const Catalog &,const GameState &,QJsonObject *offeredReferences);
