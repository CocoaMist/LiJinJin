#include "coach_replay.h"
#include "catalog.h"
#include "dashboard_chrome.h"
#include "hud_assets.h"
#include "hud_symbols.h"
#include "kda_controls.h"
#include "kda_glass_surface.h"
#include "kda_reading_surface.h"
#include <QApplication>
#include <QDir>
#include <QDynamicPropertyChangeEvent>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSet>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <utility>

namespace {
constexpr int Limit = 256;
constexpr qint64 ArchiveByteLimit = 1024 * 1024;
QString replayDocumentCss(){
    return QStringLiteral("body{font-family:'Inter','Noto Sans SC';font-size:13px;font-weight:400;color:%1;}h2{font-size:17px;margin:10px 0 5px;}p{margin:8px 0;}small{font-size:12px;color:%2;}td{padding:6px 3px;}hr{color:%3;} .value{color:%4;font-size:16px;}")
        .arg(Kda::color("text").name(),Kda::color("muted").name(),Kda::color("border").name(),Kda::color("ice").name());
}
void rethemeReplayDocument(QTextBrowser *browser,const QString &previousSkin){
    // Recolor the existing document rather than replacing its content or image
    // resources. Native selection and both scroll positions survive the switch.
    const auto selection=browser->textCursor();
    const int vertical=browser->verticalScrollBar()->value(),horizontal=browser->horizontalScrollBar()->value();
    struct Run{int position,length;QColor ink;};QList<Run> runs;
    for(auto block=browser->document()->begin();block.isValid();block=block.next()){
        for(auto it=block.begin();!it.atEnd();++it){
            const auto fragment=it.fragment();if(!fragment.isValid())continue;
            const auto brush=fragment.charFormat().foreground();if(brush.style()==Qt::NoBrush)continue;
            for(const char *role:{"text","muted","border","ice"}){
                if(brush.color().rgb()!=Kda::skinColor(previousSkin,role).rgb())continue;
                auto ink=Kda::color(role);ink.setAlpha(brush.color().alpha());runs.append({fragment.position(),fragment.length(),ink});break;
            }
        }
    }
    QTextCursor edit(browser->document());edit.beginEditBlock();
    for(const auto &run:runs){edit.setPosition(run.position);edit.setPosition(run.position+run.length,QTextCursor::KeepAnchor);QTextCharFormat format;format.setForeground(run.ink);edit.mergeCharFormat(format);}
    edit.endEditBlock();browser->document()->setDefaultStyleSheet(replayDocumentCss());browser->setTextCursor(selection);
    browser->verticalScrollBar()->setValue(vertical);browser->horizontalScrollBar()->setValue(horizontal);browser->viewport()->update();
}
class ReplayThemeObserver final:public QObject {
public:
    ReplayThemeObserver(QObject *owner,std::function<void(const QString &)> callback):QObject(owner),refresh(std::move(callback)),appliedSkin(Kda::skin()){if(qApp)qApp->installEventFilter(this);}
protected:
    bool eventFilter(QObject *watched,QEvent *event)override{
        if(watched==qApp&&event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinSkin"){
            const auto next=Kda::skin();if(next!=appliedSkin){const auto previous=appliedSkin;appliedSkin=next;refresh(previous);}
        }
        return QObject::eventFilter(watched,event);
    }
private:std::function<void(const QString &)> refresh;QString appliedSkin;
};
bool number(const QJsonValue &value, double minimum, double maximum) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= minimum && value.toDouble() <= maximum;
}
int identity(const QJsonValue &value) {
    return number(value, 1, 2147483647) && std::floor(value.toDouble()) == value.toDouble() ? value.toInt() : 0;
}
QString uuid(const QString &text) {
    const QUuid value(text);
    if (value.isNull()) return {};
    const auto plain = value.toString(QUuid::WithoutBraces);
    return text.compare(plain, Qt::CaseInsensitive) == 0 || text.compare(value.toString(), Qt::CaseInsensitive) == 0 ? plain : QString();
}
QByteArray signature(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QString clockText(double seconds) { const int value = int(seconds); return QStringLiteral("%1:%2").arg(value / 60, 2, 10, QChar('0')).arg(value % 60, 2, 10, QChar('0')); }
bool affirmative(const QJsonObject &object, const QString &key) { const auto value = object.value(key); return value.isBool() && value.toBool(); }
bool oneOf(const QJsonValue &value, const QSet<QString> &allowed) { return value.isString() && allowed.contains(value.toString()); }
const QString CurrentCardScore = QStringLiteral("local_current_gain_percent");
const QString DecisionCardScore = QStringLiteral("local_decision_gain_percent");
bool isLocalCardScore(const QJsonValue &source) {
    return source == CurrentCardScore || source == DecisionCardScore;
}
QString archivePath(const QString &directory, const QString &matchRef) {
    static const QRegularExpression ref(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    if (directory.isEmpty() || !ref.match(matchRef).hasMatch()) return {};
    return QDir(directory).filePath(matchRef + QStringLiteral(".json"));
}
bool validIds(const QJsonValue &value, int maxCount) {
    if (!value.isArray()) return false;
    const auto ids = value.toArray();
    return ids.size() <= maxCount && std::all_of(ids.cbegin(), ids.cend(), [](const auto &id){return identity(id) > 0;});
}
bool onlyKeys(const QJsonObject &data, std::initializer_list<const char *> allowed) {
    const auto keys=data.keys();
    return std::all_of(keys.cbegin(),keys.cend(),[allowed](const auto &key){
        return std::any_of(allowed.begin(),allowed.end(),[&key](const auto name){return key==QLatin1StringView(name);});
    });
}
QJsonObject purchaseSandbox(const Catalog *catalog,const QJsonObject &input,int selected);
bool validArchive(const QJsonObject &data) {
    static const QRegularExpression patchPattern(QStringLiteral("\\A[0-9]{1,3}\\.[0-9]{1,3}(?:\\.[0-9]{1,6})?\\z"));
    static const QSet<QString> kinds{QStringLiteral("purchase"),QStringLiteral("cards"),QStringLiteral("selected"),
        QStringLiteral("equipment"),QStringLiteral("inventory")};
    if (data.size() != 3 || data.value("schema_version") != QJsonValue(1) ||
        !number(data.value("last_time"), 0, 86400) || !data.value("entries").isArray()) return false;
    const auto entries = data.value("entries").toArray();
    if (entries.isEmpty() || entries.size() > Limit) return false;
    for (const auto &value : entries) {
        const auto row = value.toObject();
        if (row.size() != 6 || row.value("synthetic") != QJsonValue(false) ||
            !number(row.value("game_time"), 0, data.value("last_time").toDouble()) || !identity(row.value("champion_id")) ||
            (!row.value("observed_patch").isNull()&&!patchPattern.match(row.value("observed_patch").toString()).hasMatch()) || !oneOf(row.value("kind"), kinds) ||
            !row.value("data").isObject()) return false;
        const auto kind = row.value("kind").toString();
        const auto payload = row.value("data").toObject();
        if (kind == QStringLiteral("cards")) {
            const auto cards = payload.value("candidates").toArray();
            if (cards.isEmpty() || cards.size() > 3 || !onlyKeys(payload,{"candidates","change","offer_revision","partial_offer"}) ||
                (payload.contains("partial_offer")&&payload.value("partial_offer")!=QJsonValue(true)) ||
                !oneOf(payload.value("change"),{QStringLiteral("offer"),QStringLiteral("reroll"),QStringLiteral("hint")})) return false;
            const auto revision=payload.value("offer_revision");
            if(!revision.isUndefined()&&(!number(revision,1,1e12)||std::floor(revision.toDouble())!=revision.toDouble()))return false;
            QSet<int> occupiedSlots;
            for (const auto &value : cards) {
                const auto card = value.toObject();
                const auto slot = card.value("slot_index");
                if (!identity(card.value("augment_id")) || !number(slot, 0, 2) || slot != QJsonValue(slot.toInt()) ||
                    occupiedSlots.contains(slot.toInt()) || !card.value("metric").isString() || card.value("metric").toString().size() > 64 ||
                    !onlyKeys(card,{"slot_index","augment_id","metric","numeric_gain_available","numeric_source","partial","recommendation","recommended","explanation"}) ||
                    !card.value("numeric_gain_available").isBool() || !card.value("partial").isBool() ||
                    (card.contains("numeric_source")&&(!card.value("numeric_gain_available").toBool()||!isLocalCardScore(card.value("numeric_source")))) ||
                    (card.contains("recommended")&&!card.value("recommended").isBool()) ||
                    (card.contains("recommendation")&&(!card.value("recommendation").isString()||card.value("recommendation").toString().size()>32)) ||
                    (card.contains("explanation")&&(!card.value("explanation").isString()||card.value("explanation").toString().size()>144))) return false;
                occupiedSlots.insert(slot.toInt());
            }
        } else if (kind == QStringLiteral("selected")) {
            if (!onlyKeys(payload,{"augment_ids","complete","source"}) || !payload.value("complete").isBool() ||
                !validIds(payload.value("augment_ids"),8) || payload.value("augment_ids").toArray().isEmpty() ||
                (payload.contains("source")&&!oneOf(payload.value("source"),{QStringLiteral("manual_confirmation"),QStringLiteral("visible_self_hud_tooltip")})))return false;
        } else if (kind == QStringLiteral("inventory")) {
            if (payload.size()!=2 || !validIds(payload.value("added"),10) || !validIds(payload.value("removed"),10) ||
                (payload.value("added").toArray().isEmpty() && payload.value("removed").toArray().isEmpty())) return false;
        } else if (kind == QStringLiteral("purchase")) {
            const auto item=identity(payload.value("item_id"));
            if(!item||!onlyKeys(payload,{"item_id","reference","purchase_sandbox"})||
                (payload.contains("reference")&&payload.value("reference")!=QJsonValue(true)))return false;
            if(payload.contains("purchase_sandbox")){
                const auto sandbox=payload.value("purchase_sandbox").toObject();
                if(sandbox.isEmpty()||purchaseSandbox(nullptr,sandbox,item)!=sandbox)return false;
            }
        } else if (payload.size()!=6 || !identity(payload.value("item_id")) || !identity(payload.value("champion_id")) ||
            payload.value("kind")!=QStringLiteral("enemy_item_added") || !oneOf(payload.value("team"),{QStringLiteral("ORDER"),QStringLiteral("CHAOS")}) ||
            !number(payload.value("count"),1,6) || payload.value("count")!=QJsonValue(payload.value("count").toInt()) ||
            payload.value("game_time")!=row.value("game_time"))return false;
    }
    return true;
}
QJsonObject readArchive(const QString &directory, const QString &matchRef) {
    const auto path = archivePath(directory, matchRef);
    if (path.isEmpty()) return {};
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 || info.size() > ArchiveByteLimit) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.read(ArchiveByteLimit + 1);
    if (bytes.size() > ArchiveByteLimit) return {};
    auto document = QJsonDocument::fromJson(bytes).object();
    if (document.take("match_ref") != QJsonValue(matchRef) || !validArchive(document)) return {};
    return document;
}
void copyNumeric(QJsonObject &out, const QJsonObject &source, const QString &key, double low, double high, bool integral=false) {
    const auto value=source.value(key);
    if(value.isNull()){out[key]=QJsonValue::Null;return;}
    if(!number(value,low,high)||(integral&&std::floor(value.toDouble())!=value.toDouble()))return;
    // Do not turn insignificant floating-point changes into new timeline nodes.
    out[key]=integral?value:QJsonValue(std::round(value.toDouble()*1000.)/1000.);
}
QJsonArray knownCodes(const QJsonArray &input,const QSet<QString> &allowed,int maximum){
    QSet<QString> seen;for(int i=0;i<std::min(maximum,int(input.size()));++i)if(oneOf(input[i],allowed))seen.insert(input[i].toString());
    auto ordered=seen.values();ordered.sort();QJsonArray out;for(const auto &code:ordered)out.append(code);return out;
}
QJsonObject purchaseSandbox(const Catalog *catalog,const QJsonObject &input,int selected){
    const auto knownItem=[catalog](int id){return id>0&&(!catalog||catalog->items.contains(id));};
    if(!number(input.value("schema_version"),1,1)||input.value("policy")!=QStringLiteral("budgeted_sandbox_v1")
        ||identity(input.value("selected_item_id"))!=selected||!knownItem(selected))return {};
    const QSet<QString> statuses{QStringLiteral("ready"),QStringLiteral("conditional"),QStringLiteral("unavailable")};
    const QSet<QString> kinds{QStringLiteral("immediate"),QStringLiteral("growth_conditioned"),QStringLiteral("quest_conditioned")};
    if(!oneOf(input.value("status"),statuses)||!oneOf(input.value("recommendation_kind"),kinds))return {};
    const QSet<QString> reasons{QStringLiteral("budget_affordable"),QStringLiteral("recipe_credit"),QStringLiteral("target_specific"),QStringLiteral("growth_sensitive"),
        QStringLiteral("quest_item_dependency"),QStringLiteral("model_partial"),QStringLiteral("inventory_full"),QStringLiteral("gold_unobserved"),QStringLiteral("scenario_not_forecast")};
    QJsonObject out{{"schema_version",1},{"policy",QStringLiteral("budgeted_sandbox_v1")},{"status",input.value("status")},
        {"selected_item_id",selected},{"recommendation_kind",input.value("recommendation_kind")},
        {"rationale_codes",knownCodes(input.value("rationale_codes").toArray(),reasons,24)},
        {"all_effects_complete",false},{"actual_win_rate",QJsonValue::Null}};
    QJsonArray candidates;QSet<int> seenItems;const auto sourceCandidates=input.value("candidates").toArray();
    for(int i=0;i<std::min(8,int(sourceCandidates.size()));++i){
        const auto source=sourceCandidates[i].toObject();const int id=identity(source.value("item_id"));
        if(!knownItem(id)||seenItems.contains(id))continue;seenItems.insert(id);QJsonObject item{{"item_id",id}};
        copyNumeric(item,source,QStringLiteral("remaining_cost"),0,10000000,true);
        if(source.value("affordable").isBool())item["affordable"]=source.value("affordable");
        for(const auto &key:{QStringLiteral("immediate_gain"),QStringLiteral("score")})copyNumeric(item,source,key,-1e12,1e12);
        copyNumeric(item,source,QStringLiteral("unknown_count"),0,1000000,true);candidates.append(item);
    }
    out["candidates"]=candidates;
    const QSet<QString> conditions{QStringLiteral("heartsteel_growth"),QStringLiteral("item_stacks"),QStringLiteral("combat_triggers"),QStringLiteral("level_growth")};
    QJsonArray predictions;const auto sourcePredictions=input.value("conditional_predictions").toArray();
    for(int i=0;i<std::min(12,int(sourcePredictions.size()));++i){
        const auto source=sourcePredictions[i].toObject();const int id=identity(source.value("item_id"));
        if(!knownItem(id)||!oneOf(source.value("condition_code"),conditions)||!affirmative(source,QStringLiteral("model_only")))continue;
        QJsonObject item{{"condition_code",source.value("condition_code")},{"item_id",id},{"model_only",true}};
        copyNumeric(item,source,QStringLiteral("trigger_count"),0,10000000);
        copyNumeric(item,source,QStringLiteral("permanent_hp"),0,1e9);
        copyNumeric(item,source,QStringLiteral("score_gain"),-1e12,1e12);predictions.append(item);
    }
    out["conditional_predictions"]=predictions;
    const QSet<QString> gapCodes{QStringLiteral("unobserved_triggers"),QStringLiteral("unobserved_stacks"),QStringLiteral("partial_effects"),
        QStringLiteral("incomplete_opponents"),QStringLiteral("unsupported_level"),QStringLiteral("unmodeled_tactical_actions")};
    QMap<QString,int> counts;const auto sourceGaps=input.value("gaps").toArray();
    for(int i=0;i<std::min(24,int(sourceGaps.size()));++i){const auto gap=sourceGaps[i].toObject();const auto count=gap.value("count");
        if(oneOf(gap.value("code"),gapCodes)&&number(count,0,1000000)&&std::floor(count.toDouble())==count.toDouble())
            counts[gap.value("code").toString()]=std::max(counts.value(gap.value("code").toString()),count.toInt());}
    QJsonArray gaps;for(auto it=counts.cbegin();it!=counts.cend();++it)gaps.append(QJsonObject{{"code",it.key()},{"count",it.value()}});out["gaps"]=gaps;
    return out;
}
QString sandboxLabel(const QString &code){
    static const QMap<QString,QString> names{{"budget_affordable",QStringLiteral("当前金币足够")},{"recipe_credit",QStringLiteral("计入已持有配件")},
        {"target_specific",QStringLiteral("按当前目标防御比较")},{"growth_sensitive",QStringLiteral("排序受成长条件影响")},{"quest_item_dependency",QStringLiteral("依赖任务或授装条件")},
        {"model_partial",QStringLiteral("部分效果尚未建模")},{"inventory_full",QStringLiteral("装备栏空间受限")},{"gold_unobserved",QStringLiteral("当前金币未知")},
        {"scenario_not_forecast",QStringLiteral("条件情景，不是未来预测")},{"heartsteel_growth",QStringLiteral("心之钢成长假设")},{"item_stacks",QStringLiteral("装备层数假设")},
        {"combat_triggers",QStringLiteral("战斗触发次数假设")},{"level_growth",QStringLiteral("等级成长假设")},{"unobserved_triggers",QStringLiteral("触发次数未观测")},
        {"unobserved_stacks",QStringLiteral("装备层数未观测")},{"partial_effects",QStringLiteral("效果覆盖不完整")},{"incomplete_opponents",QStringLiteral("对手状态不完整")},
        {"unsupported_level",QStringLiteral("等级尚不支持")},{"unmodeled_tactical_actions",QStringLiteral("战术行为尚未建模")}};
    return names.value(code,QStringLiteral("未知条件"));
}
QString scenarioNumber(const QJsonValue &value){return value.isDouble()&&std::isfinite(value.toDouble())?QString::number(value.toDouble(),'f',3).remove(QRegularExpression(QStringLiteral("\\.?0+$"))):QStringLiteral("未知");}
QString scenarioPercent(const QJsonValue &value){return value.isDouble()&&std::isfinite(value.toDouble())?(value.toDouble()>0?QStringLiteral("+"):QString())+QString::number(value.toDouble()*100.,'f',1)+QStringLiteral("%"):QStringLiteral("未知");}
QString eventName(const QString &kind) {
    if(kind==QStringLiteral("purchase"))return QStringLiteral("出装建议");
    if(kind==QStringLiteral("cards"))return QStringLiteral("候选牌面");
    if(kind==QStringLiteral("selected"))return QStringLiteral("已选海克斯");
    if(kind==QStringLiteral("inventory"))return QStringLiteral("自身装备变化");
    return QStringLiteral("敌方装备");
}
QString recordName(const QString &kind, const QJsonObject &data) {
    if (kind == QStringLiteral("purchase") && affirmative(data,QStringLiteral("reference"))) return QStringLiteral("出装参考");
    if (kind != QStringLiteral("cards")) return eventName(kind);
    const auto change = data.value("change").toString();
    const auto label=change==QStringLiteral("reroll")?QStringLiteral("重随牌面"):
        change==QStringLiteral("hint")?QStringLiteral("牌面提示更新"):eventName(kind);
    return data.value("partial_offer").toBool()?label+QStringLiteral(" · 观测不全"):label;
}
QString inventoryNames(const Catalog &catalog, const QJsonObject &data) {
    QStringList groups;
    for (const auto &change : {qMakePair("added",QStringLiteral("新增")),qMakePair("removed",QStringLiteral("移出"))}) {
        QStringList names;
        for (const auto &id : data.value(change.first).toArray()) names.append(catalog.itemName(id.toInt()));
        if (!names.isEmpty()) groups.append(change.second + QStringLiteral(" ") + names.join(QStringLiteral("、")));
    }
    return groups.join(QStringLiteral(" · "));
}
QString augmentName(const Catalog &catalog,int id){return catalog.augments.value(id).value("name_zh_cn").toString(QStringLiteral("海克斯 %1").arg(id));}
QColor eventColor(const QString &kind){return Kda::color(kind==QStringLiteral("equipment")?"danger":kind==QStringLiteral("selected")?"ice":"lilac");}
HudSymbols::Symbol eventSymbol(const QString &kind){return kind==QStringLiteral("purchase")?HudSymbols::Symbol::Coins:(kind==QStringLiteral("equipment")||kind==QStringLiteral("inventory"))?HudSymbols::Symbol::Sword:kind==QStringLiteral("selected")?HudSymbols::Symbol::Check:HudSymbols::Symbol::Star;}
void drawText(QPainter &p,const QRectF &r,const QString &text,int px,const QColor &ink,bool numeric=false,int alignment=Qt::AlignVCenter|Qt::AlignLeft){
    const auto font=Kda::font(numeric?Kda::FontRole::Numeric:Kda::FontRole::Body,px);p.setFont(font);p.setPen(ink);
    p.drawText(r,alignment,QFontMetrics(font).elidedText(text,Qt::ElideRight,std::max(0,int(r.width()))));
}
QPixmap artwork(HudAssets *assets,const QString &kind,int id){return kind==QStringLiteral("item")?assets->item(id):kind==QStringLiteral("champion")?assets->champion(id):assets->augment(id);}
void drawArt(QPainter &p,const QRect &r,HudAssets *assets,const QString &kind,int id,qreal dpr){
    const auto image=artwork(assets,kind,id);const qreal radius=kind==QStringLiteral("champion")?r.width()/2.:3;
    p.setPen(QPen(Kda::color("border"),1));p.setBrush(Kda::color("raised"));p.drawRoundedRect(QRectF(r).adjusted(.5,.5,-.5,-.5),radius,radius);
    if(!image.isNull())p.drawPixmap(r,Kda::roundedPixmap(image,r.size(),radius,dpr));
    else HudSymbols::draw(p,kind==QStringLiteral("item")?HudSymbols::Symbol::Shield:kind==QStringLiteral("champion")?HudSymbols::Symbol::Team:HudSymbols::Symbol::Star,QRectF(r).adjusted(7,7,-7,-7),Kda::color("muted"));
}
class ReplayTree final:public QTreeWidget {
public:using QTreeWidget::QTreeWidget;
protected:void paintEvent(QPaintEvent *event)override{
    QTreeWidget::paintEvent(event);bool any=false;for(int i=0;i<topLevelItemCount();++i)any|=!topLevelItem(i)->isHidden();if(any)return;
    QPainter p(viewport());Kda::configurePainter(p);const auto center=viewport()->rect().center();
    const QRect emblem(center.x()-27,center.y()-70,54,54);auto badge=Kda::color("raised");badge.setAlpha(140);p.setPen(QPen(Kda::color("border"),1));p.setBrush(badge);p.drawPath(Kda::facet(emblem,9));
    HudSymbols::draw(p,HudSymbols::Symbol::History,QRectF(emblem).adjusted(14,14,-14,-14),Kda::color("lilac"));
    drawText(p,QRect(12,center.y()+1,viewport()->width()-24,28),topLevelItemCount()?QStringLiteral("暂无此类记录"):QStringLiteral("等待本局的第一个节点"),16,Kda::color("silver"),false,Qt::AlignCenter);
    drawText(p,QRect(12,center.y()+34,viewport()->width()-24,24),QStringLiteral("选牌 · 出装建议 · 敌方装备"),12,Kda::color("muted"),false,Qt::AlignCenter);
}
};
class ReplayEmpty final:public QWidget {
public:ReplayEmpty(){setObjectName(QStringLiteral("replayEmpty"));setAccessibleName(QStringLiteral("本局还没有记录，进入对局后自动记录。"));}
protected:void paintEvent(QPaintEvent *)override{
    QPainter p(this);Kda::configurePainter(p);const auto center=rect().center();
    const QRect emblem(center.x()-24,center.y()-78,48,48);auto edge=Kda::color("border");edge.setAlpha(110);p.setPen(QPen(edge,.8));p.setBrush(Qt::NoBrush);p.drawRoundedRect(emblem,10,10);
    HudSymbols::draw(p,HudSymbols::Symbol::History,QRectF(emblem).adjusted(13,13,-13,-13),Kda::color("silver"));
    drawText(p,QRect(16,center.y()-12,width()-32,30),QStringLiteral("本局还没有记录"),20,Kda::color("text"),false,Qt::AlignCenter);
    drawText(p,QRect(16,center.y()+28,width()-32,24),QStringLiteral("进入对局后，自动记录选牌、出装建议与敌方装备变化"),12,Kda::color("muted"),false,Qt::AlignCenter);
}
};
class ReplayDelegate final:public QStyledItemDelegate {
public:ReplayDelegate(const Catalog &c,HudAssets *a,QObject *parent):QStyledItemDelegate(parent),catalog(c),assets(a){}
    QSize sizeHint(const QStyleOptionViewItem &,const QModelIndex &index)const override{
        return {340,index.data(Qt::UserRole).toJsonObject().value("kind")==QStringLiteral("cards")?110:84};
    }
    void paint(QPainter *painter,const QStyleOptionViewItem &option,const QModelIndex &index)const override{
        auto &p=*painter;p.save();Kda::configurePainter(p);p.setClipRect(option.rect);p.translate(option.rect.topLeft());
        const auto record=index.data(Qt::UserRole).toJsonObject();const auto kind=record.value("kind").toString();const auto data=record.value("data").toObject();const auto ink=eventColor(kind);const int w=option.rect.width();
        p.setPen(QPen(Kda::color("border"),.7));p.drawLine(QPointF(34,0),QPointF(34,option.rect.height()));p.setBrush(Kda::color("surface"));p.setPen(QPen(Kda::color("border"),.7));p.drawRoundedRect(QRectF(22,42,24,24),3,3);
        HudSymbols::draw(p,eventSymbol(kind),QRectF(27,47,14,14),ink,1.4);drawText(p,QRect(0,5,68,26),clockText(record.value("game_time").toDouble()),17,Kda::color("silver"),true,Qt::AlignCenter);
        const QRectF panel(75,4,w-83,option.rect.height()-8);QPainterPath shape;shape.addRoundedRect(panel,Kda::radius("section"),Kda::radius("section"));const bool selected=option.state.testFlag(QStyle::State_Selected);
        auto reading=Kda::color(selected?"raised":"surface");reading.setAlpha(selected?108:52);p.fillPath(shape,reading);auto edge=Kda::color(selected?"silver":"border");edge.setAlpha(selected?115:80);p.setPen(QPen(edge,.7));p.setBrush(Qt::NoBrush);p.drawPath(shape);
        if(selected){p.save();p.setClipPath(shape);p.fillRect(QRectF(panel.left(),panel.top()+10,2,panel.height()-20),Kda::color("silver"));p.restore();}
        drawText(p,QRect(88,9,w-108,25),recordName(kind,data),15,ink);
        if(kind==QStringLiteral("cards")){
            const auto cards=data.value("candidates").toArray();const int cell=std::max(1,(w-110)/std::max(1,int(cards.size())));
            for(int i=0;i<cards.size();++i){const auto card=cards[i].toObject();
                if(w<570){
                    const int y=36+i*22;drawArt(p,QRect(88,y,18,18),assets,QStringLiteral("augment"),card.value("augment_id").toInt(),option.widget?option.widget->devicePixelRatioF():1.);
                    const bool partial=card.value("partial").toBool(),tied=card.value("recommendation").toString().contains(QStringLiteral("同优"))||card.value("recommendation")==QStringLiteral("均可");
                    const auto symbol=!card.value("numeric_gain_available").toBool()?HudSymbols::Symbol::Clock:partial?HudSymbols::Symbol::Warning:tied?HudSymbols::Symbol::Equal:card.value("recommended").toBool()?HudSymbols::Symbol::Check:HudSymbols::Symbol::Chevron;
                    HudSymbols::draw(p,symbol,QRectF(112,y+2,14,14),Kda::color(partial?"silver":"ice"),1.6);
                    drawText(p,QRect(133,y,w-248,20),augmentName(catalog,card.value("augment_id").toInt()),13,Kda::color("silver"));
                    auto value=card.value("metric").toString();if(value.startsWith(QStringLiteral("适配 "))){bool valid=false;const double n=value.mid(3,value.size()-4).toDouble(&valid);if(valid&&std::isfinite(n))value=QStringLiteral("≈ %1%2%").arg(n>0?QStringLiteral("+"):QString(),QString::number(n,'g',4));}
                    drawText(p,QRect(w-109,y-1,91,21),value,17,Kda::color(card.value("recommended").toBool()?"ice":"silver"),card.value("numeric_gain_available").toBool(),Qt::AlignRight|Qt::AlignVCenter);continue;
                }
                const int x=88+i*cell;drawArt(p,QRect(x,41,28,28),assets,QStringLiteral("augment"),card.value("augment_id").toInt(),option.widget?option.widget->devicePixelRatioF():1.);
                drawText(p,QRect(x+33,40,cell-39,28),augmentName(catalog,card.value("augment_id").toInt()),13,Kda::color("silver"));
                const auto metric=card.value("metric").toString();drawText(p,QRect(x,72,cell-6,21),metric,14,Kda::color(card.value("recommended").toBool()?"ice":"silver"));}
        }else if(kind==QStringLiteral("selected")){
            const auto ids=data.value("augment_ids").toArray();const int visibleSlots=std::min(int(ids.size()),std::max(1,(w-108)/34));QStringList names;
            for(int i=0;i<ids.size();++i)names<<augmentName(catalog,ids[i].toInt());
            for(int i=0;i<visibleSlots;++i){const int x=88+i*34;if(i==visibleSlots-1&&ids.size()>visibleSlots)drawText(p,QRect(x,35,26,26),QStringLiteral("+%1").arg(ids.size()-visibleSlots+1),16,Kda::color("silver"),true);else drawArt(p,QRect(x,35,26,26),assets,QStringLiteral("augment"),ids[i].toInt(),option.widget?option.widget->devicePixelRatioF():1.);}
            drawText(p,QRect(88,62,w-110,20),names.join(QStringLiteral(" · ")),12,Kda::color("muted"));
        }else if(kind==QStringLiteral("inventory")){
            drawText(p,QRect(88,39,w-108,28),inventoryNames(catalog,data),14,Kda::color("text"));
        }else{
            const int id=data.value("item_id").toInt();int x=88;const auto dpr=option.widget?option.widget->devicePixelRatioF():1.;
            if(kind==QStringLiteral("equipment")){drawArt(p,QRect(x,39,32,32),assets,QStringLiteral("champion"),data.value("champion_id").toInt(),dpr);x+=38;}
            drawArt(p,QRect(x,38,34,34),assets,QStringLiteral("item"),id,dpr);x+=44;
            drawText(p,QRect(x,33,w-x-20,26),catalog.itemName(id),15,Kda::color("text"));
            QString caption=kind==QStringLiteral("purchase")?(affirmative(data,QStringLiteral("reference"))?QStringLiteral("当时参考"):QStringLiteral("当时推荐")):catalog.championName(data.value("champion_id").toInt())+QStringLiteral(" · 新装备");
            if(kind==QStringLiteral("purchase"))for(const auto &value:data.value("purchase_sandbox").toObject().value("candidates").toArray()){
                const auto candidate=value.toObject();if(candidate.value("item_id").toInt()==id&&candidate.value("remaining_cost").isDouble())caption+=QStringLiteral(" · 补购 %1 G").arg(candidate.value("remaining_cost").toInt());
            }
            drawText(p,QRect(x,60,w-x-20,20),caption,12,Kda::color("muted"));
        }
        p.restore();
    }
private:const Catalog &catalog;HudAssets *assets;
};
}

class ReplaySummary final:public QWidget {
public:ReplaySummary(const Catalog &c,HudAssets *a):catalog(c),assets(a){setFixedHeight(56);}int champion=0;double time=-1;int count=0;bool synthetic=false;
protected:void paintEvent(QPaintEvent *)override{
    QPainter p(this);Kda::configurePainter(p);auto divider=Kda::color("border");divider.setAlpha(90);p.setPen(QPen(divider,.7));p.drawLine(QPointF(.5,height()-.5),QPointF(width()-.5,height()-.5));
    if(champion>0)drawArt(p,QRect(6,10,34,34),assets,QStringLiteral("champion"),champion,devicePixelRatioF());
    drawText(p,QRect(52,2,width()-220,28),champion>0?catalog.championName(champion):QStringLiteral("本局轨迹"),18,Kda::color("text"));
    drawText(p,QRect(53,29,width()-220,20),synthetic?QStringLiteral("合成演示"):QStringLiteral("选牌与出装轨迹"),11,Kda::color(synthetic?"danger":"muted"));
    drawText(p,QRect(width()-142,1,132,30),time>=0?clockText(time):QStringLiteral("—"),24,Kda::color("silver"),true,Qt::AlignRight|Qt::AlignVCenter);
    drawText(p,QRect(width()-142,30,132,18),QStringLiteral("%1 条记录").arg(count),11,Kda::color("muted"),false,Qt::AlignRight|Qt::AlignVCenter);
}
private:const Catalog &catalog;HudAssets *assets;
};

CoachReplay::CoachReplay(const Catalog &value, QWidget *parent, bool embeddedMode) : QMainWindow(parent), catalog(value), embedded(embeddedMode) {
    setObjectName(QStringLiteral("coachReplay"));setWindowTitle(QStringLiteral("本局记录"));setFont(Kda::font(Kda::FontRole::Body,13));
    if(embedded){setWindowFlags(Qt::Widget);setMinimumSize(0,0);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);}
    else{setMinimumSize(680,500);resize(760,520);}
    Kda::bindThemeStyle(this,[]{return Kda::overlayStyleSheet()+QStringLiteral("QWidget#replayRoot,QWidget#replayBody,QWidget#replayTitlebar{background:transparent;}QTreeWidget#replayEntries{background:transparent;border:0;outline:0;}QTreeWidget#replayEntries::item{padding:0;border:0;background:transparent;}QTextBrowser#replayDetail{background:transparent;border:0;border-left:1px solid %1;border-radius:0;padding:10px 14px;}QSplitter::handle{background:transparent;width:8px;}QLabel#replayStatus{color:%2;font-size:13px;}").arg(Kda::color("border").name(),Kda::color("muted").name());});
    assets=new HudAssets(catalog,this);
    auto root=new QWidget(this);root->setObjectName(QStringLiteral("replayRoot"));setCentralWidget(root);auto layout=new QVBoxLayout(root);layout->setContentsMargins(embedded?0:1,0,embedded?0:1,embedded?0:1);layout->setSpacing(0);
    if(!embedded){
        auto topbar=new QWidget;topbar->setObjectName(QStringLiteral("replayTitlebar"));topbar->setFixedHeight(DashboardChrome::TitleBarHeight);auto top=new QHBoxLayout(topbar);top->setContentsMargins(15,0,6,0);
        auto brand=new QLabel(QStringLiteral("李金金"));brand->setObjectName(QStringLiteral("brand"));auto title=new QLabel(QStringLiteral("本局记录"));title->setFont(Kda::font(Kda::FontRole::Title,13));top->addWidget(brand);top->addSpacing(14);top->addWidget(title);top->addStretch();chrome=new DashboardChrome(this,topbar);top->addWidget(chrome->controls());layout->addWidget(topbar);
    }
    auto body=new Kda::ReadingSurface;body->setObjectName(QStringLiteral("replayBody"));auto content=new QVBoxLayout(body);content->setContentsMargins(Kda::Density::pageInset,Kda::Density::sectionGap,Kda::Density::pageInset,Kda::Density::sectionGap);content->setSpacing(Kda::Density::sectionGap);layout->addWidget(body,1);
    summary=new ReplaySummary(catalog,assets);content->addWidget(summary);
    filterBar=new QWidget;auto filterRow=new QHBoxLayout(filterBar);filterRow->setContentsMargins(0,0,0,0);filterRow->setSpacing(6);
    const QList<QPair<QString,QString>> kinds{{QString(),QStringLiteral("全部")},{QStringLiteral("cards"),QStringLiteral("选牌")},{QStringLiteral("selected"),QStringLiteral("已选")},{QStringLiteral("purchase"),QStringLiteral("出装")},{QStringLiteral("inventory"),QStringLiteral("装备")},{QStringLiteral("equipment"),QStringLiteral("敌方")}};
    for(const auto &kind:kinds){auto button=new Kda::Button(kind.second);button->setObjectName(QStringLiteral("replayFilter_")+(kind.first.isEmpty()?QStringLiteral("all"):kind.first));button->setProperty("toolbarAction",true);button->setProperty("labelPixels",11);button->setProperty("labelRegular",true);button->setFixedHeight(Kda::Density::controlHeight);button->setProperty("kind",kind.first);button->setProperty("label",kind.second);button->setCheckable(true);button->setMinimumWidth(56);filters.append(button);filterRow->addWidget(button);connect(button,&QPushButton::clicked,this,[this,kind]{activeFilter=kind.first;followLatest=false;applyFilter();refreshPresentation();});}filterRow->addStretch();
    followAction=new Kda::Button(QStringLiteral("跟随最新"));followAction->setObjectName(QStringLiteral("replayFollowLatest"));followAction->setProperty("toolbarAction",true);followAction->setProperty("labelPixels",11);followAction->setCheckable(true);followAction->setFixedHeight(Kda::Density::controlHeight);followAction->setMinimumWidth(80);followAction->setToolTip(QStringLiteral("开启后跟随新记录；查看旧记录时自动暂停跟随"));filterRow->addWidget(followAction);content->addWidget(filterBar);
    emptyState=new ReplayEmpty;content->addWidget(emptyState,1);
    splitter=new QSplitter(Qt::Horizontal);splitter->setObjectName(QStringLiteral("replaySplit"));splitter->setChildrenCollapsible(false);splitter->setHandleWidth(12);content->addWidget(splitter,1);
    list = new ReplayTree; list->setObjectName(QStringLiteral("replayEntries"));list->setMinimumWidth(340);
    list->viewport()->setAutoFillBackground(false);
    list->setHeaderLabels({QStringLiteral("时间"), QStringLiteral("事件"), QStringLiteral("内容")});
    list->setRootIsDecorated(false);list->setHeaderHidden(true);list->setUniformRowHeights(false);list->setAlternatingRowColors(false);list->setIndentation(0);list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);list->setItemDelegate(new ReplayDelegate(catalog,assets,list));
    list->setSelectionMode(QAbstractItemView::SingleSelection); list->setTextElideMode(Qt::ElideRight);
    // The header's default last-section stretch keeps its previous width as a
    // minimum, even when hidden; use only the explicit stretch section here.
    list->header()->setStretchLastSection(false);list->header()->setSectionResizeMode(0,QHeaderView::Stretch);list->hideColumn(1);list->hideColumn(2);splitter->addWidget(list);
    detail = new QTextBrowser; detail->setObjectName(QStringLiteral("replayDetail"));detail->setFont(Kda::font(Kda::FontRole::Body,13));
    detail->viewport()->setAutoFillBackground(false);
    detail->setOpenLinks(false);detail->setOpenExternalLinks(false);detail->setMinimumWidth(250);splitter->addWidget(detail);splitter->setStretchFactor(0,1);splitter->setStretchFactor(1,0);splitter->setSizes({560,300});
    stateLabel=new QLabel;stateLabel->setObjectName(QStringLiteral("replayStatus"));content->addWidget(stateLabel);
    connect(list,&QTreeWidget::currentItemChanged,this,[this]{if(!updatingSelection&&list->currentItem()){followLatest=false;followAction->setChecked(false);}showDetail();});
    connect(followAction,&QPushButton::clicked,this,[this](bool checked){followLatest=checked;if(checked){QScopedValueRollback<bool> guard(updatingSelection,true);for(int i=0;i<list->topLevelItemCount();++i)if(!list->topLevelItem(i)->isHidden()){list->setCurrentItem(list->topLevelItem(i));list->scrollToItem(list->currentItem(),QAbstractItemView::PositionAtTop);break;}}});
    assets->onChanged=[this]{list->viewport()->update();summary->update();const int scroll=detail->verticalScrollBar()->value();displayedRecord.clear();showDetail();detail->verticalScrollBar()->setValue(scroll);};
    new ReplayThemeObserver(this,[this](const QString &previousSkin){rethemeReplayDocument(detail,previousSkin);});
    clearMemory();
}
void CoachReplay::paintEvent(QPaintEvent *event){
    if(embedded){QMainWindow::paintEvent(event);return;}
    QPainter p(this);Kda::configurePainter(p);p.setCompositionMode(QPainter::CompositionMode_Source);p.fillRect(rect(),Qt::transparent);p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    QPainterPath shape;shape.addRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),isMaximized()?0:Kda::radius("window"),isMaximized()?0:Kda::radius("window"));p.fillPath(shape,QColor("#0c0c11"));p.setPen(QPen(QColor("#373740"),1));p.drawPath(shape);
    // Keep the titlebar's existing pixels; only the body follows the material.
    p.save();p.setClipRect(QRectF(0,DashboardChrome::TitleBarHeight,width(),std::max(0,height()-DashboardChrome::TitleBarHeight)));p.setClipPath(shape,Qt::IntersectClip);p.fillPath(shape,Kda::color("bg"));p.setPen(QPen(Kda::color("border"),1));p.drawPath(shape);p.restore();p.setPen(QPen(QColor("#292832"),1));p.drawLine(1,DashboardChrome::TitleBarHeight,width()-2,DashboardChrome::TitleBarHeight);
}
void CoachReplay::showDetail(){
    auto item=list->currentItem();if(!item||item->isHidden()){displayedRecord.clear();detail->clear();detail->setPlaceholderText({});return;}
    const auto record=item->data(0,Qt::UserRole).toJsonObject();const auto kind=record.value("kind").toString();const auto data=record.value("data").toObject();
    const auto key=signature(record);if(key==displayedRecord)return;displayedRecord=key;
    auto image=[this](const QString &type,int id){const QUrl url(QStringLiteral("replay:%1/%2").arg(type).arg(id));auto icon=artwork(assets,type,id);if(icon.isNull()){icon=QPixmap(40,40);icon.fill(Kda::color("raised"));QPainter p(&icon);HudSymbols::draw(p,type==QStringLiteral("augment")?HudSymbols::Symbol::Star:HudSymbols::Symbol::Shield,QRectF(10,10,20,20),Kda::color("lilac"));}detail->document()->addResource(QTextDocument::ImageResource,url,icon.toImage());return QStringLiteral("<img src='%1' width='36' height='36'>").arg(url.toString().toHtmlEscaped());};
    QString html=QStringLiteral("<style>")+replayDocumentCss()+QStringLiteral("</style>")+QStringLiteral("<h2>%1</h2><p><span class='value'>%2</span>　<small>%3</small></p><hr>")
        .arg(recordName(kind,data).toHtmlEscaped(),clockText(record.value("game_time").toDouble()),catalog.championName(record.value("champion_id").toInt()).toHtmlEscaped());
    if(kind==QStringLiteral("cards")){
        html+=QStringLiteral("<table width='100%'>");for(const auto &value:data.value("candidates").toArray()){const auto card=value.toObject();const int id=card.value("augment_id").toInt();
            const auto reason = card.value("explanation").toString();
            const auto source = card.value("numeric_source");
            const auto scope = source == DecisionCardScore ? QStringLiteral("<br>五名对手 · 3/8/20 秒条件估算") :
                source == CurrentCardScore ? QStringLiteral("<br>8 秒条件估算") : QString();
            html+=QStringLiteral("<tr><td width='44'>%1</td><td>%2<br><span class='value'>%3</span><br><small>%4%5%6%7</small></td></tr>")
                .arg(image(QStringLiteral("augment"),id),augmentName(catalog,id).toHtmlEscaped(),card.value("metric").toString().toHtmlEscaped(),
                    card.value("recommendation").toString().toHtmlEscaped(),card.value("partial").toBool()?QStringLiteral(" · 部分规则"):QString(),scope,
                    reason.isEmpty()?QString():QStringLiteral("<br>")+reason.toHtmlEscaped());}
        html+=QStringLiteral("</table><p><small>适配数值为当时的本地模型估算。</small></p>");
        if(data.value("partial_offer").toBool())html+=QStringLiteral("<p><small>牌面观测不全，仅列出识别到的候选。</small></p>");
    }else if(kind==QStringLiteral("selected")){
        html+=QStringLiteral("<table width='100%'>");for(const auto &value:data.value("augment_ids").toArray())html+=QStringLiteral("<tr><td width='44'>%1</td><td>%2</td></tr>").arg(image(QStringLiteral("augment"),value.toInt()),augmentName(catalog,value.toInt()).toHtmlEscaped());
        const auto source=data.value("source").toString();
        const auto evidence=source==QStringLiteral("manual_confirmation")?QStringLiteral("手动确认"):
            source==QStringLiteral("visible_self_hud_tooltip")?QStringLiteral("本人游戏面板实况"):QStringLiteral("选牌来源未标记");
        html+=QStringLiteral("</table><p><small>%1 · %2</small></p>").arg(evidence,data.value("complete").toBool()?QStringLiteral("组合已记录"):QStringLiteral("仅记录到部分"));
    }else if(kind==QStringLiteral("inventory")){
        html+=QStringLiteral("<p>%1</p><p><small>来自装备栏实况。变动也可能来自合成、授予或撤销。</small></p>").arg(inventoryNames(catalog,data).toHtmlEscaped());
    }else{
        const int id=data.value("item_id").toInt();if(kind==QStringLiteral("equipment"))html+=QStringLiteral("<p>%1　%2</p>").arg(image(QStringLiteral("champion"),data.value("champion_id").toInt()),catalog.championName(data.value("champion_id").toInt()).toHtmlEscaped());
        const auto annotation=kind!=QStringLiteral("purchase")?QStringLiteral("观察到的装备栏变化"):
            affirmative(data,QStringLiteral("reference"))?QStringLiteral("当时显示的出装参考"):QStringLiteral("当时的推荐目标");
        html+=QStringLiteral("<p>%1</p><h2>%2</h2><p><small>%3</small></p>").arg(image(QStringLiteral("item"),id),catalog.itemName(id).toHtmlEscaped(),annotation);
        if(kind==QStringLiteral("purchase")&&data.value("purchase_sandbox").isObject()){
            const auto sandbox=data.value("purchase_sandbox").toObject();const auto type=sandbox.value("recommendation_kind").toString();
            const QString typeName=type==QStringLiteral("growth_conditioned")?QStringLiteral("成长条件推荐"):type==QStringLiteral("quest_conditioned")?QStringLiteral("任务条件推荐"):QStringLiteral("当前条件推荐");
            html+=QStringLiteral("<h2>当时的购买比较</h2><p>%1 · %2</p>").arg(typeName,sandbox.value("status")==QStringLiteral("unavailable")?QStringLiteral("模型信息不足"):sandbox.value("status")==QStringLiteral("conditional")?QStringLiteral("有条件成立"):QStringLiteral("已完成情景比较"));
            QStringList reasons;for(const auto &code:sandbox.value("rationale_codes").toArray())reasons<<sandboxLabel(code.toString());
            if(!reasons.isEmpty())html+=QStringLiteral("<p><small>依据：%1</small></p>").arg(reasons.join(QStringLiteral("；")).toHtmlEscaped());
            const auto candidates=sandbox.value("candidates").toArray();
            if(!candidates.isEmpty()){
                html+=QStringLiteral("<table width='100%'>");
                for(const auto &value:candidates){const auto candidate=value.toObject();const int candidateId=candidate.value("item_id").toInt();
                    const auto affordable=candidate.value("affordable");const auto cost=candidate.value("remaining_cost");const auto unknown=candidate.value("unknown_count");
                    html+=QStringLiteral("<tr><td width='38'>%1</td><td>%2%3<br><small>条件模型增益 %4 · 即期 %5<br>补购成本 %6G · %7%8</small></td></tr>")
                        .arg(image(QStringLiteral("item"),candidateId),catalog.itemName(candidateId).toHtmlEscaped(),candidateId==id?QStringLiteral("（当时推荐）"):QString(),
                            scenarioPercent(candidate.value("score")),scenarioPercent(candidate.value("immediate_gain")),scenarioNumber(cost),
                            affordable.isBool()?(affordable.toBool()?QStringLiteral("当前可负担"):QStringLiteral("当前金币不足")):QStringLiteral("金币未知"),
                            unknown.isDouble()?QStringLiteral(" · 未覆盖 %1 项").arg(unknown.toInt()):QStringLiteral(" · 覆盖未知"));
                }
                html+=QStringLiteral("</table>");
            }
            const auto predictions=sandbox.value("conditional_predictions").toArray();
            if(!predictions.isEmpty()){
                html+=QStringLiteral("<h2>条件性情景</h2><p><small>以下触发次数、成长与层数是模型假设，尚未证明本局会发生。</small></p>");
                for(const auto &value:predictions){const auto prediction=value.toObject();QStringList conditions;
                    if(prediction.value("condition_code")==QStringLiteral("heartsteel_growth"))conditions<<QStringLiteral("参考目标 / 8秒窗口；与上方多目标候选比较的口径不同");
                    if(prediction.contains("trigger_count"))conditions<<QStringLiteral("假设次数 / 层数 %1").arg(scenarioNumber(prediction.value("trigger_count")));
                    if(prediction.contains("permanent_hp"))conditions<<QStringLiteral("假设永久生命 %1").arg(scenarioNumber(prediction.value("permanent_hp")));
                    html+=QStringLiteral("<p>%1 · %2<br><small>%3；条件模型增益 %4</small></p>")
                        .arg(catalog.itemName(prediction.value("item_id").toInt()).toHtmlEscaped(),sandboxLabel(prediction.value("condition_code").toString()),
                            conditions.isEmpty()?QStringLiteral("条件数值未知"):conditions.join(QStringLiteral("；")),scenarioPercent(prediction.value("score_gain")));
                }
            }
            const auto gaps=sandbox.value("gaps").toArray();
            if(!gaps.isEmpty()){
                QStringList lines;for(const auto &value:gaps){const auto gap=value.toObject();lines<<sandboxLabel(gap.value("code").toString())+QStringLiteral(" %1 项").arg(gap.value("count").toInt());}
                html+=QStringLiteral("<h2>当时的缺口</h2><p><small>%1</small></p>").arg(lines.join(QStringLiteral("；")).toHtmlEscaped());
            }
            html+=QStringLiteral("<p><small>条件模型增益只用于所设情景的内部比较，不是实战胜率。尚有未覆盖效果；该节点也不表示玩家实际购买或未来已获得这些成长。</small></p>");
        }
    }
    html+=QStringLiteral("<hr><p><small>%1 · %2</small></p>").arg(record.value("observed_patch").toString().toHtmlEscaped(),
        !archive.isEmpty()?QStringLiteral("决策回看"):synthetic?QStringLiteral("合成演示"):QStringLiteral("本局记录"));detail->setHtml(html);
}
void CoachReplay::applyFilter(){
    QScopedValueRollback<bool> guard(updatingSelection,true);
    for(auto button:filters)button->setChecked(button->property("kind").toString()==activeFilter);
    QTreeWidgetItem *first=nullptr;for(int i=0;i<list->topLevelItemCount();++i){auto item=list->topLevelItem(i);const bool hidden=!activeFilter.isEmpty()&&item->data(0,Qt::UserRole).toJsonObject().value("kind").toString()!=activeFilter;item->setHidden(hidden);if(!hidden&&!first)first=item;}
    if(!list->currentItem()||list->currentItem()->isHidden())list->setCurrentItem(first);detail->setVisible(first!=nullptr);showDetail();list->viewport()->update();
}
void CoachReplay::refreshPresentation(){
    const bool archived = !archive.isEmpty();
    const auto shown = archived ? archive.value("entries").toArray() : records;
    const bool hasRecords=!shown.isEmpty();emptyState->setVisible(!hasRecords);summary->setVisible(hasRecords);filterBar->setVisible(hasRecords);splitter->setVisible(hasRecords);followAction->setVisible(!archived);followAction->setChecked(followLatest);
    summary->time=archived?archive.value("last_time").toDouble():lastTime;summary->count=int(shown.size());summary->synthetic=!archived&&synthetic;summary->champion=hasRecords?shown.last().toObject().value("champion_id").toInt():0;summary->update();
    for(auto button:filters){const auto kind=button->property("kind").toString();int total=0;for(const auto &record:shown)if(kind.isEmpty()||record.toObject().value("kind").toString()==kind)++total;button->setText(button->property("label").toString()+(total?QStringLiteral(" %1").arg(total):QString()));button->setChecked(kind==activeFilter);}
    if(archived){stateLabel->setText(QStringLiteral("本地回看 · 仅显示当时记录到的内容"));return;}
    QString state=status==QStringLiteral("recording")?QStringLiteral("记录中"):status==QStringLiteral("paused_stale")?QStringLiteral("数据暂停"):status==QStringLiteral("paused_version")?QStringLiteral("版本待核实"):status==QStringLiteral("waiting")?QStringLiteral("等待本局数据"):QStringLiteral("等待有效数据");
    if(context.isEmpty()&&!hasRecords)state=QStringLiteral("等待对局");
    stateLabel->setText((synthetic?QStringLiteral("合成演示 · "):QString())+state+QStringLiteral(" · 对局核对成功后可从战绩回看"));
}
void CoachReplay::resetPresentation() {
    QScopedValueRollback<bool> guard(updatingSelection,true);
    list->clear();detail->clear();activeFilter.clear();followLatest=archive.isEmpty();displayedRecord.clear();
    const auto shown=archive.isEmpty()?records:archive.value("entries").toArray();
    for(const auto &record:shown)insertRow(record.toObject());
    refreshPresentation();applyFilter();
}
void CoachReplay::clearMemory() {
    const bool changed=!context.isEmpty()||!records.isEmpty()||status!=QStringLiteral("waiting");
    context.clear(); records = {}; lastPurchase.clear(); lastOffer.clear(); lastCards.clear(); lastSelection.clear(); lastCue.clear();
    observedPatch.clear();
    lastInventory={};inventoryObserved=false;
    lastTime = -1; synthetic = false; status = QStringLiteral("waiting");
    if(archive.isEmpty()&&changed)resetPresentation();
}
void CoachReplay::append(const QString &kind, const QJsonObject &payload, double time, int championId) {
    const QJsonObject record{{"kind", kind}, {"game_time", time}, {"observed_patch", observedPatch.isEmpty()?QJsonValue::Null:QJsonValue(observedPatch)},
        {"champion_id", championId}, {"synthetic", synthetic}, {"data", payload}};
    records.append(record);
    if(records.size()>Limit)records.removeAt(0);
    if(!archive.isEmpty())return;
    QScopedValueRollback<bool> guard(updatingSelection,true);
    insertRow(record);
    if(list->topLevelItemCount()>Limit)delete list->takeTopLevelItem(list->topLevelItemCount()-1);
    applyFilter();auto item=list->topLevelItem(0);if(followLatest&&!item->isHidden()){list->setCurrentItem(item);list->scrollToTop();}refreshPresentation();
}
void CoachReplay::insertRow(const QJsonObject &record) {
    const auto kind=record.value("kind").toString();
    const auto payload=record.value("data").toObject();
    QString event, text;
    if (kind == QStringLiteral("purchase")) { event = recordName(kind,payload); text = catalog.itemName(payload.value("item_id").toInt()); }
    else if (kind == QStringLiteral("cards")) {
        event = recordName(kind,payload); QStringList names;
        for (const auto &value : payload.value("candidates").toArray()) {
            const auto card = value.toObject(); QString label = augmentName(catalog,card.value("augment_id").toInt());
            label += QStringLiteral(" · ") + card.value("metric").toString();
            if (!card.value("recommendation").toString().isEmpty()) label += QStringLiteral(" · ") + card.value("recommendation").toString();
            names.append(label);
        }
        text = names.join(QStringLiteral(" / "));
    } else if (kind == QStringLiteral("selected")) {
        event = QStringLiteral("已选"); QStringList names;
        for (const auto &id : payload.value("augment_ids").toArray()) names.append(augmentName(catalog,id.toInt()));
        text = names.join(QStringLiteral(" / "));
    } else if(kind==QStringLiteral("inventory")){
        event=eventName(kind);text=inventoryNames(catalog,payload);
    } else { event = QStringLiteral("敌方出装"); text = catalog.championName(payload.value("champion_id").toInt()) + QStringLiteral(" · ") + catalog.itemName(payload.value("item_id").toInt()); }
    auto item = new QTreeWidgetItem({clockText(record.value("game_time").toDouble()), event, text});item->setData(0,Qt::UserRole,record);item->setToolTip(0,event+QStringLiteral("\n")+text);list->insertTopLevelItem(0,item);item->setFirstColumnSpanned(true);
}
void CoachReplay::observe(const QJsonObject &snapshot) {
    if (snapshot.isEmpty()) { clearMemory(); return; }
    const auto coach = snapshot.value("coach").toObject();
    const auto live = snapshot.value("live_combat_summary").toObject();
    const auto mode = snapshot.value("capture_mode").toString();
    const bool isSynthetic = affirmative(snapshot, QStringLiteral("synthetic_game_state")) || affirmative(snapshot, QStringLiteral("synthetic_file_feed_test"))
        || mode == QStringLiteral("replay") || mode == QStringLiteral("demo");
    QString rawContext = snapshot.value("match_context").toObject().value("context_id").toString();
    if (rawContext.isEmpty()) rawContext = snapshot.value("context_id").toString();
    if (rawContext.isEmpty() && isSynthetic) rawContext = live.value("context_id").toString();
    QString nextContext = uuid(rawContext);
    if (nextContext.isEmpty() && isSynthetic && (rawContext.isEmpty() || rawContext == QStringLiteral("synthetic_replay") || rawContext == QStringLiteral("synthetic-demo")))
        nextContext = mode == QStringLiteral("replay") ? QStringLiteral("synthetic:replay") : QStringLiteral("synthetic:demo");
    if (nextContext.isEmpty()) { clearMemory(); status = QStringLiteral("invalid_context");if(archive.isEmpty())refreshPresentation(); return; }
    if (context != nextContext || synthetic != isSynthetic) { clearMemory(); context = nextContext; synthetic = isSynthetic; }
    auto fresh = snapshot.value("live_data_fresh");
    if (fresh.isUndefined()) fresh = snapshot.value("fresh");
    if (fresh.isUndefined()) fresh = live.value("fresh");
    if (fresh.isUndefined()) fresh = coach.value("fresh");
    if (!fresh.isBool() || !fresh.toBool()) { status = QStringLiteral("paused_stale");if(archive.isEmpty())refreshPresentation(); return; }
    static const QRegularExpression patchPattern(QStringLiteral("\\A[0-9]{1,3}\\.[0-9]{1,3}(?:\\.[0-9]{1,6})?\\z"));
    const auto reportedPatch=snapshot.value("observed_patch").toString();
    observedPatch=patchPattern.match(reportedPatch).hasMatch()?reportedPatch:QString();
    auto gameTime = snapshot.value("game_time"); if (gameTime.isUndefined()) gameTime = coach.value("game_time"); if (gameTime.isUndefined()) gameTime = live.value("game_time");
    if (!number(gameTime, 0, 86400) || gameTime.toDouble() < lastTime) { status = QStringLiteral("invalid_or_old_time");if(archive.isEmpty())refreshPresentation(); return; }
    int championId = identity(snapshot.value("current_champion_id")); if (championId <= 0) championId = identity(coach.value("champion_id"));
    if (!catalog.champions.contains(championId)) { status = QStringLiteral("champion_unknown");if(archive.isEmpty())refreshPresentation(); return; }
    const double time = gameTime.toDouble(); lastTime = time; status = QStringLiteral("recording");
    const auto nextPurchase=coach.value("economy").toObject().value("next_purchase").toObject();
    const int calculatedPurchaseId=identity(nextPurchase.value("item_id"));
    const auto shownPurchase=snapshot.value("shown_purchase_item_id");
    const int purchaseId=shownPurchase.isUndefined()?calculatedPurchaseId:identity(shownPurchase);
    const bool currentPurchase=purchaseId==calculatedPurchaseId
        &&(!coach.contains("fresh")||affirmative(coach,QStringLiteral("fresh")));
    if(catalog.items.contains(purchaseId)&&(currentPurchase||!shownPurchase.isUndefined())){
        QJsonObject purchase{{"item_id", purchaseId}};
        if(currentPurchase){
            const auto sandbox=purchaseSandbox(&catalog,coach.value("plan").toObject().value("purchase_sandbox").toObject(),purchaseId);
            if(!sandbox.isEmpty())purchase["purchase_sandbox"]=sandbox;
        }else purchase["reference"]=true;
        const auto key = signature(purchase);
        if (key != lastPurchase) { lastPurchase = key; append(QStringLiteral("purchase"), purchase, time, championId); }
    }
    const bool currentModel = affirmative(snapshot.value("selection_overlay").toObject(), QStringLiteral("current_model"));
    QMap<int, QJsonObject> hints;
    const auto shownHints = snapshot.value("card_hints").toArray();
    for (int i = 0; i < std::min(3, int(shownHints.size())); ++i) {
        const auto hint = shownHints[i].toObject(); const int id = identity(hint.value("augment_id"));
        if (catalog.augments.contains(id) && hint.value("visible") != QJsonValue(false)) hints.insert(id, hint);
    }
    QMap<int, QJsonObject> ordered; QSet<int> seen;
    const auto offerStatus=snapshot.value("status");
    const auto candidates = offerStatus.isUndefined() || offerStatus==QStringLiteral("open") ? snapshot.value("candidates").toArray() : QJsonArray{};
    static const QRegularExpression metric(QStringLiteral("\\A(?:适配 )?([+-]?(?:[0-9]+(?:\\.[0-9]+)?)(?:[eE][+-]?[0-9]+)?)%\\z"));
    static const QRegularExpression partialLabel(QStringLiteral("^部分 #[1-3]$"));
    static const QSet<QString> recommendations{QStringLiteral("推荐"), QStringLiteral("均可"), QStringLiteral("备选"), QStringLiteral("可选"), QStringLiteral("慎选"), QStringLiteral("已知优"), QStringLiteral("已知同优"), QStringLiteral("数值领先"), QStringLiteral("数值同优"), QStringLiteral("部分")};
    for (int i = 0; i < std::min(3, int(candidates.size())); ++i) {
        const auto candidate = candidates[i].toObject(); const int id = identity(candidate.value("augment_id")); const auto rawSlot = candidate.value("candidate_index");
        if (!rawSlot.isUndefined() && (!number(rawSlot, 0, 2) || std::floor(rawSlot.toDouble()) != rawSlot.toDouble())) continue;
        const int slot = rawSlot.toInt(i);
        if (!catalog.augments.contains(id) || seen.contains(id) || slot < 0 || slot > 2 || ordered.contains(slot)) continue;
        seen.insert(id); const auto hint = hints.value(id);
        QJsonObject card{{"slot_index", slot}, {"augment_id", id}, {"metric", QStringLiteral("待评")}, {"numeric_gain_available", false}, {"partial", hint.value("partial").toBool()}};
        const QString localMetric = hint.value("metric").toString();
        const auto numericSource = hint.value("numeric_source");
        const auto numeric = localMetric.size()<=60 ? metric.match(localMetric) : QRegularExpressionMatch();
        if (currentModel && affirmative(hint, QStringLiteral("numeric_gain_available")) && isLocalCardScore(numericSource) && numeric.hasMatch()) {
            const auto numericText = numeric.captured(1); bool valid = false; const double gain = numericText.toDouble(&valid);
            if (valid && std::isfinite(gain)) {
                card["metric"] = QStringLiteral("适配 ")+numericText+QLatin1Char('%'); card["numeric_gain_available"] = true;
                card["numeric_source"] = numericSource;
                const QString label = hint.value("recommendation").toString();
                if (recommendations.contains(label) || partialLabel.match(label).hasMatch()) card["recommendation"] = label;
                card["recommended"] = hint.value("recommended").toBool();
            }
        } else if (localMetric == QStringLiteral("计算中")) card["metric"] = localMetric;
        // The caller projects this from its visible, local mechanism copy. Raw
        // model output, tooltips and candidate source strings stay outside replay.
        const auto explanation=hint.value("explanation_source")==QStringLiteral("local_displayed_mechanism")
            ?hint.value("explanation").toString().simplified().left(144):QString();
        if(!explanation.isEmpty()){
            card["explanation"]=explanation;
            if(!card.value("numeric_gain_available").toBool())card["metric"]=QStringLiteral("机制提示");
        }
        ordered.insert(slot, card);
    }
    QJsonArray cards, offerShape;
    for(const auto &card:ordered){
        cards.append(card);
        offerShape.append(QJsonArray{card.value("slot_index"),card.value("augment_id")});
    }
    if(!cards.isEmpty()){
        QJsonObject payload{{"candidates",cards}};
        if(cards.size()<3||affirmative(snapshot.value("selection_overlay").toObject(),QStringLiteral("awaiting_complete_offer")))
            payload["partial_offer"]=true;
        const auto revision=snapshot.value("offer_revision");
        if(number(revision,1,1e12)&&std::floor(revision.toDouble())==revision.toDouble())payload["offer_revision"]=revision;
        const auto offer=signature(QJsonObject{{"revision",revision},{"slots",offerShape}});
        const auto key=signature(payload);
        const bool changed=offer!=lastOffer;
        if(key!=lastCards&&(changed||!hints.isEmpty())){
            payload["change"]=!changed?QStringLiteral("hint"):affirmative(snapshot,QStringLiteral("offer_is_reroll"))?QStringLiteral("reroll"):QStringLiteral("offer");
            lastOffer=offer;lastCards=key;append(QStringLiteral("cards"),payload,time,championId);
        }
    }else if(offerStatus.isString()&&offerStatus!=QStringLiteral("open")){
        lastOffer.clear();lastCards.clear();
    }
    QJsonArray selected; QSet<int> selectedIds;
    const auto selectedInput = snapshot.value("confirmed_selected").toArray();
    for (int i = 0; i < std::min(16, int(selectedInput.size())); ++i) {
        const int id = identity(selectedInput[i]); if (selected.size() >= 8) break;
        if (catalog.augments.contains(id) && !selectedIds.contains(id)) { selectedIds.insert(id); selected.append(id); }
    }
    if (!selected.isEmpty()) {
        QJsonObject payload{{"augment_ids", selected}, {"complete", affirmative(snapshot, QStringLiteral("selected_complete"))}};
        const auto source=snapshot.value("selection_source");
        if(source==QStringLiteral("manual_confirmation")||source==QStringLiteral("visible_self_hud_tooltip"))payload["source"]=source;
        const auto key = signature(payload); if (key != lastSelection) { lastSelection = key; append(QStringLiteral("selected"), payload, time, championId); }
    }
    const auto inventory=snapshot.value("own_inventory");
    if(validIds(inventory,10)){
        const auto current=inventory.toArray();
        if(inventoryObserved&&current!=lastInventory){
            auto unused=lastInventory;
            QJsonArray added;
            for(const auto &id:current){
                qsizetype found=0;
                while(found<unused.size()&&unused[found]!=id)++found;
                if(found==unused.size())added.append(id);else unused.removeAt(found);
            }
            if(!added.isEmpty()||!unused.isEmpty())append(QStringLiteral("inventory"),QJsonObject{{"added",added},{"removed",unused}},time,championId);
        }
        lastInventory=current;inventoryObserved=true;
    }
    const auto cue = snapshot.value("equipment_cues").toObject().value("latest").toObject();
    const QString cueContext = uuid(cue.value("context_id").toString()), team = cue.value("team").toString();
    const int cueItem = identity(cue.value("item_id")), cueChampion = identity(cue.value("champion_id"));
    if (!cueContext.isEmpty() && cueContext == context && cue.value("kind") == QStringLiteral("enemy_item_added")
        && cue.value("source") == QStringLiteral("live_client_inventory_difference") && (team == QStringLiteral("ORDER") || team == QStringLiteral("CHAOS"))
        && catalog.items.contains(cueItem) && catalog.champions.contains(cueChampion)
        && number(cue.value("count"), 1, 6) && std::floor(cue.value("count").toDouble()) == cue.value("count").toDouble()
        && number(cue.value("game_time"), 0, time)) {
        const QJsonObject payload{{"kind", QStringLiteral("enemy_item_added")}, {"team", team}, {"item_id", cueItem},
            {"champion_id", cueChampion}, {"count", cue.value("count").toInt()}, {"game_time", cue.value("game_time")}};
        const auto key = signature(payload); if (key != lastCue) { lastCue = key; append(QStringLiteral("equipment"), payload, cue.value("game_time").toDouble(), championId); }
    }
    if(archive.isEmpty())refreshPresentation();
}
QJsonObject CoachReplay::archiveSnapshot(const QString &expectedContext) const {
    if(synthetic||context.isEmpty()||context!=uuid(expectedContext)||records.isEmpty())return {};
    return {{"schema_version",1},{"last_time",lastTime},{"entries",records}};
}
bool CoachReplay::saveArchive(const QString &directory, const QString &verifiedMatchRef, const QJsonObject &captured) {
    const auto path=archivePath(directory,verifiedMatchRef);
    if(path.isEmpty()||!validArchive(captured)||QFileInfo(path).isSymLink()||!QDir().mkpath(directory))return false;
    auto document=captured;
    document["match_ref"]=verifiedMatchRef;
    const auto bytes=signature(document);
    if(bytes.size()>ArchiveByteLimit)return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit();
}
bool CoachReplay::archiveAvailable(const QString &directory, const QString &verifiedMatchRef) {
    return !readArchive(directory,verifiedMatchRef).isEmpty();
}
bool CoachReplay::openArchive(const QString &directory, const QString &verifiedMatchRef) {
    const auto loaded=readArchive(directory,verifiedMatchRef);
    if(loaded.isEmpty())return false;
    archive=loaded;resetPresentation();
    return true;
}
void CoachReplay::showLive() {
    if(archive.isEmpty())return;
    archive={};resetPresentation();
}
QJsonObject CoachReplay::report() const {
    int visible=0;for(int i=0;i<list->topLevelItemCount();++i)visible+=!list->topLevelItem(i)->isHidden();
    const bool archived=!archive.isEmpty();
    const auto shown=archived?archive.value("entries").toArray():records;
    return {{"schema_version", 1}, {"scope", archived?QStringLiteral("local_match_archive"):QStringLiteral("current_match_memory_only")},
        {"status", archived?QStringLiteral("archived"):status}, {"archived",archived},
        {"has_context", archived||!context.isEmpty()}, {"synthetic", !archived&&synthetic}, {"entry_count", shown.size()}, {"limit", Limit},
        {"entries", shown}, {"visible_rows", visible}, {"active_filter",activeFilter}, {"follow_latest",followLatest}, {"empty_state",shown.isEmpty()}, {"detail_visible",!detail->isHidden()}, {"presentation",QStringLiteral("kda_icon_timeline")}, {"embedded",embedded}, {"raw_json_visible",false}, {"titlebar_height",embedded?0:DashboardChrome::TitleBarHeight}, {"disk_writer", false}, {"network", false}, {"game_execution_controls", false}};
}
