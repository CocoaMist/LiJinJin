#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <array>
#include <optional>
#include <memory>

class Catalog;
struct GameState;

enum class Stat : int { Ap, Ad, Hp, Mana, Armor, Mr, BonusAs, As, Crit, CritDamage, Haste,
    Ms, MsPercent, MagicPen, MagicPenPercent, ArmorPen, ArmorPenPercent, Lethality,
    Lifesteal, Omnivamp, HealShield, Tenacity, SlowResist, Range, HpRegen, ManaRegen,
    HpRegenPercent, ManaRegenPercent, CooldownReduction, AsMultiplier, ItemHaste,
    QHaste, WHaste, EHaste, RHaste, SpellAmp, DamageAmp, Shield, Shred, ArmorShred, MagicShred, MagicAmp, Count };
struct CombatStats {
    std::array<double,static_cast<int>(Stat::Count)> values{};
    double &operator[](Stat s) {return values[static_cast<int>(s)];}
    double operator[](Stat s) const {return values[static_cast<int>(s)];}
    CombatStats &operator+=(const CombatStats &);
    QJsonObject json() const;
};
std::optional<Stat> combatStat(const QString &);
struct CombatContext {
    int championId=0, level=18;
    bool ranged=false,usesMana=true;
    CombatStats base;
    CombatStats observedOffset;
    // Additive residual before transforms and amplifiers. The legacy final
    // offset remains available when the observed baseline cannot be inverted.
    CombatStats observedInputOffset;
    CombatStats eventStats;
    CombatStats permanentEventStats;
    // Reconstructed from committed item-attached ledger effects; never an
    // external aggregate mana counter. Candidate evaluation filters ownership.
    QMap<int,CombatStats> itemAttachedStats;
    QJsonArray itemAttachedEffects;
    // Explicitly verified form ordering. The source defines the multiplier;
    // callers must separately establish where it enters item conversions.
    QJsonArray formStatTransforms;
    double baseAs=.625, asRatio=.625;
    QMap<QString,int> ranks;
    QMap<QString,double> counters;
    // Scenario counters may describe caller-admitted hits in a bounded skill
    // window. They are not proof of observed live buffs or future hit counts.
    QSet<QString> conditions;
    // One baseline and metric set for every candidate in a comparison. The
    // chosen augments define source-bound dependencies, never a role preset.
    QJsonObject growthObjectiveBasis;
    double targetArmor=80, targetMr=60, targetHp=2500, targetMs=350, horizon=8;
    // Optional conditional cast schedule on [0,horizon). Each cumulative
    // window shares t=0, so interval differences never award a new opener.
    // Empty ready-in means all skills ready at t=0, an explicit scenario.
    bool discreteSkillCasts=false;
    QMap<QString,double> skillReadyIn;
    // Counts admitted by a persistent action ledger. In this mode horizon
    // never creates attacks, casts, channel time or item proc opportunities.
    bool explicitActionCounts=false;
    double explicitAttackCount=0;
    QMap<QString,double> explicitCastCounts,explicitActiveSeconds,explicitProcCounts;
    QSet<QString> explicitComponentKeys;
    // A strict empty selection admits no skill phase. Per-phase scales convert
    // a source total/per-second amount to one admitted tick without multiplying
    // the action count, resource payment or item proc opportunities.
    bool restrictExplicitComponents=false;
    QMap<QString,double> explicitComponentScales;
    // Mayhem-specific global rules, distinct from ordinary Summoner's Rift.
    double attackSpeedCap=5,abilityHasteCap=500,overflowCritAd=.45,overflowCritAp=.75;
};
struct FormulaInstruction {int op=0, stat=0, part=0, count=0;double a=0,b=0;};
struct CombatFormula {
    QVector<FormulaInstruction> code;
    bool valid=false;
    std::optional<double> evaluate(const CombatContext &,const CombatStats &) const;
};
CombatFormula compileCombatFormula(const QJsonObject &,const CombatContext &,int rank=1);
QString combatComponentIdentity(const QJsonObject &);
// Shared comparison interval for damage and growth; does not infer recasts,
// refunds, ammo or server timing. Flat CDR retains its existing model semantics.
bool combatSkillCooldownHasteApplies(const QJsonObject &hero,const QJsonObject &component);
double effectiveCombatSkillCooldown(double base,const QString &slot,bool hasteApplies,
    const CombatStats &,const CombatContext &,bool ultimateHasteOnly=false,double ultimateHasteMultiplier=1);
// Shared resistance/penetration/type-amplifier stage for admitted raw effects.
// Spell, critical, repeat-hit and global source modifiers are resolved by the
// caller's explicit effect contract, never inferred from this helper.
std::optional<double> mitigateCombatDamage(const QString &type,double raw,
    const CombatStats &,const CombatContext &,const QMap<QString,double> &typeAmps={});
struct CombatEntry {
    int id=0,price=-1,requiredLevel=1,epicness=0;
    bool eligible=false,boots=false,finished=false;
    // Retained shop exclusions are separate from final-build eligibility.
    QStringList purchaseExclusions;
    // Actor-dependent shop gates survive compilation and database loading.
    // They do not change the static candidate flag into current shop evidence.
    QJsonArray conditionalPurchaseRestrictions;
    CombatStats stats;
    QJsonObject values,calculations,triggerContract,mechanismStateV1;
    QJsonArray effects;
    QJsonArray actions,eventRules,growthStackInputs;
    QJsonArray temporalBindingsV4;
    QString coverage;
    QMap<QString,int> groups;
    QList<int> from;
};
class CombatDatabase {
public:
    bool load(const QString &,QString *);
    QMap<int,QJsonObject> champions;
    QMap<int,CombatEntry> items,augments;
    QString revision, patch;
    QJsonObject audit;
    CombatContext context(int championId,int level) const;
};
struct CombatEvaluation {
    CombatStats stats;
    QJsonObject growthObjective;
    // Conditional self-sustain is reported separately from observed healing,
    // actual shield absorption and the base HP/resistance capacity.
    QJsonObject selfSustain;
    QJsonObject damageTypeMultipliers;
    QJsonArray conversionAudit;
    double attackDamage=0,skillDamage=0,procDamage=0,onhitDamage=0,replacementAttackDamage=0,healing=0,survival=0,score=0;
    double globalDamageMultiplier=1, copiedTrueDamageRatio=0;
    bool valid=false;
    QStringList unknowns;
    QJsonObject json() const;
};
CombatEvaluation evaluateCombat(const CombatDatabase &,const CombatContext &,const QList<int> &items,const QList<int> &augments,bool prepareWholePool=false,const QString &style={});
QJsonObject anchorObservedCombatStats(const CombatDatabase &,CombatContext &,const QList<int> &items,
                                     const QList<int> &augments,const QJsonObject &observed,const QString &style={});
bool compatibleEquipment(const CombatDatabase &,const QList<int> &items,int newId,int championId,int level);
int remainingBuildCost(const CombatDatabase &,const QList<int> &build,const QList<int> &owned);
QJsonObject optimizeCombatBuild(const Catalog &,const GameState &,int championId,const QString &style,int beamWidth=24);
