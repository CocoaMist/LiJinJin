#include "panel_state.h"
#include "offer_match_binding.h"
#include "../native/headliner_context.h"
#include <QSet>
#include <QJsonDocument>
#include <algorithm>

namespace {
bool boundedInteger(QJsonValue value, int minimum, int maximum) {
    return value.isDouble() && value.toDouble() == value.toInt() && value.toInt() >= minimum && value.toInt() <= maximum;
}
QString sourceReference(const QJsonValue &value) {
    const auto ref=value.toString();
    if(ref.size()!=64||!std::all_of(ref.cbegin(),ref.cend(),[](const QChar c){
        return (c>=QLatin1Char('0')&&c<=QLatin1Char('9'))||(c>=QLatin1Char('a')&&c<=QLatin1Char('f'));
    }))return {};
    return ref;
}
void recordResolution(PanelState &state,const QString &reason,QDateTime at,bool resolved) {
    QJsonArray offered;for(const auto &card:state.candidates)offered.append(card.toObject().value("augment_id"));
    state.selectionRoundEvidence.append(QJsonObject{{"round",state.round},{"offer_revision",qint64(state.offerRevision)},
        {"context_id",state.liveContextId},{"match_ref",state.liveMatchRef},{"session_ref",state.session},
        {"offer_observed_at",state.observed.toString(Qt::ISODateWithMs)},
        {"offered_ids",offered},{"source",resolved?"wegame_local_log":"lixin_offer_guard"},{"reason",reason},
        {"observed_at",at.toString(Qt::ISODateWithMs)},{"round_resolved",resolved},
        {"selected_id",QJsonValue(QJsonValue::Null)},{"selected_complete",false}});
    while(state.selectionRoundEvidence.size()>16)state.selectionRoundEvidence.removeFirst();
}
}
bool PanelState::bindLiveMatch(const QString &contextId,const QString &matchRef) {
    if(replay)return false;
    const bool valid=!contextId.isEmpty()&&contextId.size()<=256&&validOfferMatchRef(matchRef);
    const QString nextContext=valid?contextId:QString(),nextRef=valid?matchRef:QString();
    if(liveContextId==nextContext&&liveMatchRef==nextRef)return false;
    const auto revision=offerRevision+1;
    *this=PanelState(false);offerRevision=revision;liveContextId=nextContext;liveMatchRef=nextRef;return true;
}
bool PanelState::liveBoundTo(const QString &contextId) const {
    return !replay&&!contextId.isEmpty()&&liveContextId==contextId&&validOfferMatchRef(liveMatchRef);
}
void PanelState::clearLiveOffers() {
    if(status=="open")recordResolution(*this,QStringLiteral("binding_lost_or_feed_reset"),QDateTime::currentDateTimeUtc(),false);
    if(status!="waiting"||!candidates.isEmpty())++offerRevision;
    candidates={};status=QStringLiteral("waiting");headlinerContext={};headlinerObserved={};
    offerSourceRef.clear();awaitingReroll=false;
}
bool PanelState::apply(const QJsonObject &event, QDateTime now) {
    if (event.value("schema_version").toInt() != 1 || event.value("source").toString() != "wegame_local_log") return false;
    if (event.value("capture_mode").toString() != (replay ? "replay" : "live")) return false;
    if(!replay&&(!liveBoundTo(liveContextId)||event.value("match_ref").toString()!=liveMatchRef
        ||event.value("match_binding_schema")!="lixin.match_ref.v1"||event.value("match_binding_status")!="known"
        ||event.value("match_ref_scope")!="provider_game_id"))return false;
    const auto at = QDateTime::fromString(event.value("observed_at").toString(), Qt::ISODateWithMs);
    if (!at.isValid() || (!replay && (at.msecsTo(now) > 10000 || at.msecsTo(now) < -5000))) return false;
    const auto ref = event.value("session_ref").toString();
    const auto kind = event.value("kind").toString();
    auto applyHeadliner=[&](){
        const auto value=sanitizeHeadlinerContext(event.value("headliner_context").toObject(),replay);
        if(value.isEmpty()||(headlinerObserved.isValid()&&at<headlinerObserved)
            ||(!headlinerContext.isEmpty()&&value.value("game_time").toDouble()<headlinerContext.value("game_time").toDouble()))return false;
        headlinerContext=value;headlinerObserved=at;return true;
    };
    if(kind=="headliner_context"){
        if(ref.isEmpty()||ref.size()>100||event.value("game_mode")!="KIWI"||(!session.isEmpty()&&ref!=session))return false;
        if(!applyHeadliner())return false;session=ref;return true;
    }
    if (kind == "offer" || kind == "reroll_pending") {
        if (ref.isEmpty() || ref.size() > 100 || event.value("game_mode").toString() != "KIWI"
            || !boundedInteger(event.value("pick_count"), 1, 100)) return false;
        const auto next = event.value("pick_count").toInt();
        if ((!replay||session == ref) && ((observed.isValid() && at < observed) || next < round || next <= resolvedRound)) return false;
        const auto sourceFile = event.value("source_file").toString();
        const auto sourceOffset = event.value("source_offset").toInteger(-1);
        const auto providerRoundValue = event.value("provider_round");
        const auto providerRound = providerRoundValue.isDouble() ? providerRoundValue : QJsonValue();
        // A millisecond can contain several events. Only an increasing offset
        // in the same log establishes their order when the timestamps tie.
        const bool laterInSameLog = at == observed && !offerSourceFile.isEmpty()
            && sourceFile == offerSourceFile && offerSourceOffset >= 0 && sourceOffset > offerSourceOffset;
        if (!replay && at == observed && !offerSourceFile.isEmpty() && offerSourceOffset >= 0 && !laterInSameLog) return false;
        const bool isReroll = event.value("is_reroll") == QJsonValue(true);
        const bool newerOffer = at > observed || laterInSameLog;
        const auto updateObservation = [&] {
            observed = at;
            offerSourceFile = sourceFile;
            offerSourceOffset = sourceOffset;
            offerProviderRound = providerRound;
            offerSourceRef = sourceReference(event.value("offer_source_ref"));
        };
        const auto invalidCards = [&] {
            if (replay || status != "open" || session != ref || next != round || !observed.isValid() || !newerOffer || !isReroll) return false;
            ++offerRevision;
            candidates = {};
            status = "waiting";
            awaitingReroll = true;
            updateObservation();
            return true;
        };
        const auto candidateValue = event.value("candidates");
        const auto cards = candidateValue.toArray();
        if (kind == "reroll_pending")
            return providerRound == offerProviderRound && candidateValue.isArray() && cards.isEmpty() && invalidCards();
        if (cards.isEmpty() || cards.size() > 8) return invalidCards();
        QSet<int> seen;
        for (const auto &value : cards) {
            const auto card = value.toObject();
            const auto name = card.value("augment_name").toString();
            if (!boundedInteger(card.value("augment_id"), 1, 1000000) || name.isEmpty() || name.size() > 128
                || name.contains(QChar('\n')) || seen.contains(card.value("augment_id").toInt())) return invalidCards();
            seen.insert(card.value("augment_id").toInt());
        }
        if(status!="open"||session!=ref||round!=next||candidates!=cards
            ||(isReroll&&newerOffer))++offerRevision;
        if (replay&&session != ref) { resolvedRound = 0; seenCandidates.clear();selectionRoundEvidence={};headlinerContext={};headlinerObserved={}; }
        seenCandidates.unite(seen);
        session = ref; round = next; candidates = cards; status = "open"; awaitingReroll = false;
        updateObservation();
        if(event.value("headliner_context").isObject())applyHeadliner();
        return true;
    }
    if (ref.isEmpty() || ref != session || (observed.isValid() && at < observed)) return false;
    if (kind == "clear") {
        const auto value = event.value("provider_clear_round");
        if (!boundedInteger(value, 1, 100)) return false;
        if(value.toInt()<round||value.toInt()<resolvedRound)return false;
        if(status!="closed"||value.toInt()>resolvedRound){recordResolution(*this,QStringLiteral("provider_clear_round"),at,true);++offerRevision;}
        resolvedRound = std::max(resolvedRound, value.toInt());
        candidates = {}; status = "closed"; offerSourceRef.clear(); awaitingReroll = false; return true;
    }
    if (kind == "pick_signal") {
        if(round<=0)return false;
        if(status!="closed"){recordResolution(*this,QStringLiteral("provider_pick_signal"),at,true);++offerRevision;}
        resolvedRound = std::max(resolvedRound, round);
        candidates = {}; status = "closed"; offerSourceRef.clear(); awaitingReroll = false; return true;
    }
    // Overlay visibility, narration, highlights and history never select a card.
    return false;
}
bool PanelState::expire(QDateTime now) {
    if (!replay && status == "open" && observed.msecsTo(now) > 60000) {
        recordResolution(*this,QStringLiteral("offer_expired"),now,false);++offerRevision;
        candidates = {}; status = "stale"; offerSourceRef.clear(); awaitingReroll = false; return true;
    }
    return false;
}
QString PanelState::decisionBinding() const {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"revision",QString::number(offerRevision)},
        {"context",liveContextId},{"match_ref",liveMatchRef},{"session",session},{"round",round},{"status",status}}).toJson(QJsonDocument::Compact));
}
QJsonObject PanelState::json() const {
    return {{"capture_mode", replay ? "replay" : "live"}, {"status", status}, {"session_ref", session},
            {"live_context_id",liveContextId},{"match_ref",liveMatchRef.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(liveMatchRef)},
            {"pick_count", round},{"resolved_round",resolvedRound},{"offer_revision",qint64(offerRevision)},
            {"selection_round_evidence",selectionRoundEvidence}, {"candidates", candidates}, {"confirmed_selected", QJsonArray{}},
            {"selected_complete", false}, {"skill_binding_complete", false},{"headliner_context",headlinerContext},
            {"headliner_observed_at",headlinerObserved.toString(Qt::ISODateWithMs)}};
}
