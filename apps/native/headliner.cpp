#include "headliner.h"
#include "analysis.h"

QJsonObject observedHeadliner(const GameState &s) {
    if(s.gameMode!=QStringLiteral("KIWI")||!s.fresh())return {};
    auto value=sanitizeHeadlinerContext(s.headlinerContext,s.source==GameState::Source::Replay);
    if(value.isEmpty()||value.value(QStringLiteral("game_time")).toDouble()>s.gameTime+5)return {};
    if(s.source==GameState::Source::Live&&s.observedPatch!=value.value(QStringLiteral("patch")).toString())return {};
    return value;
}
QList<int> effectiveSelectedAugments(const Catalog &c,const GameState &s) {
    auto selected=s.selected;const auto context=observedHeadliner(s);
    if(c.patch==QStringLiteral("16.21")&&context.value(QStringLiteral("rule_key"))==QStringLiteral("Pentakill"))
        for(const auto &v:context.value(QStringLiteral("facts")).toObject().value(QStringLiteral("pentakill_reward_ids")).toArray())
            if(c.rewardOnlyAugment(v.toInt())&&!selected.contains(v.toInt()))selected.append(v.toInt());
    return selected;
}
QStringList headlinerComparisonUnknowns(const GameState &s,int candidateId) {
    const auto context=observedHeadliner(s);const auto key=context.value(QStringLiteral("rule_key")).toString();
    const auto facts=context.value(QStringLiteral("facts")).toObject();
    if(key==QStringLiteral("Heartsteel")){
        const auto ids=facts.value(QStringLiteral("heartsteel_modified_augment_ids"));
        if(!ids.isArray()||ids.toArray().contains(candidateId))return {QStringLiteral("headliner:Heartsteel:modifier_unobserved")};
    }
    return {};
}
QJsonObject evaluateHeadliner(const Catalog &c,const GameState &s) {
    const auto context=observedHeadliner(s);const auto key=context.value(QStringLiteral("rule_key")).toString();
    QJsonObject out{{QStringLiteral("available"),false},{QStringLiteral("patch"),c.patch},{QStringLiteral("actual_offer_pool_known"),false},
        {QStringLiteral("server_enabled_verified"),false},{QStringLiteral("long_term_progression_used"),false},
        {QStringLiteral("reward_augment_ids"),QJsonArray{}},{QStringLiteral("extra_augment_slots"),QJsonValue::Null}};
    if(c.patch!=QStringLiteral("16.21")||!modelRulesUsable(c,s)||context.isEmpty()){
        out[QStringLiteral("reason")]=QStringLiteral("headliner_context_unobserved");return out;
    }
    const auto facts=context.value(QStringLiteral("facts")).toObject();QStringList missing;QJsonObject rule;
    if(key==QStringLiteral("KDA")){
        rule={{QStringLiteral("kind"),QStringLiteral("collect_cards_extra_augment")},{QStringLiteral("official_threshold"),4}};
        if(facts.contains(QStringLiteral("kda_cards_collected")))rule[QStringLiteral("threshold_observed")]=facts.value(QStringLiteral("kda_cards_collected")).toInt()>=4;
        else missing.append(QStringLiteral("kda_cards_collected"));
        if(facts.contains(QStringLiteral("extra_augment_slots_observed")))out[QStringLiteral("extra_augment_slots")]=facts.value(QStringLiteral("extra_augment_slots_observed"));
        else missing.append(QStringLiteral("extra_augment_slots_observed"));
    }else if(key==QStringLiteral("Heartsteel")){
        rule[QStringLiteral("kind")]=QStringLiteral("augment_specific_modifiers");missing.append(QStringLiteral("modifier_coefficients_and_triggers"));
    }else if(key==QStringLiteral("TrueDamage")){
        rule[QStringLiteral("kind")]=QStringLiteral("team_anvil_drops");if(!facts.contains(QStringLiteral("anvils_dropped")))missing.append(QStringLiteral("anvils_dropped"));missing.append(QStringLiteral("drop_probability_and_rewards"));
    }else if(key==QStringLiteral("Pentakill")){
        rule={{QStringLiteral("kind"),QStringLiteral("ability_upgrade_rewards")},{QStringLiteral("official_reward_phases"),QJsonArray{QStringLiteral("game_start"),QStringLiteral("level_18")}}};
        // Level 18 alone never grants an unobserved reward or ability binding.
        out[QStringLiteral("reward_augment_ids")]=facts.value(QStringLiteral("pentakill_reward_ids")).toArray();
        if(!facts.contains(QStringLiteral("pentakill_reward_ids")))missing.append(QStringLiteral("pentakill_reward_ids"));
        if(!facts.contains(QStringLiteral("pentakill_upgraded_slots")))missing.append(QStringLiteral("pentakill_upgraded_slots"));
    }else if(key==QStringLiteral("Disco")){
        rule={{QStringLiteral("kind"),QStringLiteral("dance_gold")},{QStringLiteral("wallet_adjustment"),0}};
        if(!facts.contains(QStringLiteral("disco_gold_awarded")))missing.append(QStringLiteral("disco_gold_awarded"));
    }else if(key==QStringLiteral("CafeCuties")){
        rule[QStringLiteral("kind")]=QStringLiteral("empowered_health_relics");if(!facts.contains(QStringLiteral("relics_collected")))missing.append(QStringLiteral("relics_collected"));missing.append(QStringLiteral("relic_effect_observation"));
    }else if(key==QStringLiteral("Bees")){
        rule[QStringLiteral("kind")]=QStringLiteral("honey_and_bee_hazards");if(!facts.contains(QStringLiteral("honey_pots_collected")))missing.append(QStringLiteral("honey_pots_collected"));missing.append(QStringLiteral("honey_reward_and_bee_hit_observation"));
    }else if(key==QStringLiteral("CommandLine")){
        rule={{QStringLiteral("kind"),QStringLiteral("higher_tier_reroll")},{QStringLiteral("rarity_weights_known"),false}};
        if(!facts.contains(QStringLiteral("command_line_upgraded_ids")))missing.append(QStringLiteral("command_line_upgraded_ids"));missing.append(QStringLiteral("eligible_pool_and_rarity_weights"));
    }
    out[QStringLiteral("available")]=true;out[QStringLiteral("rule_key")]=key;out[QStringLiteral("facts")]=facts;
    out[QStringLiteral("source")]=context.value(QStringLiteral("source"));out[QStringLiteral("observed_game_time")]=context.value(QStringLiteral("game_time"));
    out[QStringLiteral("rule")]=rule;out[QStringLiteral("unknowns")]=QJsonArray::fromStringList(missing);
    out[QStringLiteral("numeric_effects_inferred")]=false;return out;
}
