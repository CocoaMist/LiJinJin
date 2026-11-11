#pragma once
#include "catalog.h"
// A caller-declared condition is not an authenticated live observation.
// These audits check necessary inputs and actual native delivery coverage.
QJsonObject chainMechanismInputs(const Catalog &,const QJsonObject &scenario);
QJsonObject chainMechanismExecution(const Catalog &,const QJsonObject &scenario,const QJsonObject &nativeResult);
// Resolve skill_state admissions from the same source transition used by the
// executor; a caller's event type/slot alone does not determine cast semantics.
QJsonObject chainActionContract(const Catalog &,const QJsonObject &event,const QJsonObject &actor);
