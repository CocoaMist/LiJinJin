#include "offer_match_binding.h"
#include "panel_state.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QFile>
#include <cstdio>

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);int checks=0,failures=0;
    auto check=[&](bool pass,const char *name){++checks;if(!pass){++failures;std::fprintf(stderr,"FAIL %s\n",name);}};
    check(offerMatchRefForGameId(QStringLiteral("1"))==QStringLiteral("match-v1-4fa951954c2d41a99c1150d9dfb0aa0eab0a2873f5379cb6a76cfa763931bbde"),"python_native_vector_1");
    const auto ref=offerMatchRefForGameId(QStringLiteral("42"));
    check(ref==QStringLiteral("match-v1-f9cbb33a75c4691530a462d3f811965dc57d8c94778780b3e76db23de05556cf"),"python_native_vector_42");
    check(offerMatchRefForGameId(QStringLiteral("9007199254740991"))==QStringLiteral("match-v1-f37e2453a95ca0d300541e7cdd39f0381eda071bcddd4636cac15f5d1d9fc076"),"python_native_vector_max_json_integer");
    check(offerMatchRefForGameId(QStringLiteral("9223372036854775807"))==QStringLiteral("match-v1-f4bab78b07128ea82cb7e0b717a67dd48413b9787a682ccca7153fc93b3176e3"),"python_native_vector_max_string_integer");
    check(offerMatchRefForGameId(QStringLiteral("00042"))==ref&&offerMatchRefForGameId(42)==ref,"canonical_id_semantics");
    for(const auto &bad:QJsonArray{QJsonValue::Null,true,0,-1,1.5,9007199254740992.,QStringLiteral(""),QStringLiteral(" 42"),QStringLiteral("+42"),QStringLiteral("4e2"),QStringLiteral("４２"),QStringLiteral("9223372036854775808"),QString(129,'0')})check(offerMatchRefForGameId(bad).isEmpty(),"invalid_id_has_no_binding");
    const auto now=QDateTime::currentDateTimeUtc();GameState s;s.source=GameState::Source::Live;s.received=now;
    s.gameMode="KIWI";s.observedPatch="16.21";s.gameTime=120;s.championId=1;
    s.matchContext={{"context_id","local-context-a"},{"active",true},{"stage","in_game"}};
    QJsonObject e{{"schema_version",1},{"source","wegame_local_log"},{"capture_mode","live"},{"observed_at",now.toString(Qt::ISODateWithMs)},
        {"session_ref","session-1"},{"kind","offer"},{"game_mode","KIWI"},{"game_time",120},{"champion_id",1},
        {"match_binding_schema","lixin.match_ref.v1"},{"match_ref_scope","provider_game_id"},{"match_binding_status","known"},
        {"match_binding_source","observed_provider_game_id"},{"match_ref",ref},{"pick_count",2},
        {"candidates",QJsonArray{QJsonObject{{"augment_id",1022},{"augment_name","first"}},QJsonObject{{"augment_id",1097},{"augment_name","second"}}}}};
    auto admitted=[&](const QJsonObject &event,const GameState &state){return liveOfferAdmission(event,state,"local-context-a",ref,"16.21",now).value("accepted").toBool();};
    check(admitted(e,s),"current_match_offer_admitted");
    auto wrong=e;wrong["match_ref"]=offerMatchRefForGameId(QStringLiteral("43"));
    check(!admitted(wrong,s),"same_hero_clock_session_but_other_match_rejected");
    wrong=e;wrong.remove("match_ref");check(!admitted(wrong,s),"legacy_unbound_live_offer_rejected");
    wrong=e;wrong["match_binding_source"]="inherited_recent_provider_context";check(!admitted(wrong,s),"new_offer_requires_explicit_provider_game_id");
    wrong=e;wrong["game_time"]=116.9;check(!admitted(wrong,s),"game_clock_outside_three_seconds_rejected");
    wrong=e;wrong["observed_at"]=now.addMSecs(-3001).toString(Qt::ISODateWithMs);check(!admitted(wrong,s),"wall_clock_outside_three_seconds_rejected");
    wrong=e;wrong["champion_id"]=98;check(!admitted(wrong,s),"inconsistent_source_hero_rejected");
    auto stale=s;stale.received=now.addMSecs(-4000);check(!admitted(e,stale),"stale_live_state_rejected");
    stale=s;stale.observedPatch="16.20";check(admitted(e,stale),"version_change_keeps_verified_current_match_offer");
    stale.observedPatch.clear();check(admitted(e,stale),"unknown_patch_keeps_match_identity_and_clock_admission");
    stale=s;stale.matchContext["stage"]="loading";check(!admitted(e,stale),"loading_not_current_gameplay");
    stale=s;stale.matchContext["context_id"]="other-context";check(!admitted(e,stale),"local_context_mismatch_rejected");
    auto unknown=e;unknown["match_ref"]=QJsonValue::Null;unknown["match_binding_status"]="unknown";
    check(unboundOfferRevokesCurrent(unknown,s,now,now),"fresh_valid_unbound_offer_revokes_older_cards");
    check(!unboundOfferRevokesCurrent(unknown,s,now.addMSecs(1),now),"older_unknown_offer_cannot_revoke_newer_cards");
    auto expiredUnknown=unknown;expiredUnknown["observed_at"]=now.addSecs(-11).toString(Qt::ISODateWithMs);
    check(!unboundOfferRevokesCurrent(expiredUnknown,s,{},now),"stale_unknown_offer_cannot_revoke_current_cards");
    check(!unboundOfferRevokesCurrent(QJsonObject{{"kind","offer"},{"source","wegame_local_log"},{"capture_mode","live"},{"match_binding_status","unknown"}},s,{},now),"incomplete_unknown_signal_has_no_revocation_authority");
    unknown["candidates"]=QJsonArray{};check(!unboundOfferRevokesCurrent(unknown,s,{},now),"malformed_unknown_offer_cannot_revoke_current_cards");
    PanelState panel;check(!panel.apply(e,now),"state_has_no_implicit_live_trust");
    panel.bindLiveMatch("local-context-a",ref);check(panel.apply(e,now)&&panel.candidates[0].toObject().value("augment_id")==1022,"bound_offer_preserves_provider_order");
    PanelState rerollPanel;
    rerollPanel.bindLiveMatch("local-context-a",ref);
    auto initial=e;initial["provider_round"]=1;initial["source_file"]="coach_client2.log";initial["source_offset"]=100;
    initial["offer_source_ref"]=QString(64,QLatin1Char('a'));
    check(rerollPanel.apply(initial,now)&&rerollPanel.offerSourceRef==initial.value("offer_source_ref").toString()
        &&rerollPanel.providerOfferRound()==1&&!rerollPanel.awaitingReroll,"original_offer_available_before_reroll");
    const auto initialRevision=rerollPanel.offerRevision;
    auto pending=initial;pending["kind"]="reroll_pending";pending["is_reroll"]=true;
    pending["candidates"]=QJsonArray{};pending["source_offset"]=200;pending["offer_source_ref"]=QString(64,QLatin1Char('b'));
    auto wrongPending=pending;wrongPending["game_time"]=116.9;
    check(!admitted(wrongPending,s),"reroll_pending_requires_current_match_clock");
    wrongPending=pending;wrongPending["match_binding_source"]="inherited_recent_provider_context";
    check(!admitted(wrongPending,s),"reroll_pending_requires_observed_game_id");
    check(admitted(pending,s)&&rerollPanel.apply(pending,now)&&rerollPanel.candidates.isEmpty()
          &&rerollPanel.status==QStringLiteral("waiting")&&rerollPanel.awaitingReroll&&rerollPanel.offerRevision>initialRevision
          &&rerollPanel.offerSourceRef==pending.value("offer_source_ref").toString(),"bound_reroll_pending_revokes_old_cards");
    const auto pendingRevision=rerollPanel.offerRevision;
    auto replacement=initial;replacement["is_reroll"]=true;replacement["source_offset"]=300;
    replacement["offer_source_ref"]=QString(64,QLatin1Char('c'));
    check(rerollPanel.apply(replacement,now)&&rerollPanel.offerRevision>pendingRevision&&!rerollPanel.awaitingReroll
          &&rerollPanel.offerSourceRef==replacement.value("offer_source_ref").toString()
          &&rerollPanel.candidates==initial.value("candidates").toArray(),"identical_replacement_retains_its_own_source_generation");
    rerollPanel.clearLiveOffers();check(rerollPanel.offerSourceRef.isEmpty()&&!rerollPanel.awaitingReroll,"clearing_offer_drops_the_active_source_proof");
    wrong=e;wrong["match_ref"]=offerMatchRefForGameId(QStringLiteral("43"));
    check(!panel.apply(wrong,now)&&panel.candidates.size()==2,"invalid_event_does_not_replace_current_cards");
    auto clear=e;clear["kind"]="clear";clear["provider_clear_round"]=2;clear["match_binding_source"]="inherited_recent_provider_context";
    check(admitted(clear,s)&&panel.apply(clear,now)&&panel.candidates.isEmpty(),"same_match_inherited_clear_only_closes_offer");
    auto rewind=e;rewind["session_ref"]="session-2";rewind["pick_count"]=1;
    check(!panel.apply(rewind,now),"capture_restart_cannot_reset_current_match_watermark");
    panel.clearLiveOffers();check(!panel.apply(rewind,now),"file_reset_cannot_reset_resolved_round");
    panel.bindLiveMatch("local-context-b",offerMatchRefForGameId(QStringLiteral("43")));
    check(!panel.liveBoundTo("local-context-a")&&panel.candidates.isEmpty()&&panel.seenCandidates.isEmpty(),"match_change_clears_old_offer_state");
    panel.bindLiveMatch({},{});check(!panel.liveBoundTo("local-context-b")&&!panel.apply(e,now),"revocation_blocks_reinjection");
    PanelState replay(true);e["capture_mode"]="replay";e.remove("match_ref");e["observed_at"]=now.addDays(-1).toString(Qt::ISODateWithMs);
    check(replay.apply(e,now),"explicit_historical_replay_remains_supported");
    check(!QJsonDocument(panel.json()).toJson().contains("game_id"),"public_state_does_not_export_private_game_id");
    if(argc>1) {
        QFile file(QString::fromLocal8Bit(argv[1]));
        const bool opened=file.open(QIODevice::ReadOnly)&&file.size()<=65536;
        const auto fixture=opened?QJsonDocument::fromJson(file.readAll()).object():QJsonObject{};
        const auto event=fixture.value("matching_event").toObject();
        const auto at=QDateTime::fromString(event.value("observed_at").toString(),Qt::ISODateWithMs);
        check(opened&&!fixture.isEmpty()&&at.isValid(),"python_capture_fixture_loaded");
        auto live=s;live.received=at;PanelState generated;generated.bindLiveMatch("local-context-a",ref);
        const bool accepted=liveOfferAdmission(event,live,"local-context-a",ref,"16.21",at).value("accepted").toBool()&&generated.apply(event,at);
        check(accepted&&generated.candidates.size()==2&&generated.candidates[0].toObject().value("augment_id")==1022,"python_capture_to_native_current_offer");
        check(!liveOfferAdmission(fixture.value("other_match_event").toObject(),live,"local-context-a",ref,"16.21",at).value("accepted").toBool(),"python_capture_same_session_other_match_rejected");
        const auto missing=fixture.value("unbound_event").toObject();
        check(!liveOfferAdmission(missing,live,"local-context-a",ref,"16.21",at).value("accepted").toBool()
            &&unboundOfferRevokesCurrent(missing,live,at,at),"python_capture_unknown_binding_withholds_old_offer");
    }
    std::printf("offer_match_binding_checks %d checks %d failures\n",checks,failures);return failures?1:0;
}
