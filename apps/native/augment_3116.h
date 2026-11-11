#pragma once
#include "mechanic_actions.h"
// nativeReceipt is supplied only by the C++ admission/settlement path. Public
// observations use the explicit caller-trusted external schema instead.
bool executeAugment3116(const CombatEntry &,const CombatContext &,const CombatStats &,
    const QJsonObject &,MechanicLedger &,QStringList *,bool nativeReceipt=false);
bool validateAugment3116State(const MechanicLedger &,QString *error=nullptr);
void advanceAugment3116State(MechanicLedger &,double time);
