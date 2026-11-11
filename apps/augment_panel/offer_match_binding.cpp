#include "offer_match_binding.h"
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

QString offerMatchRefForGameId(const QJsonValue &value) {
    QString canonical;
    if(value.isString()) {
        canonical=value.toString();
        static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
        if(canonical.isEmpty()||canonical.size()>128||!digits.match(canonical).hasMatch())return {};
        while(canonical.startsWith(QLatin1Char('0')))canonical.remove(0,1);
        const QString maximum=QStringLiteral("9223372036854775807");
        if(canonical.isEmpty()||canonical.size()>maximum.size()||(canonical.size()==maximum.size()&&canonical>maximum))return {};
    } else if(value.isDouble()) {
        const double number=value.toDouble();
        if(!std::isfinite(number)||number<=0||number>9007199254740991.||std::floor(number)!=number)return {};
        canonical=QString::number(number,'f',0);
    } else return {};
    const QByteArray domain("lixin.match_ref.v1\0",19);
    return QStringLiteral("match-v1-")+QString::fromLatin1(QCryptographicHash::hash(domain+canonical.toLatin1(),QCryptographicHash::Sha256).toHex());
}

bool validOfferMatchRef(const QString &value) {
    static const QRegularExpression format(QStringLiteral("^match-v1-[0-9a-f]{64}$"));
    return format.match(value).hasMatch();
}

bool liveOfferContextReady(const GameState &state,const QString &contextId,const QString &matchRef,const QString &modelPatch,QDateTime now) {
    const auto age=state.received.msecsTo(now);
    return state.source==GameState::Source::Live&&state.received.isValid()&&age>=0&&age<4000
        &&!contextId.isEmpty()&&contextId==state.matchContext.value("context_id").toString()
        &&validOfferMatchRef(matchRef)&&state.matchContext.value("active").toBool()
        &&state.matchContext.value("stage")==QStringLiteral("in_game")&&state.gameMode==QStringLiteral("KIWI")
        &&!modelPatch.isEmpty()&&std::isfinite(state.gameTime)&&state.gameTime>=0;
}

QJsonObject liveOfferAdmission(const QJsonObject &event,const GameState &state,const QString &contextId,const QString &matchRef,const QString &modelPatch,QDateTime now) {
    auto result=[](const char *reason,bool allowed=false){return QJsonObject{{"accepted",allowed},{"reason",QString::fromLatin1(reason)}};};
    if(!liveOfferContextReady(state,contextId,matchRef,modelPatch,now))return result("current_match_binding_not_ready");
    if(event.value("match_binding_schema")!=QStringLiteral("lixin.match_ref.v1")
        ||event.value("match_ref_scope")!=QStringLiteral("provider_game_id")
        ||event.value("match_binding_status")!=QStringLiteral("known")
        ||event.value("match_ref").toString()!=matchRef)return result("offer_match_not_verified");
    const auto source=event.value("match_binding_source").toString();
    if(source!=QStringLiteral("observed_provider_game_id")&&source!=QStringLiteral("inherited_recent_provider_context"))return result("offer_binding_source_unknown");
    const auto kind=event.value("kind").toString();
    if(kind==QStringLiteral("offer")||kind==QStringLiteral("reroll_pending")||kind==QStringLiteral("headliner_context")) {
        if(source!=QStringLiteral("observed_provider_game_id"))return result("offer_requires_observed_game_id");
        const auto clock=event.value("game_time");
        if(!clock.isDouble()||!std::isfinite(clock.toDouble())||clock.toDouble()<0||std::abs(clock.toDouble()-state.gameTime)>3.)return result("offer_game_clock_not_aligned");
        const auto stamp=QDateTime::fromString(event.value("observed_at").toString(),Qt::ISODateWithMs);
        if(!stamp.isValid()||std::abs(stamp.msecsTo(state.received))>3000)return result("offer_state_wall_time_not_aligned");
        const auto hero=event.value("champion_id");
        if(!hero.isNull()&&!hero.isUndefined()&&(!hero.isDouble()||hero.toDouble()!=state.championId))return result("offer_champion_mismatch");
    }
    return result("matched_current_game",true);
}

bool unboundOfferRevokesCurrent(const QJsonObject &event,const GameState &state,QDateTime priorOffer,QDateTime now) {
    if(event.value("schema_version")!=1||event.value("kind")!="offer"||event.value("source")!="wegame_local_log"
        ||event.value("capture_mode")!="live"||event.value("game_mode")!="KIWI"
        ||event.value("match_binding_schema")!="lixin.match_ref.v1"||event.value("match_ref_scope")!="provider_game_id"
        ||event.value("match_binding_status")!="unknown"||!event.value("match_ref").isNull())return false;
    const auto stamp=QDateTime::fromString(event.value("observed_at").toString(),Qt::ISODateWithMs);
    const auto age=state.received.msecsTo(now);
    if(state.source!=GameState::Source::Live||!state.received.isValid()||age<0||age>=4000||!stamp.isValid()
        ||std::abs(stamp.msecsTo(state.received))>3000||(priorOffer.isValid()&&stamp<priorOffer))return false;
    const auto clock=event.value("game_time"),hero=event.value("champion_id"),round=event.value("pick_count");
    if(!clock.isDouble()||!std::isfinite(clock.toDouble())||clock.toDouble()<0||std::abs(clock.toDouble()-state.gameTime)>3.
        ||(!hero.isNull()&&!hero.isUndefined()&&(!hero.isDouble()||hero.toDouble()!=state.championId))
        ||!round.isDouble()||round.toDouble()!=round.toInt()||round.toInt()<1||round.toInt()>100)return false;
    if(!event.value("candidates").isArray())return false;
    const auto cards=event.value("candidates").toArray();if(cards.isEmpty()||cards.size()>8)return false;
    QSet<int> ids;
    for(const auto &v:cards){const auto card=v.toObject();const auto id=card.value("augment_id");const auto name=card.value("augment_name").toString();
        if(!id.isDouble()||id.toDouble()!=id.toInt()||id.toInt()<1||id.toInt()>1000000||ids.contains(id.toInt())
            ||name.isEmpty()||name.size()>128||name.contains(QChar('\n')))return false;ids.insert(id.toInt());}
    return true;
}
