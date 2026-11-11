#include "recommendation_history.h"
#include <QJsonArray>
#include <QSet>
#include <QStringList>
#include <cmath>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
bool integer(const QJsonValue &v, double lo, double hi) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= lo
        && v.toDouble() <= hi && std::floor(v.toDouble()) == v.toDouble();
}
const QStringList fields{k("kills"), k("deaths"), k("assists"), k("gold_earned"),
    k("damage_to_champions"), k("damage_taken"), k("kda"), k("gold_per_minute"),
    k("damage_per_minute"), k("taken_per_minute"), k("deaths_per_minute"),
    k("teammate_heal_per_minute"), k("shield_per_minute"), k("physical_damage_per_minute"),
    k("magic_damage_per_minute"), k("true_damage_per_minute")};
QJsonObject summary(const QJsonObject &raw) {
    QJsonObject out{{k("available"), false}};
    if (!raw.value(k("available")).toBool() || !integer(raw.value(k("sampled_games")), 0, 10000000)
        || !integer(raw.value(k("games")), 0, 1000000000)
        || raw.value(k("sampled_games")).toDouble() > raw.value(k("games")).toDouble()) return out;
    const int sampled = raw.value(k("sampled_games")).toInt();
    QJsonObject metrics, samples;
    for (const auto &name : fields) {
        const auto value = raw.value(k("metrics")).toObject().value(name);
        const auto count = raw.value(k("field_samples")).toObject().value(name);
        if (!integer(count, 0, sampled)) continue;
        samples[name] = count;
        metrics[name] = count.toInt() > 0 && value.isDouble() && std::isfinite(value.toDouble())
            && value.toDouble() >= 0 ? value : QJsonValue(QJsonValue::Null);
    }
    out[k("available")] = sampled > 0;
    out[k("games")] = raw.value(k("games")); out[k("sampled_games")] = sampled;
    out[k("metrics")] = metrics; out[k("field_samples")] = samples;
    out[k("sample_limited")] = raw.value(k("sample_limited")).toBool();
    out[k("historical_reference_only")] = true; out[k("causal_effect_known")] = false;
    return out;
}
QJsonObject counts(const QJsonObject &raw,const QStringList &names){
    QJsonObject out;for(const auto &name:names)if(integer(raw.value(name),0,1e12))out[name]=raw.value(name);return out;
}
void copyEpochMilliseconds(QJsonObject &out,const QJsonObject &raw,const QString &name){
    // Millisecond epochs passed 10^12 in 2001. They are not observation counts;
    // keep their own bounded integer range, then enforce the query cutoff at
    // consumption. Reusing counts() silently removed real 2026 end times.
    if(integer(raw.value(name),1,4102444800000.-1))out[name]=raw.value(name);
}
QJsonObject profileReferences(const QJsonObject &raw){
    auto out=counts(raw,{k("eligible_games"),k("sampled_games"),k("known_outcomes"),k("champion_id"),k("all_patch_sampled_games"),k("same_patch_sampled_games")});
    out[k("available")]=raw.value(k("available")).toBool();
    for(const auto &name:{k("champions"),k("items"),k("fixed_items"),k("augments")}){
        QJsonArray rows;for(const auto &v:raw.value(name).toArray()){
            if(rows.size()==6)break;const auto row=counts(v.toObject(),{k("id"),k("games")});
            if(row.value(k("id")).toDouble()>0&&row.value(k("games")).toDouble()>0)rows.append(row);
        }if(!rows.isEmpty())out[name]=rows;
    }
    QJsonArray combinations;for(const auto &v:raw.value(k("build_combinations")).toArray()){
        if(combinations.size()==3)break;const auto row=v.toObject();auto clean=counts(row,{k("games")});
        for(const auto &name:{k("item_ids"),k("augment_ids")}){QJsonArray ids;for(const auto &id:row.value(name).toArray()){if(ids.size()==6)break;if(integer(id,1,1e7))ids.append(id);}clean[name]=ids;}
        if(clean.value(k("games")).toDouble()>0)combinations.append(clean);
    }if(!combinations.isEmpty())out[k("build_combinations")]=combinations;
    out[k("evidence_stage")]=k("terminal_cooccurrence");out[k("choice_quality_known")]=false;return out;
}
QJsonObject projectionProfile(const QJsonObject &raw){
    QJsonObject out{{k("status"),k("known")}};
    for(const auto &name:{k("overall"),k("champion_experience"),k("same_patch"),k("same_patch_champion")}){
        const auto value=raw.value(name).toObject();auto clean=counts(value,{k("games"),k("known_outcomes"),k("timeline_games"),k("recorded_augment_events")});
        copyEpochMilliseconds(clean,value,k("latest_ended_at_ms"));
        const auto rate=value.value(k("historical_win_rate"));if(rate.isDouble()&&std::isfinite(rate.toDouble())&&rate.toDouble()>=0&&rate.toDouble()<=1)clean[k("historical_win_rate")]=rate;
        if(!clean.isEmpty())out[name]=clean;
    }
    const auto reference=raw.value(k("conditional_performance_reference")).toObject();
    if(reference.value(k("scope"))==k("same_champion_same_patch_leave_one_player_out")){
        QJsonObject components;for(const auto &name:{k("gold_earned_per_minute"),k("damage_to_champions_per_minute"),k("deaths_per_minute"),k("teammate_heal_per_minute"),k("shield_per_minute")}){
            const auto value=reference.value(k("components")).toObject().value(name).toObject();if(value.isEmpty())continue;
            auto clean=counts(value,{k("own_participant_rows"),k("other_participant_rows"),k("other_distinct_players")});
            const bool supported=value.value(k("status"))==k("supported_comparison")&&clean.value(k("own_participant_rows")).toDouble()>=5&&clean.value(k("other_participant_rows")).toDouble()>=20&&clean.value(k("other_distinct_players")).toDouble()>=3;
            clean[k("status")]=supported?k("supported_comparison"):k("insufficient_samples");const auto direction=value.value(k("direction")).toString();
            if(supported&&QStringList{k("above_reference"),k("below_reference"),k("similar_reference")}.contains(direction))clean[k("direction")]=direction;components[name]=clean;
        }
        out[k("conditional_performance_reference")]=QJsonObject{{k("scope"),reference.value(k("scope"))},{k("components"),components},{k("causal_effect_known"),false}};
    }
    const auto extension=sanitizeHistoricalProjectionExtension(raw);for(auto it=extension.constBegin();it!=extension.constEnd();++it)out[it.key()]=it.value();
    out[k("ability_inference")]=QJsonObject{{k("mechanics"),k("unknown")},{k("team_awareness"),k("unknown")}};return out;
}
QJsonObject analysisFreshness(const QJsonObject &raw){
    const auto status=raw.value(k("status")).toString(),refresh=raw.value(k("refresh_status")).toString();
    QJsonObject out{{k("status"),QStringList{k("fresh"),k("stale"),k("missing"),k("unknown")}.contains(status)?status:k("unknown")}};
    if(QStringList{k("not_requested"),k("not_configured"),k("identity_unresolved"),k("fresh"),k("queued"),k("active"),k("succeeded"),k("failed"),k("expired"),k("pending"),k("done"),k("queue_unavailable")}.contains(refresh))out[k("refresh_status")]=refresh;
    for(const auto &name:{k("source_halted"),k("accessible_history_exhausted")})if(raw.value(name).isBool())out[name]=raw.value(name);return out;
}
QString side(const QJsonValue &value, const QString &own) {
    const auto team = value.toString();
    if (team == k("ORDER") || team == k("CHAOS")) return team;
    if (team == k("ALLY")) return own;
    if (team == k("ENEMY")) return own == k("ORDER") ? k("CHAOS") : k("ORDER");
    return {};
}
}

QJsonObject sanitizeHistoricalProjectionExtension(const QJsonObject &raw){
    QJsonObject out;
    if(integer(raw.value(k("projection_version")),1,2))out[k("projection_version")]=raw.value(k("projection_version"));
    for(const auto &name:{k("details_available"),k("requires_projection_upgrade")})if(raw.value(name).isBool())out[name]=raw.value(name);
    const auto recent=raw.value(k("recent")).toObject();
    if(!recent.isEmpty()){
        auto clean=counts(recent,{k("limit"),k("games"),k("known_outcomes")});copyEpochMilliseconds(clean,recent,k("latest_ended_at_ms"));QJsonObject metrics,samples;
        for(const auto &name:fields+QStringList{k("damage_mitigated")}){
            const auto value=recent.value(k("metrics")).toObject().value(name),n=recent.value(k("field_samples")).toObject().value(name);
            if(!integer(n,0,20))continue;samples[name]=n;
            metrics[name]=n.toDouble()>0&&value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0?value:QJsonValue(QJsonValue::Null);
        }
        clean[k("metrics")]=metrics;clean[k("field_samples")]=samples;out[k("recent")]=clean;
    }
    const auto process=counts(raw.value(k("process_coverage")).toObject(),{k("frame_observations"),k("item_events"),k("real_candidate_event_records"),k("selected_augment_event_records"),k("permanent_growth_observed_games"),k("maximum_observed_level"),k("position_samples"),k("inferred_unobserved_states"),k("process_item_games"),k("process_augment_games"),k("growth_observation_games"),k("timeline_games"),k("timeline_sampled_games")});
    if(!process.isEmpty())out[k("process_coverage")]=process;
    QJsonObject dimensions;for(const auto &name:{k("damage"),k("tanking"),k("support"),k("control"),k("engage"),k("builds"),k("augments"),k("growth")}){
        const auto value=raw.value(k("evidence_dimensions")).toObject().value(name).toObject();if(value.isEmpty())continue;
        const auto status=value.value(k("status")).toString();const auto n=value.value(k("samples"));
        const bool observed=status==k("observed")&&integer(n,1,1e12);
        dimensions[name]=QJsonObject{{k("status"),observed?k("observed"):status==k("missing")?k("missing"):k("unobserved")},
            {k("samples"),observed?n:QJsonValue(QJsonValue::Null)},{k("basis"),k("player_analysis_observations")}};
    }
    if(!dimensions.isEmpty())out[k("evidence_dimensions")]=dimensions;return out;
}

QJsonObject buildRecommendationHistory(const Catalog &catalog, const GameState &state,
                                      const QJsonObject &history) {
    const bool loadingMapping = state.players.size() < 10 && state.matchContext.value(k("active")).toBool()
        && state.matchContext.value(k("stage")) == k("loading");
    QJsonObject pressure{{k("available"), false}, {k("reason"), k("history_unavailable")},
        {k("known_enemy_count"), 0}, {k("minimum_field_samples"), 5}, {k("weights"), QJsonArray{}},
        {k("scope"), k("historical_output_share_sensitivity_scenario")},
        {k("current_dps_known"), false}, {k("skill_multiplier"), false}, {k("causal_effect_known"), false},
        {k("current_damage_share_observed"), false}, {k("actual_win_rate"), QJsonValue(QJsonValue::Null)}};
    QJsonObject out{{k("schema_version"), 1}, {k("available"), false}, {k("status"), k("unavailable")},
        {k("historical_reference_only"), true}, {k("causal_effect_known"), false},
        {k("identity_exported"), false}, {k("mode"), k("KIWI")}, {k("queue_id"), 2400},
        {k("mapping_source"), loadingMapping ? k("loading_visible_roster") : k("current_observed_roster")},
        {k("live_patch_observed"), !state.observedPatch.isEmpty()}, {k("catalog_patch_assumption"), false},
        {k("usable_as_live"), !loadingMapping && state.source == GameState::Source::Live
            && state.fresh() && !catalog.patch.isEmpty() && state.observedPatch == catalog.patch},
        {k("players"), QJsonArray{}}, {k("historical_pressure_scenario"), pressure}};
    const auto fail = [&](const char *reason) {
        auto result = out; result[k("reason")] = k(reason); result[k("usable_as_live")] = false;
        auto p = pressure; p[k("reason")] = k(reason); result[k("historical_pressure_scenario")] = p;
        return result;
    };
    const auto context = state.matchContext.value(k("context_id")).toString();
    if (context.isEmpty() || history.value(k("context_id")).toString() != context) return fail("context_mismatch");
    if (state.gameMode != k("KIWI") || history.value(k("mode")) != k("KIWI")
        || history.value(k("queue_id")).toInt() != 2400) return fail("mode_mismatch");
    if (loadingMapping) {
        // A loading roster provides identity only. Never manufacture level,
        // inventory, skill ranks or a live observation in the caller's state.
        const auto visible = state.matchContext.value(k("roster")).toArray();
        if (visible.size() != 10) return fail("loading_visible_roster_incomplete");
        if (catalog.patch.isEmpty() || history.value(k("patch")).toString() != catalog.patch
            || (!state.observedPatch.isEmpty() && state.observedPatch != catalog.patch)) return fail("patch_mismatch");
        GameState mapping = state; mapping.players = {}; int selves = 0, selfChampion = 0;
        for (const auto &v : visible) {
            const auto p = v.toObject(); const auto team = p.value(k("team")).toString();
            const auto champion = p.value(k("champion_id")); const auto isSelf = p.value(k("self"));
            if ((team != k("ALLY") && team != k("ENEMY")) || !integer(champion, 1, 1000000)
                || !catalog.champions.contains(champion.toInt()) || !isSelf.isBool()) return fail("loading_roster_identity_invalid");
            if (isSelf.toBool()) { if (team != k("ALLY")) return fail("loading_self_side_invalid"); ++selves; selfChampion = champion.toInt(); }
            QJsonObject identity{{k("champion_id"), champion}, {k("team"), team == k("ALLY") ? k("ORDER") : k("CHAOS")}, {k("self"), isSelf}};
            if (integer(p.value(k("cell")), 0, 1000)) identity[k("cell")] = p.value(k("cell"));
            mapping.players.append(identity);
        }
        if (selves != 1) return fail("self_mapping_unverified");
        mapping.championId = selfChampion;
        const bool assumedPatch = state.observedPatch.isEmpty();
        if (assumedPatch) mapping.observedPatch = catalog.patch;
        auto result = buildRecommendationHistory(catalog, mapping, history);
        result[k("mapping_source")] = k("loading_visible_roster");
        result[k("live_patch_observed")] = !assumedPatch; result[k("catalog_patch_assumption")] = assumedPatch;
        result[k("usable_as_live")] = false;
        return result;
    }
    if (catalog.patch.isEmpty() || state.observedPatch != catalog.patch
        || history.value(k("patch")).toString() != catalog.patch) return fail("patch_mismatch");
    const auto at = history.value(k("as_of_ms"));
    if (!integer(at, 1, double(QDateTime::currentMSecsSinceEpoch()))) return fail("invalid_or_future_history_cutoff");
    if (history.value(k("status")) != k("complete")) return fail("history_query_incomplete");
    if (state.players.size() != 10 || history.value(k("players")).toArray().size() > 10) return fail("roster_size_invalid");
    QJsonObject self; int selves = 0;
    for (const auto &v : state.players) if (v.toObject().value(k("self")).toBool()) { ++selves; self = v.toObject(); }
    const auto own = self.value(k("team")).toString();
    if (selves != 1 || self.value(k("champion_id")).toInt() != state.championId
        || (own != k("ORDER") && own != k("CHAOS"))) return fail("self_mapping_unverified");
    QSet<QString> liveKeys, historicalKeys; int allies = 0, enemies = 0;
    for (const auto &v : state.players) {
        const auto p = v.toObject(); const auto team = side(p.value(k("team")), own);
        const auto champion = p.value(k("champion_id"));
        if (team.isEmpty() || !integer(champion, 1, 1000000) || !catalog.champions.contains(champion.toInt())) return fail("live_roster_identity_invalid");
        const auto key = team + QChar(u':') + QString::number(champion.toInt());
        if (liveKeys.contains(key)) return fail("duplicate_live_team_champion");
        liveKeys.insert(key); allies += team == own; enemies += team != own;
    }
    if (allies != 5 || enemies != 5) return fail("team_roster_incomplete");
    QMap<QString, QJsonObject> references;
    for (const auto &v : history.value(k("players")).toArray()) {
        const auto p = v.toObject(); const auto team = side(p.value(k("team")), own);
        const auto champion = p.value(k("champion_id"));
        if (team.isEmpty() || !integer(champion, 1, 1000000)) return fail("history_roster_identity_invalid");
        const auto key = team + QChar(u':') + QString::number(champion.toInt());
        if (historicalKeys.contains(key)) return fail("duplicate_history_team_champion");
        if (!liveKeys.contains(key)) return fail("history_roster_mismatch");
        historicalKeys.insert(key); references[key] = p;
    }
    const auto materialized = history.value(k("multidimensional_context")).toObject();
    const bool verifiedMaterialized = materialized.value(k("status")) == k("ready")
        && materialized.value(k("freshness_verified")).toBool() && !materialized.value(k("requires_materialization")).toBool(true)
        && materialized.value(k("as_of_ms")) == at;
    out[k("context_id")] = context; out[k("patch")] = catalog.patch; out[k("as_of_ms")] = at;
    out[k("multidimensional_profile_consumed")] = false;
    out[k("multidimensional_freshness_verified")] = verifiedMaterialized;
    out[k("history_summary_cutoff_policy")] = k("completed_matches_strictly_before_query_as_of");
    QJsonArray players, weights,allyWeights; double total = 0,allyTotal=0; int known = 0,allyKnown=0,mapped = 0,knownHistory=0,freshHistory=0,enginePlayers=0,materializedPlayers=0,sourceAnalyzed=0;
    int knownAllies=0,knownEnemies=0;QJsonArray missingPlayers;
    for (int index = 0; index < state.players.size(); ++index) {
        const auto current = state.players[index].toObject(); const auto team = side(current.value(k("team")), own);
        const int champion = current.value(k("champion_id")).toInt();
        const auto key = team + QChar(u':') + QString::number(champion); const auto raw = references.value(key);
        QJsonObject row{{k("player_index"), index}, {k("champion_id"), champion}, {k("team"), team == own ? k("ALLY") : k("ENEMY")},
            {k("self"), current.value(k("self")).toBool()}, {k("status"), k("history_missing")}};
        const auto rawStatus=raw.value(k("status")).toString();
        row[k("history_status")]=QStringList{k("known"),k("no_data"),k("fields_missing"),k("identity_missing"),k("ambiguous_identity"),k("duplicate_identity")}.contains(rawStatus)?rawStatus:k("history_missing");
        row[k("freshness")]=analysisFreshness(raw.value(k("freshness")).toObject());
        row[k("coverage")]=counts(raw.value(k("coverage")).toObject(),{k("eligible_games"),k("sampled_games"),k("timeline_games"),k("timeline_sampled_games"),k("process_item_games"),k("process_augment_games"),k("growth_observation_games"),k("permanent_growth_observation_games"),k("real_offer_observations"),k("confirmed_choice_observations")});
        bool rowKnown=false,rowEngine=false,rowMaterialized=false,rowSourceAnalyzed=false;
        bool valid = !raw.isEmpty();
        if (valid && (!raw.value(k("self")).isBool() || raw.value(k("self")).toBool() != current.value(k("self")).toBool())) return fail("history_self_mismatch");
        if (valid && integer(raw.value(k("cell")), 0, 1000) && integer(current.value(k("cell")), 0, 1000)
            && raw.value(k("cell")).toInt() != current.value(k("cell")).toInt()) return fail("history_cell_mismatch");
        valid &= raw.value(k("status")) == k("known") || raw.value(k("status")) == k("fields_missing");
        const auto engine=raw.value(k("player_analysis")).toObject();
        const auto engineStatus=engine.value(k("status")).toString();const auto recent=engine.value(k("recent")).toObject();
        const bool recentCutoffValid=integer(recent.value(k("games")),0,20)
            &&(recent.value(k("games")).toInt()==0||integer(recent.value(k("latest_ended_at_ms")),1,at.toDouble()-1));
        if(engine.value(k("source"))==k("bounded_source_engine")&&engine.value(k("as_of_ms"))==at
            &&engine.value(k("source_snapshot_verified")).toBool()&&recentCutoffValid
            &&QStringList{k("known"),k("fields_missing"),k("no_data")}.contains(engineStatus)
            &&(valid||rawStatus==k("no_data"))){
            auto clean=sanitizeHistoricalProjectionExtension(engine);clean[k("status")]=engineStatus;clean[k("source")]=k("bounded_source_engine");clean[k("as_of_ms")]=at;
            if(integer(engine.value(k("engine_version")),1,1000))clean[k("engine_version")]=engine.value(k("engine_version"));
            clean[k("source_snapshot_verified")]=true;clean[k("historical_reference_only")]=true;clean[k("causal_effect_known")]=false;clean[k("skill_multiplier")]=false;
            row[k("player_analysis")]=clean;rowSourceAnalyzed=true;
            rowEngine=engineStatus!=k("no_data")&&clean.value(k("recent")).toObject().value(k("games")).toDouble()>0;rowKnown|=rowEngine;
        }else if(!engine.isEmpty()){
            const auto reason=engine.value(k("reason")).toString();QJsonObject clean{{k("status"),engineStatus==k("unavailable")?k("unavailable"):k("unverified")},{k("source_snapshot_verified"),false}};
            if(QStringList{k("identity_unresolved"),k("identity_missing"),k("no_data"),k("ambiguous_identity"),k("duplicate_identity")}.contains(reason))clean[k("reason")]=reason;
            row[k("player_analysis")]=clean;
        }
        if (valid) {
            ++mapped; row[k("status")] = k("mapped");
            for (const auto &scope : {k("same_patch_champion"), k("same_patch"), k("same_champion"), k("history")}) {
                const auto s = summary(raw.value(scope).toObject()); if (s.value(k("available")).toBool()){row[scope] = s;rowKnown=true;}
            }
            for(const auto &name:{k("profile_summary"),k("champion_references")})if(raw.value(name).isObject()){
                const auto profile=profileReferences(raw.value(name).toObject());row[name]=profile;
                rowKnown|=profile.value(k("available")).toBool()&&profile.value(k("sampled_games")).toDouble()>0;
            }
            const auto profile=raw.value(k("multidimensional_profile")).toObject();
            if(verifiedMaterialized&&profile.value(k("status"))==k("known")){
                auto clean=projectionProfile(profile);
                rowMaterialized=clean.value(k("overall")).toObject().value(k("games")).toDouble()>0
                    ||clean.value(k("champion_experience")).toObject().value(k("games")).toDouble()>0;
                if(rowMaterialized){row[k("multidimensional_profile")]=clean;rowKnown=true;}
            }
            QString selected; QJsonObject reference;
            for (const auto &scope : {k("same_patch_champion"), k("same_patch")}) {
                const auto s = row.value(scope).toObject();
                const auto dpm = s.value(k("metrics")).toObject().value(k("damage_per_minute"));
                const auto n = s.value(k("field_samples")).toObject().value(k("damage_per_minute"));
                if (s.value(k("available")).toBool() && dpm.isDouble() && std::isfinite(dpm.toDouble()) && dpm.toDouble() >= 0
                    && integer(n, 5, 10000000)) { selected = scope; reference = s; break; }
            }
            if (!selected.isEmpty()) {
                row[k("selected_pressure_scope")] = selected;
                if (team != own) {
                    const double dpm = reference.value(k("metrics")).toObject().value(k("damage_per_minute")).toDouble();
                    weights.append(QJsonObject{{k("player_index"), index}, {k("champion_id"), champion}, {k("damage_per_minute"), dpm},
                        {k("samples"), reference.value(k("field_samples")).toObject().value(k("damage_per_minute"))}, {k("scope"), selected}});
                    total += dpm; ++known;
                }else{
                    const double dpm=reference.value(k("metrics")).toObject().value(k("damage_per_minute")).toDouble();
                    allyWeights.append(QJsonObject{{k("player_index"),index},{k("champion_id"),champion},{k("self"),current.value(k("self")).toBool()},
                        {k("damage_per_minute"),dpm},{k("samples"),reference.value(k("field_samples")).toObject().value(k("damage_per_minute"))},{k("scope"),selected}});
                    allyTotal+=dpm;++allyKnown;
                }
            }
        }
        row[k("history_analysis_status")]=rowEngine?k("analyzed"):rowKnown?k("summary_only"):k("history_missing");
        row[k("player_analysis_consumed")]=rowEngine;row[k("source_analysis_verified")]=rowSourceAnalyzed;row[k("multidimensional_profile_consumed")]=rowMaterialized;
        if(rowKnown){++knownHistory;if(team==own)++knownAllies;else ++knownEnemies;
            if(row.value(k("freshness")).toObject().value(k("status"))==k("fresh"))++freshHistory;}
        else missingPlayers.append(index);
        enginePlayers+=rowEngine;materializedPlayers+=rowMaterialized;sourceAnalyzed+=rowSourceAnalyzed;
        players.append(row);
    }
    pressure[k("known_enemy_count")] = known;
    pressure[k("reason")] = known != 5 ? k("five_same_patch_enemy_damage_references_required")
        : !std::isfinite(total) ? k("historical_enemy_damage_sum_invalid")
        : total <= 0 ? k("historical_enemy_damage_sum_is_zero") : k("available");
    if (known == 5 && total > 0 && std::isfinite(total)) {
        QJsonArray normalized;
        for (const auto &v : weights) { auto w = v.toObject(); w[k("weight")] = w.value(k("damage_per_minute")).toDouble() / total; normalized.append(w); }
        pressure[k("available")] = true; pressure[k("weights")] = normalized;
    }
    QJsonObject allyScenario{{k("available"),false},{k("known_ally_count"),allyKnown},{k("weights"),QJsonArray{}},{k("minimum_field_samples"),5},
        {k("scope"),k("historical_ally_output_share_sensitivity_scenario")},{k("current_dps_known"),false},{k("skill_multiplier"),false},
        {k("causal_effect_known"),false},{k("damage_taken_is_current_hp"),false},{k("reason"),k("five_same_patch_ally_damage_references_required")}};
    if(allyKnown==5&&allyTotal>0&&std::isfinite(allyTotal)){
        QJsonArray normalized;for(const auto &v:allyWeights){auto w=v.toObject();w[k("weight")]=w.value(k("damage_per_minute")).toDouble()/allyTotal;normalized.append(w);}
        allyScenario[k("available")]=true;allyScenario[k("weights")]=normalized;allyScenario[k("reason")]=k("five_ally_historical_dpm_weights_validated");
    }else if(allyKnown==5)allyScenario[k("reason")]=k("historical_ally_damage_sum_invalid_or_zero");
    out[k("available")] = knownHistory > 0; out[k("status")] = knownHistory==10&&freshHistory==10&&enginePlayers==10?k("ready"):knownHistory>0?k("partial"):k("no_roster_history");
    if (mapped == 0) out[k("usable_as_live")] = false;
    out[k("mapped_players")] = mapped;out[k("evaluated_players")]=10;out[k("known_history_players")]=knownHistory;out[k("fresh_history_players")]=freshHistory;
    out[k("player_analysis_consumed_players")]=enginePlayers;out[k("multidimensional_profile_consumed_players")]=materializedPlayers;out[k("multidimensional_profile_consumed")]=materializedPlayers>0;
    out[k("source_analyzed_players")]=sourceAnalyzed;
    out[k("analysis_coverage")]=QJsonObject{{k("requested_players"),10},{k("analyzed_players"),sourceAnalyzed},{k("known_history_players"),knownHistory},{k("fresh_history_players"),freshHistory},{k("all_ten_analyzed"),sourceAnalyzed==10}};
    out[k("coverage")]=QJsonObject{{k("expected_players"),10},{k("evaluated_players"),10},{k("known_history_players"),knownHistory},{k("fresh_history_players"),freshHistory},
        {k("known_ally_players"),knownAllies},{k("known_enemy_players"),knownEnemies},{k("player_analysis_consumed_players"),enginePlayers},{k("missing_player_indices"),missingPlayers},
        {k("all_ten_have_history"),knownHistory==10},{k("all_ten_history_fresh"),freshHistory==10},{k("all_ten_source_analyses_consumed"),enginePlayers==10}};
    out[k("players")] = players; out[k("historical_pressure_scenario")] = pressure;out[k("historical_ally_output_scenario")]=allyScenario;
    return out;
}
