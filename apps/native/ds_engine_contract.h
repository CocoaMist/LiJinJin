#pragma once
#include <QJsonObject>
#include <QString>

// Offline preflight only. Does not call a provider, read credentials, run a
// recommendation, publish knowledge, or grant a purchase/rule/state change.
QJsonObject prepareDsEngineReviewContext(const QJsonObject &rawContext);
// The context argument is prepareDsEngineReviewContext(...)["context"].
// Success replaces response with a bounded analysis-only receipt. Referenced
// evidence remains source text/conditional output, never semantic validation.
bool validateDsEngineReview(QJsonObject &response, const QJsonObject &context,
                            QString *error = nullptr);
QString dsEngineReviewInstruction();
