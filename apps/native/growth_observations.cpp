#include "growth_observations.h"
#include "analysis.h"
#include "combat.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
constexpr qint64 maxAgeMs=3000,minFrameIntervalMs=100;
constexpr double minValidationConfidence=.95,maxGameTimeLag=2;
const QString captureSource=QStringLiteral("visible_game_tooltip_ocr");
struct Binding {
    QString source,key,unit,rule;
    int id=0;bool numeric=false;double maximum=1e9;
    QStringList titles;
};
bool finite(const QJsonValue &v){return v.isDouble()&&std::isfinite(v.toDouble());}
bool integer(const QJsonValue &v,int low,int high){return finite(v)&&v.toDouble()>=low&&v.toDouble()<=high&&v.toDouble()==std::floor(v.toDouble());}
bool keysKnown(const QJsonObject &o,const QStringList &allowed){for(auto it=o.begin();it!=o.end();++it)if(!allowed.contains(it.key()))return false;return true;}
qint64 utcMs(const QJsonValue &v){
    if(!v.isString())return -1;const auto s=v.toString();
    static const QRegularExpression iso(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}(?:\\.\\d{1,3})?Z$"));
    if(!iso.match(s).hasMatch())return -1;auto d=QDateTime::fromString(s,Qt::ISODateWithMs);if(!d.isValid())d=QDateTime::fromString(s,Qt::ISODate);return d.isValid()?d.toMSecsSinceEpoch():-1;
}
QString digest(const QString &text){return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(),QCryptographicHash::Sha256).toHex());}
bool sha(const QJsonValue &v){static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));return v.isString()&&re.match(v.toString()).hasMatch();}
double boundValue(const CombatEntry &e,const QString &name){auto value=e.values.value(name);if(value.isArray()&&!value.toArray().isEmpty())value=value.toArray().first();return finite(value)&&value.toDouble()>0?value.toDouble():0;}
QList<Binding> bindings(const Catalog &catalog){
    QList<Binding> out;if(!catalog.combat||catalog.patch.isEmpty()||catalog.combat->patch!=catalog.patch)return out;const auto &db=*catalog.combat;
    if(db.items.contains(3084)){
        const auto &entry=db.items[3084];bool numeric=false;for(const auto &v:entry.effects){const auto e=v.toObject();
            if(e.value(k("op"))==k("stack")&&e.value(k("stat"))==k("hp")&&e.value(k("counter"))==k("item:3084:permanent_hp")&&finite(e.value(k("value")))&&e.value(k("value")).toDouble()==1)numeric=true;}
        const QStringList titles{catalog.itemName(3084),QStringLiteral("心之钢"),QStringLiteral("Heartsteel")};
        out.append({k("item"),k("item:3084:permanent_hp"),k("health_points"),k("heartsteel_permanent_hp_v1"),3084,numeric,1e9,titles});
        out.append({k("item"),k("item:3084:trigger_count"),k("count"),k("heartsteel_trigger_count_v1"),3084,false,1e9,titles});
    }
    const auto quest=db.augments.constFind(1152);if(quest!=db.augments.cend()&&quest->triggerContract.value(k("event"))==k("quest_progress")&&quest->triggerContract.value(k("requires")).toArray().contains(k("item:3084"))){
        const double maximum=boundValue(*quest,k("StackThreshold"));if(maximum>0)out.append({k("augment"),k("augment:1152:heartsteel_progress"),k("health_points"),k("steel_your_heart_progress_v1"),1152,false,maximum,{QStringLiteral("钢化你心"),QStringLiteral("Quest: Steel Your Heart"),QStringLiteral("Steel Your Heart")}});
    }
    for(int id:{3070,3003,3004}){const auto it=db.items.constFind(id);if(it==db.items.cend())continue;const double maximum=boundValue(*it,k("MaxMana"));if(maximum<=0)continue;
        QStringList titles{catalog.itemName(id)};if(id==3070)titles.append(QStringLiteral("Tear of the Goddess"));if(id==3003)titles.append(QStringLiteral("Archangel's Staff"));if(id==3004)titles.append(QStringLiteral("Manamune"));
        out.append({k("item"),k("item:")+QString::number(id)+k(":mana_charge"),k("mana_points"),k("tear_mana_charge_v1"),id,false,maximum,titles});}
    for(int id:{1082,3041}){const auto it=db.items.constFind(id);if(it==db.items.cend())continue;const double maximum=boundValue(*it,k("MaxGloryStacks"));if(maximum<=0)continue;
        out.append({k("item"),k("item:")+QString::number(id)+k(":glory_stacks"),k("count"),k("glory_stacks_v1"),id,false,maximum,{catalog.itemName(id),id==1082?QStringLiteral("Dark Seal"):QStringLiteral("Mejai's Soulstealer")}});}
    return out;
}
QJsonObject bindingJson(const Binding &b){return {{k("source"),b.source},{k("id"),b.id},{k("counter_key"),b.key},{k("unit"),b.unit},{k("parser_rule"),b.rule},{k("minimum"),0},{k("maximum"),b.maximum},{k("counter_applied"),b.numeric},{k("disposition"),b.numeric?k("numeric_baseline"):k("observation_only")}};}
bool numberMatches(const QString &s,double value){QString n=s;n.remove(QChar(u','));bool ok=false;const double parsed=n.toDouble(&ok);return ok&&std::isfinite(parsed)&&std::abs(parsed-value)<=1e-9*std::max(1.,std::abs(value));}
bool textMatches(const Binding &b,const QString &text,double value){
    const auto lines=text.split(QChar(u'\n'),Qt::SkipEmptyParts);if(lines.size()<2)return false;bool title=false;
    for(const auto &allowed:b.titles)if(!allowed.trimmed().isEmpty()&&lines.first().trimmed().compare(allowed.trimmed(),Qt::CaseInsensitive)==0)title=true;if(!title)return false;
    const QString number=QStringLiteral("((?:[0-9]+|[0-9]{1,3}(?:,[0-9]{3})+)(?:\\.[0-9]+)?)");QString expression;bool fraction=false;
    if(b.rule==k("heartsteel_permanent_hp_v1"))expression=QStringLiteral("^(?:已获得(?:的)?(?:永久|额外)?生命值|永久生命值加成|已叠加(?:的)?生命值)\\s*[:：]\\s*")+number+QStringLiteral("\\s*(?:生命值|点)?$");
    if(b.rule==k("heartsteel_trigger_count_v1"))expression=QStringLiteral("^(?:触发次数|已触发次数)\\s*[:：]\\s*")+number+QStringLiteral("\\s*次$");
    if(b.rule==k("steel_your_heart_progress_v1")){expression=QStringLiteral("^(?:任务进度|当前进度|当前层数|已获得永久生命值)\\s*[:：]\\s*")+number+QStringLiteral("\\s*/\\s*")+number+QStringLiteral("\\s*(?:生命值|点|层)?$");fraction=true;}
    if(b.rule==k("tear_mana_charge_v1")){expression=QStringLiteral("^(?:法力积攒|已积攒法力|已获得额外法力值)\\s*[:：]\\s*")+number+QStringLiteral("\\s*/\\s*")+number+QStringLiteral("\\s*(?:法力值|点)?$");fraction=true;}
    if(b.rule==k("glory_stacks_v1"))expression=QStringLiteral("^(?:荣耀|荣耀层数|荣誉层数)\\s*[:：]\\s*")+number+QStringLiteral("\\s*层$");
    if(expression.isEmpty())return false;const QRegularExpression re(expression);int found=0;
    for(int i=1;i<lines.size();++i){const auto m=re.match(lines[i].trimmed());if(!m.hasMatch())continue;if(!numberMatches(m.captured(1),value)||(fraction&&!numberMatches(m.captured(2),b.maximum)))return false;++found;}return found==1;
}
QString evidenceIssue(const Binding &b,const QJsonObject &e,double value,qint64 captured,qint64 now){
    if(!keysKnown(e,{k("text_sha256"),k("image_sha256"),k("parser_rule"),k("matched_text"),k("repeats")}))return k("unknown_evidence_field");
    const auto text=e.value(k("matched_text")).toString();if(text.isEmpty()||text.size()>4096||!sha(e.value(k("text_sha256")))||!sha(e.value(k("image_sha256")))||digest(text)!=e.value(k("text_sha256")).toString())return k("evidence_hash_or_text_invalid");
    if(e.value(k("parser_rule"))!=b.rule||!textMatches(b,text,value))return k("evidence_semantics_do_not_match_counter");
    if(!e.value(k("repeats")).isArray())return k("repeat_capture_evidence_missing");const auto frames=e.value(k("repeats")).toArray();if(frames.size()<2||frames.size()>8)return k("repeat_capture_count_invalid");
    QSet<QString> frameIds;QSet<qint64> frameTimes;qint64 first=now,last=-1;QJsonObject latest;
    for(const auto &v:frames){if(!v.isObject())return k("repeat_capture_invalid");const auto frame=v.toObject();
        if(!keysKnown(frame,{k("frame_id"),k("captured_at_utc"),k("image_sha256"),k("text_sha256"),k("parser_rule"),k("matched_text"),k("value"),k("unit")}))return k("unknown_repeat_capture_field");
        const auto id=frame.value(k("frame_id")).toString(),t=frame.value(k("matched_text")).toString();const qint64 time=utcMs(frame.value(k("captured_at_utc")));
        if(id.isEmpty()||id.size()>128||frameIds.contains(id)||time<0||time>now||now-time>maxAgeMs||frameTimes.contains(time)||time>captured)return k("repeat_capture_identity_or_time_invalid");
        if(!finite(frame.value(k("value")))||frame.value(k("value")).toDouble()!=value||frame.value(k("unit"))!=b.unit||frame.value(k("parser_rule"))!=b.rule||t.isEmpty()||t.size()>4096||!sha(frame.value(k("image_sha256")))||!sha(frame.value(k("text_sha256")))||digest(t)!=frame.value(k("text_sha256")).toString()||!textMatches(b,t,value))return k("repeat_capture_value_or_evidence_mismatch");
        frameIds.insert(id);frameTimes.insert(time);first=std::min(first,time);if(time>last){last=time;latest=frame;}
    }
    if(last-first<minFrameIntervalMs||last!=captured)return k("repeat_capture_interval_or_latest_time_invalid");
    for(const auto &name:{k("text_sha256"),k("image_sha256"),k("parser_rule"),k("matched_text")})if(latest.value(name)!=e.value(name))return k("latest_capture_evidence_mismatch");return {};
}
QString structuredEvidenceIssue(const Binding &b,const QJsonObject &row,const QJsonObject &input,const GrowthObservationPolicy &policy){
    const auto e=row.value(k("evidence")).toObject();if(!keysKnown(e,{k("provider_id"),k("schema_revision"),k("record_id"),k("source_ref"),k("record_sha256"),k("record_json")}))return k("unknown_structured_evidence_field");
    const auto provider=e.value(k("provider_id")).toString(),revision=e.value(k("schema_revision")).toString(),source=input.value(k("source")).toString();bool registered=false;
    for(const auto &v:policy.structuredProviders){const auto p=v.toObject();if(!keysKnown(p,{k("provider_id"),k("source"),k("schema_revision"),k("counter_keys")}))continue;
        if(!provider.isEmpty()&&!revision.isEmpty()&&p.value(k("provider_id"))==provider&&p.value(k("schema_revision"))==revision&&p.value(k("source"))==source&&p.value(k("counter_keys")).toArray().contains(b.key))registered=true;}
    if(!registered)return k("structured_provider_not_registered_for_counter");
    const auto recordId=e.value(k("record_id")).toString(),reference=e.value(k("source_ref")).toString(),raw=e.value(k("record_json")).toString();
    if(recordId.trimmed().isEmpty()||recordId.size()>256||reference.trimmed().isEmpty()||reference.size()>1024||raw.isEmpty()||raw.size()>16384||!sha(e.value(k("record_sha256")))||digest(raw)!=e.value(k("record_sha256")).toString())return k("structured_record_identity_or_hash_invalid");
    QJsonParseError error;const auto document=QJsonDocument::fromJson(raw.toUtf8(),&error);if(error.error!=QJsonParseError::NoError||!document.isObject())return k("structured_record_json_invalid");const auto record=document.object();
    if(!keysKnown(record,{k("context_id"),k("champion_id"),k("game_time"),k("captured_at_utc"),k("source"),k("id"),k("counter_key"),k("value"),k("unit")}))return k("unknown_structured_record_field");
    for(const auto &name:{k("context_id"),k("champion_id"),k("game_time"),k("captured_at_utc")})if(!record.contains(name)||record.value(name)!=input.value(name))return k("structured_record_match_or_clock_mismatch");
    for(const auto &name:{k("source"),k("id"),k("counter_key"),k("value"),k("unit")})if(!record.contains(name)||record.value(name)!=row.value(name))return k("structured_record_counter_mismatch");return {};
}
bool inventoryObserved(const GameState &state){
    int count=0;bool known=false;for(const auto &v:state.players){const auto p=v.toObject();if(!p.value(k("self")).toBool())continue;++count;
        if(!p.value(k("items_complete")).isBool()||!p.value(k("items_complete")).toBool()||p.value(k("champion_id")).toInt()!=state.championId||!p.value(k("items")).isArray())continue;
        QList<int> actual;bool valid=true;for(const auto &item:p.value(k("items")).toArray()){if(!integer(item,1,1000000)){valid=false;break;}actual.append(item.toInt());}
        auto held=state.owned;std::sort(actual.begin(),actual.end());std::sort(held.begin(),held.end());known=valid&&actual==held;
    }return count==1&&known;
}
}

QJsonArray growthObservationBindings(const Catalog &catalog){QJsonArray out;for(const auto &b:bindings(catalog))out.append(bindingJson(b));return out;}

QJsonArray growthObservationMetricCapabilities(const QJsonObject &input){
    QJsonArray out;
    for(const auto &key:{k("item:3084:permanent_hp"),k("augment:1152:heartsteel_progress"),k("augment:3116:target_stacks"),k("item:1082:glory_stacks"),k("item:3041:glory_stacks")}){
        const bool captureBinding=input.value(k("reviewed_capture_counter_keys")).toArray().contains(key),numericAdapter=input.value(k("numeric_capture_counter_keys")).toArray().contains(key);
        bool registered=false,observed=false,applied=false;QJsonValue value=QJsonValue::Null;
        for(const auto &v:input.value(k("registered_provider_counter_keys")).toArray())if(v==key)registered=true;
        for(const auto &v:input.value(k("accepted")).toArray()){const auto row=v.toObject();if(row.value(k("counter_key"))==key){observed=true;applied=row.value(k("counter_applied")).toBool();value=row.value(k("value"));}}
        out.append(QJsonObject{{k("counter_key"),key},{k("capture_binding_available"),captureBinding},
            {k("provider_registered_for_metric"),registered},{k("numeric_adapter_available"),numericAdapter},
            {k("metric_observed"),observed},{k("numeric_value_applied"),applied},{k("value"),value},
            {k("status"),observed?k("observed"):registered?k("not_observed"):k("provider_not_provided")},
            {k("missing_is_zero"),false},{k("direct_live_stack_read_certified"),false}});
    }
    return out;
}
QJsonObject growthObservationPublicReport(const QJsonObject &input){
    QJsonObject out;
    for(const auto &name:{k("schema_version"),k("champion_id"),k("patch"),k("status"),k("game_time"),k("baseline_game_time"),
            k("captured_at_utc"),k("expires_at_ms"),k("coverage"),k("applied_counter_keys"),k("cleared_counter_keys"),
            k("missing_is_zero"),k("reward_events_created"),k("mechanic_events_created"),k("max_health_difference_inference_used"),k("pending_newer_snapshot"),
            k("recognizer_confidence_available"),k("confidence_meaning"),k("ocr_opt_in"),k("registered_structured_provider_count"),k("self_panel_used_for_counters"),k("metric_capabilities")})
        if(input.contains(name))out[name]=input.value(name);
    QJsonArray facts,rejected;
    for(const auto &value:input.value(k("accepted")).toArray()){
        const auto row=value.toObject();QJsonObject fact;
        for(const auto &name:{k("source"),k("id"),k("counter_key"),k("value"),k("unit"),k("counter_applied"),k("disposition"),
                k("measurement_kind"),k("game_time"),k("captured_at_utc"),k("expires_at_ms"),k("counter_semantics"),k("selection_observed_here")})
            if(row.contains(name))fact[name]=row.value(name);
        facts.append(fact);
    }
    for(const auto &value:input.value(k("rejected")).toArray()){
        const auto row=value.toObject();rejected.append(QJsonObject{{k("index"),row.value(k("index"))},{k("reason"),row.value(k("reason"))}});
    }
    out[k("accepted")]=facts;out[k("rejected")]=rejected;
    return out;
}

QJsonObject applyGrowthObservations(const Catalog &catalog,GameState &state,const QJsonObject &input,qint64 nowMs,const GrowthObservationPolicy &policy){
    const qint64 now=nowMs==0?QDateTime::currentMSecsSinceEpoch():nowMs;const auto allow=bindings(catalog);
    // Only this module's narrow numeric allowlist can be revoked. No arbitrary
    // key from a sidecar or old report can erase unrelated observations.
    QJsonArray cleared;for(const auto &v:state.growthObservations.value(k("applied_counter_keys")).toArray())if(v.toString()==k("item:3084:permanent_hp")&&state.effectCounters.remove(v.toString())>0)cleared.append(v);
    QJsonObject report{{k("schema_version"),1},{k("context_id"),state.matchContext.value(k("context_id"))},{k("champion_id"),state.championId},{k("patch"),catalog.patch},
        {k("source"),input.value(k("source")).toString(k("structured_provider_unavailable"))},{k("accepted"),QJsonArray{}},{k("rejected"),QJsonArray{}},{k("applied_counter_keys"),QJsonArray{}},{k("cleared_counter_keys"),cleared},
        {k("missing_is_zero"),false},{k("reward_events_created"),0},{k("mechanic_events_created"),0},{k("max_health_difference_inference_used"),false},
        {k("recognizer_confidence_available"),false},{k("confidence_kind"),input.value(k("confidence_kind")).toString(k("provider_schema_validation"))},
        {k("confidence_meaning"),k("input_contract_validation_not_measurement_probability")},{k("ocr_opt_in"),policy.allowTooltipOcr},{k("registered_structured_provider_count"),policy.structuredProviders.size()},{k("self_panel_used_for_counters"),false}};
    QJsonArray accepted,rejected,applied;int numeric=0;
    QJsonArray registeredKeys;
    for(const auto &provider:policy.structuredProviders)for(const auto &key:provider.toObject().value(k("counter_keys")).toArray())if(!registeredKeys.contains(key))registeredKeys.append(key);
    report[k("registered_provider_counter_keys")]=registeredKeys;
    QJsonArray captureKeys,numericKeys;
    for(const auto &b:allow){captureKeys.append(b.key);if(b.numeric)numericKeys.append(b.key);}
    report[k("reviewed_capture_counter_keys")]=captureKeys;report[k("numeric_capture_counter_keys")]=numericKeys;
    auto finish=[&](const QString &issue={}){if(!issue.isEmpty())rejected.append(QJsonObject{{k("index"),-1},{k("reason"),issue}});
        report[k("accepted")]=accepted;report[k("rejected")]=rejected;report[k("applied_counter_keys")]=applied;report[k("rule_basis")]=modelRuleBasis(catalog,state);
        report[k("metric_capabilities")]=growthObservationMetricCapabilities(report);
        report[k("status")]=!accepted.isEmpty()?k("observed"):rejected.isEmpty()?k("unknown"):k("rejected");
        report[k("coverage")]=QJsonObject{{k("observations_received"),input.value(k("observations")).toArray().size()},{k("accepted_observations"),accepted.size()},{k("numeric_baselines_applied"),numeric},{k("observation_only_count"),accepted.size()-numeric},{k("rejected_records"),rejected.size()},{k("all_growth_values_observed"),false},{k("future_growth_predicted"),false}};
        state.growthObservations=report;return report;};
    if(input.isEmpty())return finish();
    if(!keysKnown(input,{k("schema_version"),k("context_id"),k("champion_id"),k("game_time"),k("captured_at_utc"),k("source"),k("observations"),k("status"),k("unknowns"),k("confidence_kind"),k("recognizer_confidence_available"),k("provider"),k("capabilities"),k("self_panel"),k("game_input_sent"),k("memory_read"),k("screen_capture_used")}))return finish(k("unknown_sidecar_field"));
    if(!integer(input.value(k("schema_version")),1,1)||!integer(input.value(k("champion_id")),1,1000000)||!input.value(k("observations")).isArray()||input.value(k("observations")).toArray().size()>32)return finish(k("sidecar_schema_invalid"));
    const auto source=input.value(k("source")).toString();const bool ocr=source==captureSource,structured=source==k("wegame_local_log")||source==k("official_provider")||source==k("official_event_derived"),unavailable=source==k("structured_provider_unavailable");
    if(ocr&&!policy.allowTooltipOcr)return finish(k("ocr_not_enabled"));
    if((!ocr&&!structured&&!unavailable)||input.value(k("confidence_kind"))!=(ocr?k("rule_validation"):k("provider_schema_validation"))||!input.value(k("recognizer_confidence_available")).isBool()||input.value(k("recognizer_confidence_available")).toBool())return finish(k("observation_source_or_confidence_semantics_invalid"));
    if(input.contains(k("provider"))&&(!input.value(k("provider")).isString()||input.value(k("provider")).toString().size()>128))return finish(k("provider_diagnostic_invalid"));
    if((input.contains(k("capabilities"))&&!input.value(k("capabilities")).isObject())||(input.contains(k("self_panel"))&&!input.value(k("self_panel")).isObject()))return finish(k("provider_diagnostic_invalid"));
    for(const auto &name:{k("game_input_sent"),k("memory_read")})if(input.contains(name)&&(!input.value(name).isBool()||input.value(name).toBool()))return finish(k("unsupported_provider_operation_reported"));
    if(input.contains(k("screen_capture_used"))&&(!input.value(k("screen_capture_used")).isBool()||(!ocr&&input.value(k("screen_capture_used")).toBool())))return finish(k("structured_provider_reported_screen_capture"));
    if(!QStringList{k("observed"),k("unknown"),k("unavailable"),k("error")}.contains(input.value(k("status")).toString()))return finish(k("sidecar_status_invalid"));
    if(!input.value(k("unknowns")).isArray()||input.value(k("unknowns")).toArray().size()>64)return finish(k("sidecar_unknowns_invalid"));for(const auto &v:input.value(k("unknowns")).toArray())if(!v.isString()||v.toString().size()>256)return finish(k("sidecar_unknowns_invalid"));
    if(state.source!=GameState::Source::Live||!state.received.isValid()||now<state.received.toMSecsSinceEpoch()||now-state.received.toMSecsSinceEpoch()>=4000)return finish(k("current_live_snapshot_required"));
    if(!modelRulesUsable(catalog,state)||state.gameMode!=k("KIWI"))return finish(k("local_rules_and_kiwi_state_required"));
    const auto context=state.matchContext.value(k("context_id")).toString();if(!state.matchContext.value(k("active")).toBool()||state.matchContext.value(k("stage"))!=k("in_game")||context.isEmpty()||context.size()>128||input.value(k("context_id"))!=context)return finish(k("current_match_context_mismatch"));
    if(state.championId<=0||!catalog.combat->champions.contains(state.championId)||input.value(k("champion_id")).toInt()!=state.championId)return finish(k("current_champion_mismatch"));
    const qint64 captured=utcMs(input.value(k("captured_at_utc")));if(captured<0||captured>now||now-captured>maxAgeMs)return finish(k("capture_time_invalid_or_stale"));
    const auto started=state.matchContext.value(k("started_at_ms"));if(finite(started)&&captured<started.toDouble())return finish(k("capture_predates_current_match"));
    const auto game=input.value(k("game_time"));if(!finite(game)||!std::isfinite(state.gameTime)||game.toDouble()<0||game.toDouble()>state.gameTime||state.gameTime-game.toDouble()>maxGameTimeLag)return finish(k("capture_game_time_invalid_or_stale"));
    report[k("game_time")]=game;report[k("baseline_game_time")]=game;report[k("captured_at_utc")]=input.value(k("captured_at_utc"));report[k("expires_at_ms")]=double(captured+maxAgeMs);report[k("unknowns")]=input.value(k("unknowns"));
    const auto observations=input.value(k("observations")).toArray();if(!observations.isEmpty()&&(input.value(k("status"))!=k("observed")||unavailable))return finish(k("non_observed_or_unavailable_source_contains_values"));
    QMap<QString,int> multiplicity;for(const auto &v:observations){const auto o=v.toObject();++multiplicity[o.value(k("source")).toString()+QChar(u':')+QString::number(o.value(k("id")).toInt())+QChar(u':')+o.value(k("counter_key")).toString()];}
    const bool observedInventory=inventoryObserved(state);
    for(int index=0;index<observations.size();++index){const auto value=observations[index];const auto o=value.toObject();QString issue;const Binding *binding=nullptr;
        if(!value.isObject()||!keysKnown(o,{k("source"),k("id"),k("counter_key"),k("value"),k("unit"),k("confidence"),k("evidence"),k("context_id"),k("champion_id"),k("game_time"),k("captured_at_utc")}))issue=k("unknown_or_invalid_observation_field");
        else if(!integer(o.value(k("id")),1,1000000))issue=k("observation_identity_invalid");
        else {for(const auto &b:allow)if(b.source==o.value(k("source")).toString()&&b.id==o.value(k("id")).toInt()&&b.key==o.value(k("counter_key")).toString()){binding=&b;break;}if(!binding)issue=k("counter_not_in_reviewed_allowlist");}
        if(issue.isEmpty())for(const auto &name:{k("context_id"),k("champion_id"),k("game_time"),k("captured_at_utc")})if(o.contains(name)&&o.value(name)!=input.value(name)){issue=k("observation_match_or_clock_mismatch");break;}
        if(issue.isEmpty()&&multiplicity.value(o.value(k("source")).toString()+QChar(u':')+QString::number(o.value(k("id")).toInt())+QChar(u':')+o.value(k("counter_key")).toString())!=1)issue=k("duplicate_or_conflicting_counter_records");
        if(issue.isEmpty()&&(!finite(o.value(k("value")))||o.value(k("value")).toDouble()<0||o.value(k("value")).toDouble()>binding->maximum||(binding->unit==k("count")&&o.value(k("value")).toDouble()!=std::floor(o.value(k("value")).toDouble()))))issue=k("observation_number_invalid");
        if(issue.isEmpty()&&o.value(k("unit"))!=binding->unit)issue=k("observation_unit_missing_or_mismatched");
        if(issue.isEmpty()&&(!finite(o.value(k("confidence")))||o.value(k("confidence")).toDouble()<minValidationConfidence||o.value(k("confidence")).toDouble()>1))issue=k("observation_validation_confidence_insufficient");
        if(issue.isEmpty()&&binding->source==k("item")&&(!observedInventory||!state.owned.contains(binding->id)))issue=k("item_not_observed_in_current_inventory");
        if(issue.isEmpty()&&binding->source==k("augment")&&(!state.selected.contains(binding->id)||!observedInventory||!state.owned.contains(3084)))issue=k("augment_or_required_item_not_present");
        if(issue.isEmpty()&&!o.value(k("evidence")).isObject())issue=k("observation_evidence_missing");
        if(issue.isEmpty())issue=ocr?evidenceIssue(*binding,o.value(k("evidence")).toObject(),o.value(k("value")).toDouble(),captured,now):structuredEvidenceIssue(*binding,o,input,policy);
        if(!issue.isEmpty()){rejected.append(QJsonObject{{k("index"),index},{k("reason"),issue},{k("counter_key"),o.value(k("counter_key"))}});continue;}
        auto row=o;row[k("counter_applied")]=binding->numeric;row[k("disposition")]=binding->numeric?k("numeric_baseline"):k("observation_only");row[k("confidence_kind")]=input.value(k("confidence_kind"));row[k("recognizer_confidence_available")]=false;row[k("expires_at_ms")]=double(captured+maxAgeMs);row[k("counter_semantics")]=k("current_total_not_delta");
        row[k("measurement_kind")]=ocr?k("visible_tooltip_reading"):source==k("official_event_derived")?k("event_derived_counter"):k("direct_structured_counter");
        row[k("selection_observed_here")]=false;row[k("game_time")]=game;row[k("captured_at_utc")]=input.value(k("captured_at_utc"));accepted.append(row);
        if(binding->numeric){state.effectCounters[binding->key]=o.value(k("value")).toDouble();applied.append(binding->key);++numeric;}
    }
    return finish();
}
