#include "live_client.h"
#include <QRegularExpression>

void LiveClient::probeAugmentCapabilities(std::function<void(QJsonObject)> callback) {
    discover();
    QJsonObject report{{QStringLiteral("checked_at"),QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("metadata_only"),true},{QStringLiteral("game_api_requests"),0},{QStringLiteral("credential_persisted"),false}};
    if(!connection.valid()){report.insert(QStringLiteral("error"),connection.diagnostic);callback(report);return;}
    struct Inventory {QJsonObject report;int pending=2;};auto inventory=std::make_shared<Inventory>();inventory->report=report;
    const auto base=QStringLiteral("https://127.0.0.1:%1").arg(connection.port);
    for(const auto &path:{QStringLiteral("/help"),QStringLiteral("/swagger/v2/swagger.json")})
        get(QUrl(base+path),true,[inventory,path,callback](QJsonValue data,QString error,int status){
            QJsonObject row{{QStringLiteral("endpoint"),path},{QStringLiteral("http_status"),status},
                {QStringLiteral("parsed"),data.isObject()||data.isArray()}};
            if(!error.isEmpty())row.insert(QStringLiteral("error"),error);
            row.insert(QStringLiteral("top_level_keys"),QJsonArray::fromStringList(data.toObject().keys()));
            QSet<QString> paths,descriptors,allPaths;int nodes=0;bool truncated=false;
            const QRegularExpression relevant(QStringLiteral("augment|hextech|kiwi|cherry"),QRegularExpression::CaseInsensitiveOption);
            auto candidate=[&](const QString &s){if(s.size()>=300)return;
                if(s.startsWith(QChar(u'/')))allPaths.insert(s);
                if(!relevant.match(s).hasMatch())return;
                if(s.startsWith(QChar(u'/')))paths.insert(s);else descriptors.insert(s);
            };
            std::function<void(QJsonValue,int)> scan=[&](QJsonValue value,int depth){
                if(++nodes>100000||depth>30){truncated=true;return;}
                if(value.isString())candidate(value.toString());
                else if(value.isArray())for(const auto &v:value.toArray())scan(v,depth+1);
                else if(value.isObject()){const auto object=value.toObject();for(auto it=object.begin();it!=object.end();++it){candidate(it.key());scan(it.value(),depth+1);}}
            };scan(data,0);auto sorted=paths.values();sorted.sort();
            row.insert(QStringLiteral("augment_related_paths"),QJsonArray::fromStringList(sorted));
            auto names=descriptors.values();names.sort();
            row.insert(QStringLiteral("related_metadata_names"),QJsonArray::fromStringList(names));
            row.insert(QStringLiteral("listed_path_count"),allPaths.size());
            row.insert(QStringLiteral("scan_truncated"),truncated);
            auto rows=inventory->report.value(QStringLiteral("requests")).toArray();rows.append(row);inventory->report.insert(QStringLiteral("requests"),rows);
            if(--inventory->pending==0)callback(inventory->report);
        });
}

void LiveClient::probeCustomGame(std::function<void(QJsonObject)> callback) {
    discover();
    QJsonObject initial{{QStringLiteral("checked_at"),QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("credential_persisted"),false},{QStringLiteral("strict_tls"),true},
        {QStringLiteral("game_api_requests"),0}};
    if (!connection.valid()) { initial.insert(QStringLiteral("error"),connection.diagnostic); callback(initial); return; }
    const auto base=QStringLiteral("https://127.0.0.1:%1").arg(connection.port);
    const auto generation=credentialGeneration;
    get(QUrl(base+QStringLiteral("/lol-gameflow/v1/gameflow-phase")),true,
        [this,base,generation,initial,callback](QJsonValue phaseData,QString error,int status) mutable {
        initial.insert(QStringLiteral("phase_http_status"),status);
        if (!error.isEmpty()||status!=200||!phaseData.isString()) { initial.insert(QStringLiteral("error"),error);callback(initial);return; }
        const auto observedPhase=phaseData.toString(); initial.insert(QStringLiteral("phase"),observedPhase);
        const auto path=observedPhase==QStringLiteral("Lobby")?QStringLiteral("/lol-lobby/v2/lobby"):QStringLiteral("/lol-gameflow/v1/session");
        get(QUrl(base+path),true,[this,generation,initial,callback,observedPhase,path](QJsonValue value,QString e,int code) mutable {
            initial.insert(QStringLiteral("session_endpoint"),path);initial.insert(QStringLiteral("session_http_status"),code);
            if (!e.isEmpty()||code!=200||generation!=credentialGeneration) {initial.insert(QStringLiteral("error"),e.isEmpty()?QStringLiteral("session_changed"):e);callback(initial);return;}
            const auto object=value.toObject();
            const auto game=object.value(QStringLiteral("gameData")).toObject();
            const auto config=object.value(QStringLiteral("gameConfig")).toObject();
            const auto queueData=game.value(QStringLiteral("queue")).toObject();
            if(observedPhase==QStringLiteral("Lobby")) {
                const auto member=object.value(QStringLiteral("localMember")).toObject();
                initial.insert(QStringLiteral("lobby_capabilities"),QJsonObject{
                    {QStringLiteral("can_start_activity"),object.value(QStringLiteral("canStartActivity"))},
                    {QStringLiteral("member_count"),object.value(QStringLiteral("members")).toArray().size()},
                    {QStringLiteral("spectator_count"),object.value(QStringLiteral("gameConfig")).toObject().value(QStringLiteral("customSpectators")).toArray().size()},
                    {QStringLiteral("local_is_leader"),member.value(QStringLiteral("isLeader"))},
                    {QStringLiteral("game_type_config_id"),config.value(QStringLiteral("gameTypeConfigId"))}});
            }
            const bool custom=config.value(QStringLiteral("isCustom")).toBool()||game.value(QStringLiteral("isCustomGame")).toBool();
            const int queueId=config.value(QStringLiteral("queueId")).toInt(queueData.value(QStringLiteral("id")).toInt());
            const int mapId=config.value(QStringLiteral("mapId")).toInt(queueData.value(QStringLiteral("mapId")).toInt());
            const auto mode=config.value(QStringLiteral("gameMode")).toString(queueData.value(QStringLiteral("gameMode")).toString());
            initial.insert(QStringLiteral("custom_game"),custom); initial.insert(QStringLiteral("queue_id"),queueId);
            initial.insert(QStringLiteral("map_id"),mapId); initial.insert(QStringLiteral("game_mode"),mode);
            const bool allowed=custom&&queueId==3270&&mapId==12&&mode==QStringLiteral("KIWI");
            initial.insert(QStringLiteral("custom_kiwi_confirmed"),allowed);
            if (!allowed||(observedPhase!=QStringLiteral("InProgress")&&observedPhase!=QStringLiteral("Reconnect"))) {callback(initial);return;}
            initial.insert(QStringLiteral("game_api_requests"),1);
            get(QUrl(QStringLiteral("https://127.0.0.1:2999/liveclientdata/allgamedata")),false,
                [this,generation,initial,callback](QJsonValue payload,QString liveError,int liveCode) mutable {
                initial.insert(QStringLiteral("live_http_status"),liveCode);
                if (!liveError.isEmpty()||liveCode!=200||generation!=credentialGeneration) {initial.insert(QStringLiteral("error"),liveError.isEmpty()?QStringLiteral("session_changed"):liveError);callback(initial);return;}
                const auto data=payload.toObject();const auto state=parseLiveData(data,catalog);
                initial.insert(QStringLiteral("live_verified"),state.source==GameState::Source::Live);
                initial.insert(QStringLiteral("champion_id"),state.championId);initial.insert(QStringLiteral("level"),state.level);
                initial.insert(QStringLiteral("game_time"),state.gameTime);initial.insert(QStringLiteral("gold"),state.gold);
                initial.insert(QStringLiteral("combat_stats"),state.liveStats); initial.insert(QStringLiteral("players"),state.players);
                QJsonArray items;for(int id:state.owned)items.append(id);initial.insert(QStringLiteral("owned_items"),items);
                QJsonObject ranks;for(const auto &slot:{QStringLiteral("Q"),QStringLiteral("W"),QStringLiteral("E"),QStringLiteral("R")})
                    ranks.insert(slot,state.abilities.value(slot).toObject().value(QStringLiteral("abilityLevel")));
                initial.insert(QStringLiteral("ability_ranks"),ranks);
                const auto gameData=data.value(QStringLiteral("gameData")).toObject();
                QJsonObject publicGame;for(const auto &key:{QStringLiteral("gameMode"),QStringLiteral("mapName"),QStringLiteral("mapNumber"),QStringLiteral("mapTerrain")})publicGame.insert(key,gameData.value(key));
                initial.insert(QStringLiteral("public_game_data"),publicGame);
                // Retain field names only, never a raw payload or player identity.
                QJsonArray fields;auto scan=[&fields](const QJsonObject &o,const QString &prefix){for(auto it=o.begin();it!=o.end();++it)
                    if(it.key().contains(QStringLiteral("augment"),Qt::CaseInsensitive)||it.key().contains(QStringLiteral("hextech"),Qt::CaseInsensitive))fields.append(prefix+it.key());};
                scan(data,QString());scan(data.value(QStringLiteral("activePlayer")).toObject(),QStringLiteral("activePlayer."));
                for(const auto &player:data.value(QStringLiteral("allPlayers")).toArray())scan(player.toObject(),QStringLiteral("allPlayers[]."));
                initial.insert(QStringLiteral("augment_related_field_names"),fields);callback(initial);
            });
        });
    });
}
