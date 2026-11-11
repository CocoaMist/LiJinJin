#pragma once
#include <QJsonObject>
#include <QString>
class Catalog;
struct GameState;

// An intent is a disclosed comparison goal, never evidence that ally utility is
// modeled. Only a complete matching current augment observation can add an
// offensive-augment route; AP alone is not proof of offensive support intent.
QJsonObject combatRoleIntent(const Catalog &, const GameState &, const QString &requestedStyle);

// Separate source-backed routes from the numerical winner. This never changes
// the computed plan, grants hidden utility, or turns provider ordering into a
// local score/win rate. A next-item reference requires verified current budget,
// inventory, level and recipe legality; otherwise only the full route remains.
QJsonObject roleAwareRecommendation(const Catalog &, const GameState &,
    const QString &requestedStyle, const QJsonObject &computedPlan = {});

// An explicitly separate public-build reference when the complete combat
// objective cannot rank purchases. Continues observed materials where possible;
// every target/component uses the shared transaction validator. No missing
// augment is supplied, no provider row becomes a computed winner or win rate.
QJsonObject sourcePurchaseReference(const Catalog &,const GameState &);
