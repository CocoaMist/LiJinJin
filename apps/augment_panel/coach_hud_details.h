#pragma once
#include "hud_symbols.h"
#include "kda_theme.h"
#include "kda_surface.h"
#include "kda_hud_style.h"
#include <QLabel>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>
#include <QComboBox>

class CoachStrategyCombo final : public QComboBox {
public:
    explicit CoachStrategyCombo(QWidget *parent=nullptr):QComboBox(parent){
        setFixedSize(40,24);setCursor(Qt::PointingHandCursor);
        connect(this,&QComboBox::currentIndexChanged,this,[this]{setAccessibleName(QStringLiteral("出装策略：")+currentText());setToolTip(accessibleName());update();});
    }
    QSize sizeHint()const override{return {40,24};}
    QSize minimumSizeHint()const override{return sizeHint();}
protected:
    void paintEvent(QPaintEvent *)override{
        Kda::OverlayCanvas canvas(this);auto &p=canvas.paint();
        p.setPen(hasFocus()?QPen(Kda::color("silver"),.9):underMouse()?QPen(Kda::color("border"),.65):QPen(Qt::NoPen));auto wash=Kda::color("raised");wash.setAlpha(underMouse()||hasFocus()?120:32);p.setBrush(wash);
        p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),Kda::radius("tag"),Kda::radius("tag"));
        const auto choice=currentData().toString();const auto icon=choice.contains(QStringLiteral("承伤"))?HudSymbols::Symbol::Shield:choice.contains(QStringLiteral("普攻"))?HudSymbols::Symbol::Sword:choice.contains(QStringLiteral("技能"))?HudSymbols::Symbol::Star:HudSymbols::Symbol::Team;
        HudSymbols::draw(p,icon,QRectF(5,4,16,16),Kda::color("silver"));p.save();p.translate(29,12);p.rotate(90);HudSymbols::draw(p,HudSymbols::Symbol::Chevron,QRectF(-4,-4,8,8),Kda::color("muted"));p.restore();
    }
};

class CoachContextStrip final : public QLabel {
public:
    using QLabel::QLabel;
    void setOutlook(const QJsonObject &value){outlook=value;update();}
    void setContext(const QJsonObject &next,double seconds,bool fresh){scores=next;time=seconds;active=fresh;setProperty("liveContext",active);update();}
protected:
    void paintEvent(QPaintEvent *event)override{
        if(active&&outlook.value("available").toBool()){
            Kda::OverlayCanvas canvas(this);auto &p=canvas.paint();p.setFont(Kda::hudFont(Kda::FontRole::Numeric,12,QFont::DemiBold));
            const auto axes=outlook.value("axes").toObject();const auto kills=axes.value("kills").toObject(),alive=axes.value("alive").toObject(),equipment=axes.value("catalogue_equipment_value").toObject();
            const auto counter=[&](int x,int w,HudSymbols::Symbol icon,const QString &value){HudSymbols::draw(p,icon,QRectF(x,3,12,12),Kda::HudStyle::color("muted"));p.setPen(Kda::HudStyle::color("silver"));p.drawText(QRect(x+16,0,w-16,height()),Qt::AlignLeft|Qt::AlignVCenter,value);};
            counter(0,67,HudSymbols::Symbol::Sword,kills.value("available").toBool()?QStringLiteral("%1:%2").arg(kills.value("ally").toInt()).arg(kills.value("enemy").toInt()):QStringLiteral("—"));
            counter(71,57,HudSymbols::Symbol::Team,alive.value("available").toBool()?QStringLiteral("%1:%2").arg(alive.value("ally").toInt()).arg(alive.value("enemy").toInt()):QStringLiteral("—"));
            const int gap=equipment.value("delta").toInt();const auto amount=std::abs(gap)>=1000?QString::number(gap/1000.,'f',1)+QStringLiteral("k"):QString::number(gap);
            if(width()>=195)counter(137,width()-137,HudSymbols::Symbol::Coins,equipment.value("available").toBool()?QStringLiteral("≈")+(gap>0?QStringLiteral("+"):QString())+amount:QStringLiteral("—"));return;
        }
        if(!active||!text().isEmpty()){QLabel::paintEvent(event);return;}
        Kda::OverlayCanvas canvas(this);auto &p=canvas.paint();const auto f=Kda::hudFont(Kda::FontRole::Numeric,14);p.setFont(f);p.setPen(Kda::color("silver"));
        QStringList parts;for(const auto &key:{"kills","deaths","assists"}){const auto v=scores.value(QString::fromLatin1(key));parts.append(v.isDouble()?QString::number(v.toInt()):QStringLiteral("—"));}
        HudSymbols::draw(p,HudSymbols::Symbol::Sword,QRectF(0,2,14,14),Kda::color("silver"));
        const int clockX=std::max(78,width()-51);p.drawText(QRect(18,0,std::max(0,clockX-21),height()),Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(f).elidedText(parts.join('/'),Qt::ElideRight,clockX-21));
        if(clockX+51<=width()){HudSymbols::draw(p,HudSymbols::Symbol::Clock,QRectF(clockX,2,14,14),Kda::color("muted"));const int seconds=std::max(0,int(time));p.drawText(QRect(clockX+17,0,34,height()),Qt::AlignLeft|Qt::AlignVCenter,QStringLiteral("%1:%2").arg(seconds/60).arg(seconds%60,2,10,QChar(u'0')));}
    }
private:
    QJsonObject scores,outlook;double time=0;bool active=false;
};

// Static position feedback for the existing automatic presentation state.
// It is not a second navigation controller and never advances the carousel.
class CoachPageIndicator final : public QWidget {
public:
    explicit CoachPageIndicator(QWidget *parent=nullptr):QWidget(parent){
        setFixedSize(40,18);setFocusPolicy(Qt::NoFocus);
    }
    void setPage(const QString &value,bool automatic,bool historyAvailable){
        const bool changed=page!=value||autoMode!=automatic||historyReady!=historyAvailable;
        page=value;autoMode=automatic;historyReady=historyAvailable;
        const QString name=page=="purchase"?QStringLiteral("出装"):page=="roster"?QStringLiteral("阵容"):page=="history"?QStringLiteral("战绩"):QStringLiteral("等待");
        const auto label=(autoMode?QStringLiteral("自动 · "):QStringLiteral("手动 · "))+name;
        setToolTip(label);setAccessibleName(label);if(changed)update();
    }
    QJsonObject report()const{return {{"page",page},{"automatic",autoMode},{"history_available",historyReady},{"interactive",false},{"animated",false},{"theme",Kda::HudStyle::identity()},{"fixed_page_order",QJsonArray{"purchase","roster"}},{"width",width()},{"height",height()}};}
protected:
    void paintEvent(QPaintEvent *)override{
        Kda::OverlayCanvas canvas(this);auto &painter=canvas.paint();
        const QString pages[]{"purchase","roster"};
        const HudSymbols::Symbol symbols[]{HudSymbols::Symbol::Coins,HudSymbols::Symbol::Team};
        for(int i=0;i<2;++i){
            const bool selected=page==pages[i],available=i!=2||historyReady||selected;
            QColor ink=Kda::color(selected?"silver":"muted");if(!available)ink.setAlpha(85);
            if(selected){
                painter.setPen(QPen(Kda::color("silver"),autoMode?1.:1.5));
                painter.drawLine(QPointF(i*20+5,16.5),QPointF(i*20+15,16.5));
                if(!autoMode)ink=Kda::color("silver");
            }
            HudSymbols::draw(painter,symbols[i],QRectF(i*20+4,4,11,11),ink,1.4);
        }
    }
private:
    QString page;bool autoMode=true,historyReady=false;
};

class CoachEconomyStrip final : public QWidget {
public:
    explicit CoachEconomyStrip(QWidget *parent=nullptr):QWidget(parent){
        setFixedHeight(24);setMinimumWidth(0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
        setFocusPolicy(Qt::NoFocus);
    }
    void setMessage(const QString &value){message=value;values.clear();setAccessibleName(value);setToolTip(value);update();}
    void setValues(const QStringList &next){
        if(values==next&&message.isEmpty())return;values=next;message.clear();QStringList labels;
        for(int i=0;i<values.size()&&i<4;++i)labels.append(names()[i]+QStringLiteral(" ")+values[i]);
        const auto semantic=labels.join(QStringLiteral(" · "));setAccessibleName(semantic);setToolTip(semantic);update();
    }
    QJsonObject report()const{
        const QFontMetrics fm(valueFont());QJsonArray shown;bool fits=true;
        for(int i=0;i<values.size()&&i<3;++i){const auto bounds=valueRect(i);const auto text=fm.elidedText(values[i],Qt::ElideRight,bounds.width());shown.append(text);fits&=rect().contains(bounds)&&fm.height()<=bounds.height()&&fm.horizontalAdvance(text)<=bounds.width();}
        if(!message.isEmpty())fits&=QFontMetrics(messageFont()).height()<=height();
        return {{"values",QJsonArray::fromStringList(values)},{"displayed_values",shown},{"labels",QJsonArray::fromStringList(names())},{"message",message},{"displayed_message",QFontMetrics(messageFont()).elidedText(message,Qt::ElideRight,width())},{"render_fits",fits},{"geometry",QJsonArray{x(),y(),width(),height()}},{"icon_stats",true},{"numeric_font_px",16},{"stat_icon_px",14},{"data_surface","quiet_dark"},{"material_placement","none"}};
    }
protected:
    void paintEvent(QPaintEvent *)override{
        Kda::OverlayCanvas canvas(this);auto &painter=canvas.paint();
        if(!message.isEmpty()){painter.setFont(messageFont());painter.setPen(Kda::color("muted"));painter.drawText(rect(),Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(painter.font()).elidedText(message,Qt::ElideRight,width()));return;}
        const HudSymbols::Symbol icons[]{HudSymbols::Symbol::Sword,HudSymbols::Symbol::Team,HudSymbols::Symbol::Coins,HudSymbols::Symbol::Collapse};
        painter.setFont(valueFont());const QFontMetrics fm(painter.font());
        for(int i=0;i<values.size()&&i<3;++i){const auto bounds=valueRect(i);
            const QRectF symbol(bounds.x()-18,bounds.y()+3,14,14);
            HudSymbols::draw(painter,icons[i],symbol,Kda::color("muted"),1.5);
            painter.setPen(Kda::color(values[i]==QStringLiteral("—")?"muted":"silver"));
            painter.drawText(bounds,Qt::AlignLeft|Qt::AlignVCenter,fm.elidedText(values[i],Qt::ElideRight,bounds.width()));
        }
    }
private:
    static QStringList names(){return {QStringLiteral("击杀"),QStringLiteral("装备差"),QStringLiteral("金币"),QStringLiteral("等级差")};}
    static QFont valueFont(){return Kda::hudFont(Kda::FontRole::Numeric,16);}
    static QFont messageFont(){return Kda::hudFont(Kda::FontRole::Body,12,QFont::Medium);}
    QRect valueRect(int index)const{const int cell=width()/3;return QRect(index*cell+18,1,std::max(0,cell-20),22);}
    QStringList values;QString message;
};

// Pure presentation of engine-qualified conditional purchase results.
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace CoachAdaptivePurchase {
inline bool number(const QJsonValue &v) { return v.isDouble() && std::isfinite(v.toDouble()); }
inline QString amount(const QJsonValue &v, int decimals=0, bool delta=false) {
    if(!number(v))return QStringLiteral("—");
    return (delta&&v.toDouble()>0?QStringLiteral("+"):QString())+QString::number(v.toDouble(),'f',decimals);
}
inline QString metricName(const QString &key) {
    if(key==QStringLiteral("effective_damage_dealt"))return QStringLiteral("有效伤害");
    if(key==QStringLiteral("remaining_health"))return QStringLiteral("结束生命");
    if(key==QStringLiteral("effective_healing_received"))return QStringLiteral("有效治疗");
    if(key==QStringLiteral("shield_absorbed"))return QStringLiteral("护盾吸收");
    if(key==QStringLiteral("generated_self_shield"))return QStringLiteral("自身护盾");
    if(key==QStringLiteral("permanent_health_growth"))return QStringLiteral("本次估算新增永久生命");
    if(key==QStringLiteral("health_cost_received"))return QStringLiteral("生命消耗");
    return {};
}
inline QString metricsText(const QJsonObject &values, bool delta) {
    QStringList parts;
    for(const auto &key:{"effective_damage_dealt","remaining_health","effective_healing_received","shield_absorbed","generated_self_shield","permanent_health_growth","health_cost_received"}) {
        const auto name=QString::fromLatin1(key);if(!values.contains(name))continue;
        parts.append(metricName(name)+QStringLiteral(" ")+amount(values.value(name),0,delta));
    }
    return parts.isEmpty()?QStringLiteral("指标 —"):parts.join(QStringLiteral(" · "));
}
inline QString reasonText(const QString &reason) {
    if(reason==QStringLiteral("current_gold_unobserved"))return QStringLiteral("当前金币未观察到");
    if(reason==QStringLiteral("growth_inventory_transition_unverified"))return QStringLiteral("合成或更换装备后的成长继承尚未核实");
    if(reason==QStringLiteral("critical_inputs_or_complete_chain_comparison_unavailable"))return QStringLiteral("关键输入或完整候选比较尚未齐备");
    if(reason==QStringLiteral("explicit_chain_source_validity_missing_or_expired")||reason==QStringLiteral("snapshot_expired"))return QStringLiteral("条件来源或本局数据已失效，等待重算");
    if(reason==QStringLiteral("explicit_chain_binding_mismatch")||reason==QStringLiteral("explicit_chain_source_binding_invalid"))return QStringLiteral("动作条件与当前局面不一致，等待重算");
    if(reason==QStringLiteral("referenced_skill_current_cooldown_required"))return QStringLiteral("动作涉及的技能冷却尚未核实");
    if(reason==QStringLiteral("no_legal_finished_next_purchase"))return QStringLiteral("当前没有合法的下一件成装");
    if(reason.contains(QStringLiteral("heartsteel"))||reason.contains(QStringLiteral("item:3084:permanent_hp")))return QStringLiteral("心之钢成长 —，待核实");
    if(reason.startsWith(QStringLiteral("growth_input_unobserved:")))return QStringLiteral("成长层数或触发计数 —，待核实");
    if(reason.startsWith(QStringLiteral("unobserved_event:augment:")))return QStringLiteral("海克斯触发次数或命中前提 —，待核实");
    if(reason.startsWith(QStringLiteral("skill_semantic_quarantine:")))return QStringLiteral("技能 %1 的命中或效果前提待核实").arg(reason.section(QChar(u':'),1,1));
    if(reason.contains(QStringLiteral("time_budget")))return QStringLiteral("本轮比较未完成，等待重算");
    return QStringLiteral("还有未核实的动作、技能或装备条件");
}
struct View {
    bool active=false,ranking=false,selected=false;
    int itemId=0;
    QString state,title,status,html,tooltip;
    QJsonObject report;
};
inline View fromPlan(const QJsonObject &plan,double gameTime,const std::function<QString(int)> &itemName,const QJsonObject &quality={}) {
    View view;const auto sandbox=plan.value("purchase_sandbox").toObject();
    if(sandbox.value("policy")!=QStringLiteral("explicit_chain_next_purchase_v1"))return view;
    view.active=true;
    const auto planComparison=plan.value("explicit_chain_condition_comparison").toObject(),sandboxComparison=sandbox.value("explicit_chain_condition_comparison").toObject();
    const auto comparison=planComparison.isEmpty()?sandboxComparison:planComparison;
    const bool coherent=planComparison.isEmpty()||sandboxComparison.isEmpty()||planComparison==sandboxComparison;
    const auto source=comparison.value("source").toObject();
    const bool declared=source.value("kind")==QStringLiteral("caller_declared_scenario");
    const auto until=source.value("valid_until_game_time"),from=source.value("valid_from_game_time");
    const bool lease=declared&&std::isfinite(gameTime)&&number(from)&&number(until)&&from.toDouble()<=gameTime&&gameTime<=until.toDouble();
    const auto unknowns=sandbox.value("critical_unknowns").toArray();
    const bool complete=sandbox.value("enumeration_complete")==QJsonValue(true)&&comparison.value("complete_candidate_universe")==QJsonValue(true);
    const auto nativeStatus=sandbox.value("status").toString();
    const bool completed=nativeStatus==QStringLiteral("conditional_chain_purchase_available")||nativeStatus==QStringLiteral("no_affordable_positive_chain_gain");
    const auto optionalTrue=[](const QJsonObject &object,const QString &key){return !object.contains(key)||object.value(key)==QJsonValue(true);};
    const bool outerAllowed=optionalTrue(plan,QStringLiteral("ranking_available"))&&optionalTrue(plan,QStringLiteral("primary_purchase_visible"))&&optionalTrue(quality,QStringLiteral("ranking_available"))&&optionalTrue(quality,QStringLiteral("primary_purchase_visible"));
    view.ranking=coherent&&completed&&outerAllowed&&sandbox.value("score_basis")==QStringLiteral("explicit_chain_condition_comparison")&&sandbox.value("ranking_available")==QJsonValue(true)&&comparison.value("ranking_available")==QJsonValue(true)&&complete&&sandbox.value("critical_unknowns").isArray()&&unknowns.isEmpty()&&lease;
    auto rows=sandbox.value("candidates").toArray();QJsonObject chosen;
    if(rows.isEmpty())view.ranking=false;
    const int selected=sandbox.value("selected_item_id").toInt();
    for(const auto &value:rows) {
        const auto row=value.toObject();
        if(row.value("item_id").toInt()==selected&&selected>0)chosen=row;
        if(row.value("ranking_eligible")!=QJsonValue(true)||!number(row.value("score_gain"))||!row.value("event_admission_changes").isArray()||!row.value("event_admission_changes").toArray().isEmpty()||row.value("mechanism_admission_changed")!=QJsonValue(false))view.ranking=false;
    }
    const bool nativeSelection=comparison.value("selection_available")==QJsonValue(true);
    const auto purchaseCost=chosen.value("remaining_cost"),transactionCost=chosen.value("transaction").toObject().value("remaining_cost");
    const bool costVerified=number(purchaseCost)&&purchaseCost.toDouble()>=0&&number(transactionCost)&&transactionCost.toDouble()>=0&&purchaseCost.toDouble()==transactionCost.toDouble();
    view.selected=view.ranking&&nativeStatus==QStringLiteral("conditional_chain_purchase_available")&&nativeSelection&&costVerified&&selected>0&&!chosen.isEmpty()&&chosen.value("affordable_now")==QJsonValue(true)&&number(chosen.value("score_gain"))&&chosen.value("score_gain").toDouble()>0;
    view.itemId=view.selected?selected:0;
    view.state=view.selected?QStringLiteral("conditional_selected"):view.ranking?QStringLiteral("no_current_purchase"):QStringLiteral("unavailable");
    view.title=view.selected?itemName(view.itemId):view.ranking?QStringLiteral("暂不购买"):QStringLiteral("暂不排序");
    const auto unavailable=sandbox.value("unavailable_reason").toString();
    view.status=view.selected?QStringLiteral("条件比较 · 预算内"):view.ranking?(nativeStatus==QStringLiteral("no_affordable_positive_chain_gain")?QStringLiteral("预算内暂无更优候选"):!costVerified?QStringLiteral("合成费用待核实"):QStringLiteral("购买前提待核实")):!lease?QStringLiteral("动作条件待更新"):!complete?QStringLiteral("完整比较待齐"):QStringLiteral("关键条件待核实");
    const auto safe=[](const QString &s){return s.toHtmlEscaped();};
    QString html;
    if(view.selected) {
        html+=QStringLiteral("<p><b>本次估算变化</b><br>%1</p>").arg(safe(metricsText(chosen.value("metric_deltas").toObject(),true)));
        const auto transaction=chosen.value("transaction").toObject();QStringList consumed,after;
        for(const auto &id:transaction.value("consume_item_ids").toArray())if(id.toInt()>0)consumed.append(itemName(id.toInt()));
        for(const auto &id:transaction.value("inventory_after").toArray())if(id.toInt()>0)after.append(itemName(id.toInt()));
        html+=QStringLiteral("<p><small>尚需 %1G%2<br>合成后背包 · %3<br>成长继承 · %4</small></p>").arg(amount(chosen.value("remaining_cost")),consumed.isEmpty()?QString():QStringLiteral(" · 消耗 ")+safe(consumed.join(QStringLiteral(" / "))),after.isEmpty()?QStringLiteral("—"):safe(after.join(QStringLiteral(" / "))),transaction.value("growth_inventory_transition").toObject().value("numeric_transition_verified")==QJsonValue(true)?QStringLiteral("已核实当前交易"):QStringLiteral("—，待核实"));
    }
    else if(!unknowns.isEmpty()) {
        QStringList labels;for(const auto &u:unknowns){const auto label=reasonText(u.toString());if(!labels.contains(label))labels.append(label);}
        html+=QStringLiteral("<p><b>关键未知</b><br>%1</p>").arg(safe(labels.join(QStringLiteral("；"))));
    }
    else html+=QStringLiteral("<p><b>%1</b><br>%2</p>").arg(view.title,safe(view.ranking?(nativeStatus==QStringLiteral("no_affordable_positive_chain_gain")?QStringLiteral("按当前条件和配方估价，预算内暂无收益更高的候选。"):!costVerified?QStringLiteral("合成费用尚未核实，暂不提示购买。"):QStringLiteral("当前购买前提尚未核实，先看估算变化。")):!outerAllowed?quality.value("summary").toString(QStringLiteral("还需补齐关键条件。")):!unavailable.isEmpty()?reasonText(unavailable):!lease?QStringLiteral("动作前提或当前局面已变化，等待重新估算。"):!complete?QStringLiteral("还有装备未完成比较，暂不能确定首选。"):QStringLiteral("关键条件待核实。")));
    html+=QStringLiteral("<p><small>%1</small></p>").arg(declared?QStringLiteral("按给定动作估算 · 条件参考"):QStringLiteral("动作前提待核实 · 条件参考"));
    html+=QStringLiteral("<p><small>基线 · %1</small></p>").arg(safe(metricsText(comparison.value("baseline_metrics").toObject(),false)));
    const auto currentGrowth=plan.value("equipment_growth_state").toObject();QStringList observedGrowth;
    for(const auto &value:currentGrowth.value("entries").toArray()) {
        const auto entry=value.toObject();if(entry.value("actual_observation_attested")!=QJsonValue(true)||entry.value("value_supplied")!=QJsonValue(true)||!number(entry.value("value")))continue;
        const bool item=entry.value("source")==QStringLiteral("item");
        const QString name=item?itemName(entry.value("id").toInt()):QStringLiteral("海克斯成长");
        const QString unit=entry.value("kind")==QStringLiteral("permanent_health_while_item_held")?QStringLiteral("额外生命"):QStringLiteral("成长计数");
        observedGrowth.append(name+QStringLiteral(" · ")+unit+QStringLiteral(" ")+amount(entry.value("value")));
    }
    for(const auto &value:currentGrowth.value("quest_progress").toArray()) {
        const auto quest=value.toObject();if(quest.value("progress_observed")!=QJsonValue(true)||!number(quest.value("progress")))continue;
        observedGrowth.append(QStringLiteral("任务进度 %1 / %2（完成奖励未据此认定）").arg(amount(quest.value("progress")),amount(quest.value("threshold"))));
    }
    html+=QStringLiteral("<p><small>本局已观察成长 · %1</small></p>").arg(observedGrowth.isEmpty()?QStringLiteral("—"):safe(observedGrowth.join(QStringLiteral("；"))));
    QList<QJsonObject> shown;for(const auto &value:rows)shown.append(value.toObject());
    if(!view.ranking)std::sort(shown.begin(),shown.end(),[](const auto &a,const auto &b){return a.value("item_id").toInt()<b.value("item_id").toInt();});
    html+=QStringLiteral("<p><b>%1</b></p>").arg(view.ranking?QStringLiteral("备选与取舍"):QStringLiteral("目前能算出的变化 · 条件未齐"));
    int displayed=0;
    for(const auto &row:shown) {
        const int id=row.value("item_id").toInt();if(id<=0||(view.selected&&id==view.itemId))continue;if(displayed++>=2)break;
        const auto cost=row.value("remaining_cost");
        const auto rowTransactionCost=row.value("transaction").toObject().value("remaining_cost");
        const bool rowCostKnown=number(cost)&&cost.toDouble()>=0&&number(rowTransactionCost)&&rowTransactionCost.toDouble()>=0&&cost.toDouble()==rowTransactionCost.toDouble();
        const QString budget=!rowCostKnown?QStringLiteral("费用待核实"):row.value("affordable_now").isBool()?(row.value("affordable_now").toBool()?QStringLiteral("金币可覆盖"):QStringLiteral("当前预算不足")):QStringLiteral("预算 —");
        html+=QStringLiteral("<p><b>%1</b> · %2G · %3<br>%4").arg(safe(itemName(id)),rowCostKnown?amount(cost):QStringLiteral("—"),budget,safe(metricsText(row.value("metric_deltas").toObject(),true)));
        if(!row.value("event_admission_changes").toArray().isEmpty()||row.value("mechanism_admission_changed")==QJsonValue(true))html+=QStringLiteral("<br><small>动作或机制触发前提改变，当前不能直接比较。</small>");
        if(view.ranking&&row.value("affordable_now")==QJsonValue(false)&&rowCostKnown)html+=QStringLiteral("<br><small>金币达到 %1G 时可重新比较，不保证届时仍选它。</small>").arg(amount(cost));
        html+=QStringLiteral("</p>");
    }
    if(!displayed)html+=QStringLiteral("<p><small>其他可核候选 —</small></p>");
    html+=QStringLiteral("<p><b>何时改判</b><br><small>已选海克斯、当前装备、成长层数、生命或技能状态变化后，用新局面重算。未观察到的层数与任务进度保持 —。</small></p>");
    if(number(until))html+=QStringLiteral("<p><small>条件有效至对局 %1:%2，失效后撤下本轮选择。</small></p>").arg(int(until.toDouble())/60).arg(int(until.toDouble())%60,2,10,QChar(u'0'));
    const auto health=sandbox.value("health_basis").toObject();const auto focus=comparison.value("focus_player_key");bool healthFound=false;
    for(const auto &value:health.value("native_initial_states").toArray()) {
        const auto actual=value.toObject();if(focus.isUndefined()||actual.value("player_key")!=focus)continue;
        html+=QStringLiteral("<p><small>本次采用的起始生命 %1%2</small></p>").arg(amount(actual.value("initial_health")),health.value("current_health_used").toBool()?QStringLiteral(" · 绑定当前观测"):QStringLiteral(" · 按给定条件"));healthFound=true;break;
    }
    if(!healthFound)html+=QStringLiteral("<p><small>本次采用的起始生命 —</small></p>");
    html+=QStringLiteral("<p><small>在同一组给定动作与目标下比较；动作能否在实战发生仍需核实。</small></p>");
    view.html=html;view.tooltip=QStringLiteral("按给定动作估算 · 条件参考\n")+view.status;
    view.report={{"active",true},{"state",view.state},{"native_status",nativeStatus},{"ranking_available",view.ranking},{"native_selection_available",nativeSelection},{"purchase_cost_verified",costVerified},{"selected_item_id",view.itemId},{"selected_score_gain",view.selected?chosen.value("score_gain"):QJsonValue(QJsonValue::Null)},{"objective_basis",comparison.value("objective_basis")},{"source_kind",source.value("kind")},{"source_lease_current",lease},{"comparison_fields_coherent",coherent},{"complete_candidate_universe",complete},{"quality_gate_allowed",outerAllowed},{"critical_unknowns",unknowns},{"unavailable_reason",unavailable},{"score_semantics","unitless_conditional_objective_delta"},{"actual_win_rate",QJsonValue(QJsonValue::Null)},{"ui_score_calculation",false},{"nested_batch_winners_read",false}};
    return view;
}
}
