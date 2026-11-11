#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QtGlobal>

class Catalog;
struct GameState;

// Native registration is explicit: a sidecar cannot promote itself into a
// supported provider. No equipment-counter provider is presumed to exist.
struct GrowthObservationPolicy {
    bool allowTooltipOcr=false;
    // {provider_id,source,schema_revision,counter_keys:[...]}; registration is
    // performed by native code after reviewing the actual producer contract.
    QJsonArray structuredProviders;
};

// The small reviewed allowlist distinguishes total permanent HP, event counts
// and quest progress. A readable counter need not have a numeric model binding.
QJsonArray growthObservationBindings(const Catalog &);

// Public diagnostics contain reviewed facts and reason codes only. Raw source
// records, local paths and evidence payloads remain inside validation.
QJsonObject growthObservationPublicReport(const QJsonObject &);
QJsonArray growthObservationMetricCapabilities(const QJsonObject &);

// Live, current-context observations only; structured providers must be
// registered by native code and OCR requires an explicit opt-in. Replaces this module's
// previous numeric baseline (never adds a delta), and removes it on an empty,
// stale or rejected update. No mechanic event, condition or reward is created.
// nowMs=0 uses the local UTC clock; an explicit clock supports deterministic QA.
QJsonObject applyGrowthObservations(const Catalog &, GameState &,
    const QJsonObject &sidecar, qint64 nowMs=0,
    const GrowthObservationPolicy &policy={});
