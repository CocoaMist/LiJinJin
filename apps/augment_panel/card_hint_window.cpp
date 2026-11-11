#include "card_hint_window.h"
#include "hud_symbols.h"
#include "kda_theme.h"
#include "kda_surface.h"
#include "kda_hud_style.h"
#include <QFontMetrics>
#include <QHideEvent>
#include <QJsonArray>
#include <QLinearGradient>
#include <QPainter>
#include <QRegularExpression>
#include <QStringList>
#include <QTextLayout>
#include <algorithm>
#include <cmath>

namespace {
struct HintTextLayout {
    QFont titleFont, metricFont, badgeFont, captionFont, unitFont, axisFont, historyFont;
    QRect content, title, metric, badge, caption, serial, number, unit, icon, status, statusSurface;
    QRect outputIcon,output,healthIcon,health,estimate,history;
    QString outputText,healthText,explanationText,historyText,rerollText;
    QString titleText, metricText, badgeText, captionText, valueText, unitText, serialText;
    bool titleElided = false, metricFits = false, linesFit = false, textSeparated = false;
    bool compactRows = false, numeric = false;
};
enum class ComparisonState { Recommended, Tied, Partial, Alternative, Advisory, Reference, Waiting };
bool usesDecisionScore(const QJsonObject &data) {
    return data.value("numeric_source") == QStringLiteral("local_decision_gain_percent");
}
bool numericMetric(const QString &metric, QString *number = nullptr) {
    if (!metric.startsWith(QStringLiteral("适配 ")) || !metric.endsWith('%')) return false;
    const auto text = metric.mid(3, metric.size() - 4).trimmed();
    bool okay = false;
    const double value = text.toDouble(&okay);
    if (!okay || !std::isfinite(value)) return false;
    if (number) *number = text;
    return true;
}
QString cardCopy(QString text) {
    // Remove orphaned units and unexpanded values, leaving real percentages intact.
    static const QRegularExpression unexpandedValue(QStringLiteral(
        R"((?:@[^@\s]+@|\{[^{}]+\}|（原卡数值）)\s*[%％]?)"));
    static const QRegularExpression emptyPercentGroup(QStringLiteral(
        R"([（(]\s*(?:[%％]\d{0,2}\s*[%％]?)?\s*[）)]\s*[%％]?)"));
    static const QRegularExpression orphanedPercent(QStringLiteral(
        R"((?<!\d)[%％]+(?:\d{1,2}[%％])?(?![\d%％]))"));
    static const QRegularExpression spacedPercent(QStringLiteral(R"((?<=\d)\s+([%％]))"));
    text.remove(unexpandedValue).remove(emptyPercentGroup)
        .replace(spacedPercent,QStringLiteral("\\1")).remove(orphanedPercent);
    return text.simplified();
}
QString displayMetric(const QString &metric) {
    if(numericMetric(metric))return metric;
    if(metric.startsWith(QStringLiteral("适配")))return QStringLiteral("暂不可算");
    const auto cleaned=cardCopy(metric);
    return cleaned.isEmpty()?QStringLiteral("暂不可算"):cleaned;
}
ComparisonState comparisonState(const QJsonObject &data, bool numeric) {
    if (!numeric) {
        if(data.value("advisory_preferred").toBool()&&data.value("advisory").toObject().value("available").toBool())return ComparisonState::Advisory;
        return data.value("source_mechanism_reference").toObject().value("available").toBool()?ComparisonState::Reference:ComparisonState::Waiting;
    }
    if (data.value("partial").toBool()) return ComparisonState::Partial;
    const auto label = data.value("recommendation").toString();
    if (label.contains(QStringLiteral("同优")) || label == QStringLiteral("均可")) return ComparisonState::Tied;
    return data.value("recommended").toBool() ? ComparisonState::Recommended : ComparisonState::Alternative;
}
struct PublicReferenceSample {
    QString caption, detail;
};
PublicReferenceSample publicReferenceSample(const QJsonObject &data) {
    const auto advisory=data.value("advisory").toObject();
    if(!advisory.value("available").toBool())return {};
    for(const auto &value:advisory.value("source_records").toArray()){
        const auto record=value.toObject();
        if(record.value("augment_id")!=data.value("augment_id"))continue;
        const auto eligibility=record.value("sample_eligibility").toObject();
        const auto reason=eligibility.value("reason").toString();
        const auto sample=record.value("sample_size").toInteger();
        if(reason==QStringLiteral("below_minimum_sample_count"))
            return {QStringLiteral("低样本"),QStringLiteral("公开资料 %1 局，低于比较门槛 %2 局").arg(sample).arg(eligibility.value("minimum_samples").toInt())};
        if(reason==QStringLiteral("sample_counts_conflict"))
            return {QStringLiteral("样本冲突"),QStringLiteral("公开资料的样本数与对局数不一致")};
        if(reason==QStringLiteral("sample_counts_unpublished_or_invalid"))
            return {QStringLiteral("样本未知"),QStringLiteral("公开资料缺少有效样本数")};
        if(eligibility.value("eligible").toBool()&&sample>0)
            return {QStringLiteral("%1局").arg(sample),QStringLiteral("公开资料样本 %1 局").arg(sample)};
        return {};
    }
    return {};
}
QJsonObject publicReferenceComparison(const QJsonObject &data) {
    return data.value("advisory").toObject().value("public_reference_comparison").toObject();
}
QString referenceComparisonBadge(const QJsonObject &data) {
    const auto comparison=publicReferenceComparison(data);
    const auto status=comparison.value("status");
    if(status==QStringLiteral("unknown")
        ||(status==QStringLiteral("close")&&comparison.value("leader_ids").toArray().contains(data.value("augment_id"))))
        return comparison.value("badge").toString();
    return {};
}
QString stateName(ComparisonState state) {
    switch (state) {
    case ComparisonState::Recommended: return QStringLiteral("recommended");
    case ComparisonState::Tied: return QStringLiteral("tied");
    case ComparisonState::Partial: return QStringLiteral("partial");
    case ComparisonState::Alternative: return QStringLiteral("alternative");
    case ComparisonState::Advisory: return QStringLiteral("advisory");
    case ComparisonState::Reference: return QStringLiteral("reference");
    case ComparisonState::Waiting: return QStringLiteral("waiting");
    }
    return {};
}
bool fits(const QRect &outer, const QRect &inner, const QFont &font, const QString &text) {
    if (text.isEmpty()) return true;
    const QFontMetrics fm(font);
    return outer.contains(inner) && inner.height() >= fm.height() && fm.horizontalAdvance(text) <= inner.width();
}
HintTextLayout textLayout(const QSize &size, const QJsonObject &data, int slot, bool showExtra=true) {
    Q_UNUSED(slot);
    HintTextLayout p;
    const int padding=std::clamp(qRound(size.width()*.022),5,8);
    p.content=QRect(padding,3,std::max(0,size.width()-padding*2),std::max(0,size.height()-6));
    p.titleFont=Kda::hudFont(Kda::FontRole::Body,12);
    p.badgeFont=Kda::hudFont(Kda::FontRole::Body,13,QFont::Bold);
    p.captionFont=Kda::hudFont(Kda::FontRole::Numeric,11,QFont::DemiBold);
    p.unitFont=Kda::hudFont(Kda::FontRole::Numeric,14,QFont::DemiBold);
    p.metricFont=Kda::hudFont(Kda::FontRole::Numeric,26,QFont::DemiBold);
    p.metricText=data.value("metric").toString();
    p.numeric=numericMetric(p.metricText,&p.valueText);
    bool extraBadge=false;
    if(p.numeric){
        p.unitText=QStringLiteral("%");p.badgeText=data.value("recommendation").toString();
        p.captionText=usesDecisionScore(data)?QStringLiteral("3/8/20s"):QStringLiteral("≈8s");
        const auto reroll=data.value("reroll_comparison").toObject();
        const auto relation=data.value("rule_basis").toObject().value("patch_relation").toString(reroll.value("patch_relation").toString());
        const int duration=data.value("duration_hint_seconds").toInt();
        if(relation==QStringLiteral("mismatch"))p.captionText=QStringLiteral("旧版模型");
        else if(relation==QStringLiteral("unknown"))p.captionText=QStringLiteral("本地模型");
        else if(duration==3||duration==20)p.captionText=QStringLiteral("%1秒领先").arg(duration);
        if(showExtra&&size.width()>=200&&reroll.value("available").toBool()){
            const auto label=reroll.value("label").toString();
            if(!label.isEmpty()){
                p.rerollText=QStringLiteral("重随")+label;
                p.captionText+=QStringLiteral(" · ")+p.rerollText;
            }
        }
        if(p.badgeText.isEmpty())p.badgeText=data.value("partial").toBool()?QStringLiteral("部分"):data.value("rank").toInt()>0?QStringLiteral("#%1").arg(data.value("rank").toInt()):QStringLiteral("待比较");
    }else{p.valueText=displayMetric(p.metricText);p.metricFont=Kda::hudFont(Kda::FontRole::Body,16,QFont::Bold);
        const auto state=comparisonState(data,false);
        if(state==ComparisonState::Advisory){
            const auto advisory=data.value("advisory").toObject();
            p.badgeText=data.value("advisory_label").toString(QStringLiteral("条件建议"));
            if(size.width()<230)p.badgeText=advisory.value("kind")==QStringLiteral("public_reference_preference")?QStringLiteral("倾向"):QStringLiteral("建议");
        }else if(showExtra&&size.width()>=220){
            p.badgeText=referenceComparisonBadge(data);
            extraBadge=!p.badgeText.isEmpty();
        }
        p.captionText=publicReferenceSample(data).caption;
    }
    const auto finite=[](const QJsonValue &v){return v.isDouble()&&std::isfinite(v.toDouble());};
    const auto reference=data.value("source_mechanism_reference").toObject();
    const auto guidance=cardCopy(data.value("decision_hint").toString(reference.value("decision_hint").toString(reference.value("condition").toString())));
    const bool explanation=!p.numeric&&(reference.value("available").toBool()||data.value("advisory").toObject().value("available").toBool())&&!guidance.isEmpty();
    int explanationHeight=explanation?(size.height()>=64?32:16):0;
    const bool axes=!explanation&&size.height()>=52&&(finite(data.value("output_gain_percent"))||finite(data.value("effective_health_gain_percent")));
    p.axisFont=Kda::hudFont(Kda::FontRole::Numeric,14,QFont::DemiBold);
    p.historyFont=Kda::hudFont(Kda::FontRole::Body,11);
    const auto history=data.value("historical_comparison").toString();
    const QFontMetrics historyMetrics(p.historyFont);
    const int historyHeight=historyMetrics.height(),historySpace=historyHeight+1;
    int axisSpace=axes?22:0;
    bool compactHistory=false;
    if(!history.isEmpty()&&(!p.numeric||size.width()>=180)&&historyMetrics.horizontalAdvance(history)<=p.content.width()){
        const auto smallAxis=Kda::hudFont(Kda::FontRole::Numeric,12,QFont::DemiBold);
        const int smallAxisSpace=axes?std::max(14,QFontMetrics(smallAxis).height())+1:0;
        const int oneRuleLine=explanation?17:0;
        const int spaceForTop=p.content.height()-historySpace-oneRuleLine;
        const bool standard=spaceForTop-axisSpace>=24;
        const int compactTop=p.numeric?19:std::max(16,QFontMetrics(Kda::hudFont(Kda::FontRole::Body,14,QFont::Bold)).height());
        if(standard||spaceForTop-smallAxisSpace>=compactTop){
            compactHistory=!standard;
            if(compactHistory){p.axisFont=smallAxis;axisSpace=smallAxisSpace;}
            if(explanation&&p.content.height()-historySpace-explanationHeight-1<24)explanationHeight=16;
            p.historyText=history;
            p.history=QRect(p.content.left(),p.content.bottom()-historyHeight+1,p.content.width(),historyHeight);
        }
    }
    const int detailBottom=p.history.isEmpty()?p.content.bottom():p.content.bottom()-historySpace;
    const int explanationSpace=explanation?explanationHeight+(p.history.isEmpty()?2:1):0;
    const int topHeight=detailBottom-p.content.top()+1-axisSpace-explanationSpace;
    const int iconSide=compactHistory?16:!p.numeric&&!p.badgeText.isEmpty()&&size.width()<180?16:explanation&&!p.numeric&&size.height()<56?18:24;
    if(explanation&&size.height()<48)p.metricFont.setPixelSize(14);
    if(compactHistory&&!p.numeric)p.metricFont.setPixelSize(14);
    const QFontMetrics badge(p.badgeFont),unit(p.unitFont);
    const int statusWidth=24+(p.badgeText.isEmpty()?0:6+badge.horizontalAdvance(p.badgeText));
    if(!p.numeric)p.captionFont=Kda::hudFont(Kda::FontRole::Body,11,QFont::DemiBold);
    const int captionWidth=std::max(28,QFontMetrics(p.captionFont).horizontalAdvance(p.captionText));
    const int scoreWidth=p.content.width()-statusWidth-10-captionWidth;
    if(p.numeric){
        const int minimum=compactHistory?15:18;
        int px=26;for(;px>minimum;--px){p.metricFont.setPixelSize(px);const QFontMetrics fm(p.metricFont);if(fm.height()<=topHeight&&fm.horizontalAdvance(p.valueText)+unit.horizontalAdvance(p.unitText)+3<=scoreWidth)break;}
        p.metricFont.setPixelSize(px);const QFontMetrics fm(p.metricFont);
        p.compactRows=size.width()<180||fm.height()>topHeight||fm.horizontalAdvance(p.valueText)+unit.horizontalAdvance(p.unitText)+3>scoreWidth;
    }
    if(p.compactRows){
        const int firstHeight=std::max(14,badge.height());
        p.icon=QRect(p.content.left(),p.content.top()+(topHeight-iconSide)/2,iconSide,iconSide);
        p.badge=QRect(p.icon.right()+7,p.content.top(),badge.horizontalAdvance(p.badgeText),firstHeight);
        const int availableHeight=topHeight-firstHeight-1;
        const int availableWidth=p.content.right()-p.icon.right()-7;
        int px=24;for(;px>14;--px){p.metricFont.setPixelSize(px);const QFontMetrics fm(p.metricFont);if(fm.height()<=availableHeight&&fm.horizontalAdvance(p.valueText)+unit.horizontalAdvance(p.unitText)+3<=availableWidth)break;}
        p.metricFont.setPixelSize(px);const QFontMetrics fm(p.metricFont);
        const int unitWidth=unit.horizontalAdvance(p.unitText),valueWidth=fm.horizontalAdvance(p.valueText);
        p.number=QRect(p.content.right()-unitWidth-3-valueWidth+1,p.content.top()+topHeight-fm.height(),valueWidth,fm.height());
        p.unit=QRect(p.number.right()+3,p.number.top()+fm.ascent()-unit.ascent(),unitWidth,unit.height());
        p.caption=QRect(p.content.right()-captionWidth+1,p.content.top(),captionWidth,firstHeight);
    }else{
        p.icon=QRect(p.content.left(),p.content.top()+(topHeight-iconSide)/2,iconSide,iconSide);
        if(!p.badgeText.isEmpty())p.badge=QRect(p.icon.right()+7,p.content.top()+(topHeight-badge.height())/2,badge.horizontalAdvance(p.badgeText),badge.height());
        const QFontMetrics fm(p.metricFont);
        const int valueWidth=fm.horizontalAdvance(p.valueText),unitWidth=unit.horizontalAdvance(p.unitText);
        const int x=p.numeric?p.content.right()-unitWidth-3-valueWidth+1:std::max(p.icon.right(),p.badge.right())+7;
        if(!p.numeric){
            int availableWidth=p.content.right()-x+1;
            if(!p.captionText.isEmpty()){
                const int metricReserve=std::min(valueWidth,fm.horizontalAdvance(QStringLiteral("触发…")));
                if(captionWidth+5+metricReserve<=availableWidth&&QFontMetrics(p.captionFont).height()<=topHeight){
                    p.caption=QRect(p.content.right()-captionWidth+1,p.content.top(),captionWidth,topHeight);
                    availableWidth-=captionWidth+5;
                }else p.captionText.clear();
            }
            p.valueText=fm.elidedText(p.valueText,Qt::ElideRight,std::max(0,availableWidth));
        }
        const int paintedWidth=p.numeric?valueWidth:fm.horizontalAdvance(p.valueText);
        p.number=QRect(x,p.content.top()+(topHeight-fm.height())/2,paintedWidth,fm.height());
        if(p.numeric){p.unit=QRect(p.number.right()+3,p.number.top()+fm.ascent()-unit.ascent(),unitWidth,unit.height());p.caption=QRect(p.number.left()-captionWidth-2,p.content.top(),captionWidth,topHeight);}
    }
    p.metric=p.unit.isEmpty()?p.number:p.number.united(p.unit);
    p.status=p.badge.isEmpty()?p.icon:p.icon.united(p.badge);
    p.statusSurface=p.icon;
    p.metricFits=fits(p.content,p.number,p.metricFont,p.valueText)&&fits(p.content,p.unit,p.unitFont,p.unitText);
    p.linesFit=p.metricFits&&fits(p.content,p.badge,p.badgeFont,p.badgeText)&&fits(p.content,p.caption,p.captionFont,p.captionText)&&p.content.contains(p.icon);
    if(axes){
        const auto axis=[&](const QJsonValue &v){if(!finite(v))return QStringLiteral("—");const double n=v.toDouble();return (n>=0?QStringLiteral("+"):QString())+(std::abs(n)>=1000?QString::number(n,'g',3):QString::number(n,'f',1))+QStringLiteral("%");};
        p.outputText=axis(data.value("output_gain_percent"));p.healthText=axis(data.value("effective_health_gain_percent"));
        const int axisHeight=compactHistory?axisSpace-1:20,y=detailBottom-axisHeight+1,cell=p.content.width()/2;
        const int iconY=y+(axisHeight-14)/2;
        p.outputIcon=QRect(p.content.left(),iconY,14,14);p.output=QRect(p.outputIcon.right()+5,y,cell-24,axisHeight);
        p.healthIcon=QRect(p.content.left()+cell+2,iconY,14,14);p.health=QRect(p.healthIcon.right()+5,y,std::max(0,p.content.right()-p.healthIcon.right()-4),axisHeight);
        p.linesFit&=fits(p.content,p.output,p.axisFont,p.outputText)&&fits(p.content,p.health,p.axisFont,p.healthText)
            &&p.content.contains(p.outputIcon)&&p.content.contains(p.healthIcon);
    }
    if(explanation){
        p.estimate=QRect(p.content.left(),detailBottom-explanationHeight+1,p.content.width(),explanationHeight);
        const auto font=Kda::hudFont(Kda::FontRole::Body,12);const QFontMetrics fm(font);
        const int lineCount=std::max(1,explanationHeight/fm.height());
        QTextLayout layout(guidance,font);QTextOption option;option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);layout.setTextOption(option);layout.beginLayout();
        QStringList lines;
        for(int index=0;index<lineCount;++index){auto line=layout.createLine();if(!line.isValid())break;line.setLineWidth(p.estimate.width());
            const auto text=index==lineCount-1?fm.elidedText(guidance.mid(line.textStart()),Qt::ElideRight,p.estimate.width()):guidance.mid(line.textStart(),line.textLength()).trimmed();
            lines.append(text);p.linesFit&=fm.horizontalAdvance(text)<=p.estimate.width();}
        layout.endLayout();p.explanationText=lines.join(QLatin1Char('\n'));
        p.linesFit&=p.content.contains(p.estimate)&&lines.size()*fm.height()<=p.estimate.height();
    }
    p.textSeparated=true;
    const QRect boxes[]{p.icon,p.badge,p.number,p.unit,p.caption,p.outputIcon,p.output,p.healthIcon,p.health,p.estimate,p.history};
    for(int i=0;i<11;++i)for(int j=i+1;j<11;++j)if(!boxes[i].isEmpty()&&!boxes[j].isEmpty()&&boxes[i].intersects(boxes[j]))p.textSeparated=false;
    p.linesFit&=p.textSeparated&&!p.statusSurface.intersects(p.metric)&&!p.statusSurface.intersects(p.caption);
    p.linesFit&=fits(p.content,p.history,p.historyFont,p.historyText);
    if(!p.linesFit&&(!p.rerollText.isEmpty()||extraBadge))
        return textLayout(size,data,slot,false);
    if(!p.linesFit&&!p.history.isEmpty()){
        auto primary=data;
        primary.remove("historical_comparison");
        return textLayout(size,primary,slot,showExtra);
    }
    return p;
}
QJsonArray coordinates(const QRect &r) { return {r.x(), r.y(), r.width(), r.height()}; }
void paintStateResponse(QPainter &painter, const QRectF &face, const QPainterPath &anchorClip, qreal phase) {
    const qreal progress=std::clamp(phase,qreal(0),qreal(1));
    const qreal strength=std::sqrt(1.-progress)*(.35+.65*Kda::HudStyle::strength());
    painter.save();painter.setClipPath(anchorClip,Qt::IntersectClip);painter.setPen(Qt::NoPen);
    auto pearl=Kda::HudStyle::neutral();pearl.setAlpha(qRound(42*(1.-progress)));painter.fillRect(face,pearl);
    const qreal beamWidth=std::max(qreal(16),face.width()*.48),slant=std::min(qreal(9),face.height()*.32);
    const qreal center=face.left()-beamWidth+(face.width()+beamWidth*2)*progress;
    QPainterPath beam;
    beam.moveTo(center-beamWidth/2+slant,face.top());beam.lineTo(center+beamWidth/2+slant,face.top());
    beam.lineTo(center+beamWidth/2-slant,face.bottom());beam.lineTo(center-beamWidth/2-slant,face.bottom());beam.closeSubpath();
    auto tint=Kda::HudStyle::accent();tint.setAlpha(qRound(145*strength));painter.fillPath(beam,tint);
    pearl.setAlpha(qRound(190*strength));painter.setPen(QPen(pearl,1.2));
    painter.drawLine(QPointF(center+slant,face.top()),QPointF(center-slant,face.bottom()));painter.restore();

}
}

CardHintWindow::CardHintWindow(int slot) : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint
    | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput), slotIndex(slot),
    highlight(new Kda::Transition(this, 300)) {
    setWindowTitle(QStringLiteral("李金金选牌提示 %1").arg(slot + 1));
    setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents); setFocusPolicy(Qt::NoFocus);
    Kda::attachOverlayBackdrop(this);
}
void CardHintWindow::present(const QRect &rect, const QJsonObject &data, double opacity) {
    if(content==data&&geometry()==rect&&std::abs(windowOpacity()-std::clamp(opacity,.35,1.))<=.01&&isVisible())return;
    const bool firstAppearance = !isVisible();
    const bool candidateChanged = data.value("augment_id") != content.value("augment_id")
        || (!data.contains("augment_id") && data.value("title") != content.value("title"));
    const bool recommendationChanged = comparisonState(data,numericMetric(data.value("metric").toString()))
            != comparisonState(content,numericMetric(content.value("metric").toString()))
        || data.value("recommended") != content.value("recommended")
        || data.value("recommendation") != content.value("recommendation");
    content = data;
    auto label = QStringList{data.value("title").toString(), displayMetric(data.value("metric").toString()),
                                  data.value("recommendation").toString()}.join(QStringLiteral(" · "));
    if(usesDecisionScore(data)&&numericMetric(data.value("metric").toString()))label+=QStringLiteral(" · 五名对手 · 3/8/20 秒条件估算");
    if(!data.value("evaluation_hint").toString().isEmpty())label+=QStringLiteral(" · ")+data.value("evaluation_hint").toString();
    const auto decisionHint=cardCopy(data.value("decision_hint").toString());
    if(!decisionHint.isEmpty())label+=QStringLiteral(" · ")+decisionHint;
    const bool numeric=numericMetric(data.value("metric").toString());
    const auto state=comparisonState(data,numeric);
    if(state==ComparisonState::Advisory){
        label+=QStringLiteral(" · ")+data.value("advisory_label").toString()+QStringLiteral(" · ")+data.value("advisory").toObject().value("reason").toString();
    }
    const auto sample=numeric?QString():publicReferenceSample(data).detail;
    if(!sample.isEmpty())label+=QStringLiteral(" · ")+sample;
    const auto publicComparison=publicReferenceComparison(data);
    const auto publicDetail=publicComparison.value("detail").toString();
    if(!publicDetail.isEmpty())label+=QStringLiteral(" · ")+publicDetail;
    if(publicComparison.value("used_broader_scope").toBool()
        &&publicComparison.value("scope")==QStringLiteral("champion_augment_reference"))
        label+=QStringLiteral(" · 三张统一查看该英雄的汇总资料");
    const auto reroll=data.value("reroll_comparison").toObject();
    if(numeric){
        const auto basis=data.value("rule_basis").toObject();
        const auto relation=basis.value("patch_relation").toString(reroll.value("patch_relation").toString());
        const auto patch=basis.value("source_patch").toString(reroll.value("rules_patch").toString());
        if(relation==QStringLiteral("mismatch"))label+=QStringLiteral(" · 使用旧版 %1 模型").arg(patch);
        else if(relation==QStringLiteral("unknown"))label+=QStringLiteral(" · 对局版本未确认，使用本地 %1 模型").arg(patch);
        const int duration=data.value("duration_hint_seconds").toInt();
        if(duration==3||duration==20)label+=QStringLiteral(" · 短战与长战的领先牌不同，这张在 %1 秒条件下领先").arg(duration);
        if(reroll.value("available").toBool()){
            label+=QStringLiteral(" · ")+reroll.value("detail").toString();
            if(reroll.value("rule")==QStringLiteral("special_upgrade_possible"))label+=QStringLiteral(" · 本局重随可能提高品质");
        }
    }
    if(!data.value("evaluation_reason").toString().isEmpty())label+=QStringLiteral(" · ")+data.value("evaluation_reason").toString();
    if(!data.value("display_evaluation_reason").toString().isEmpty())label+=QStringLiteral(" · ")+data.value("display_evaluation_reason").toString();
    if(!data.value("comparison_reason").toString().isEmpty())label+=QStringLiteral(" · ")+data.value("comparison_reason").toString();
    if(data.value("output_gain_percent").isDouble()&&std::isfinite(data.value("output_gain_percent").toDouble()))label+=QStringLiteral(" · 输出模型变化 %1%").arg(data.value("output_gain_percent").toDouble(),0,'f',1);
    if(data.value("effective_health_gain_percent").isDouble()&&std::isfinite(data.value("effective_health_gain_percent").toDouble()))label+=QStringLiteral(" · 有效生命模型变化 %1%").arg(data.value("effective_health_gain_percent").toDouble(),0,'f',1);
    const auto history=data.value("historical_comparison_description").toString();
    if(!history.isEmpty())label+=QStringLiteral(" · ")+history;
    setAccessibleName(label);
    setToolTip(QString());
    if (geometry() != rect) setGeometry(rect);
    const double value = std::clamp(opacity, .35, 1.);
    if (std::abs(windowOpacity() - value) > .01) setWindowOpacity(value);
    update(); if (!isVisible()) show();
    if (firstAppearance || candidateChanged || recommendationChanged) {
        ++highlightCount;
        // A newer identity/state owns a complete response even if it arrives
        // during the previous one. Numeric-only updates never enter here.
        highlight->finish();
        highlight->trigger();
    }
}
void CardHintWindow::paintEvent(QPaintEvent *) {
    Kda::OverlayCanvas canvas(this);auto &painter=canvas.paint();
    const auto p=textLayout(size(),content,slotIndex);
    const auto state=comparisonState(content,p.numeric);
    const bool recommended=state==ComparisonState::Recommended;
    const bool advisory=state==ComparisonState::Advisory;
    const bool partial=state==ComparisonState::Partial;
    const bool tied=state==ComparisonState::Tied;
    const QRectF shell=QRectF(rect()).adjusted(.7,.7,-.7,-.7);
    Kda::HudStyle::paintSurface(painter,shell,false);
    if(advisory){painter.setPen(QPen(Kda::HudStyle::accent(),1.4));painter.drawLine(QPointF(shell.left()+9,shell.top()+1),QPointF(shell.right()-9,shell.top()+1));}
    const auto mark=QRectF(p.icon).adjusted(.7,.7,-.7,-.7);QPainterPath diamond;
    diamond.moveTo(mark.left()+5,mark.top());diamond.lineTo(mark.right(),mark.top());diamond.lineTo(mark.right()-5,mark.bottom());diamond.lineTo(mark.left(),mark.bottom());diamond.closeSubpath();
    QPainterPath seal=diamond;
    const QColor checkFace("#eee4fa"), checkInk("#1d1428");
    if(recommended)painter.fillPath(seal,checkFace);
    QPainterPath anchorClip;
    if(p.numeric&&!tied)anchorClip=recommended?seal:diamond;
    else anchorClip.addRoundedRect(QRectF(p.statusSurface),Kda::radius("card_inner"),Kda::radius("card_inner"));
    if(highlight->running())paintStateResponse(painter,QRectF(p.statusSurface),anchorClip,highlight->progress());
    painter.setPen(QPen(Kda::HudStyle::color(recommended?"silver":partial||tied?"silver":"muted"),recommended?.8:1.35,partial?Qt::DashLine:Qt::SolidLine));
    painter.setBrush(Qt::NoBrush);
    if(p.numeric&&!tied)painter.drawPath(recommended?seal:diamond);
    if(partial){
        painter.setPen(QPen(Kda::HudStyle::color("silver"),1.7,Qt::SolidLine,Qt::RoundCap));
        painter.drawLine(QPointF(mark.center().x(),mark.top()+6),QPointF(mark.center().x(),mark.bottom()-8));
        painter.drawPoint(QPointF(mark.center().x(),mark.bottom()-4.5));
    }else{
        const auto status=content.value("evaluation_status").toString();const bool unavailable=!p.numeric&&!status.isEmpty()&&status!=QStringLiteral("calculating");
        const auto glyph=advisory?HudSymbols::Symbol::Check:state==ComparisonState::Reference?HudSymbols::Symbol::Link:!p.numeric?(unavailable?HudSymbols::Symbol::Warning:HudSymbols::Symbol::Clock):tied?HudSymbols::Symbol::Equal:recommended?HudSymbols::Symbol::Check:HudSymbols::Symbol::Chevron;
        const qreal glyphInset=recommended?1.5:!p.numeric?0:tied?3:4;
        const auto ink=recommended?checkInk:advisory?checkFace:Kda::HudStyle::color(tied?"silver":"muted");
        HudSymbols::draw(painter,glyph,mark.adjusted(glyphInset,glyphInset,-glyphInset,-glyphInset),ink,recommended?3.:advisory?2.5:tied?2.:1.8);
    }
    painter.setFont(p.captionFont);painter.setPen(Kda::HudStyle::color("muted"));
    if(!p.caption.isEmpty())painter.drawText(p.caption,Qt::AlignVCenter|Qt::AlignLeft,p.captionText);
    painter.setFont(p.badgeFont);painter.setPen(Kda::HudStyle::color(tied||partial?"text":"silver"));painter.drawText(p.badge,Qt::AlignCenter,p.badgeText);
    painter.setFont(p.metricFont);painter.setPen(Kda::HudStyle::color(p.numeric?"text":"silver"));painter.drawText(p.number,Qt::AlignVCenter|Qt::AlignLeft,p.valueText);
    if(!p.unitText.isEmpty()){painter.setFont(p.unitFont);painter.setPen(Kda::HudStyle::color("silver"));painter.drawText(p.unit,Qt::AlignVCenter|Qt::AlignLeft,p.unitText);}
    if(!p.output.isEmpty()){
        painter.setPen(QPen(Kda::HudStyle::neutral(45),.7));painter.drawLine(QPointF(p.content.left(),p.output.top()-1),QPointF(p.content.right(),p.output.top()-1));
        const auto &f=p.axisFont;painter.setFont(f);
        HudSymbols::draw(painter,HudSymbols::Symbol::Sword,p.outputIcon,Kda::HudStyle::accent(),1.7);HudSymbols::draw(painter,HudSymbols::Symbol::Shield,p.healthIcon,Kda::HudStyle::color("silver"),1.7);
        painter.setPen(Kda::HudStyle::accent());painter.drawText(p.output,Qt::AlignVCenter|Qt::AlignLeft,QFontMetrics(f).elidedText(p.outputText,Qt::ElideRight,p.output.width()));
        painter.setPen(Kda::HudStyle::color("silver"));painter.drawText(p.health,Qt::AlignVCenter|Qt::AlignLeft,QFontMetrics(f).elidedText(p.healthText,Qt::ElideRight,p.health.width()));
    }
    if(!p.estimate.isEmpty()){painter.setFont(Kda::hudFont(Kda::FontRole::Body,12));painter.setPen(Kda::HudStyle::color("muted"));painter.drawText(p.estimate,Qt::AlignLeft|Qt::AlignVCenter,p.explanationText);}
    if(!p.history.isEmpty()){painter.setFont(p.historyFont);painter.setPen(Kda::HudStyle::color("muted"));painter.drawText(p.history,Qt::AlignLeft|Qt::AlignVCenter,p.historyText);}
}
void CardHintWindow::hideEvent(QHideEvent *event) {
    highlight->finish();
    QWidget::hideEvent(event);
}
QJsonObject CardHintWindow::report() const {
    auto row = content; row["slot_index"] = slotIndex; row["visible"] = isVisible();
    row["geometry"] = QJsonArray{x(), y(), width(), height()}; row["opacity"] = windowOpacity();
    row["click_through"] = windowFlags().testFlag(Qt::WindowTransparentForInput);
    row["accepts_keyboard_focus"] = !windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus);
    const auto p = textLayout(size(), content, slotIndex);
    row["content_rect"] = coordinates(p.content); row["title_rect"] = coordinates(p.title); row["metric_rect"] = coordinates(p.metric);
    row["badge_rect"] = coordinates(p.badge); row["number_rect"] = coordinates(p.number); row["unit_rect"] = coordinates(p.unit);
    row["caption_rect"] = coordinates(p.caption); row["slot_rect"] = coordinates(p.serial);
    row["icon_rect"] = coordinates(p.icon); row["status_rect"] = coordinates(p.status);
    row["status_surface_rect"] = coordinates(p.statusSurface);
    row["title_font_px"] = p.titleFont.pixelSize(); row["metric_font_px"] = p.metricFont.pixelSize();
    row["metric_font_family"] = p.metricFont.family();
    row["badge_font_px"] = p.badgeFont.pixelSize(); row["unit_font_px"] = p.unitFont.pixelSize();
    row["title_elided"] = p.titleElided; row["displayed_title"] = p.titleText;
    row["source_title"] = content.value("title"); row["title_visible"] = false;
    row["title_presentation"] = QStringLiteral("omitted_duplicate_game_card_label");
    row["source_title_accessible"] = accessibleName().contains(content.value("title").toString());
    row["displayed_badge"] = p.badgeText; row["badge_fits"] = fits(p.content, p.badge, p.badgeFont, p.badgeText);
    row["displayed_metric"] = p.valueText + p.unitText;
    row["explanatory_copy_visible"]=!p.estimate.isEmpty();row["style_name_visible"]=false;row["explanation_visible"]=!p.estimate.isEmpty();row["explanation_rect"]=coordinates(p.estimate);row["displayed_explanation"]=p.explanationText;
    row["historical_comparison_visible"]=!p.history.isEmpty();row["historical_comparison_rect"]=coordinates(p.history);row["displayed_historical_comparison"]=p.historyText;
    row["evaluation_reason_accessible"]=content.value("evaluation_reason").toString().isEmpty()||accessibleName().contains(content.value("evaluation_reason").toString());
    row["displayed_metric_caption"] = p.captionText;
    row["displayed_reroll"] = p.rerollText;
    const auto rerollDetail=content.value("reroll_comparison").toObject().value("detail").toString();
    row["reroll_comparison_accessible"] = !rerollDetail.isEmpty()&&accessibleName().contains(rerollDetail);
    const auto publicDetail=publicReferenceComparison(content).value("detail").toString();
    row["public_reference_comparison_accessible"] = !publicDetail.isEmpty()&&accessibleName().contains(publicDetail);
    const auto state=comparisonState(content,p.numeric);
    const auto advisoryKind=content.value("advisory").toObject().value("kind");
    if(p.numeric)row["metric_semantics"]=QStringLiteral("conditional_model_gain_not_win_rate");
    else if(state==ComparisonState::Advisory&&advisoryKind==QStringLiteral("public_reference_preference"))
        row["metric_semantics"]=QStringLiteral("public_reference_preference_not_model_gain");
    else if(state==ComparisonState::Advisory&&advisoryKind==QStringLiteral("conditional_equipment_continuation"))
        row["metric_semantics"]=QStringLiteral("conditional_equipment_reference_not_model_gain");
    else if(content.value("source_mechanism_reference").toObject().value("available").toBool())
        row["metric_semantics"]=QStringLiteral("mechanism_reference_not_numeric_gain");
    else if(!p.estimate.isEmpty())row["metric_semantics"]=QStringLiteral("conditional_option_not_numeric_gain");
    else row["metric_semantics"]=QStringLiteral("unavailable_not_numeric_gain");
    row["layout_variant"] = p.compactRows ? QStringLiteral("score_status_complete_rows") : QStringLiteral("score_status_inline");
    row["axis_icons_visible"]=!p.output.isEmpty();row["output_axis_rect"]=coordinates(p.output);row["health_axis_rect"]=coordinates(p.health);
    row["displayed_output_gain"]=p.outputText;row["displayed_effective_health_gain"]=p.healthText;
    row["axis_font_px"]=p.axisFont.pixelSize();row["text_weight"]=int(p.metricFont.weight());row["badge_weight"]=int(p.badgeFont.weight());row["rasterization"]="2x_physical_grayscale_resolve";
    row["title_fits"] = true; // No title glyphs are painted; source_title is preserved above.
    row["metric_fits"] = p.metricFits; row["line_heights_fit"] = p.linesFit;
    row["text_rects_non_overlapping"] = p.textSeparated;
    row["recommendation_symbol"] = state==ComparisonState::Advisory?QStringLiteral("check_outline_advisory"):state==ComparisonState::Reference?QStringLiteral("source_reference_link"):state==ComparisonState::Waiting?(!content.value("evaluation_status").toString().isEmpty()&&content.value("evaluation_status")!=QStringLiteral("calculating")?QStringLiteral("warning_unavailable"):QStringLiteral("clock_outline_circle")):state==ComparisonState::Partial?QStringLiteral("exclamation_dashed_diamond")
        :state==ComparisonState::Tied?QStringLiteral("equal_unframed"):state==ComparisonState::Recommended?QStringLiteral("check_flat_slash"):QStringLiteral("chevron_outline_diamond");
    row["comparison_state"] = stateName(state);
    row["preference_conveyed_by_shape"] = true;
    row["recommended_material"] = state==ComparisonState::Recommended;
    row["advisory_material"] = state==ComparisonState::Advisory;
    row["status_surface_material"] = state==ComparisonState::Recommended?QStringLiteral("flat_lilac_ink"):QStringLiteral("quiet_black");
    row["foil_texture_used"] = false;row["full_width_foil"] = false;
    row["frame_style"] = QStringLiteral("single_quiet_skin_edge");
    row["data_surface_style"] = QStringLiteral("quiet_black");
    row["status_surface_separate_from_score"] = !p.statusSurface.intersects(p.metric)&&!p.statusSurface.intersects(p.caption);
    row["card_visual_style"] = Kda::HudStyle::identity();
    row["idle_material_animated"] = false;
    row["slot_number_visible"] = false;
    row["theme"] = Kda::HudStyle::identity();row["skin_strength"]=Kda::HudStyle::strength();
    row["outer_radius"] = Kda::radius("card_outer"); row["inner_radius"] = Kda::radius("card_inner");
    row["motion"] = highlight->report(); row["highlight_trigger_count"] = highlightCount;
    row["state_response_style"] = QStringLiteral("the_baddest_slash_reveal");
    row["state_response_clip"] = QStringLiteral("24px_status_anchor");
    row["state_response_uses_image_frames"] = false;
    row["score_display_delayed"] = false; row["window_position_animated"] = false;
    row["window_opacity_animated"] = false;
    return row;
}
