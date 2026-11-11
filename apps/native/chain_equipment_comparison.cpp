#include "chain_equipment_comparison.h"
#include "combat.h"
#include "continuous_sandbox.h"
#include "chain_mechanism_contract.h"
#include "combat_unknown_policy.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
constexpr double eps = 1e-8;
bool finite(const QJsonValue &v, double minimum = 0., double maximum = 1e12) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= minimum && v.toDouble() <= maximum;
}
QJsonValue numericOrUnknown(const QJsonObject &o, const QString &key) {
    const auto v = o.value(key); return finite(v, -1e12) ? v : QJsonValue(QJsonValue::Null);
}
QString digest(const QJsonObject &o) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
QJsonObject invalid(const QString &reason) {
    return {{k("available"), false}, {k("status"), k("invalid_chain_equipment_comparison")},
            {k("reason"), reason}, {k("actual_win_probability"), QJsonValue::Null}};
}
QString playerKey(const QJsonObject &row) {
    return row.value(k("player_key")).toString(row.value(k("team")).toString() + k(":") +
        QString::number(row.value(k("champion_id")).toInt()) + k(":1"));
}
QJsonObject finalPlayer(const QJsonObject &result, const QString &key) {
    for (const auto &v : result.value(k("players_final")).toArray())
        if (v.toObject().value(k("player_key")).toString() == key) return v.toObject();
    return {};
}
bool legalInventory(const CombatDatabase &db, const QJsonValue &value, int champion, int level, QList<int> *ids) {
    if (!value.isArray() || value.toArray().size() > 6) return false;
    ids->clear();
    for (const auto &v : value.toArray()) {
        if (!finite(v, 1, std::numeric_limits<int>::max()) || std::floor(v.toDouble()) != v.toDouble()) return false;
        const int id = v.toInt();
        if (!compatibleEquipment(db, *ids, id, champion, level)) return false;
        ids->append(id);
    }
    return true;
}
QJsonArray itemJson(const QList<int> &ids) { QJsonArray out; for (int id : ids) out.append(id); return out; }
// Compare decisions only; damage/heal amounts and skill-ready times are the
// effects being compared and therefore must not be part of this signature.
QJsonObject admissions(const QJsonObject &result, const QSet<QString> &inputIds) {
    QJsonObject decisions;
    for (const auto &v : result.value(k("trace")).toArray()) {
        const auto row = v.toObject(); const auto id = row.value(k("event_id")).toString();
        if (!inputIds.contains(id) || !row.value(k("accepted")).isBool()) continue;
        auto entries = decisions.value(id).toArray();
        entries.append(QJsonObject{{k("accepted"), row.value(k("accepted"))}, {k("type"), row.value(k("type"))},
            {k("time"), row.value(k("time"))}, {k("reason"), row.value(k("reason")).toString()}});
        decisions[id] = entries;
    }
    return decisions;
}
QJsonArray decisionChanges(const QJsonObject &before, const QJsonObject &after, const QSet<QString> &ids) {
    QStringList ordered = ids.values(); ordered.sort(); QJsonArray out;
    for (const auto &id : ordered) if (before.value(id) != after.value(id))
        out.append(QJsonObject{{k("event_id"), id}, {k("baseline"), before.value(id)}, {k("candidate"), after.value(id)}});
    return out;
}
QJsonObject metrics(const QJsonObject &result, const QJsonObject &focus) {
    QJsonObject out;
    for (const auto &name : {k("initial_health"), k("maximum_health"), k("remaining_health"),
            k("effective_damage_dealt"), k("effective_damage_received"), k("shield_damage_dealt"),
            k("raw_healing_received"), k("amplified_healing_received"), k("effective_healing_received"),
            k("raw_shield_received"), k("generated_shield_received"), k("generated_self_shield"),
            k("shield_absorbed"), k("remaining_shield"), k("permanent_health_growth"),
            k("health_cost_received"), k("natural_health_regen_received"), k("respawn_health_restored")})
        out[name] = numericOrUnknown(focus, name);
    out[k("alive")] = focus.value(k("alive"));
    out[k("death_time")] = focus.value(k("death_time"));
    out[k("skill_and_resource_state")] = focus.value(k("final_action_state"));
    out[k("progression_state")] = focus.value(k("progression_state"));
    out[k("sustain_totals_scope")] = focus.value(k("sustain_totals_scope"));
    out[k("unknowns")] = focus.value(k("unknowns"));
    out[k("team_totals")] = result.value(k("team_totals"));
    return out;
}
QJsonObject deltas(const QJsonObject &before, const QJsonObject &after) {
    QJsonObject out;
    for (auto it = before.begin(); it != before.end(); ++it) {
        const auto a = after.value(it.key());
        if (it.value().isDouble() || it.value().isNull())
            out[it.key()] = finite(it.value(), -1e12) && finite(a, -1e12) ?
                QJsonValue(a.toDouble() - it.value().toDouble()) : QJsonValue(QJsonValue::Null);
    }
    return out;
}
std::optional<double> value(const QJsonObject &focus, const QJsonObject &basis) {
    if (!finite(focus.value(k("remaining_health"))) || !finite(focus.value(k("effective_damage_dealt"))) ||
            !focus.value(k("alive")).isBool()) return {};
    const double result = basis.value(k("damage_weight")).toDouble() * focus.value(k("effective_damage_dealt")).toDouble() /
            basis.value(k("enemy_initial_health")).toDouble() +
        basis.value(k("survival_weight")).toDouble() * focus.value(k("remaining_health")).toDouble() /
            basis.value(k("focus_initial_health")).toDouble() -
        basis.value(k("death_penalty")).toDouble() * (focus.value(k("alive")).toBool() ? 0. : 1.);
    return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}
bool completeTeemoTrapPayload(const Catalog &catalog,const QJsonObject &actor,const QJsonObject &event,const QJsonObject &result) {
    // The old generic R formula was quarantined because ammo/trigger state
    // was absent. Only the migrated, source-bound trap program and its full
    // native schedule/damage receipts can supersede that specific warning.
    if(actor.value(k("champion_id"))!=QJsonValue(17)||event.value(k("type"))!=k("cast")||event.value(k("slot"))!=k("R")||
        event.value(k("hit_confirmed"))!=QJsonValue(true))return false;
    const auto facts=event.value(k("facts")).toObject();
    if(facts.value(k("verified_trap_trigger"))!=QJsonValue(true)||facts.value(k("verified_trap_trigger_time"))!=QJsonValue(true)||
        facts.value(k("verified_tick_schedule"))!=QJsonValue(true)||facts.value(k("verified_target_coverage"))!=QJsonValue(true)||
        facts.value(k("trap_instance_id")).toString().isEmpty())return false;
    const auto offsets=facts.value(k("tick_offsets")).toArray(),fractions=facts.value(k("tick_fractions")).toArray();
    if(offsets.isEmpty()||offsets.size()!=fractions.size())return false;
    double requestedFraction=0;for(const auto &fraction:fractions){if(!finite(fraction,eps,1))return false;requestedFraction+=fraction.toDouble();}
    if(std::abs(requestedFraction-1)>eps)return false;
    const auto hero=catalog.combat->champions.value(17);QJsonObject binding;
    for(const auto &raw:hero.value(k("temporal_bindings_v4")).toArray()) {
        const auto row=raw.toObject();if(row.value(k("binding_id"))==k("17:R:noxious_trap_poison:temporal_v4"))binding=row;
    }
    const auto phases=binding.value(k("phases")).toArray();
    if(binding.value(k("slot"))!=k("R")||binding.value(k("kind"))!=k("dot")||phases.size()!=1||
        binding.value(k("binding_sha256")).toString().isEmpty())return false;
    const auto phase=phases.first().toObject(),component=phase.value(k("component")).toObject();
    if(phase.value(k("quantity_unit"))!=k("total")||component.value(k("calculation"))!=k("TotalDamage")||
        component.value(k("expression")).toObject().isEmpty())return false;
    const auto who=playerKey(actor),id=event.value(k("event_id")).toString(),target=event.value(k("target_key")).toString();
    const auto owner=k("hero:17"),bindingId=binding.value(k("binding_id")).toString();
    const auto trace=result.value(k("trace")).toArray();QJsonObject scheduled;bool admitted=false,completed=false;
    for(const auto &raw:trace) {
        const auto row=raw.toObject();
        if(row.value(k("event_id"))==id&&row.value(k("player_key"))==who&&row.value(k("accepted"))==QJsonValue(true)) {
            admitted|=row.value(k("type"))==k("cast");completed|=row.value(k("type"))==k("cast_complete");
        }
        if(row.value(k("type"))==k("temporal_scheduled")&&row.value(k("source_event_id"))==id&&
            row.value(k("player_key"))==who&&row.value(k("source_owner"))==owner&&row.value(k("source_binding_id"))==bindingId&&
            row.value(k("source_binding_sha256"))==binding.value(k("binding_sha256"))&&row.value(k("slot"))==k("R")&&
            row.value(k("accepted"))==QJsonValue(true)) {
            if(!scheduled.isEmpty())return false;scheduled=row;
        }
    }
    const auto instance=scheduled.value(k("cast_instance_id")).toString();const auto keys=scheduled.value(k("scheduled_phase_keys")).toArray();
    if(!admitted||!completed||instance.isEmpty()||keys.isEmpty()||keys.size()!=fractions.size()||
        scheduled.value(k("phase_count"))!=QJsonValue(keys.size()))return false;
    QSet<QString> expected,delivered;for(const auto &key:keys){if(!key.isString()||key.toString().isEmpty()||expected.contains(key.toString()))return false;expected.insert(key.toString());}
    double deliveredFraction=0;
    for(const auto &raw:trace) {
        const auto row=raw.toObject();if(row.value(k("type"))!=k("temporal_tick")||row.value(k("cast_instance_id"))!=instance)continue;
        const auto key=row.value(k("_temporal_phase_key")).toString();const auto actual=row.value(k("_temporal_component")).toObject();
        if(!expected.contains(key)||delivered.contains(key)||row.value(k("phase"))!=k("payload")||row.value(k("player_key"))!=who||
            row.value(k("target_key"))!=target||row.value(k("_temporal_owner"))!=owner||row.value(k("_temporal_binding_id"))!=bindingId||
            row.value(k("slot"))!=k("R")||!finite(row.value(k("damage")))||!finite(row.value(k("_temporal_scale")),eps,1))return false;
        for(const auto &field:{k("expression"),k("calculation"),k("source_pointer"),k("damage_type")})if(actual.value(field)!=component.value(field))return false;
        int receipts=0;for(const auto &receiptRaw:trace){const auto receipt=receiptRaw.toObject();
            if(receipt.value(k("type"))==k("damage_receipt")&&receipt.value(k("event_id"))==row.value(k("event_id"))&&
                receipt.value(k("source_key"))==who&&receipt.value(k("target_key"))==target&&finite(receipt.value(k("requested_damage")))&&
                finite(receipt.value(k("actual_damage")))&&finite(receipt.value(k("shield_damage"))))++receipts;
        }
        if(receipts!=1)return false;delivered.insert(key);deliveredFraction+=row.value(k("_temporal_scale")).toDouble();
    }
    return delivered==expected&&std::abs(deliveredFraction-1)<=eps;
}
bool fixedCastPayloadsCovered(const Catalog &catalog,const QJsonObject &scenario,const QJsonObject &result) {
    QMap<QString,QJsonObject> roster;for(const auto &v:scenario.value(k("players")).toArray()){const auto p=v.toObject();roster[playerKey(p)]=p;}
    for(const auto &v:scenario.value(k("action_events")).toArray()){const auto event=v.toObject();const auto type=event.value(k("type")).toString();if(type!=k("cast")&&type!=k("skill_state"))continue;
        const auto id=event.value(k("event_id")).toString(),who=event.value(k("player_key")).toString();const auto contract=chainActionContract(catalog,event,roster.value(who));
        if(!contract.value(k("known")).toBool())return false;if(!contract.value(k("requires_admitted_cast")).toBool())continue;
        const auto slot=contract.value(k("slot")).toString();const auto hero=catalog.combat->champions.value(roster.value(who).value(k("champion_id")).toInt());
        const bool completeTrap=completeTeemoTrapPayload(catalog,roster.value(who),event,result);bool sourceBacked=completeTrap,delivered=completeTrap;
        for(const auto &component:hero.value(k("components")).toArray())sourceBacked|=component.toObject().value(k("slot"))==slot&&!component.toObject().value(k("expression")).toObject().isEmpty();
        for(const auto &program:hero.value(k("stateful_bindings_v7")).toArray())sourceBacked|=program.toObject().value(k("slot"))==slot&&program.toObject().value(k("hero_event_contract_v1")).toBool();
        for(const auto &raw:result.value(k("trace")).toArray()){const auto row=raw.toObject();const auto nativeId=row.value(k("event_id")).toString();
            if(nativeId!=id&&!nativeId.startsWith(id+k(":"))&&row.value(k("source_cast_instance_id"))!=id)continue;
            delivered|=(row.value(k("phase"))==k("payload")&&row.value(k("player_key"))==who&&finite(row.value(k("damage"))))||
                (row.value(k("type"))==k("damage_receipt")&&row.value(k("source_key"))==who&&finite(row.value(k("requested_damage"))))||
                (row.value(k("phase"))==k("mechanic_delivery")&&row.value(k("accepted"))==QJsonValue(true)&&row.value(k("player_key"))==who);
        }
        if(!sourceBacked||!delivered)return false;
    }
    return true;
}
QStringList criticalInputs(const Catalog &catalog,const QJsonObject &scenario,const QJsonObject &result,const QJsonObject &mechanism) {
    QStringList unknowns;for(const auto &v:result.value(k("model_unknowns")).toArray())unknowns.append(v.toString());
    QMap<QString,QSet<QString>> referencedSlots,deliveredHeroSlots;QMap<QString,bool> completeTrapActors;
    QMap<QString,QJsonObject> roster;for(const auto &v:scenario.value(k("players")).toArray()){const auto p=v.toObject();roster[playerKey(p)]=p;}
    for(const auto &v:scenario.value(k("action_events")).toArray()){const auto e=v.toObject();if(e.value(k("type"))==k("cast")||e.value(k("type"))==k("skill_state")){
        const auto who=e.value(k("player_key")).toString(),slot=chainActionContract(catalog,e,roster.value(who)).value(k("slot")).toString();referencedSlots[who].insert(slot);
        if(roster.value(who).value(k("champion_id"))==QJsonValue(17)&&slot==k("R"))completeTrapActors[who]=completeTrapActors.value(who,true)&&completeTeemoTrapPayload(catalog,roster.value(who),e,result);
    }}
    for(const auto &v:result.value(k("trace")).toArray()){const auto e=v.toObject(),receipt=e.value(k("_state_hero_hit_receipt_v1")).toObject();
        if(receipt.isEmpty()||e.value(k("source_cast_instance_id")).toString().isEmpty()||e.value(k("hit_event_id")).toString().isEmpty()||e.value(k("damage_packet_id")).toString().isEmpty())continue;
        const auto origin=receipt.value(k("origin_ability_id")).toString();if(origin==e.value(k("origin_ability_id")).toString())deliveredHeroSlots[e.value(k("player_key")).toString()].insert(origin.section(QChar(u':'),-1));
    }
    QMap<QString,bool> necessary;
    for(const auto &p:result.value(k("players_final")).toArray()){const auto player=p.toObject();const auto who=player.value(k("player_key")).toString();
        for(const auto &u:player.value(k("unknowns")).toArray()){const auto text=u.toString();bool required=true;
            if(text.startsWith(k("skill_semantic_quarantine:"))){const auto slot=text.section(QChar(u':'),1,1);
                required=referencedSlots.value(who).contains(slot)&&!(text.endsWith(k(":owned_hero_hit_event_required"))&&deliveredHeroSlots.value(who).contains(slot));
                if(text==k("skill_semantic_quarantine:R:TotalDamage:ammunition_state_not_cast_cooldown")&&completeTrapActors.value(who,false))required=false;
            }
            necessary[text]=necessary.value(text,false)||required;unknowns.append(text);
        }
    }
    for(auto it=necessary.begin();it!=necessary.end();++it)if(!it.value())unknowns.removeAll(it.key());
    // Only this specific formula count is superseded by a complete native
    // prime/hit/delivery audit; no synthetic hit count is inserted.
    if(mechanism.value(k("required")).toBool()&&mechanism.value(k("complete")).toBool())unknowns.removeAll(k("unobserved_event:augment:3049:hits"));
    // This fixed-window count warns that fewer than three skill formulas are
    // available. Unused skills are not required by a fixed explicit chain.
    // Referenced casts still need source-backed native payloads; every other
    // critical formula/state/observation gap remains subject to the gate.
    if(fixedCastPayloadsCovered(catalog,scenario,result))unknowns.removeAll(k("incomplete_skill_cast_model"));
    return combatCriticalUnknowns(unknowns);
}
}

QJsonObject compareChainEquipment(const Catalog &catalog, const QJsonObject &request) {
    if (!catalog.combat) return invalid(k("combat_database_unavailable"));
    if (!request.value(k("scenario")).isObject() || !request.value(k("candidate_inventories")).isArray())
        return invalid(k("scenario_and_candidate_inventories_required"));
    const auto &db = *catalog.combat; auto scenario = request.value(k("scenario")).toObject();
    const auto candidates = request.value(k("candidate_inventories")).toArray();
    if (candidates.isEmpty() || candidates.size() > 16) return invalid(k("candidate_count_must_be_one_to_sixteen"));
    if (request.contains(k("maximum_inventory_cost")) && !finite(request.value(k("maximum_inventory_cost")), 0, 1e9))
        return invalid(k("invalid_maximum_inventory_cost"));
    const bool budgetSupplied = request.contains(k("maximum_inventory_cost"));
    if (scenario.contains(k("checkpoint")) || scenario.contains(k("participant_scope")) || scenario.contains(k("policies")))
        return invalid(k("fresh_ten_player_single_policy_scenario_required"));
    if (scenario.contains(k("auto_actions")) && scenario.value(k("auto_actions")) != QJsonValue(false))
        return invalid(k("automatic_actions_not_allowed_in_fixed_event_comparison"));
    if (!scenario.value(k("players")).isArray() || scenario.value(k("players")).toArray().size() != 10 ||
            !scenario.value(k("action_events")).isArray() || scenario.value(k("action_events")).toArray().size() > 512)
        return invalid(k("fixed_ten_players_and_at_most_512_explicit_events_required"));
    scenario[k("auto_actions")] = false;
    const auto focusKey = request.value(k("focus_player_key")).toString();
    if (focusKey.isEmpty()) return invalid(k("focus_player_key_required"));
    auto players = scenario.value(k("players")).toArray(); int focusIndex = -1;
    for (int i = 0; i < players.size(); ++i) if (playerKey(players[i].toObject()) == focusKey) {
        if (focusIndex >= 0) return invalid(k("focus_player_key_not_unique")); focusIndex = i;
    }
    if (focusIndex < 0) return invalid(k("focus_player_not_in_roster"));
    const auto focusInput = players[focusIndex].toObject(); const auto side = focusInput.value(k("team")).toString();
    const int champion = focusInput.value(k("champion_id")).toInt(), level = focusInput.value(k("level")).toInt();
    QList<int> baselineItems;
    if (!legalInventory(db, focusInput.value(k("items")), champion, level, &baselineItems)) return invalid(k("baseline_inventory_not_legal"));
    QSet<QString> inputIds;
    for (const auto &v : scenario.value(k("action_events")).toArray()) {
        const auto event = v.toObject(); const auto type = event.value(k("type")).toString();
        if (!v.isObject() || event.value(k("event_id")).toString().isEmpty()) return invalid(k("explicit_event_id_required"));
        if (inputIds.contains(event.value(k("event_id")).toString())) return invalid(k("duplicate_explicit_event_id"));
        inputIds.insert(event.value(k("event_id")).toString());
        if (QStringList{k("attack"), k("cast"), k("skill_state")}.contains(type) && event.value(k("target_key")).toString().isEmpty())
            return invalid(k("explicit_target_key_required_for_comparison_actions"));
        if (QStringList{k("purchase"), k("purchase_item"), k("sell"), k("sell_item"), k("inventory_change")}.contains(type))
            return invalid(k("inventory_transactions_require_separate_purchase_comparison"));
    }
    if (request.contains(k("objective")) && !request.value(k("objective")).isObject()) return invalid(k("invalid_comparison_objective"));
    const auto weightInput = request.value(k("objective")).toObject();
    for (const auto &name : {k("damage_weight"), k("survival_weight"), k("death_penalty")})
        if (weightInput.contains(name) && !finite(weightInput.value(name), 0, 100)) return invalid(k("invalid_comparison_objective_weight"));
    const auto baseline = runContinuousSandbox(catalog, scenario);
    if (baseline.value(k("available")) != QJsonValue(true)) {
        auto out = invalid(k("baseline_scenario_unavailable")); out[k("baseline_diagnostic")] = baseline; return out;
    }
    const auto baselineFocus = finalPlayer(baseline, focusKey), baselineMetrics = metrics(baseline, baselineFocus);
    QJsonArray initialStates;
    for(const auto &v:baseline.value(k("players_final")).toArray()){const auto p=v.toObject();initialStates.append(QJsonObject{
        {k("player_key"),p.value(k("player_key"))},{k("initial_health"),p.value(k("initial_health"))},{k("initially_dead"),p.value(k("initially_dead"))}});}
    const auto baselineAdmission = admissions(baseline, inputIds);
    const auto baselineMechanism=chainMechanismExecution(catalog,scenario,baseline);
    QStringList criticalUnknowns=criticalInputs(catalog,scenario,baseline,baselineMechanism);
    double enemyHealth = 0.;
    for (const auto &v : baseline.value(k("players_final")).toArray()) {
        const auto p = v.toObject(); if (p.value(k("team")).toString() != side && finite(p.value(k("initial_health")))) enemyHealth += p.value(k("initial_health")).toDouble();
    }
    if (!finite(baselineFocus.value(k("initial_health")), eps) || enemyHealth <= eps) {
        auto out=invalid(k("positive_baseline_health_required_for_normalization"));out[k("baseline_metrics")]=baselineMetrics;
        out[k("baseline_initial_states")]=initialStates;out[k("ranking_available")]=false;return out;
    }
    QJsonObject basis{{k("damage_weight"), weightInput.value(k("damage_weight")).toDouble(1.)},
        {k("survival_weight"), weightInput.value(k("survival_weight")).toDouble(1.)},
        {k("death_penalty"), weightInput.value(k("death_penalty")).toDouble(2.)},
        {k("focus_initial_health"), baselineFocus.value(k("initial_health"))}, {k("enemy_initial_health"), enemyHealth},
        {k("frozen_before_candidates"), true}, {k("healing_and_absorption_scored_again"), false},
        {k("formula"), k("damage_weight*focus_effective_damage/enemy_baseline_initial_HP+survival_weight*focus_remaining_HP/focus_baseline_initial_HP-death_penalty*focus_dead")},
        {k("scope"), k("fixed_scenario_output_and_remaining_health_not_win_probability")}};
    const auto baselineScore = value(baselineFocus, basis);
    const bool baselineAdmitted = baselineAdmission.size() == inputIds.size() && baseline.value(k("events_rejected")).toInt() == 0;
    QJsonArray rows, ranking; QSet<QString> labels; QVector<QPair<double, QString>> ordered;bool completeComparison=baselineScore.has_value();
    for (int i = 0; i < candidates.size(); ++i) {
        const auto supplied = candidates[i].toObject(); const auto label = supplied.value(k("candidate_id")).toString(); QList<int> ids;
        if (!candidates[i].isObject() || label.isEmpty() || label.size() > 160 || labels.contains(label)) return invalid(k("candidate_ids_must_be_unique_nonempty_strings"));
        labels.insert(label); QJsonObject row{{k("candidate_id"), label}, {k("ranking_eligible"), false}};
        if (!legalInventory(db, supplied.value(k("items")), champion, level, &ids)) {
            completeComparison=false;
            row[k("status")] = k("invalid_candidate_inventory"); row[k("score")] = QJsonValue::Null; rows.append(row); continue;
        }
        row[k("items")] = itemJson(ids);
        double inventoryCost = 0.; bool costKnown = true;
        for (int id : ids) { const int price = db.items.value(id).price; if (price < 0) costKnown = false; else inventoryCost += price; }
        row[k("inventory_cost")] = costKnown ? QJsonValue(inventoryCost) : QJsonValue(QJsonValue::Null);
        if (budgetSupplied && (!costKnown || inventoryCost > request.value(k("maximum_inventory_cost")).toDouble() + eps)) {
            completeComparison=false;
            row[k("status")] = costKnown ? k("candidate_exceeds_inventory_cost_limit") : k("candidate_inventory_cost_unknown");
            row[k("score")] = QJsonValue::Null; rows.append(row); continue;
        }
        auto changedPlayers = players; auto changedFocus = focusInput; changedFocus[k("items")] = itemJson(ids); changedPlayers[focusIndex] = changedFocus;
        auto changedScenario = scenario; changedScenario[k("players")] = changedPlayers;
        const auto replay = runContinuousSandbox(catalog, changedScenario);
        if (replay.value(k("available")) != QJsonValue(true)) {
            completeComparison=false;
            row[k("status")] = k("candidate_scenario_unavailable"); row[k("score")] = QJsonValue::Null; row[k("diagnostic")] = replay; rows.append(row); continue;
        }
        const auto candidateFocus = finalPlayer(replay, focusKey), candidateMetrics = metrics(replay, candidateFocus);
        const auto candidateAdmissions = admissions(replay, inputIds);
        const auto changedDecisions = decisionChanges(baselineAdmission, candidateAdmissions, inputIds);
        const auto candidateMechanism=chainMechanismExecution(catalog,changedScenario,replay);
        const bool mechanismComparable=baselineMechanism.value(k("complete")).toBool()&&candidateMechanism.value(k("complete")).toBool()&&baselineMechanism.value(k("decisions"))==candidateMechanism.value(k("decisions"));
        const auto candidateCritical=criticalInputs(catalog,changedScenario,replay,candidateMechanism);criticalUnknowns.append(candidateCritical);
        const bool comparable = baselineAdmitted && changedDecisions.isEmpty() && candidateAdmissions.size() == inputIds.size() && replay.value(k("events_rejected")).toInt() == 0&&mechanismComparable;
        completeComparison&=comparable;
        const auto score = comparable ? value(candidateFocus, basis) : std::nullopt;
        completeComparison&=score.has_value()&&baselineScore.has_value();
        row[k("status")] = comparable ? k("conditional_comparable") : k("not_comparable_event_admission_changed");
        if(comparable&&(!score||!baselineScore))row[k("status")]=k("objective_numeric_unavailable");
        row[k("metrics")] = candidateMetrics; row[k("metric_deltas")] = deltas(baselineMetrics, candidateMetrics);
        row[k("event_admission_changes")] = changedDecisions;
        row[k("mechanism_contract")]=candidateMechanism;row[k("mechanism_admission_changed")]=baselineMechanism.value(k("decisions"))!=candidateMechanism.value(k("decisions"));
        row[k("critical_unknowns")]=QJsonArray::fromStringList(candidateCritical);
        row[k("events_accepted")] = replay.value(k("events_accepted")); row[k("events_rejected")] = replay.value(k("events_rejected"));
        row[k("model_unknowns")] = replay.value(k("model_unknowns")); row[k("ranking_eligible")] = score.has_value() && baselineScore.has_value();
        row[k("score")] = score ? QJsonValue(*score) : QJsonValue(QJsonValue::Null);
        row[k("score_gain")] = score && baselineScore ? QJsonValue(*score - *baselineScore) : QJsonValue(QJsonValue::Null);
        if (score && baselineScore) ordered.append({*score, label});
        rows.append(row);
    }
    auto unresolved3116=[&](const QJsonObject &focus){for(const auto &u:focus.value(k("unknowns")).toArray())if(u.toString().startsWith(k("3116_")))return true;return false;};
    bool unresolved=unresolved3116(baselineFocus);
    for(const auto &raw:rows)for(const auto &u:raw.toObject().value(k("metrics")).toObject().value(k("unknowns")).toArray())if(u.toString().startsWith(k("3116_")))unresolved=true;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    for (const auto &entry : ordered) ranking.append(entry.second);
    const auto knownRanking=ranking;
    criticalUnknowns.removeDuplicates();criticalUnknowns.sort();const bool gated=unresolved||!completeComparison||!criticalUnknowns.isEmpty()||!baselineMechanism.value(k("complete")).toBool();
    if(gated){ranking=QJsonArray{};for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();row[k("known_part_comparable")]=row.value(k("ranking_eligible"));row[k("ranking_eligible")]=false;row[k("score_scope")]=k("known_components_only_no_selection_with_unresolved_inputs_or_incomplete_admission");rows[i]=row;}}
    return {{k("available"), true}, {k("schema_version"), 1}, {k("status"), k("conditional_chain_equipment_comparison")},
        {k("patch"), db.patch}, {k("combat_revision"), db.revision}, {k("focus_player_key"), focusKey},
        {k("scenario_sha256"), digest(scenario)}, {k("same_explicit_event_sequence"), true}, {k("automatic_actions"), false},
        {k("baseline_items"), itemJson(baselineItems)}, {k("baseline_metrics"), baselineMetrics}, {k("baseline_initial_states"),initialStates},{k("objective_basis"), basis},
        {k("baseline_score"), baselineScore ? QJsonValue(*baselineScore) : QJsonValue(QJsonValue::Null)},
        {k("baseline_schedule_fully_admitted"), baselineAdmitted}, {k("baseline_event_admissions"), baselineAdmission},
        {k("baseline_mechanism_contract"),baselineMechanism},{k("critical_unknowns"),QJsonArray::fromStringList(criticalUnknowns)},{k("complete_candidate_comparison"),completeComparison},
        {k("critical_input_scope"),k("explicit_referenced_actions_and_effective_stats_mechanisms_not_unused_skill_formula_count")},
        {k("baseline_unknowns"), baseline.value(k("model_unknowns"))}, {k("candidates"), rows},
        {k("conditional_ranking"), ranking}, {k("known_part_ranking"),knownRanking},{k("ranking_available"), !gated&&!ordered.isEmpty()},
        {k("selection_available"),!gated&&!ordered.isEmpty()},{k("all_effects_complete"),false},
        {k("unresolved_3116_can_change_relative_order"),unresolved},{k("ranking_scope"),gated?k("known_parts_only_no_selected_optimum"):k("explicit_conditional_scenario")},
        {k("initial_health_policy"), focusInput.value(k("is_dead"))==QJsonValue(true)?k("explicit_dead_zero_positive_health_conflict_rejected"):focusInput.contains(k("current_health")) ? k("fixed_absolute_current_health") : k("full_health_at_each_loadout_maximum_explicit_scenario")},
        {k("maximum_inventory_cost"), budgetSupplied ? request.value(k("maximum_inventory_cost")) : QJsonValue(QJsonValue::Null)},
        {k("inventory_cost_limit_checked"), budgetSupplied},
        {k("inventory_cost_limit_scope"), k("gross_selected_item_price_total_no_owned_credit_or_sale")},
        {k("purchase_transaction_or_budget_verified"), false}, {k("globally_optimal_build"), false},
        {k("live_observed_result"), false}, {k("actual_win_probability"), QJsonValue::Null}, {k("network_requests"), 0}, {k("paid_ai_calls"), 0}};
}
