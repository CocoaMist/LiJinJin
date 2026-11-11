#include "local_choice_v2.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <numeric>
#include <span>
#include <string_view>
#include <vector>

namespace {
QString key(const char *name) { return QString::fromLatin1(name); }

// The order of these arrays is part of the stored model's feature contract.
constexpr auto StatNames = std::to_array<const char *>({
    "ap", "ad", "hp", "mana", "armor", "mr", "bonus_as", "as", "crit", "crit_damage",
    "haste", "ms", "ms_percent", "magic_pen", "magic_pen_percent", "armor_pen",
    "armor_pen_percent", "lethality", "lifesteal", "omnivamp", "heal_shield", "tenacity",
    "slow_resist", "range", "hp_regen", "mana_regen", "hp_regen_percent", "mana_regen_percent",
    "cooldown_reduction", "as_multiplier", "item_haste", "q_haste", "w_haste", "e_haste",
    "r_haste", "spell_amp", "damage_amp", "shield", "shred", "armor_shred", "magic_shred",
    "magic_amp"});
constexpr auto TargetNames = std::to_array<const char *>({"attackDamage", "skillDamage", "survival"});
constexpr auto ContextNames = std::to_array<const char *>({
    "level", "ranged", "uses_mana", "target_armor", "target_mr", "target_hp",
    "target_ms", "horizon", "base_as", "as_ratio"});
constexpr auto RankNames = std::to_array<const char *>({"Q", "W", "E", "R"});

struct SingleProbe { const char *stat; double offset; };
constexpr SingleProbe SingleProbes[] = {
    {"ap", 50.}, {"ad", 50.}, {"hp", 500.}, {"haste", 50.}, {"armor", 50.}, {"mr", 50.},
    {"bonus_as", .5}, {"crit", .25}, {"crit_damage", .25}, {"magic_pen", 20.},
    {"magic_pen_percent", .2}, {"armor_pen_percent", .2}, {"lethality", 20.},
    {"spell_amp", .2}, {"damage_amp", .2}, {"heal_shield", .2}, {"q_haste", 50.},
    {"w_haste", 50.}, {"e_haste", 50.}, {"r_haste", 50.}, {"as", .5}, {"mana", 500.},
    {"shield", 200.}
};
constexpr std::size_t HpProbe = 2;
constexpr std::size_t HasteProbe = 3;
constexpr auto JointNames = std::to_array<const char *>({"ap_haste", "ad_haste", "hp_haste", "hp_double"});
constexpr std::size_t SingleCount = std::size(SingleProbes);
constexpr std::size_t ProbeCount = SingleCount + JointNames.size();
constexpr std::size_t FeatureCount = 5 * StatNames.size() + ContextNames.size() + RankNames.size()
    + (ProbeCount + SingleCount + JointNames.size() + 2) * TargetNames.size() + SingleCount + 3;
constexpr qsizetype MaxCandidates = 12;
constexpr std::array<std::size_t, 4> ModelWidths{FeatureCount, 96, 48, TargetNames.size()};
constexpr std::size_t ParameterCount = (ModelWidths[0] + 1) * ModelWidths[1]
    + (ModelWidths[1] + 1) * ModelWidths[2] + (ModelWidths[2] + 1) * ModelWidths[3];
static_assert(FeatureCount == 418 && ParameterCount == 45027 && ProbeCount == 27);

using Stats = std::array<double, StatNames.size()>;
using Components = std::array<double, TargetNames.size()>;
using Features = std::array<double, FeatureCount>;
using ProbeWeights = std::array<double, SingleCount>;

QString probeName(std::size_t index) {
    return key(index < SingleCount ? SingleProbes[index].stat : JointNames[index - SingleCount]);
}

double statScale(std::string_view stat) {
    for (const auto &probe : SingleProbes)
        if (stat == probe.stat) return probe.offset;
    if (stat == "as_multiplier") return .5;
    if (stat == "hp_regen" || stat == "mana_regen") return 5.;
    if (stat == "lifesteal" || stat == "omnivamp" || stat == "tenacity" ||
        stat == "slow_resist" || stat == "cooldown_reduction" ||
        stat.find("percent") != std::string_view::npos || stat.find("amp") != std::string_view::npos ||
        stat.find("shred") != std::string_view::npos) return .2;
    return 50.;
}

bool finite(std::span<const double> values) {
    return std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); });
}

bool number(const QJsonValue &value, double &out) {
    if (!value.isDouble() || !std::isfinite(value.toDouble())) return false;
    out = value.toDouble();
    return true;
}

bool numbers(const QJsonArray &values, std::span<double> out) {
    if (values.size() != static_cast<qsizetype>(out.size())) return false;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (!number(values.at(static_cast<qsizetype>(i)), out[i])) return false;
    return true;
}

template<std::size_t N>
bool numbers(const QJsonValue &value, const std::array<const char *, N> &names, std::array<double, N> &out) {
    if (!value.isObject()) return false;
    const auto object = value.toObject();
    for (std::size_t i = 0; i < N; ++i)
        if (!number(object.value(key(names[i])), out[i])) return false;
    return true;
}

QJsonObject componentsJson(const Components &values) {
    QJsonObject out;
    for (std::size_t i = 0; i < values.size(); ++i) out[key(TargetNames[i])] = values[i];
    return out;
}

QStringList featureNames() {
    QStringList names;
    const auto add = [&names](const QString &prefix, const auto &values) {
        for (const auto *value : values) names.append(prefix + key(value));
    };
    for (const auto *prefix : {"candidate.", "baseline.", "delta.", "base."}) add(key(prefix), StatNames);
    add(key("context."), ContextNames);
    add(key("rank."), RankNames);
    for (std::size_t i = 0; i < ProbeCount; ++i) add(key("probe.") + probeName(i) + u'.', TargetNames);
    for (const auto &probe : SingleProbes) names.append(key("basis_weight.") + key(probe.stat));
    for (const auto &probe : SingleProbes) add(key("response_contribution.") + key(probe.stat) + u'.', TargetNames);
    for (const auto *name : JointNames) add(key("interaction.") + key(name) + u'.', TargetNames);
    add(key("unspanned_delta."), StatNames);
    add(key("anchor."), TargetNames);
    add(key("local_prediction."), TargetNames);
    for (int task = 0; task < 3; ++task) names.append(key("task.") + QString::number(task));
    return names;
}

struct Layer {
    std::vector<double> weight;
    std::vector<double> bias;
};
struct Model {
    QString id;
    QString error;
    Features mean{}, scale{};
    Components targetMean{}, targetScale{};
    std::array<Layer, ModelWidths.size() - 1> layers;
    double gain = 0.;
    double clip = 0.;
};

Model loadModel() {
    QFile file(QStringLiteral(":/lixin/local-choice/component-v2.json"));
    Model model;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) {
        model.error = key("component_weights_resource_unavailable");
        return model;
    }
    const auto json = QJsonDocument::fromJson(file.readAll()).object();
    model.error = key("invalid_component_weights");
    if (json.value(key("model_id")).toString() != key("lixin-component-selector-v2") ||
        json.value(key("feature_names")).toArray() != QJsonArray::fromStringList(featureNames())) return model;
    QJsonArray targetNames;
    for (const auto *name : TargetNames) targetNames.append(key(name));
    if (json.value(key("targets")).toArray() != targetNames ||
        !numbers(json.value(key("x_mean")).toArray(), model.mean) ||
        !numbers(json.value(key("x_scale")).toArray(), model.scale) ||
        !numbers(json.value(key("y_mean")).toArray(), model.targetMean) ||
        !numbers(json.value(key("y_scale")).toArray(), model.targetScale) ||
        !number(json.value(key("residual_gain")), model.gain) ||
        !number(json.value(key("residual_clip")), model.clip) || model.clip < 0. ||
        std::any_of(model.scale.begin(), model.scale.end(), [](double value) { return value <= 0.; }))
        return model;

    const auto layers = json.value(key("layers")).toArray();
    if (layers.size() != static_cast<qsizetype>(model.layers.size())) return model;
    for (std::size_t i = 0; i < model.layers.size(); ++i) {
        const auto inputWidth = ModelWidths[i], outputWidth = ModelWidths[i + 1];
        auto &layer = model.layers[i];
        const auto layerJson = layers.at(static_cast<qsizetype>(i)).toObject();
        layer.bias.resize(outputWidth);
        layer.weight.resize(inputWidth * outputWidth);
        const auto weights = layerJson.value(key("weight")).toArray();
        if (weights.size() != static_cast<qsizetype>(outputWidth) ||
            !numbers(layerJson.value(key("bias")).toArray(), layer.bias)) return model;
        for (std::size_t row = 0; row < outputWidth; ++row) {
            const std::span<double> weightsRow(layer.weight.data() + row * inputWidth, inputWidth);
            if (!numbers(weights.at(static_cast<qsizetype>(row)).toArray(), weightsRow)) return model;
        }
    }
    model.id = json.value(key("model_id")).toString();
    model.error.clear();
    return model;
}

struct Response {
    Stats baseline{}, base{};
    Components reference{};
    std::array<double, ContextNames.size()> context{};
    std::array<double, RankNames.size()> ranks{};
    std::array<Components, ProbeCount> responses{};
    std::array<Stats, SingleCount> columns{};
    std::array<ProbeWeights, SingleCount> lower{};
    int task = 0;
};

QStringList readResponse(const QJsonObject &packet, Response &out) {
    QStringList problems;
    double task = 0.;
    if (!number(packet.value(key("task")), task) || task < 0 || task > 2 || std::floor(task) != task)
        problems.append(key("task:required_integer_0_equipment_1_augment_2_route"));
    else
        out.task = static_cast<int>(task);
    if (!numbers(packet.value(key("baseline_stats")), StatNames, out.baseline))
        problems.append(key("baseline_stats:required_native_numeric_stats"));
    if (!numbers(packet.value(key("reference_components")), TargetNames, out.reference) ||
        std::any_of(out.reference.begin(), out.reference.end(), [](double v) { return v < 0.; }))
        problems.append(key("reference_components:required_nonnegative_native_components"));

    const auto context = packet.value(key("context")).toObject();
    for (std::size_t i = 0; i < ContextNames.size(); ++i) {
        const auto field = key(ContextNames[i]);
        const auto value = context.value(field);
        if (field == key("ranged") || field == key("uses_mana")) {
            if (value.isBool()) out.context[i] = value.toBool() ? 1. : 0.;
            else problems.append(key("context.") + field + key(":required_boolean"));
        } else if (!number(value, out.context[i])) {
            problems.append(key("context.") + field + key(":required_finite_number"));
        }
    }
    if (!numbers(context.value(key("base_stats")), StatNames, out.base))
        problems.append(key("context.base_stats:required_native_numeric_stats"));
    if (!numbers(context.value(key("ranks")), RankNames, out.ranks))
        problems.append(key("context.ranks:required_skill_ranks"));

    const auto probes = packet.value(key("context_probe")).toObject();
    for (std::size_t i = 0; i < ProbeCount; ++i) {
        const auto name = probeName(i);
        const auto probe = probes.value(name).toObject();
        if (!numbers(probe.value(key("component_delta")), TargetNames, out.responses[i]))
            problems.append(key("context_probe.") + name + key(".component_delta:required_native_response"));
        if (i < SingleCount && !numbers(probe.value(key("final_stats_delta")), StatNames, out.columns[i]))
            problems.append(key("context_probe.") + name + key(".final_stats_delta:required_realized_stats"));
    }
    return problems;
}

bool factorResponse(Response &response) {
    for (auto &column : response.columns)
        for (std::size_t i = 0; i < StatNames.size(); ++i) column[i] /= statScale(StatNames[i]);
    // Regularized Gram matrix of realized native changes, in the original
    // projection coordinates. A probe's requested offset is not its response.
    for (std::size_t i = 0; i < SingleCount; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            const auto &a = response.columns[i];
            const auto &b = response.columns[j];
            auto &lower = response.lower;
            const double gram = std::inner_product(a.begin(), a.end(), b.begin(), 0.) + (i == j ? 1e-6 : 0.);
            const double value = gram - std::inner_product(lower[i].begin(), lower[i].begin() + j, lower[j].begin(), 0.);
            if (!std::isfinite(value)) return false;
            lower[i][j] = i == j ? std::sqrt(std::max(value, 1e-12)) : value / lower[j][j];
            if (!std::isfinite(lower[i][j])) return false;
        }
    }
    return true;
}

ProbeWeights project(const Response &response, const Stats &delta) {
    ProbeWeights solution{}, intermediate{};
    const auto &lower = response.lower;
    for (std::size_t i = 0; i < SingleCount; ++i) {
        const auto &column = response.columns[i];
        const auto rhs = std::inner_product(column.begin(), column.end(), delta.begin(), 0.);
        const auto known = std::inner_product(lower[i].begin(), lower[i].begin() + i, intermediate.begin(), 0.);
        intermediate[i] = (rhs - known) / lower[i][i];
    }
    for (std::size_t i = SingleCount; i-- > 0;) {
        double known = 0.;
        for (std::size_t j = i + 1; j < SingleCount; ++j) known += lower[j][i] * solution[j];
        solution[i] = (intermediate[i] - known) / lower[i][i];
    }
    return solution;
}

bool makeFeatures(const Response &response, const Stats &candidate, Features &features, Components &local) {
    Stats delta{}, scaled{}, remaining{};
    for (std::size_t i = 0; i < StatNames.size(); ++i) {
        delta[i] = candidate[i] - response.baseline[i];
        scaled[i] = delta[i] / statScale(StatNames[i]);
    }
    const auto weights = project(response, scaled);
    if (!finite(weights)) return false;
    std::array<Components, SingleCount> contributions{};
    std::array<Components, JointNames.size()> interactions{};
    for (std::size_t target = 0; target < TargetNames.size(); ++target) {
        double contribution = 0., interaction = 0.;
        for (std::size_t i = 0; i < SingleCount; ++i) {
            contributions[i][target] = weights[i] * response.responses[i][target];
            contribution += contributions[i][target];
        }
        // AP/AD/HP are the first three probes, in joint-haste order.
        for (std::size_t i = 0; i < 3; ++i) {
            const auto cross = response.responses[SingleCount + i][target] -
                response.responses[i][target] - response.responses[HasteProbe][target];
            interactions[i][target] = weights[i] * weights[HasteProbe] * cross;
            interaction += interactions[i][target];
        }
        const auto hpWeight = weights[HpProbe];
        interactions.back()[target] = .5 * hpWeight * (hpWeight - 1.) *
            (response.responses.back()[target] - 2. * response.responses[HpProbe][target]);
        interaction += interactions.back()[target];
        const auto component = response.reference[target] + contribution + interaction;
        if (!std::isfinite(component)) return false;
        local[target] = std::max(0., component);
    }
    for (std::size_t stat = 0; stat < StatNames.size(); ++stat) {
        double span = 0.;
        for (std::size_t i = 0; i < SingleCount; ++i) span += weights[i] * response.columns[i][stat];
        remaining[stat] = scaled[stat] - span;
    }

    auto output = features.begin();
    const auto append = [&output](const auto &values) { output = std::copy(values.begin(), values.end(), output); };
    append(candidate);
    append(response.baseline);
    append(delta);
    append(response.base);
    append(response.context);
    append(response.ranks);
    for (const auto &values : response.responses) append(values);
    append(weights);
    for (const auto &values : contributions) append(values);
    for (const auto &values : interactions) append(values);
    append(remaining);
    append(response.reference);
    append(local);
    for (int task = 0; task < 3; ++task) *output++ = task == response.task ? 1. : 0.;
    if (!finite(features)) return false;
    for (auto &value : features) value = std::copysign(std::log1p(std::abs(value)), value);
    return true;
}

bool predict(const Model &model, const Response &response, const Stats &candidate,
             Components &ratios, Components &predicted, bool &nonnegativeProjection) {
    Features features{};
    Components local{};
    if (!makeFeatures(response, candidate, features, local)) return false;
    for (std::size_t i = 0; i < features.size(); ++i)
        features[i] = (features[i] - model.mean[i]) / model.scale[i];
    if (!finite(features)) return false;

    std::vector<double> input(features.begin(), features.end());
    for (std::size_t i = 0; i < model.layers.size(); ++i) {
        const auto &layer = model.layers[i];
        std::vector<double> output(layer.bias.size());
        for (std::size_t row = 0; row < output.size(); ++row) {
            const auto *weights = layer.weight.data() + row * input.size();
            const auto value = std::inner_product(input.begin(), input.end(), weights, 0.) + layer.bias[row];
            if (!std::isfinite(value)) return false;
            output[row] = i + 1 < model.layers.size() ? std::max(0., value) : value;
        }
        input.swap(output);
    }

    for (std::size_t i = 0; i < TargetNames.size(); ++i) {
        const auto residual = input[i] * model.targetScale[i] + model.targetMean[i];
        if (!std::isfinite(residual)) return false;
        const auto baselineLog = std::log1p(response.reference[i]);
        const auto localRatio = std::log1p(local[i]) - baselineLog;
        ratios[i] = model.gain * std::clamp(residual, -model.clip, model.clip) + localRatio;
        const auto value = std::expm1(baselineLog + ratios[i]);
        if (!std::isfinite(ratios[i]) || !std::isfinite(value)) return false;
        nonnegativeProjection |= value < 0.;
        predicted[i] = std::max(0., value);
    }
    return true;
}

QJsonValue nullIfMissing(const QJsonValue &value) {
    return value.isUndefined() ? QJsonValue(QJsonValue::Null) : value;
}

QJsonValue candidateId(const QJsonObject &candidate) {
    const auto id = candidate.value(key("candidate_id"));
    return id.isUndefined() || id.isNull() ? nullIfMissing(candidate.value(key("id"))) : id;
}

bool validIdentity(const QJsonValue &id) {
    if (id.isString()) return !id.toString().trimmed().isEmpty() && id.toString().size() <= 128;
    double value = 0.;
    return number(id, value) && value > 0 && value <= 9007199254740991. && std::floor(value) == value;
}

void appendUnknowns(QJsonArray &all, const QJsonValue &unknowns) {
    if (unknowns.isArray()) {
        for (const auto &value : unknowns.toArray())
            if (!all.contains(value)) all.append(value);
    } else if (!unknowns.isNull() && !unknowns.isUndefined() && !all.contains(unknowns)) {
        all.append(unknowns);
    }
}
} // namespace

QJsonObject localChoiceV2ProbeOffsets() {
    QJsonObject probes;
    for (const auto &probe : SingleProbes)
        probes[key(probe.stat)] = QJsonObject{{key(probe.stat), probe.offset}};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto &stat = SingleProbes[i];
        const auto &haste = SingleProbes[HasteProbe];
        probes[key(JointNames[i])] = QJsonObject{{key(stat.stat), stat.offset}, {key(haste.stat), haste.offset}};
    }
    const auto &hp = SingleProbes[HpProbe];
    probes[key(JointNames.back())] = QJsonObject{{key(hp.stat), 2. * hp.offset}};
    return probes;
}

QJsonObject evaluateLocalChoiceV2(const QJsonObject &packet) {
    const QJsonValue null(QJsonValue::Null);
    QJsonArray names;
    for (const auto *name : TargetNames) names.append(key(name));
    const auto context = packet.value(key("context")).toObject();
    QJsonObject result{
        {key("schema"), key("lixin-component-comparison-v2")}, {key("available"), false},
        {key("status"), key("unavailable")}, {key("model_id"), null}, {key("components"), names},
        {key("scope"), key("conditional_native_partial_components_excluding_unresolved_and_proc_effects")},
        {key("analysis_only"), true}, {key("gpu_required"), false}, {key("execution_eligible"), false},
        {key("decision_ranking_authority"), false}, {key("overall_utility"), null},
        {key("strict_admission_used"), false}, {key("source_provenance_verified"), false},
        {key("training_support_verified"), false}, {key("actual_game_outcome_predicted"), false},
        {key("affordability_or_shop_used"), false}, {key("route_order_or_purchase_timing_learned"), false},
        {key("scenario_assumptions"), QJsonObject{
            {key("model_training_assumes_all_skills_ready_at_zero"), true},
            {key("packet_skills_ready_at_zero_scenario_assumption"),
             nullIfMissing(context.value(key("skills_ready_at_zero_scenario_assumption")))},
            {key("current_cooldowns_used_by_component_model"), false},
            {key("attack_and_cast_counts_are_model_assumptions"), true}}},
        {key("baseline_components"), null}, {key("candidates"), QJsonArray{}},
        {key("reasons"), QJsonArray{}}, {key("input_problems"), QJsonArray{}}
    };

    QJsonArray unknowns;
    appendUnknowns(unknowns, packet.value(key("reference_unknowns")));
    const auto probes = packet.value(key("context_probe")).toObject();
    for (std::size_t i = 0; i < ProbeCount; ++i)
        appendUnknowns(unknowns, probes.value(probeName(i)).toObject().value(key("unknowns")));
    result[key("native_unknowns")] = unknowns;

    const auto candidates = packet.value(key("candidates")).toArray();
    if (candidates.isEmpty() || candidates.size() > MaxCandidates) {
        result[key("reasons")] = QJsonArray{key("candidate_count_out_of_range")};
        result[key("input_problems")] = QJsonArray{key("candidates:requires_1_to_12_rows")};
        return result;
    }
    for (const auto &candidate : candidates) appendUnknowns(unknowns, candidate.toObject().value(key("unknowns")));
    result[key("native_unknowns")] = unknowns;

    Response response;
    const auto problems = readResponse(packet, response);
    Components baseline{};
    if (numbers(packet.value(key("reference_components")), TargetNames, baseline) &&
        std::none_of(baseline.begin(), baseline.end(), [](double value) { return value < 0.; }))
        result[key("baseline_components")] = componentsJson(baseline);
    QString failure;
    const Model *model = nullptr;
    if (!problems.isEmpty()) {
        result[key("input_problems")] = QJsonArray::fromStringList(problems);
        failure = std::any_of(problems.begin(), problems.end(), [](const QString &problem) {
            return problem.startsWith(QStringLiteral("context_probe."));
        }) ? key("missing_or_invalid_required_response_data") : key("missing_or_invalid_shared_numeric_input");
    } else {
        static const Model weights = loadModel();
        if (!weights.error.isEmpty()) failure = weights.error;
        else if (!factorResponse(response)) failure = key("non_finite_native_response_projection");
        else {
            model = &weights;
            result[key("model_id")] = weights.id;
            result[key("parameter_count")] = static_cast<int>(ParameterCount);
        }
    }

    QJsonArray rows;
    qsizetype available = 0;
    for (qsizetype i = 0; i < candidates.size(); ++i) {
        const auto candidate = candidates.at(i).toObject();
        const auto id = candidateId(candidate);
        const auto actualId = candidate.value(key("id"));
        QJsonObject row{
            {key("candidate_id"), id}, {key("id"), actualId.isUndefined() ? id : actualId},
            {key("candidate_index"), static_cast<int>(i)}, {key("available"), false},
            {key("baseline_components"), result.value(key("baseline_components"))},
            {key("predicted_components"), null}, {key("predicted_log_component_ratios"), null},
            {key("log_component_ratios"), null},
            {key("candidate_native_unknowns"), nullIfMissing(candidate.value(key("unknowns")))},
            {key("reasons"), QJsonArray{}}
        };
        const auto duplicates = std::count_if(candidates.begin(), candidates.end(), [&id](const QJsonValue &v) {
            return candidateId(v.toObject()) == id;
        });
        Stats stats{};
        Components ratios{}, predicted{};
        bool nonnegativeProjection = false;
        if (!model) row[key("reasons")] = QJsonArray{failure};
        else if (!validIdentity(id) || duplicates != 1)
            row[key("reasons")] = QJsonArray{key("missing_or_ambiguous_candidate_identity")};
        else if (!numbers(candidate.value(key("stats")), StatNames, stats))
            row[key("reasons")] = QJsonArray{key("missing_or_invalid_candidate_stats")};
        else if (!predict(*model, response, stats, ratios, predicted, nonnegativeProjection))
            row[key("reasons")] = QJsonArray{key("non_finite_candidate_prediction")};
        else {
            ++available;
            row[key("available")] = true;
            row[key("predicted_components")] = componentsJson(predicted);
            row[key("predicted_log_component_ratios")] = componentsJson(ratios);
            row[key("log_component_ratios")] = componentsJson(ratios);
            row[key("nonnegative_projection_applied")] = nonnegativeProjection;
        }
        rows.append(row);
    }
    result[key("candidates")] = rows;
    if (available) {
        result[key("available")] = true;
        result[key("status")] = key("conditional_partial_estimates");
    }
    if (!failure.isEmpty()) result[key("reasons")] = QJsonArray{failure};
    else if (available != candidates.size()) result[key("reasons")] = QJsonArray{key("one_or_more_candidates_unavailable")};
    return result;
}
