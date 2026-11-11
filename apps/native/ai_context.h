#pragma once
#include "analysis.h"
struct AiCandidateContext {QJsonObject context;QList<int> items,augments;};
AiCandidateContext makeAiCandidateContext(const Catalog &,const GameState &,int championId,const QString &preference,const QJsonObject *computedPlan=nullptr);
QJsonObject lockOverlayAiContext(const QJsonObject &);
bool validateOverlayAiAdvice(QJsonObject &,const QJsonObject &lockedContext,QString *error);
bool overlayAdviceMatchesAction(const QJsonObject &plan,const QJsonObject &coach);
