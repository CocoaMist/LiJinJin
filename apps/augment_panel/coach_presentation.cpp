#include "coach_presentation.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString q(const char *value) { return QString::fromLatin1(value); }
QString normalizedStage(const QString &value) {
    if (value == q("champ_select") || value == q("ChampSelect")) return q("champ_select");
    if (QStringList{q("loading"), q("GameStart"), q("LoadingScreen"), q("Loading"), q("PreStart"), q("Start")}.contains(value)) return q("loading");
    if (value == q("in_game") || value == q("InProgress") || value == q("Reconnect")) return q("in_game");
    return {};
}
int itemId(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0
        && value.toDouble() <= std::numeric_limits<int>::max() && std::floor(value.toDouble()) == value.toDouble() ? int(value.toDouble()) : 0;
}
}

void CoachPresentation::reset() {
    if (!context.isEmpty()) ++resets;
    context.clear(); stage.clear(); seenCueOrder.clear(); seenCues.clear();
    lastItem = 0; lastDead = -1; lastAffordable = false;
    lastExpanded = lastPurchaseVisible = lastRosterVisible = false;
    lastShownPage = lastLayout = q("waiting");
}

void CoachPresentation::resumeAutomatic(qint64 monotonicMs) {
    // Older callers may still offer this action. It cannot change the layout,
    // erase cue identities or replay a purchase event in the passive HUD.
    (void)monotonicMs;
}

QJsonObject CoachPresentation::update(const QJsonObject &input, qint64 monotonicMs) {
    ++updates;
    const auto now = std::clamp(monotonicMs, qint64(0), std::numeric_limits<qint64>::max() - 60000);
    const bool clockReset = lastMs >= 0 && now < lastMs;
    lastMs = now;
    if (clockReset) reset();
    const auto incomingStage = normalizedStage(input.value(q("stage")).toString());
    const auto incomingContext = input.value(q("context_id")).toString();
    const bool allowed = input.value(q("overlays_allowed")).toBool();
    const bool fresh = input.value(q("fresh")).toBool();
    const bool active = fresh && allowed && !incomingStage.isEmpty() && !incomingContext.isEmpty() && incomingContext.size() <= 96;
    const auto result = [&](const QString &shownPage, const QString &layout, bool purchaseVisible,
                            bool rosterVisible, const QString &reason, const QJsonArray &events = QJsonArray{}) {
        lastReason = reason; lastLayout = layout; lastExpanded = layout == q("expanded") || layout == q("pregame");
        lastShownPage = shownPage; lastPurchaseVisible = purchaseVisible; lastRosterVisible = rosterVisible;
        return QJsonObject{{q("page"),shownPage},{q("compact_page"),shownPage},{q("layout"),layout},
            {q("expanded"),lastExpanded},{q("purchase_visible"),purchaseVisible},{q("roster_visible"),rosterVisible},
            {q("requires_interaction"),false},{q("reason"),reason},{q("events"),events},
            {q("next_switch_in_ms"),0},{q("rotation_period_ms"),0},{q("automatic"),true},
            {q("manual_override"),false},{q("manual_pinned"),false},{q("manual_hold_remaining_ms"),0},
            {q("visible"),active},
            {q("current_context"),context.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(context)},
            {q("context_id"),context.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(context)}};
    };
    if (!active) {
        reset();
        return result(q("waiting"),q("waiting"),false,false,
            !fresh ? q("stale_waiting") : !allowed ? q("overlays_disabled") : q("outside_match"));
    }
    if (incomingContext != context) { reset(); context = incomingContext; }
    stage = incomingStage;
    if (stage != q("in_game")) {
        // The roster block also owns the honest "not public yet" placeholder.
        // A previous live purchase or death state cannot leak into pregame.
        lastItem = 0; lastDead = -1; lastAffordable = false;
        return result(q("roster"),q("pregame"),false,true,
            stage == q("champ_select") ? q("champ_select_roster") : q("loading_roster"));
    }

    const int nextItem = itemId(input.value(q("next_item_id")));
    const bool purchaseVisible = nextItem > 0;
    const bool rosterVisible = input.value(q("roster_available")).toBool();
    const bool deadKnown = input.value(q("self_dead")).isBool();
    const bool dead = deadKnown && input.value(q("self_dead")).toBool();
    const bool deathEdge = dead && lastDead != 1;
    const bool respawnEdge = deadKnown && !dead && lastDead == 1;
    if (deadKnown) lastDead = dead ? 1 : 0;

    const bool affordableKnown = input.value(q("gold_affordable")).isBool();
    const bool affordable = affordableKnown && input.value(q("gold_affordable")).toBool() && purchaseVisible;
    const bool itemChanged = purchaseVisible && nextItem != lastItem;
    const bool affordableEdge = affordable && (!lastAffordable || itemChanged);
    if (affordableKnown) lastAffordable = affordable;
    else if (itemChanged) lastAffordable = false;
    lastItem = nextItem;

    const auto cueValue = input.value(q("equipment_cue"));
    const auto cue = cueValue.toObject();
    const auto kind = cueValue.isString() ? cueValue.toString() : cue.value(q("kind")).toString();
    const auto key = cue.value(q("key")).toString(input.value(q("equipment_cue_key")).toString());
    const bool relevantCue = QStringList{q("enemy_major_item"),q("enemy_major_equipment"),q("enemy_major")}.contains(kind);
    bool newCue = false;
    if (relevantCue && !key.isEmpty() && key.size() <= 256 && !seenCues.contains(key)) {
        newCue = true; seenCues.insert(key); seenCueOrder.append(key);
        if (seenCueOrder.size() > 128) seenCues.remove(seenCueOrder.takeFirst());
    }
    QJsonArray events;
    if (deathEdge) events.append(q("entered_death"));
    if (respawnEdge) events.append(q("respawned"));
    if (affordableEdge) events.append(q("purchase_now_affordable"));
    if (newCue) events.append(q("enemy_major_equipment"));
    urgencyEvents += events.size();

    const auto shownPage = purchaseVisible ? q("purchase") : rosterVisible ? q("roster") : q("waiting");
    if (shownPage == q("waiting")) return result(shownPage,q("waiting"),false,false,q("waiting_available_content"),events);
    const auto layout = dead ? q("expanded") : q("compact");
    const auto reason = !events.isEmpty() ? events.first().toString() : clockReset ? q("monotonic_reset")
        : dead ? q("dead_passive") : q("alive_passive");
    // Page is a compatibility summary, not an exclusive view: both blocks stay
    // available in the same layout, including during equipment/purchase events.
    return result(shownPage,layout,purchaseVisible,rosterVisible,reason,events);
}

QJsonObject CoachPresentation::report() const {
    return {{q("schema_version"),2},{q("has_current_context"),!context.isEmpty()},
        {q("stage"),stage},{q("page"),lastShownPage},{q("layout"),lastLayout},{q("expanded"),lastExpanded},
        {q("purchase_visible"),lastPurchaseVisible},{q("roster_visible"),lastRosterVisible},
        {q("reason"),lastReason},{q("requires_interaction"),false},{q("updates"),updates},
        {q("rotations"),0},{q("context_resets"),resets},{q("urgency_events"),urgencyEvents},
        {q("seen_equipment_cues"),seenCues.size()},{q("rotation_period_ms"),0},{q("manual_hold_ms"),0},
        {q("manual_pinned"),false},{q("manual_pin_scope"),q("none")},
        {q("uses_monotonic_time"),true},{q("identity_fields_retained"),false}};
}
