#include "corner_combat_hud.h"
#include "analysis.h"
#include "hud_assets.h"
#include "hud_symbols.h"
#include "kda_hud_style.h"
#include "kda_surface.h"
#include <QFontMetrics>
#include <QPainterPath>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
const QColor allyInk("#9edbe8"),enemyInk("#e1c9ff");
const QJsonValue unknown(QJsonValue::Null);
const std::array<QString,5> axisNames{QStringLiteral("输出"),QStringLiteral("装备"),QStringLiteral("参团"),QStringLiteral("少阵亡"),QStringLiteral("承伤")};

QJsonValue amount(const QJsonValue &value){
    return value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0?value:unknown;
}
QString countText(const QJsonValue &value){
    return value.isDouble()?QString::number(value.toInt()):QStringLiteral("—");
}
QString damageText(const QJsonValue &value){
    if(!value.isDouble())return QStringLiteral("—");
    const double damage=value.toDouble();
    return damage>=1000?QString::number(damage/1000.,'f',1)+QStringLiteral("k"):QString::number(damage,'f',0);
}
int uniqueLeader(const QJsonArray &rows,const char *field){
    int leader=-1;
    double highest=0;
    for(int i=0;i<rows.size();++i){
        const auto value=rows.at(i).toObject().value(field);
        if(!value.isDouble())return -1;
        const double score=value.toDouble();
        if(score>highest){highest=score;leader=i;}
        else if(score==highest)leader=-1;
    }
    return leader;
}
QJsonArray scaledAxes(const QJsonArray &values,const QJsonArray &maxima){
    QJsonArray result;
    for(int i=0;i<5;++i){
        const auto v=values.at(i);
        const double maximum=maxima.at(i).toDouble();
        result.append(!v.isDouble()?unknown:i==3?QJsonValue(maximum>0?1.-v.toDouble()/maximum:1.)
                       :QJsonValue(maximum>0?v.toDouble()/maximum:0.));
    }
    return result;
}
void paintRadar(QPainter &p,const QRectF &box,const QJsonArray &own,const QJsonArray &enemy){
    const auto center=box.center()+QPointF(0,-1);
    const qreal radius=std::clamp(std::min((box.width()-48.)/2.9,(box.height()-23.)/2.9),22.,36.);
    auto point=[&](int i,qreal ratio){
        const double angle=6.283185307179586*i/5.-1.570796326794897;
        return center+QPointF(std::cos(angle)*radius*ratio,std::sin(angle)*radius*ratio);
    };
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Kda::HudStyle::neutral(40),.8));
    for(qreal fraction:{.33,.66,1.}){
        QPolygonF grid;
        for(int i=0;i<5;++i)grid.append(point(i,fraction));
        p.drawPolygon(grid);
    }
    for(int i=0;i<5;++i)p.drawLine(center,point(i,1));
    auto fill=[&](const QJsonArray &values,QColor color){
        QPolygonF polygon;
        for(int i=0;i<5;++i){
            if(!values.at(i).isDouble())return;
            polygon.append(point(i,values.at(i).toDouble()));
        }
        color.setAlpha(29);
        p.setPen(Qt::NoPen);p.setBrush(color);p.drawPolygon(polygon);
    };
    fill(own,allyInk);
    fill(enemy,enemyInk);
    auto series=[&](const QJsonArray &values,QColor color,Qt::PenStyle style){
        p.setPen(QPen(color,1.55,style));
        for(int i=0;i<5;++i){
            const int next=(i+1)%5;
            if(values.at(i).isDouble()&&values.at(next).isDouble())
                p.drawLine(point(i,values.at(i).toDouble()),point(next,values.at(next).toDouble()));
        }
        p.setPen(Qt::NoPen);p.setBrush(color);
        for(int i=0;i<5;++i)if(values.at(i).isDouble())p.drawEllipse(point(i,values.at(i).toDouble()),2,2);
    };
    series(enemy,enemyInk,Qt::SolidLine);
    series(own,allyInk,Qt::DashLine);
    p.setFont(Kda::hudFont(Kda::FontRole::Body,11));
    for(int i=0;i<5;++i){
        const auto anchor=point(i,1.45);
        const bool known=own.at(i).isDouble()&&enemy.at(i).isDouble();
        p.setPen(known?Kda::HudStyle::color("text"):Kda::HudStyle::color("muted"));
        p.drawText(QRectF(anchor-QPointF(31,7),QSizeF(62,14)),Qt::AlignCenter,
                   axisNames[i]+(known?QString():QStringLiteral(" —")));
    }
}
}

CornerCombatHud::CornerCombatHud(HudAssets *source)
    : QWidget(nullptr,Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint
              |Qt::WindowDoesNotAcceptFocus|Qt::WindowTransparentForInput),assets(source){
    setWindowTitle(QStringLiteral("李金金 · 本局战况"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    Kda::attachOverlayBackdrop(this);
}

void CornerCombatHud::setCombatState(const Catalog &catalog,const GameState &game,const QJsonObject &summary,bool demo){
    auto project=[&]() -> QJsonObject {
        if(!game.fresh()||game.players.size()!=10||game.gameMode!=QStringLiteral("KIWI"))return {};
        const bool sample=demo&&game.source==GameState::Source::Replay;
        if(!sample&&(!game.matchContext.value("active").toBool()
                    ||game.matchContext.value("stage")!=QStringLiteral("in_game")))return {};
        int self=-1;
        for(int i=0;i<game.players.size();++i)if(game.players.at(i).toObject().value("self")==QJsonValue(true)){
            if(self>=0)return {};
            self=i;
        }
        if(self<0)return {};
        const auto own=game.players.at(self).toObject();
        const auto side=own.value("team");
        if(side!=QStringLiteral("ORDER")&&side!=QStringLiteral("CHAOS"))return {};
        QList<int> allies{self},enemies;
        QHash<QString,double> teamKills;
        QHash<QString,int> knownTeamKills;
        for(int i=0;i<game.players.size();++i){
            const auto player=game.players.at(i).toObject();
            const auto team=player.value("team").toString();
            if(player.value("champion_id").toInt()<=0||(team!=QStringLiteral("ORDER")&&team!=QStringLiteral("CHAOS")))return {};
            if(i!=self)(team==side.toString()?allies:enemies).append(i);
            const auto kills=amount(player.value("scores").toObject().value("kills"));
            if(kills.isDouble()){teamKills[team]+=kills.toDouble();++knownTeamKills[team];}
        }
        if(allies.size()!=5||enemies.size()!=5)return {};
        // Stable order across score updates, even if the local API reorders its roster.
        auto championOrder=[&](int a,int b){return game.players.at(a).toObject().value("champion_id").toInt()
                                                   <game.players.at(b).toObject().value("champion_id").toInt();};
        std::stable_sort(allies.begin()+1,allies.end(),championOrder);
        std::stable_sort(enemies.begin(),enemies.end(),championOrder);

        const auto modeled=summary.value("players").toArray();
        const bool sameMatch=summary.value("context_id")==game.matchContext.value("context_id");
        const bool actualDamage=sameMatch&&summary.value("actual_damage_available")==QJsonValue(true);
        QJsonArray allyRows,enemyRows;
        QJsonArray maxima{0,0,0,0,0};
        int ownDeaths=0,knownRows=0;
        const int samplePairs[5][5]={{1,2,0,1,1},{1,0,2,0,0},{2,0,1,0,1},{4,1,0,1,0},{0,0,0,1,0}};
        const int sampleDamage[]{8900,11600,10200,18400,2800};
        const QString sampleRoles[]{QStringLiteral("开团"),QStringLiteral("切后"),QStringLiteral("远程"),QStringLiteral("主输出"),QStringLiteral("保护")};
        auto combatValues=[&](int index,int sampleEnemy,QJsonObject &model){
            const auto row=game.players.at(index).toObject();
            const auto scores=row.value("scores").toObject();
            const auto team=row.value("team").toString();
            if(sameMatch&&index<modeled.size()){
                const auto candidate=modeled.at(index).toObject();
                if(candidate.value("row_index").toInt(-1)==index&&candidate.value("team")==row.value("team")
                   &&candidate.value("champion_id")==row.value("champion_id"))model=candidate;
            }
            auto damage=actualDamage?amount(model.value("damage_to_champions")):unknown;
            auto taken=actualDamage?amount(model.value("damage_taken")):unknown;
            if(sample){
                damage=sampleEnemy>=0?sampleDamage[sampleEnemy]:12700-index*1100;
                taken=6800+index*720;
            }
            const auto kills=amount(scores.value("kills")),assists=amount(scores.value("assists"));
            const double total=teamKills.value(team);
            QJsonValue participation=unknown;
            if(knownTeamKills.value(team)==5&&total>0&&kills.isDouble()&&assists.isDouble()&&kills.toDouble()+assists.toDouble()<=total)
                participation=(kills.toDouble()+assists.toDouble())/total;
            QJsonValue equipment=amount(model.value("equipment_value"));
            if(sample&&!equipment.isDouble())equipment=4200+index*490;
            auto deaths=amount(scores.value("deaths"));
            if(sample&&sampleEnemy<0){
                const int column=allies.indexOf(index);
                int observed=0;
                for(const auto &pair:samplePairs)observed+=pair[column];
                deaths=observed;
            }
            QJsonArray values{damage,equipment,participation,deaths,taken};
            for(int i=0;i<5;++i)if(values.at(i).toDouble()>maxima.at(i).toDouble())maxima[i]=values.at(i);
            return values;
        };
        for(int index:allies){
            const auto player=game.players.at(index).toObject();
            const int champion=player.value("champion_id").toInt();
            QJsonObject model;
            allyRows.append(QJsonObject{{"champion",champion},{"name",catalog.championName(champion)},
                                       {"axes",combatValues(index,-1,model)}});
        }
        for(int i=0;i<enemies.size();++i){
            const int index=enemies.at(i);
            const auto player=game.players.at(index).toObject();
            const int champion=player.value("champion_id").toInt();
            const auto targets=player.value("kills_by_roster_row").toArray();
            QJsonArray kills;
            for(int column=0;column<allies.size();++column)
                kills.append(sample?QJsonValue(samplePairs[i][column]):targets.size()==10?amount(targets.at(allies.at(column))):unknown);
            const auto selfKills=kills.first();
            if(selfKills.isDouble()){++knownRows;ownDeaths+=selfKills.toInt();}
            QJsonObject model;
            const auto axes=combatValues(index,i,model);
            const auto caps=model.value("tactical_role").toObject().value("capabilities").toObject();
            const auto role=sample?sampleRoles[i]:caps.value("engage").toBool()?QStringLiteral("开团")
                           :caps.value("support").toBool()||caps.value("peel").toBool()?QStringLiteral("保护"):QString();
            enemyRows.append(QJsonObject{{"champion",champion},{"name",catalog.championName(champion)},{"pairs",kills},
                {"self_kills",selfKills},{"kills",amount(player.value("scores").toObject().value("kills"))},
                {"damage",axes.first()},{"role",role},{"axes",axes}});
        }
        const int outputLead=uniqueLeader(enemyRows,"damage"),selfLead=uniqueLeader(enemyRows,"self_kills");
        const int killLead=uniqueLeader(enemyRows,"kills");
        const int focus=selfLead>=0?selfLead:outputLead>=0?outputLead:killLead;
        if(outputLead>=0){auto row=enemyRows.at(outputLead).toObject();row["role"]=QStringLiteral("主输出");enemyRows[outputLead]=row;}
        if(selfLead>=0&&selfLead!=outputLead){
            auto row=enemyRows.at(selfLead).toObject();
            if(row.value("role").toString().isEmpty())row["role"]=QStringLiteral("击杀我");
            enemyRows[selfLead]=row;
        }
        if(focus>=0&&focus!=selfLead&&focus!=outputLead){
            auto row=enemyRows.at(focus).toObject();
            if(row.value("role").toString().isEmpty())row["role"]=QStringLiteral("击杀多");
            enemyRows[focus]=row;
        }
        const auto safeClock=std::isfinite(game.gameTime)?std::clamp(game.gameTime,0.,86400.):0.;
        QJsonObject result{{"available",true},{"synthetic",sample},{"allies",allyRows},{"enemies",enemyRows},
            {"self_dead",own.value("is_dead")==QJsonValue(true)},{"focus",focus},{"self_lead",selfLead},
            {"known_rows",knownRows},{"own_enemy_kills",knownRows==5?QJsonValue(ownDeaths):unknown},
            {"actual_damage_available",actualDamage},{"game_seconds",int(safeClock)}};
        if(focus>=0){
            result["own_axes"]=scaledAxes(allyRows.first().toObject().value("axes").toArray(),maxima);
            result["enemy_axes"]=scaledAxes(enemyRows.at(focus).toObject().value("axes").toArray(),maxima);
        }
        return result;
    };
    const auto next=project();
    if(next==snapshot)return;
    snapshot=next;
    setAccessibleName(ready()?QStringLiteral("本局战况，敌方击杀己方次数，已核对 %1 行").arg(snapshot.value("known_rows").toInt())
                             :QStringLiteral("本局战况未就绪"));
    update();
}

void CornerCombatHud::present(const QRect &bounds,bool details,double opacity){
    if(bounds.isEmpty()){hide();return;}
    if(expanded!=details){expanded=details;update();}
    if(geometry()!=bounds)setGeometry(bounds);
    if(std::abs(windowOpacity()-opacity)>.01)setWindowOpacity(opacity);
    if(!isVisible())show();
}

void CornerCombatHud::paintEvent(QPaintEvent *){
    if(!ready())return;
    Kda::OverlayCanvas canvas(this);
    auto &p=canvas.paint();
    const qreal zoom=std::min(double(height())/(expanded?expandedHeight:compactHeight),double(width())/172.);
    p.scale(zoom,zoom);
    const int logicalWidth=qRound(width()/zoom);
    Kda::HudStyle::paintSurface(p,QRectF(0,0,width()/zoom,height()/zoom).adjusted(.6,.6,-.6,-.6));

    const auto ink=Kda::HudStyle::color("text"),muted=Kda::HudStyle::color("muted");
    const bool tight=logicalWidth<225,showRoles=logicalWidth>=295;
    const int left=tight?8:10,right=logicalWidth-left;
    const int damageWidth=tight?34:43,roleWidth=showRoles?50:16;
    const int namesWidth=logicalWidth>=250?72:62;
    const int gridLeft=left+(tight?24:namesWidth),damageLeft=right-damageWidth-roleWidth;
    const qreal cell=(damageLeft-gridLeft)/5.;
    constexpr int rowsTop=60,rowHeight=20,rowsBottom=rowsTop+5*rowHeight;
    constexpr int columnHeaderTop=rowsTop-19,gridHeaderTop=columnHeaderTop-15;
    const auto allies=snapshot.value("allies").toArray(),enemies=snapshot.value("enemies").toArray();
    const int focus=snapshot.value("focus").toInt(-1),selfLead=snapshot.value("self_lead").toInt(-1);
    auto text=[&](const QRectF &box,const QString &label,const QColor &color,int alignment=Qt::AlignCenter){
        p.setPen(color);
        p.drawText(box,alignment|Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(label,Qt::ElideRight,int(box.width())));
    };
    auto plate=[&](const QRectF &box,const QColor &color,qreal radius=4){
        p.setPen(Qt::NoPen);p.setBrush(color);p.drawRoundedRect(box,radius,radius);
    };
    auto portrait=[&](const QRectF &box,const QJsonObject &hero,bool self=false){
        const auto icon=assets->champion(hero.value("champion").toInt());
        if(!icon.isNull())p.drawPixmap(box.topLeft(),Kda::roundedPixmap(icon,box.size().toSize(),3,devicePixelRatioF()));
        else text(box,hero.value("name").toString().left(1),muted);
        if(self){
            p.setBrush(Qt::NoBrush);p.setPen(QPen(allyInk,1));
            p.drawRoundedRect(box,3,3);
        }
    };

    p.setFont(Kda::hudFont(Kda::FontRole::Body,15,QFont::DemiBold));
    text(QRectF(left,2,84,23),QStringLiteral("本局战况"),ink,Qt::AlignLeft);
    const int seconds=snapshot.value("game_seconds").toInt();
    const auto clock=QStringLiteral("%1:%2").arg(seconds/60).arg(seconds%60,2,10,QChar('0'));
    const auto timestamp=(snapshot.value("synthetic").toBool()?QStringLiteral("示例  "):QString())+clock;
    p.setFont(Kda::hudFont(Kda::FontRole::Body,11));
    text(QRectF(right-76,2,76,23),timestamp,muted,Qt::AlignRight);

    plate(QRectF(gridLeft,gridHeaderTop+2,cell,rowsBottom-gridHeaderTop-2),QColor(158,219,232,16));
    p.setFont(Kda::hudFont(Kda::FontRole::Body,10,QFont::DemiBold));
    text(QRectF(left,columnHeaderTop,gridLeft-left-2,17),QStringLiteral("敌方"),muted,Qt::AlignLeft);
    text(QRectF(gridLeft,gridHeaderTop,cell,15),QStringLiteral("我"),allyInk);
    text(QRectF(gridLeft+cell,gridHeaderTop,4*cell,15),QStringLiteral("击杀己方"),muted);
    text(QRectF(damageLeft,columnHeaderTop,damageWidth-2,17),QStringLiteral("输出"),muted,Qt::AlignRight);
    if(showRoles)
        text(QRectF(right-roleWidth+5,columnHeaderTop,roleWidth-5,17),QStringLiteral("作用"),muted,Qt::AlignLeft);
    const int allyPortraitSize=cell<19?14:17;
    for(int i=0;i<5;++i)
        portrait(QRectF(gridLeft+cell*(i+.5)-allyPortraitSize/2.,columnHeaderTop,allyPortraitSize,allyPortraitSize),
                 allies.at(i).toObject(),i==0);

    p.setPen(QPen(Kda::HudStyle::neutral(35),.8));
    p.drawLine(QPointF(left,rowsTop-.5),QPointF(right,rowsTop-.5));
    const int outputLead=uniqueLeader(enemies,"damage");
    for(int i=0;i<5;++i){
        const auto row=enemies.at(i).toObject();
        const auto pairs=row.value("pairs").toArray();
        const int top=rowsTop+i*rowHeight;
        if(i==focus)
            plate(QRectF(left-3,top+1,right-left+6,rowHeight-2),QColor(190,164,255,20));
        p.setFont(Kda::hudFont(Kda::FontRole::Body,11,QFont::DemiBold));
        portrait(QRectF(left,top+1,18,18),row);
        if(!tight)
            text(QRectF(left+24,top,gridLeft-left-28,rowHeight),row.value("name").toString(),ink,Qt::AlignLeft);

        for(int j=0;j<5;++j){
            const auto count=pairs.at(j);
            const auto tile=QRectF(gridLeft+cell*j+1,top+1,cell-2,rowHeight-2);
            const bool isSelfThreat=i==selfLead&&j==0;
            if(isSelfThreat)plate(tile,allyInk);
            p.setFont(Kda::hudFont(Kda::FontRole::Numeric,tight?12:13,isSelfThreat||count.toInt()>0?QFont::DemiBold:QFont::Normal));
            text(tile,countText(count),isSelfThreat?QColor("#15282d"):count.toInt()>0?ink:muted);
        }

        const auto damage=row.value("damage");
        p.setFont(Kda::hudFont(Kda::FontRole::Numeric,12,i==outputLead?QFont::DemiBold:QFont::Normal));
        text(QRectF(damageLeft,top,damageWidth-2,rowHeight),damageText(damage),
             !damage.isDouble()?muted:i==outputLead?enemyInk:ink,Qt::AlignRight);
        const auto role=row.value("role").toString();
        if(role.isEmpty())continue;
        const auto roleColor=i==focus||i==outputLead?enemyInk:muted;
        const auto symbol=role==QStringLiteral("保护")?HudSymbols::Symbol::Shield:
                          role==QStringLiteral("开团")?HudSymbols::Symbol::Team:
                          role==QStringLiteral("主输出")?HudSymbols::Symbol::Star:
                          role==QStringLiteral("远程")?HudSymbols::Symbol::Chevron:HudSymbols::Symbol::Sword;
        HudSymbols::draw(p,symbol,QRectF(right-roleWidth+4,top+4,12,12),roleColor);
        if(showRoles){
            p.setFont(Kda::hudFont(Kda::FontRole::Body,10));
            text(QRectF(right-roleWidth+19,top,roleWidth-19,rowHeight),role,roleColor,Qt::AlignLeft);
        }
    }

    const QRectF footer(left,rowsBottom+6,right-left,22);
    plate(footer,Kda::HudStyle::color("surface"));
    const auto ownDeaths=snapshot.value("own_enemy_kills");
    p.setFont(Kda::hudFont(Kda::FontRole::Body,11));
    const auto total=ownDeaths.isDouble()?QStringLiteral("我被击杀 %1 次").arg(ownDeaths.toInt())
        :QStringLiteral("击杀已核对 %1/5").arg(snapshot.value("known_rows").toInt());
    text(footer.adjusted(6,0,-6,0),total,allyInk,Qt::AlignLeft);
    if(selfLead>=0){
        const auto leader=enemies.at(selfLead).toObject();
        const auto count=QStringLiteral("×%1").arg(leader.value("self_kills").toInt());
        const auto label=tight?count:leader.value("name").toString()+QStringLiteral("  ")+count;
        const auto width=std::min(QFontMetrics(p.font()).horizontalAdvance(label)+2,tight?26:96);
        portrait(QRectF(right-26-width,footer.top()+3,15,15),leader);
        text(QRectF(right-width-6,footer.top(),width,footer.height()),label,allyInk,Qt::AlignRight);
    }

    if(!expanded||focus<0)return;
    p.setPen(QPen(Kda::HudStyle::neutral(34),.8));
    p.drawLine(QPointF(left,compactHeight+.5),QPointF(right,compactHeight+.5));
    p.setFont(Kda::hudFont(Kda::FontRole::Body,12,QFont::DemiBold));
    const int labelWidth=tight?(right-left)/2:91;
    text(QRectF(left,compactHeight+12,labelWidth,18),QStringLiteral("五维对照"),ink,Qt::AlignLeft);
    p.setFont(Kda::hudFont(Kda::FontRole::Body,11));
    auto legend=[&](int x,int y,const QJsonObject &hero,const QColor &color,Qt::PenStyle style){
        p.setPen(QPen(color,1.4,style));
        p.drawLine(QPointF(x,y+8),QPointF(x+12,y+8));
        text(QRectF(x+17,y,labelWidth-17,17),hero.value("name").toString(),color,Qt::AlignLeft);
    };
    legend(left,compactHeight+(tight?26:40),allies.first().toObject(),allyInk,Qt::DashLine);
    legend(tight?left+labelWidth:left,compactHeight+(tight?26:62),enemies.at(focus).toObject(),enemyInk,Qt::SolidLine);
    const auto radar=tight?QRectF(left,compactHeight+42,right-left,77)
                          :QRectF(left+labelWidth,compactHeight+2,right-left-labelWidth,117);
    paintRadar(p,radar,
               snapshot.value("own_axes").toArray(),snapshot.value("enemy_axes").toArray());
}

QJsonObject CornerCombatHud::report() const{
    auto value=snapshot;
    value["visible"]=isVisible();
    value["expanded"]=isVisible()&&expanded;
    value["geometry"]=QJsonArray{x(),y(),width(),height()};
    value["click_through"]=windowFlags().testFlag(Qt::WindowTransparentForInput);
    value["accepts_keyboard_focus"]=!windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus);
    value["entry_motion"]=QJsonObject{{"enabled",false},{"running",false}};
    return value;
}
