#pragma once
#include "mechanic_actions.h"

bool hasHextech40Conversion(const CombatEntry &);
bool hextech40CrownReady(const CombatContext &);
bool applyHextech40Conversions(const CombatDatabase &,const CombatContext &,const QList<int> &items,
    const QList<int> &augments,double ownedFlatMana,CombatEvaluation &);
double hextech40CrownMovement(const CombatEntry &,const CombatContext &);
bool executeHextech40Conversion(const CombatDatabase *,const CombatEntry &,const CombatContext &,
    const QJsonObject &,MechanicLedger &,QStringList *);
void projectHextech40ConversionState(const MechanicLedger &,CombatContext &);
bool hextech40ConversionLocksItem(const MechanicLedger &,int item,double time);
