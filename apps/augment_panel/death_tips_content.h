#pragma once
#include "analysis.h"
#include "purchase_planner.h"
#include <QJsonArray>
#include <QStringList>
#include <algorithm>
#include <cmath>

inline bool deathPurchaseFitsCatalogueBudget(const Catalog &catalog, const GameState &game, const QJsonObject &row) {
    const auto item = row.value("item_id"), cost = row.value("remaining_cost");
    const int id = item.toInt();
    if (id <= 0 || item != QJsonValue(id) || row.value("affordable") != QJsonValue(true)
        || !cost.isDouble() || !std::isfinite(cost.toDouble()) || cost.toDouble() <= 0 || cost.toDouble() > game.gold) return false;
    const auto quote = planInventoryPurchase(catalog, game.championId, game.level, game.owned, id);
    return quote.value("legal") == QJsonValue(true) && quote.value("remaining_cost") == cost;
}

// Presentation of existing observations and bounded models only. This never
// searches for a purchase, infers a hidden augment, or predicts a skill hit.
inline QJsonArray deathTipsForState(const Catalog &catalog, const GameState &game,
                                    const QJsonObject &coach, const QJsonObject &liveSummary,
                                    const QJsonObject &equipmentCue) {
    QJsonArray out;
    if (game.source != GameState::Source::Live || !game.fresh() || game.gameMode != QStringLiteral("KIWI")
        || !modelRulesUsable(catalog, game) || (game.phase != QStringLiteral("InProgress") && game.phase != QStringLiteral("Reconnect"))) return out;
    QJsonObject self; int selves = 0;
    for (const auto &v : game.players) if (v.toObject().value("self").toBool()) { self = v.toObject(); ++selves; }
    const auto own = self.value("team").toString();
    if (selves != 1 || self.value("champion_id").toInt() != game.championId || !self.value("is_dead").isBool()
        || !self.value("is_dead").toBool() || (own != QStringLiteral("ORDER") && own != QStringLiteral("CHAOS"))) return out;
    const auto add = [&](const QString &id, const QString &title, const QString &text, const char *source) {
        if (out.size() < 6 && !text.isEmpty()) out.append(QJsonObject{{"tip_id", id}, {"title", title},
            {"text", text.left(90)}, {"source", QString::fromLatin1(source)}, {"conditional", true},{"expanded",true}});
    };
    const auto visualize=[&](const QString &id,const QJsonObject &visual){
        for(int i=0;i<out.size();++i){auto tip=out.at(i).toObject();if(tip.value("tip_id")==id){tip["visual"]=visual;out[i]=tip;return;}}
    };
    const auto modelTime = coach.value("model_game_time");
    const bool coachCurrent = modelTime.isDouble() && std::isfinite(modelTime.toDouble())
        && game.gameTime >= modelTime.toDouble() && game.gameTime - modelTime.toDouble() <= 8;
    const auto quality = coach.value("recommendation_quality").toObject();
    const auto actions = quality.value("candidate_actions").toObject();
    const auto observation = quality.value("augment_observation").toObject();
    const auto context = game.matchContext.value("context_id").toString();
    bool purchaseShown = false;
    if (coachCurrent && quality.value("ranking_available").toBool() && quality.value("primary_purchase_visible").toBool()
        && !quality.value("candidate_only").toBool(true) && observation.value("complete").toBool()
        && self.value("augments_complete").isBool() && self.value("augments_complete").toBool()
        && self.value("augment_ids").isArray() && self.value("augment_ids") == observation.value("observed_ids")
        && observation.value("model_selection_matches_observation").toBool() && actions.value("budget_known").toBool()
        && !context.isEmpty() && game.matchContext.value("active") == QJsonValue(true)
        && self.value("items_complete") == QJsonValue(true) && self.value("items").isArray() && self.value("level") == QJsonValue(game.level)
        && game.goldKnown && std::isfinite(game.gold) && game.gold >= 0) {
        QList<int> observed; bool valid = true;
        for (const auto &item : self.value("items").toArray()) {
            const int id = item.toInt();
            if (id <= 0 || item != QJsonValue(id)) { valid = false; break; }
            observed.append(id);
        }
        auto owned = game.owned;
        std::sort(observed.begin(), observed.end()); std::sort(owned.begin(), owned.end());
        if (valid && observed == owned) for (const auto &v : actions.value("available_now").toArray()) {
            const auto row = v.toObject(); const auto cost = row.value("remaining_cost"); const auto item = row.value("item_id");
            if (!deathPurchaseFitsCatalogueBudget(catalog, game, row)) continue;
            add(QStringLiteral("purchase_candidate"), QStringLiteral("复活前看商店"),
                QStringLiteral("%1的配方估价在当前预算内，实际价格请核对商店。收益限于已建模条件。" ).arg(catalog.itemName(item.toInt())),
                "current_quality_gated_budget_candidate");
            visualize(QStringLiteral("purchase_candidate"),{{"kind","budget"},{"budget",game.gold},{"cost",cost},{"item",catalog.itemName(item.toInt())},{"caption",QStringLiteral("当前金币 / 配方估价")},{"source","observed_gold_and_quality_gated_candidate"}});
            purchaseShown = true; break;
        }
    }
    if (!purchaseShown) add(QStringLiteral("purchase_boundary"), QStringLiteral("出装依据"),
        coachCurrent && !quality.value("summary").toString().isEmpty() ? quality.value("summary").toString()
            : QStringLiteral("当前出装依据未齐；先核对金币、强化和商店，再比较候选。"), "current_quality_boundary");
    if(!purchaseShown)visualize(QStringLiteral("purchase_boundary"),{{"kind","steps"},{"nodes",QJsonArray{QStringLiteral("金币"),QStringLiteral("强化"),QStringLiteral("商店")}},{"caption",QStringLiteral("复活前核对")},{"source","verification_workflow_not_numeric_evidence"}});

    const auto reference = coach.value("hero_execution_reference").toObject();
    const auto referenceTime = reference.value("model_game_time");
    const auto referenceChampion = reference.value("champion_id");
    const auto referenceSequence = reference.value("reference_sequence").toString();
    const auto sequenceParts = referenceSequence.split(QStringLiteral(" → "), Qt::KeepEmptyParts);
    bool sequenceValid = !referenceSequence.isEmpty() && referenceSequence.size() <= 160
        && !sequenceParts.isEmpty() && sequenceParts.size() <= 24;
    for (const auto &part : sequenceParts)
        sequenceValid &= QStringList{QStringLiteral("AA"), QStringLiteral("Q"), QStringLiteral("W"), QStringLiteral("E"), QStringLiteral("R")}.contains(part);
    const auto referenceAssumptions = reference.value("assumptions").toArray();
    if (coachCurrent && !context.isEmpty() && game.matchContext.value("active").toBool()
        && reference.value("context_id").toString() == context && reference.value("patch").toString() == catalog.patch
        && referenceChampion.isDouble() && referenceChampion.toDouble() == referenceChampion.toInt()
        && referenceChampion.toInt() == game.championId && referenceTime.isDouble()
        && std::isfinite(referenceTime.toDouble()) && game.gameTime >= referenceTime.toDouble()
        && game.gameTime - referenceTime.toDouble() <= 8
        && reference.value("synthetic_reference") == QJsonValue(true)
        && reference.value("source_state_mutated") == QJsonValue(false)
        && reference.value("current_state_execution") == QJsonValue(false)
        && reference.value("current_damage_ranking_available") == QJsonValue(false)
        && reference.value("reference_damage_ranking_available") == QJsonValue(true)
        && reference.value("complete") == QJsonValue(true)
        && reference.value("execution_status") == QStringLiteral("executed_reference") && sequenceValid
        && referenceAssumptions.contains(QStringLiteral("learned_skill_remaining_cooldowns_explicitly_zero"))
        && referenceAssumptions.contains(QStringLiteral("actor_resource_explicitly_full_source_known_maximum_not_live_observation"))) {
        auto shortSequence = sequenceParts.mid(0, 8).join(QStringLiteral(" → "));
        if (sequenceParts.size() > 8) shortSequence += QStringLiteral("…");
        add(QStringLiteral("hero_execution_reference"), QStringLiteral("复活后连招参考"),
            QStringLiteral("%1。仅技能就绪、资源充足且命中时；护控收益未齐。").arg(shortSequence),
            "bounded_continuous_reference_ready_skills_full_resource_conditional_hits");
        QJsonArray nodes;for(const auto &part:sequenceParts.mid(0,5))nodes.append(part);
        visualize(QStringLiteral("hero_execution_reference"),{{"kind","sequence"},{"nodes",nodes},{"caption",QStringLiteral("就绪 / 满资源假设")},{"source","bounded_reference_sequence"}});
    }
    const auto summaryTime = liveSummary.value("game_time");
    const bool summaryCurrent = !context.isEmpty() && game.matchContext.value("active").toBool()
        && liveSummary.value("fresh").toBool() && liveSummary.value("current_context").toBool()
        && liveSummary.value("context_id").toString() == context && summaryTime.isDouble()
        && std::isfinite(summaryTime.toDouble()) && game.gameTime >= summaryTime.toDouble()
        && game.gameTime - summaryTime.toDouble() <= 4;
    QJsonArray rows; QSet<int> seenIndices;
    if (summaryCurrent) for (const auto &v : liveSummary.value("players").toArray()) {
        const auto row = v.toObject(); const auto index = row.value("row_index");
        if (!index.isDouble() || index.toDouble() != index.toInt() || index.toInt() < 0 || index.toInt() >= game.players.size()) continue;
        if (seenIndices.contains(index.toInt())) { rows = {}; break; } seenIndices.insert(index.toInt());
        const auto observed = game.players.at(index.toInt()).toObject();
        if (observed.value("team") != QStringLiteral("ORDER") && observed.value("team") != QStringLiteral("CHAOS")) continue;
        const auto side = observed.value("team").toString() == own ? QStringLiteral("ALLY") : QStringLiteral("ENEMY");
        if (row.value("champion_id") == observed.value("champion_id") && row.value("team").toString() == side
            && catalog.champions.contains(row.value("champion_id").toInt())) rows.append(row);
    }
    const auto leader = [&](const char *field) {
        QJsonObject best; double highest = -1; int count = 0;
        for (const auto &v : rows) { const auto row = v.toObject(); const auto n = row.value(QString::fromLatin1(field));
            if (row.value("team") != QStringLiteral("ENEMY")) continue;
            if (!row.value("model_available").toBool() || !row.value("items_complete").toBool()
                || !n.isDouble() || !std::isfinite(n.toDouble()) || n.toDouble() < 0) return QJsonObject{};
            ++count; if (n.toDouble() > highest) { highest = n.toDouble(); best = row; }
        } return count == 5 ? best : QJsonObject{};
    };
    const auto damage = leader("output_8s_estimate"), tank = leader("effective_health_estimate");
    if (!damage.isEmpty()) add(QStringLiteral("enemy_output"), QStringLiteral("敌方输出线索"),
        QStringLiteral("8秒条件输出模型较高：%1。不等于实战贡献或集火指令。" ).arg(catalog.championName(damage.value("champion_id").toInt())), "current_live_combat_summary_known_model");
    if (!tank.isEmpty()) add(QStringLiteral("enemy_durability"), QStringLiteral("敌方承伤线索"),
        QStringLiteral("条件承伤模型较高：%1。实际技能、站位和触发仍需确认。" ).arg(catalog.championName(tank.value("champion_id").toInt())), "current_live_combat_summary_known_model");
    const auto modelBars=[&](const char *field,const QString &caption){
        QList<QJsonObject> enemies;for(const auto &v:rows)if(v.toObject().value("team")==QStringLiteral("ENEMY"))enemies.append(v.toObject());
        const QString key=QString::fromLatin1(field);std::sort(enemies.begin(),enemies.end(),[&](const auto &a,const auto &b){return a.value(key).toDouble()>b.value(key).toDouble();});
        QJsonArray bars;for(int i=0;i<std::min(3,int(enemies.size()));++i)bars.append(QJsonObject{{"label",catalog.championName(enemies[i].value("champion_id").toInt())},{"champion_id",enemies[i].value("champion_id")},{"value",enemies[i].value(key)}});
        return QJsonObject{{"kind","bars"},{"bars",bars},{"caption",caption},{"source","current_conditional_combat_model_not_observed_damage"}};
    };
    if(!damage.isEmpty())visualize(QStringLiteral("enemy_output"),modelBars("output_8s_estimate",QStringLiteral("8秒条件模型 · 非实战统计")));
    if(!tank.isEmpty())visualize(QStringLiteral("enemy_durability"),modelBars("effective_health_estimate",QStringLiteral("条件承伤模型 · 非实战统计")));
    const auto abilityTip = [&](const QString &side, const QString &axis) {
        for (const auto &v : rows) { const auto row = v.toObject();
            if (row.value("team") != side || (side == QStringLiteral("ALLY") && row.value("self").toBool())) continue;
            const auto kit = row.value("tactical_role").toObject().value("capabilities").toObject();
            if (!kit.value("reviewed").toBool() || !kit.value(axis).toBool()) continue;
            QStringList abilitySlots;
            for (const auto &e : kit.value("ability_evidence").toArray()) { const auto evidence = e.toObject(); const auto slot = evidence.value("slot").toString();
                if (evidence.value("axis") == axis && QStringList{"Q", "W", "E", "R"}.contains(slot) && !abilitySlots.contains(slot)) abilitySlots.append(slot); }
            if (abilitySlots.isEmpty()) continue;
            const auto name = catalog.championName(row.value("champion_id").toInt()) + QStringLiteral(" ") + abilitySlots.join('/');
            if (axis == QStringLiteral("engage")) add(QStringLiteral("enemy_engage"), QStringLiteral("留意开团技能"),
                QStringLiteral("%1 具备开团能力；冷却、命中和可达性未核实。" ).arg(name), "reviewed_current_roster_ability_capability");
            else add(QStringLiteral("ally_support"), QStringLiteral("复活后协同"),
                QStringLiteral("队友%1 具备保护或增益；配合前确认存活、冷却与距离。" ).arg(name), "reviewed_current_roster_ability_capability");
            break;
        }
    };
    abilityTip(QStringLiteral("ENEMY"), QStringLiteral("engage")); abilityTip(QStringLiteral("ALLY"), QStringLiteral("support"));
    const auto needs = coach.value("team_needs").toObject(); const auto axes = needs.value("axes").toArray();
    if (coachCurrent && needs.value("available").toBool() && !axes.isEmpty()) {
        const auto axis = axes.first().toObject(); const auto need = axis.value("need"); const auto label = axis.value("axis").toString();
        if (need.isDouble() && std::isfinite(need.toDouble()) && need.toDouble() > 0 && need.toDouble() <= 1
            && QStringList{QStringLiteral("普攻输出"), QStringLiteral("技能输出"), QStringLiteral("前排承伤"), QStringLiteral("续航支援")}.contains(label))
            add(QStringLiteral("team_need"), QStringLiteral("团队配合线索"), QStringLiteral("阵容定位粗估偏弱：%1。仅供分工参考，不代表必须改装。" ).arg(label), "current_team_needs_role_heuristic");
    }
    const auto latest = equipmentCue.value("latest").toObject(); const auto expiry = latest.value("expires_at_ms");
    if (summaryCurrent && equipmentCue.value("available").toBool() && equipmentCue.value("context_id").toString() == context
        && latest.value("context_id").toString() == context && latest.value("kind") == QStringLiteral("enemy_item_added")
        && latest.value("source") == QStringLiteral("live_client_inventory_difference") && expiry.isDouble()
        && expiry.toDouble() > QDateTime::currentMSecsSinceEpoch() && catalog.items.contains(latest.value("item_id").toInt())) {
        bool enemy = false; for (const auto &v : rows) { const auto row = v.toObject(); enemy |= row.value("team") == QStringLiteral("ENEMY") && row.value("champion_id") == latest.value("champion_id"); }
        if (enemy) {
            if(out.size()>=6)out.removeLast();
            add(QStringLiteral("enemy_equipment"), QStringLiteral("敌方装备变化"),
                QStringLiteral("观测到%1 新增%2；先确认装备效果与当前战况。" ).arg(catalog.championName(latest.value("champion_id").toInt()), catalog.itemName(latest.value("item_id").toInt())), "fresh_enemy_inventory_difference");
        }
    }
    return out;
}
