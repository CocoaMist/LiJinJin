#pragma once
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace roster_history_basis_detail {
inline QJsonObject fields(const QJsonObject &source,const QStringList &keys){
    QJsonObject out;for(const auto &key:keys)if(source.contains(key))out[key]=source.value(key);return out;
}
inline const QStringList &metrics(){
    static const QStringList keys{"kills","deaths","assists","gold_earned","damage_to_champions","damage_taken","kda","gold_per_minute","damage_per_minute","taken_per_minute","deaths_per_minute","teammate_heal_per_minute","shield_per_minute","physical_damage_per_minute","magic_damage_per_minute","true_damage_per_minute"};return keys;
}
inline QJsonObject summary(const QJsonObject &source){
    auto out=fields(source,{"available","reason","games","sampled_games","sample_limited"});
    for(const auto &key:{"metrics","field_samples"})if(source.value(key).isObject())out[key]=fields(source.value(key).toObject(),metrics());
    if(source.value("missing_fields").isArray()){QJsonArray missing;for(const auto &value:source.value("missing_fields").toArray())if(metrics().contains(value.toString()))missing.append(value);out["missing_fields"]=missing;}
    return out;
}
inline QJsonObject profile(const QJsonObject &source){
    auto out=fields(source,{"available","reason","eligible_games","sampled_games","known_outcomes","wins","champion_id","all_patch_sampled_games","same_patch_sampled_games","sample_limited","patch_known","patch","historical_win_rate","evidence_stage","choice_state_verified","choice_quality_known","fixed_items_replaceable","reference_limit_applied"});
    for(const auto &key:{"champions","fixed_items","items","augments"})if(source.value(key).isArray()){
        QJsonArray rows;for(const auto &value:source.value(key).toArray())rows.append(fields(value.toObject(),{"id","games"}));out[key]=rows;
    }
    if(source.value("build_combinations").isArray()){QJsonArray rows;for(const auto &value:source.value("build_combinations").toArray())rows.append(fields(value.toObject(),{"item_ids","augment_ids","games"}));out["build_combinations"]=rows;}
    return out;
}
inline QJsonObject multidimensional(const QJsonObject &source){
    auto out=fields(source,{"status"});
    for(const auto &key:{"overall","champion_experience"})if(source.value(key).isObject())out[key]=fields(source.value(key).toObject(),{"games","known_outcomes","historical_win_rate","timeline_games","recorded_augment_events","mechanical_quality_known"});
    for(const auto &key:{"same_patch","same_patch_champion"})if(source.value(key).isObject())out[key]=fields(source.value(key).toObject(),{"games"});
    if(source.value("ability_inference").isObject())out["ability_inference"]=fields(source.value("ability_inference").toObject(),{"mechanics","team_awareness"});
    if(source.value("conditional_performance_reference").isObject()){
        const auto raw=source.value("conditional_performance_reference").toObject();auto reference=fields(raw,{"scope","causal_effect_known"});QJsonObject components;
        for(const auto &key:{"gold_earned_per_minute","damage_to_champions_per_minute","deaths_per_minute","teammate_heal_per_minute","shield_per_minute"})if(raw.value("components").toObject().value(key).isObject())components[key]=fields(raw.value("components").toObject().value(key).toObject(),{"status","own_participant_rows","other_participant_rows","other_distinct_players","direction"});
        reference["components"]=components;out["conditional_performance_reference"]=reference;
    }
    return out;
}
}

// Anonymous evidence and its availability invalidate the current analysis.
// Collection clocks, source revisions and unknown extensions do not.
inline QByteArray rosterHistoryEvidenceBasis(const QJsonObject &source){
    using namespace roster_history_basis_detail;
    auto out=fields(source,{"status","patch","mode","queue_id"});QJsonArray players;
    if(source.value("multidimensional_context").isObject())out["multidimensional_context"]=fields(source.value("multidimensional_context").toObject(),{"status","freshness_status","freshness_verified","requires_materialization"});
    for(const auto &value:source.value("players").toArray()){
        if(players.size()>=10)break;const auto raw=value.toObject();auto player=fields(raw,{"cell","team","self","champion_id","status","identity_source","resolved_server"});
        for(const auto &key:{"history","same_patch","same_champion","same_patch_champion"})if(raw.value(key).isObject())player[key]=summary(raw.value(key).toObject());
        for(const auto &key:{"profile_summary","champion_references"})if(raw.value(key).isObject())player[key]=profile(raw.value(key).toObject());
        if(raw.value("coverage").isObject())player["coverage"]=fields(raw.value("coverage").toObject(),{"eligible_games","sampled_games","timeline_games","timeline_sampled_games","process_item_games","process_augment_games","growth_observation_games","permanent_growth_observation_games","real_offer_observations","confirmed_choice_observations","sample_limited","timeline_sample_limited","observations_sample_limited","all_time_history_complete","historical_alternatives_complete"});
        if(raw.value("learning_status").isObject())player["learning_status"]=fields(raw.value("learning_status").toObject(),{"status","real_offer_observations","confirmed_choice_observations","matched_result_observations","recommendation_available","causal_effect_known","terminal_builds_are_choice_samples"});
        if(raw.value("multidimensional_profile").isObject())player["multidimensional_profile"]=multidimensional(raw.value("multidimensional_profile").toObject());
        if(raw.value("player_analysis").isObject())player["player_analysis"]=fields(raw.value("player_analysis").toObject(),{"status","reason","available","basis","source_snapshot_verified","sampled_games","recent","process_coverage","evidence_dimensions"});
        if(raw.value("freshness").isObject())player["freshness"]=fields(raw.value("freshness").toObject(),{"status","refresh_status","source_halted","accessible_history_exhausted"});
        players.append(player);
    }
    out["players"]=players;
    return QCryptographicHash::hash(QJsonDocument(out).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256);
}
