#pragma once
#include "analysis.h"
#include <cmath>

// Reports existing inputs only. This helper never writes health counters,
// invents a capture provider, or changes the comparison-window equations.
inline QJsonObject combatHealthContract(const Catalog &catalog, const GameState &s, bool actionScenario=false) {
    const auto k=[](const char *v){return QString::fromLatin1(v);};
    const auto finite=[](const QJsonValue &v){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=0;};
    const bool live=s.source==GameState::Source::Live,replay=s.source==GameState::Source::Replay;
    int selves=0;bool sameActor=false;
    for(const auto &v:s.players){const auto p=v.toObject();if(p.value(k("self")).toBool()){++selves;sameActor=p.value(k("champion_id")).toInt()==s.championId;}}
    const bool context=s.matchContext.value(k("active")).toBool()&&s.matchContext.value(k("stage"))==k("in_game")&&!s.matchContext.value(k("context_id")).toString().isEmpty()&&selves==1&&sameActor;
    const bool bound=live&&s.fresh()&&modelRulesUsable(catalog,s)&&s.gameMode==k("KIWI")&&context;
    const auto maximum=s.liveStats.value(k("maxHealth")),current=s.liveStats.value(k("currentHealth"));
    const bool maxValid=finite(maximum)&&maximum.toDouble()>0;
    const bool currentRange=finite(current)&&maxValid&&current.toDouble()<=maximum.toDouble();
    const bool replayCurrent=finite(current)&&(!s.liveStats.contains(k("maxHealth"))||currentRange);
    const bool currentValid=replay?replayCurrent:currentRange;
    const auto status=[&](const QString &key,bool valid){return !s.liveStats.contains(key)?k("missing"):!valid?k("invalid"):live&&!bound?k("unbound_or_stale"):live?k("observed_valid"):replay?k("caller_replay_reference"):k("unobserved_reference");};
    QStringList unknowns;QJsonArray required;
    if(live){
        required.append(k("maxHealth"));
        if(!bound)unknowns.append(k("required_observed_field:snapshot_identity_patch_or_freshness"));
        if(!maxValid)unknowns.append(k("required_observed_field:maxHealth:")+(s.liveStats.contains(k("maxHealth"))?k("invalid"):k("missing")));
        if(actionScenario){required.append(k("currentHealth"));if(!currentRange)unknowns.append(k("required_observed_field:currentHealth:missing_or_invalid"));}
    }
    // Replay inputs are explicitly caller supplied; a missing replay maximum
    // must not erase the pre-existing explicit current-health scenario API.
    const bool scenarioCurrent=actionScenario&&((bound&&currentRange)||(replay&&replayCurrent));
    QJsonObject explicitHealth;
    for(const auto &key:{k("self_current_hp"),k("self_max_hp"),k("self_missing_hp"),k("self_health_fraction"),k("self_missing_health_fraction")})
        if(s.effectCounters.contains(key))explicitHealth[key]=QJsonObject{{k("value"),s.effectCounters.value(key)},{k("source"),k("explicit_context_counter_not_panel_binding")}};
    QString summary;
    if(live&&!bound)summary=QStringLiteral("当前观测已过期或不属于本局，仅保留模型参考。");
    else if(live&&!maxValid)summary=QStringLiteral("最大生命未读取或无效，本次仅展示模型估算，暂不选出购买结论。");
    else if(actionScenario)summary=scenarioCurrent?QStringLiteral("动作场景使用已提供的剩余生命；场景结果仍不代表实战命中。"):QStringLiteral("剩余生命未读取，满血仅作为动作场景参考。");
    else summary=QStringLiteral("按比较窗口估算；实时剩余生命未参与这次出装比较。");
    return {{k("mode"),actionScenario?k("explicit_action_scenario"):k("modeled_3_8_20_second_comparison_windows")},
        {k("maximum_health_source"),maxValid?(bound?k("live_panel_calibration"):k("caller_or_unbound_panel_reference")):k("catalog_model_reference")},
        {k("maximum_health_status"),status(k("maxHealth"),maxValid)},
        {k("current_health_status"),status(k("currentHealth"),currentValid)},
        {k("current_health_used"),scenarioCurrent},
        {k("current_health_used_semantics"),k("panel_currentHealth_bound_into_numeric_context_not_presence_of_explicit_health_counters")},
        {k("explicit_health_assumptions"),explicitHealth},{k("required_observed_fields"),required},
        {k("required_observation_unknowns"),QJsonArray::fromStringList(unknowns)},
        {k("source_is_live"),live},{k("snapshot_bound"),bound},{k("context_id"),s.matchContext.value(k("context_id"))},
        {k("champion_id"),s.championId},{k("patch"),s.observedPatch},
        {k("source_received_at_ms"),s.received.isValid()?QJsonValue(double(s.received.toMSecsSinceEpoch())):QJsonValue(QJsonValue::Null)},
        {k("valid_until_ms"),live&&s.received.isValid()?QJsonValue(double(s.received.toMSecsSinceEpoch()+4000)):QJsonValue(QJsonValue::Null)},
        {k("current_health_known_zero"),currentValid&&current.toDouble()==0&&(bound||replay)},
        {k("missing_is_zero"),false},{k("live_certified"),false},{k("summary"),summary}};
}
inline QStringList combatHealthUnknowns(const QJsonObject &contract) {
    QStringList result;for(const auto &v:contract.value(QStringLiteral("required_observation_unknowns")).toArray())result.append(v.toString());return result;
}
inline QJsonObject combatHealthEligibilityBasis(const GameState &s) {
    const auto key=QStringLiteral("maxHealth");const auto v=s.liveStats.value(key);
    const bool valid=v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>0;
    return {{QStringLiteral("source"),int(s.source)},
        {QStringLiteral("required_maximum_health_status"),s.source!=GameState::Source::Live?QStringLiteral("model_reference"):
            !s.liveStats.contains(key)?QStringLiteral("missing"):valid?QStringLiteral("valid"):QStringLiteral("invalid")}};
}
