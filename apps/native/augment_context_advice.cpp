#include "augment_context_advice.h"
#include "analysis.h"
#include "coach_recommendation_quality.h"
#include "combat.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {
QString k(const char *value) { return QString::fromLatin1(value); }
QString identity(int id) {
    switch (id) {
    case 3049: return k("ArmoredImpact");
    case 1029: return k("ARAM_EtherealWeapon");
    case 2087: return k("ARAM_Archmage");
    case 3081: return k("WeaponOfDestiny");
    case 2126: return k("KeepGoing");
    case 2148: return k("DoubleStrike");
    case 3059: return k("NoManaNoProblem");
    case 3163: return k("EnchantingButterflies");
    default: return {};
    }
}
bool trustedCard(const Catalog &catalog, int id) {
    const auto expected = identity(id);
    const auto card = catalog.augments.value(id);
    static const QRegularExpression checksum(k("^[0-9a-fA-F]{64}$"));
    const auto text = plainText(card.value(k("description")).toString())
        + QLatin1Char('\n') + plainText(card.value(k("description_template_html")).toString());
    const bool mechanism = id == 3049 ? text.contains(QStringLiteral("施放技能"))
            && text.contains(QStringLiteral("下一次普通攻击")) && text.contains(QStringLiteral("物理伤害"))
        : id == 1029 ? text.contains(QStringLiteral("技能"))
            && (text.contains(k("Item_Keyword_OnHit")) || text.contains(QStringLiteral("攻击特效")))
        : id == 2087 ? text.contains(QStringLiteral("施放一个技能"))
            && text.contains(QStringLiteral("另一个随机技能")) && text.contains(QStringLiteral("冷却时间"))
        : id == 3081 ? text.contains(QStringLiteral("第一件")) && text.contains(QStringLiteral("传说"))
            && (text.contains(QStringLiteral("无法售出")) || text.contains(QStringLiteral("无法出售")))
        : id == 2126 ? text.contains(k("SpellName")) && text.contains(QStringLiteral("冷却"))
            && (text.contains(k("Item_Keyword_OnHit")) || text.contains(QStringLiteral("攻击特效")))
        : id == 2148 ? text.contains(k("SpellName")) && text.contains(QStringLiteral("额外"))
            && (text.contains(k("Item_Keyword_OnHit")) || text.contains(QStringLiteral("攻击特效")))
        : id == 3059 ? text.contains(QStringLiteral("装备")) && text.contains(QStringLiteral("法力值"))
            && text.contains(QStringLiteral("法术强度")) && text.contains(QStringLiteral("转"))
        : id == 3163 ? text.contains(QStringLiteral("友方英雄")) && text.contains(QStringLiteral("治疗"))
            && text.contains(QStringLiteral("护盾")) && text.contains(QStringLiteral("敌方英雄"))
            && text.contains(QStringLiteral("蝴蝶")) : false;
    return mechanism && !expected.isEmpty() && catalog.ordinaryOfferEligible(id)
        && card.value(k("key")) == expected && card.value(k("patch")) == catalog.patch
        && card.value(k("description_patch")) == catalog.patch
        && checksum.match(card.value(k("source_sha256")).toString()).hasMatch();
}
bool verifiedInventory(const Catalog &catalog, const GameState &state) {
    if (state.owned.size() > 6) return false;
    QJsonObject self; int count = 0;
    for (const auto &value : state.players) if (value.toObject().value(k("self")) == QJsonValue(true)) {
        self = value.toObject(); ++count;
    }
    if (count != 1 || self.value(k("champion_id")).toInt() != state.championId
        || self.value(k("items_complete")) != QJsonValue(true) || !self.value(k("items")).isArray()) return false;
    QList<int> observed;
    for (const auto &value : self.value(k("items")).toArray()) {
        if (!value.isDouble() || value.toDouble() != value.toInt() || value.toInt() <= 0) return false;
        observed.append(value.toInt());
    }
    for (int id : state.owned) if (!catalog.items.contains(id)) return false;
    auto owned = state.owned;
    std::sort(owned.begin(), owned.end()); std::sort(observed.begin(), observed.end());
    return owned == observed;
}
QJsonObject condition(const char *id, const char *status, const QString &reason, const char *scope = "required_execution") {
    return {{k("id"), k(id)}, {k("status"), k(status)}, {k("reason"), reason}, {k("scope"), k(scope)}};
}
QJsonObject communitySource(bool destiny = false) {
    return {{k("id"), destiny ? k("cn-08") : k("cn-01")},
        {k("url"), destiny ? k("https://aramgg.com/zh-CN/blog/weapon-of-destiny-first-item-guide")
                              : k("https://aramgg.com/zh-CN/blog/armored-impact-item-build-guide")},
        {k("kind"), k("community_guide")}, {k("published_at"), destiny ? k("2026-10-26") : k("2026-10-25")},
        {k("claimed_patch"), k("26.21")}, {k("numeric_rules_imported"), false},
        {k("authority"), k("reviewed_conditional_hypothesis_not_mechanic_authority")}};
}
QJsonObject championSynergySource(const char *id, const char *url, const char *kind) {
    return {{k("id"), k(id)}, {k("url"), k(url)}, {k("kind"), k(kind)},
        {k("numeric_rules_imported"), false}};
}
struct ArmorSupport {
    bool known = false, formula = false, fit = false;
    double armor = 0;
};
ArmorSupport armorSupport(const Catalog &catalog, const GameState &state, bool inventory) {
    ArmorSupport out;
    if (!inventory || !catalog.combat || catalog.combat->patch != catalog.patch
        || !catalog.combat->augments.contains(3049)) return out;
    for (int id : state.owned) {
        if (!catalog.combat->items.contains(id)) return out;
        const double armor = catalog.combat->items.value(id).stats[Stat::Armor];
        if (!std::isfinite(armor)) return out;
        out.armor += armor;
    }
    if (!std::isfinite(out.armor) || out.armor < 0) return out;
    const auto formula = catalog.combat->augments.value(3049).calculations.value(k("ArmorDamage")).toObject();
    bool armorDependency = false, bounded = true; int nodes = 0;
    std::function<void(const QJsonValue &, int)> walk = [&](const QJsonValue &value, int depth) {
        if (++nodes > 2048 || depth > 24) { bounded = false; return; }
        if (value.isArray()) { for (const auto &entry : value.toArray()) if (bounded) walk(entry, depth + 1); return; }
        if (!value.isObject()) return;
        const auto object = value.toObject();
        if (object.value(k("op")) == k("stat") && object.value(k("part")).toInt() != 1
            && combatStat(object.value(k("stat")).toString()) == Stat::Armor) armorDependency = true;
        for (auto it = object.constBegin(); it != object.constEnd() && bounded; ++it) walk(it.value(), depth + 1);
    };
    walk(formula, 0);
    if (!bounded || !armorDependency || formula.isEmpty()) return out;
    CombatContext context; context.level = std::clamp(state.level, 1, 18);
    const auto compiled = compileCombatFormula(formula, context);
    CombatStats before, probe;
    // Establish a positive source-formula response even for an empty backpack;
    // this is not a forecast of actual casts, procs or mitigated damage.
    probe[Stat::Armor] = out.armor > 0 ? out.armor : 1.;
    const auto a = compiled.evaluate(context, before), b = compiled.evaluate(context, probe);
    if (!a || !b || !std::isfinite(*a) || !std::isfinite(*b) || !(*b > *a)) return out;
    out.known = true; out.formula = true; out.fit = out.armor > 0;
    return out;
}
}

QString augmentContextAdviceVersion() { return k("reviewed_context_conditions_v3_20261104"); }

QJsonObject buildAugmentContextAdvice(const Catalog &catalog, const GameState &state, int offeredId) {
    QJsonObject out{{k("available"), false}, {k("version"), augmentContextAdviceVersion()},
        {k("numeric_ranking"), false}, {k("ranking_available"), false}, {k("reference_only"), true},
        {k("unknown_is_zero"), false}, {k("verified_inventory_formula_fit"), false}};
    auto reject = [&](const char *why) { out[k("reason_code")] = k(why); return out; };
    if (!state.fresh() || !modelRulesUsable(catalog, state) || state.gameMode != k("KIWI")
        || (state.queueId > 0 && state.queueId != 2400) || !catalog.champions.contains(state.championId))
        return reject("fresh_same_patch_kiwi_required");
    if (state.offers.size() != 3 || !state.offers.contains(offeredId)) return reject("three_current_offers_required");
    QSet<int> unique;
    for (int id : state.offers) {
        if (unique.contains(id) || !catalog.ordinaryOfferEligible(id)) return reject("three_unique_eligible_offers_required");
        unique.insert(id);
    }
    if (!trustedCard(catalog, offeredId)) return reject("reviewed_identity_and_description_receipt_required");

    const auto champion = catalog.champion(state.championId);
    const bool currentChampion = catalog.patch == k("16.21") && champion.value(k("patch")) == catalog.patch;
    const auto championKey = champion.value(k("key")).toString();
    const bool asheArchmage = currentChampion && state.championId == 22 && championKey == k("Ashe") && offeredId == 2087;
    const bool ireliaArmored = currentChampion && state.championId == 39 && championKey == k("Irelia") && offeredId == 3049;
    const bool sorakaButterflies = currentChampion && state.championId == 16 && championKey == k("Soraka") && offeredId == 3163;
    if ((offeredId == 2087 && !asheArchmage) || (offeredId == 3163 && !sorakaButterflies))
        return reject("reviewed_champion_synergy_required");

    const bool inventory = verifiedInventory(catalog, state);
    const auto readiness = coachPurchaseInputReadiness(catalog, state);
    const auto observation = readiness.value(k("augment_observation")).toObject();
    const auto status = observation.value(k("status")).toString();
    const bool conflict = observation.value(k("complete")).toBool()
        && !observation.value(k("model_selection_matches_observation")).toBool();
    const bool selectedValid = !conflict && observation.value(k("self_identity_verified")).toBool()
        && (status == k("complete") || status == k("partial") || status == k("unknown"));
    const bool complete = selectedValid && readiness.value(k("ready")).toBool();
    const QJsonArray observed = selectedValid ? observation.value(k("observed_ids")).toArray() : QJsonArray{};
    const auto presence = [&](int id) {
        if (!trustedCard(catalog, id)) return k("unknown");
        if (selectedValid && observed.contains(id)) return k("met");
        return complete ? k("unmet") : k("unknown");
    };
    QJsonArray facts, requirements, tradeoffs, counterexamples;
    if (inventory) facts.append(QJsonObject{{k("id"), k("inventory_multiset_verified")}, {k("reason"), QStringLiteral("本人背包及重复组件已核对")}});
    out[k("available")] = true; out[k("augment_id")] = offeredId; out[k("inventory_verified")] = inventory;
    out[k("observed_selected_ids")] = observed; out[k("selected_list_complete")] = complete;
    out[k("selection_observation_status")] = conflict ? k("conflict") : status;
    out[k("selection_source_valid")] = selectedValid;
    out[k("mechanic_source")] = QJsonObject{{k("kind"), k("pinned_catalog_identity_description_and_formula")},
        {k("patch"), catalog.patch}, {k("source_sha256"), catalog.augments.value(offeredId).value(k("source_sha256"))}};
    if (offeredId == 3049) {
        const auto support = armorSupport(catalog, state, inventory);
        out[k("verified_inventory_formula_fit")] = support.fit;
        out[k("inventory_armor")] = support.known ? QJsonValue(support.armor) : QJsonValue(QJsonValue::Null);
        out[k("source_formula_inventory_armor_supported")] = support.known ? QJsonValue(support.formula) : QJsonValue(QJsonValue::Null);
        requirements.append(condition("current_equipment_armor_support", !support.known ? "unknown" : support.fit ? "met" : "unmet",
            !support.known ? QStringLiteral("背包或原护甲公式尚未核对，装备联动保持未知")
                : support.fit ? QStringLiteral("现有装备提供护甲，原卡公式随该属性提高；不是实际触发伤害")
                              : QStringLiteral("已核对当前装备没有护甲；后续转型收益仍待判断"), "current_build_fit"));
        if (support.fit) facts.append(QJsonObject{{k("id"), k("verified_item_armor_formula_link")}, {k("armor_from_items"), support.armor},
            {k("reason"), QStringLiteral("已核对的装备静态护甲与原卡公式正向联动")}});
        requirements.append(condition("cast_then_attack_delivery", "unknown", QStringLiteral("需施法后接普攻；能否接触目标、实际触发次数未观测")));
        const auto ethereal = presence(1029);
        requirements.append(condition("ethereal_weapon_observation", ethereal.toLatin1().constData(),
            ethereal == k("met") ? QStringLiteral("已记录虚幻武器；不代表它能触发装甲冲击")
                : ethereal == k("unmet") ? QStringLiteral("完整记录中没有虚幻武器；仍按原卡施法后普攻条件判断")
                                         : QStringLiteral("虚幻武器是否已选尚未核对"), "interaction_context"));
        if (ethereal == k("met")) requirements.append(condition("ethereal_armored_interaction", "unknown", QStringLiteral("攻略未独立验证虚幻武器联动，不能省略普攻前提"), "unverified_interaction"));
        tradeoffs.append(QStringLiteral("护甲联动不等于纯护甲最优；按战况为魔抗、蓝量、急速和移速留位置"));
        counterexamples.append(QStringLiteral("施法后无法普攻到目标，额外护甲不能自动兑现冲击触发收益"));
        out[k("concise_condition")] = ethereal == k("met") ? QStringLiteral("已有虚幻武器也先按接普攻判断；联动待核实")
            : support.fit ? QStringLiteral("现有护甲有联动；施法后接普攻，兼顾魔抗与蓝量")
                          : QStringLiteral("走护甲路线再考虑；需施法接普攻，兼顾魔抗与蓝量");
        out[k("sources")] = QJsonArray{communitySource()};
        if (ireliaArmored) {
            out[k("champion_synergy")] = k("irelia_armored_impact");
            requirements.append(condition("irelia_q_consumes_armored_impact", "unknown",
                QStringLiteral("社区报告利刃冲击可直接触发。是否需要先施放其他技能准备冲击，当前版本缺少分开的命中记录。")));
            counterexamples.append(QStringLiteral("Q因命中标记目标或击杀而刷新属于技能自身效果，无法据此判断是否触发冲击。"));
            out[k("concise_condition")] = QStringLiteral("能施法接普攻时考虑，Q单独触发待核实");
            out[k("sources")] = QJsonArray{communitySource(),
                championSynergySource("irelia-armored-community", "https://lolhaidou.cn/hero/irelia.html", "community_guide")};
        }
    } else if (offeredId == 1029) {
        const auto armored = presence(3049);
        requirements.append(condition("per_ability_onhit_producer_consumer", "unknown", QStringLiteral("需逐个核对具体技能、攻击特效与触发限制，不能假设所有技能或被动都生效")));
        requirements.append(condition("armored_impact_observation", armored.toLatin1().constData(),
            armored == k("met") ? QStringLiteral("已记录装甲冲击，但技能施加攻击特效能否触发它仍未知")
                : armored == k("unmet") ? QStringLiteral("完整记录中没有装甲冲击") : QStringLiteral("装甲冲击是否已选尚未核对"), "interaction_context"));
        if (armored == k("met")) requirements.append(condition("ethereal_armored_interaction", "unknown", QStringLiteral("未实测的技能到装甲冲击链不计入数值，不默认替代普攻"), "unverified_interaction"));
        tradeoffs.append(QStringLiteral("技能命中、每目标触发限制及特效消费者需分别核对，不能把攻击频率当作触发频率"));
        counterexamples.append(QStringLiteral("技能施加攻击特效，不代表所有英雄被动和施法后普攻效果都会被启动"));
        out[k("concise_condition")] = armored == k("met") ? QStringLiteral("已选装甲冲击；虚幻武器能否代替普攻尚未核实")
                                                         : QStringLiteral("逐技能核对攻击特效；不要默认所有被动都能触发");
        out[k("sources")] = QJsonArray{communitySource()};
    } else if (offeredId == 3081) {
        requirements.append(condition("first_legendary_purchase_history", "unknown", QStringLiteral("当前背包不能证明第一件传说装备及购买顺序，需真实购买历史")));
        requirements.append(condition("future_growth", "unknown", QStringLiteral("后续参与击杀与成长数值未观测，不能提前记为收益")));
        tradeoffs.append(QStringLiteral("原卡写明命运之武器不能出售；不要假设卖装、换装或重选可以重置绑定"));
        counterexamples.append(QStringLiteral("当前持有六神装或某件装备，不足以倒推哪件是第一件传说装备"));
        out[k("concise_condition")] = QStringLiteral("先核对首件传说装；背包不代表购买顺序，未来成长未知");
        out[k("sources")] = QJsonArray{communitySource(true)};
    } else if (offeredId == 2126 || offeredId == 2148) {
        requirements.append(condition("match_assigned_ability", "unknown", QStringLiteral("原卡依赖本局指定技能；静态英雄技能表不能代替本局绑定")));
        requirements.append(condition("ability_onhit_delivery", "unknown", QStringLiteral("指定技能及其攻击特效触发方式需分别核对；不能按全部技能或全部普攻计算")));
        if (offeredId == 2126) {
            requirements.append(condition("attacks_during_ability_duration", "unknown", QStringLiteral("需要技能持续期间能够命中普攻；持续伤害跳数不是普攻次数")));
            tradeoffs.append(QStringLiteral("减冷却是否可兑现，取决于指定技能、持续期和普攻机会；不猜静态绑定或额外施法次数"));
            counterexamples.append(QStringLiteral("无法持续普攻，或技能持续期间冷却不运行时，说明文本不足以证明减冷却收益"));
            out[k("concise_condition")] = QStringLiteral("先看本局指定技能；持续期间能接普攻再考虑");
        } else {
            tradeoffs.append(QStringLiteral("额外施加攻击特效不等于技能伤害翻倍，也不等于施加一次攻击时效果"));
            counterexamples.append(QStringLiteral("把额外攻击特效当成多一次完整普攻，会错误增加伤害、暴击或施法后攻击收益"));
            out[k("concise_condition")] = QStringLiteral("先核对指定技能与攻击特效；不是全部伤害翻倍");
        }
        out[k("assigned_ability")] = QJsonValue(QJsonValue::Null);
        out[k("sources")] = QJsonArray{};
    } else if (offeredId == 3059) {
        bool manaKnown = inventory && catalog.combat && catalog.combat->patch == catalog.patch;
        double mana = 0;
        if (manaKnown) for (int id : state.owned) {
            if (!catalog.combat->items.contains(id)) { manaKnown = false; break; }
            const double value = catalog.combat->items.value(id).stats[Stat::Mana];
            if (!std::isfinite(value) || value < 0) { manaKnown = false; break; }
            mana += value;
        }
        manaKnown = manaKnown && std::isfinite(mana);
        out[k("inventory_static_mana")] = manaKnown ? QJsonValue(mana) : QJsonValue(QJsonValue::Null);
        out[k("full_item_mana_bucket")] = QJsonValue(QJsonValue::Null);
        requirements.append(condition("static_equipment_mana", !manaKnown ? "unknown" : mana > 0 ? "met" : "unmet",
            !manaKnown ? QStringLiteral("装备静态蓝量尚未核对，不能把未知当成零")
                : mana > 0 ? QStringLiteral("当前装备有静态蓝量可作转换方向参考；附加成长蓝量与完整转换次序仍待核对")
                           : QStringLiteral("当前装备无静态蓝量；卡片的法强增幅和未来出装仍需独立判断"), "current_build_fit"));
        requirements.append(condition("mana_cost_pressure", "unknown", QStringLiteral("会失去来自装备的蓝量；实际施法消耗、蓝量余裕与回复尚未核对")));
        requirements.append(condition("conversion_order_and_dynamic_item_stats", "unknown", QStringLiteral("额外装备成长、其他属性转换和结算次序不能从静态背包倒推")));
        tradeoffs.append(QStringLiteral("装备蓝量转法强不代表全部法力值转走；缺蓝压力与法强增幅要分开判断"));
        counterexamples.append(QStringLiteral("没有装备蓝量不代表海克斯零收益；有高蓝量也不能忽略施法资源损失"));
        out[k("concise_condition")] = manaKnown && mana > 0 ? QStringLiteral("已有装备蓝量可转法强；先确认失去蓝量后仍够施法")
            : QStringLiteral("装备蓝量转法强；增幅另算，缺蓝压力需核对");
        out[k("sources")] = QJsonArray{};
    } else if (asheArchmage) {
        out[k("champion_synergy")] = k("ashe_archmage");
        requirements.append(condition("archmage_ultimate_refund", "unknown",
            QStringLiteral("万箭齐发可以提供施法机会，魔法水晶箭须正在冷却且实际被随机选中才有返还。")));
        requirements.append(condition("archmage_repeated_cast_sequence", "unknown",
            QStringLiteral("连续收益取决于其他技能能否继续施放、蓝量和每次实际返还落点。")));
        counterexamples.append(QStringLiteral("一次施法后大招提前转好，仍需后续有技能可用才能重复。"));
        out[k("concise_condition")] = QStringLiteral("能频繁放W时考虑，随机返还也可能落到别的技能");
        out[k("sources")] = QJsonArray{
            championSynergySource("ashe-archmage-community", "https://lolhaidou.cn/hero/ashe.html", "community_guide")};
    } else if (sorakaButterflies) {
        out[k("champion_synergy")] = k("soraka_enchanting_butterflies");
        requirements.append(condition("soraka_ally_heal_delivery", "unknown",
            QStringLiteral("W治疗队友，R只把实际治疗队友的部分作为积累候选，具体层数和满血判定仍需核实。")));
        requirements.append(condition("butterflies_damage_after_ready", "unknown",
            QStringLiteral("蝴蝶聚满后，还要实际伤到敌方英雄，Q命中英雄可以提供伤害机会。")));
        counterexamples.append(QStringLiteral("Q命中后的自身回复和R的自疗部分都不按队友治疗计算。"));
        out[k("concise_condition")] = QStringLiteral("先用W或R治疗队友，蝴蝶攒满后用Q或E伤到敌方英雄");
        out[k("sources")] = QJsonArray{
            championSynergySource("butterflies-riot-26-21",
                "https://www.leagueoflegends.com/en-us/news/game-updates/league-of-legends-patch-26-21-notes/", "official_patch_notes"),
            championSynergySource("soraka-riot-abilities", "https://www.leagueoflegends.com/en-us/champions/soraka/", "official_champion_abilities")};
    }
    out[k("facts")] = facts; out[k("requirements")] = requirements;
    out[k("tradeoffs")] = tradeoffs; out[k("counterexamples")] = counterexamples;
    out[k("actual_trigger_count")] = QJsonValue(QJsonValue::Null);
    out[k("mechanic_execution_promoted")] = false; out[k("purchase_authorized")] = false;
    return out;
}
