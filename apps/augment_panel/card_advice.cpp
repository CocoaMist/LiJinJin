#include "card_advice.h"
#include <QJsonArray>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QRect lowerCardInterior(const QRect &card,double scale){
    // Preserve at least a 6px card border. On narrower supported cards spend
    // spare width on the readable 150px hint rather than a rounded-up inset.
    const int desiredInset=std::max(6,qRound(20*scale));
    const int inset=std::min(desiredInset,std::max(6,(card.width()-150)/2));
    const int top=card.top()+qRound(card.height()*.76);
    const int bottom=card.bottom()-std::max(8,qRound(32*scale));
    return QRect(card.left()+inset,top,card.width()-2*inset,bottom-top+1);
}
QRect upperCardContent(const QRect &card,const QRect &interior){
    return QRect(card.left(),card.top(),card.width(),interior.top()-card.top());
}
}
SelectionCardLayout selectionCardLayout(const QRect &v,const QList<QRect> &additionalControlAreas) {
    SelectionCardLayout out;
    if(v.width()<640||v.height()<360){out.fallbackReason=QStringLiteral("viewport_too_small");return out;}
    const double scale=std::min(v.width()/1920.,v.height()/1080.);
    const QPointF origin(v.x()+(v.width()-1920*scale)/2,v.y()+(v.height()-1080*scale)/2);
    out.referenceFrame=QRect(qRound(origin.x()),qRound(origin.y()),qRound(1920*scale),qRound(1080*scale));
    // Reroll, hide/confirm and the bottom HUD remain separate protected bands.
    // The label now occupies the card's lower interior, matching the supplied
    // visual reference rather than using the space below these controls.
    out.referenceFooterControls=QRect(out.referenceFrame.left(),qRound(origin.y()+800*scale),out.referenceFrame.width(),std::max(1,qRound(36*scale)));
    const int gameHudTop=v.y()+qRound(v.height()*.86);
    out.referenceGameHud=QRect(v.left(),gameHudTop,v.width(),v.bottom()-gameHudTop+1);
    out.gap=std::max(6,qRound(10*scale));out.desiredHintHeight=std::clamp(qRound(68*scale),44,68);
    for(const auto &area:additionalControlAreas){if(!area.isValid()){out.fallbackReason=QStringLiteral("invalid_control_geometry");return out;}out.additionalControlAreas.append(area);}
    for(int slot=0;slot<3;++slot){
        const int x=qRound(origin.x()+(432+slot*368)*scale);
        const QRect card(x,qRound(origin.y()+180*scale),qRound(320*scale),qRound(518*scale));
        const auto interior=lowerCardInterior(card,scale);
        out.cards.append(card);out.referenceHintInteriors.append(interior);
        out.referenceCardContentAreas.append(upperCardContent(card,interior));
    }
    const int actionTop=out.cards.first().bottom()+1;
    out.referenceCardActions=QRect(out.referenceFrame.left(),actionTop,out.referenceFrame.width(),std::max(0,out.referenceFooterControls.top()-actionTop));
    if(out.referenceHintInteriors.first().width()<150){out.fallbackReason=QStringLiteral("card_interior_width_below_readable_minimum");return out;}
    // All labels keep their slot and one common baseline. Avoid caller supplied
    // controls only inside the lower interior; never drift into original copy.
    const auto band=out.referenceHintInteriors.first();
    for(int height=out.desiredHintHeight;height>=44;--height){
        const int firstTop=band.bottom()-height+1;
        int y=firstTop;
        while(y>=band.top()){
            QList<QRect> hints;int nextY=y;
            for(int slot=0;slot<out.cards.size();++slot){
                const auto &card=out.cards[slot];
                const int width=std::min(out.referenceHintInteriors[slot].width(),std::max(150,qRound(260*scale)));
                QRect hint(card.center().x()-(width-1)/2,y,width,height);
                // QRect centres round toward zero on negative-origin monitors.
                hint.translate(card.center().x()-hint.center().x(),0);hints.append(hint);
                for(const auto &area:out.additionalControlAreas)if(hint.intersects(area))nextY=std::min(nextY,area.top()-out.gap-height);
            }
            if(nextY!=y){y=nextY;continue;}
            out.hints=hints;out.valid=selectionCardLayoutSafe(v,out);
            if(out.valid){
                out.mode=height<out.desiredHintHeight?QStringLiteral("compact_inside"):y<firstTop?QStringLiteral("shifted_inside"):QStringLiteral("inside");
                if(height<out.desiredHintHeight)out.fallbackReason=QStringLiteral("limited_card_interior_compact");
                else if(y<firstTop)out.fallbackReason=QStringLiteral("protected_controls_shifted_inside");
                return out;
            }
            break;
        }
    }
    out.hints.clear();out.fallbackReason=QStringLiteral("insufficient_space_inside_cards");
    return out;
}
bool selectionCardLayoutSafe(const QRect &v,const SelectionCardLayout &p) {
    if(v.width()<640||v.height()<360||p.cards.size()!=3||p.hints.size()!=3
        ||p.referenceCardContentAreas.size()!=3||p.referenceHintInteriors.size()!=3)return false;
    const double scale=std::min(v.width()/1920.,v.height()/1080.);
    const QPointF origin(v.x()+(v.width()-1920*scale)/2,v.y()+(v.height()-1080*scale)/2);
    const QRect top(v.x(),v.y(),v.width(),qRound(v.height()*.08));
    const int bottomTop=v.y()+qRound(v.height()*.86);
    const QRect bottom(v.x(),bottomTop,v.width(),v.bottom()-bottomTop+1);
    for(int i=0;i<3;++i){
        const QRect card(qRound(origin.x()+(432+i*368)*scale),qRound(origin.y()+180*scale),qRound(320*scale),qRound(518*scale));
        const auto interior=lowerCardInterior(card,scale),content=upperCardContent(card,interior);
        const auto &r=p.hints[i];
        // Recompute the allowed band: editing report metadata cannot authorize
        // an overlay on top of a card's title, description or border.
        if(p.cards[i]!=card||p.referenceHintInteriors[i]!=interior||p.referenceCardContentAreas[i]!=content
            ||!v.contains(card)||r.width()<150||r.height()<44||r.height()>68||!interior.contains(r)
            ||!v.contains(r)||r.intersects(top)||r.intersects(bottom)||r.intersects(content)
            ||r.center().x()!=card.center().x()||r.top()!=p.hints.first().top()||r.height()!=p.hints.first().height()
            ||r.intersects(p.referenceCardActions)||r.intersects(p.referenceFooterControls))return false;
        for(const auto &area:p.additionalControlAreas)if(!area.isValid()||r.intersects(area))return false;
        for(int j=0;j<3;++j)if(i!=j&&(r.intersects(p.cards[j])||r.intersects(p.hints[j])))return false;
    }
    return true;
}
QJsonObject selectionCardLayoutJson(const SelectionCardLayout &p) {
    auto rect=[](const QRect &r){return QJsonArray{r.x(),r.y(),r.width(),r.height()};};
    QJsonArray cards,hints,controls,contents,interiors,slotRows;
    for(const auto &r:p.cards)cards.append(rect(r));
    for(const auto &r:p.hints)hints.append(rect(r));
    for(const auto &r:p.additionalControlAreas)controls.append(rect(r));
    for(const auto &r:p.referenceCardContentAreas)contents.append(rect(r));
    for(const auto &r:p.referenceHintInteriors)interiors.append(rect(r));
    for(int i=0;i<p.cards.size();++i)slotRows.append(QJsonObject{{"slot_index",i},{"card_rect",rect(p.cards[i])},
        {"hint_rect",i<p.hints.size()?QJsonValue(rect(p.hints[i])):QJsonValue(QJsonValue::Null)},
        {"card_content_protection",i<p.referenceCardContentAreas.size()?QJsonValue(rect(p.referenceCardContentAreas[i])):QJsonValue(QJsonValue::Null)},
        {"allowed_lower_interior",i<p.referenceHintInteriors.size()?QJsonValue(rect(p.referenceHintInteriors[i])):QJsonValue(QJsonValue::Null)}});
    const QString message=!p.valid?QStringLiteral("卡牌内部空间不足，暂时隐藏提示以保留原卡说明与操作区")
        :p.mode==QStringLiteral("inside")?QString():QStringLiteral("已在卡牌底部空白区内避让并压紧提示");
    return {{"valid",p.valid},{"reference_card_rects",cards},{"hint_rects",hints},
        {"position_source","reference_1920x1080_centered_16_9"},{"real_game_alignment_verified",false},
        {"requested_anchor","inside_each_card_lower_interior"},{"placement_mode",p.mode},{"minimum_gap_px",p.gap},{"desired_hint_height_px",p.desiredHintHeight},
        {"reference_ui_frame",rect(p.referenceFrame)},{"reference_footer_control_protection",rect(p.referenceFooterControls)},
        {"reference_card_action_protection",rect(p.referenceCardActions)},{"reference_game_hud_protection",rect(p.referenceGameHud)},
        {"reference_card_content_protection",contents},{"reference_hint_interiors",interiors},
        {"additional_control_rects",controls},{"slots",slotRows},{"fallback_reason",p.fallbackReason},{"fallback_message",message},
        {"insufficient_space_policy","shift_or_compact_inside_lower_interior_then_hide_all_three_without_changing_slots"},{"reroll_confirm_geometry_verified",false},
        {"geometry_scope","Reference lower card interiors only, below protected card art/title/description and above the inset bottom border. Reroll, hide/confirm and bottom HUD remain clear. Exact game alignment and text extents still require visible in-game verification."}};
}
QJsonArray orderedCardHints(const PanelState &state, const QJsonObject &decision,
                           const QJsonObject &ai, const QList<int> &selected, bool currentModel) {
    Q_UNUSED(ai); Q_UNUSED(selected);
    if (state.status != "open" || state.candidates.isEmpty() || state.candidates.size() > 3) return {};
    QMap<int, QJsonObject> bySlot, values;
    QSet<int> ids, ambiguousValues;
    for (const auto &v : state.candidates) {
        const auto card = v.toObject(); const auto index = card.value("candidate_index");
        const int slot = index.toInt(-1), id = card.value("augment_id").toInt();
        if (!index.isDouble() || index.toDouble() != slot || slot < 0 || slot > 2
            || bySlot.contains(slot) || id <= 0 || ids.contains(id)) return {};
        bySlot[slot] = card; ids.insert(id);
    }
    if (currentModel) for (const auto &v : decision.value("offers").toArray()) {
        const auto row = v.toObject(); const int id = row.value("augment_id").toInt();
        if (values.contains(id)) ambiguousValues.insert(id);
        values[id] = row;
    }
    // A round uses one score contract. Once the multi-window contract is
    // present, an unavailable candidate must not fall back to the old score.
    bool decisionRound=false;
    for(const auto &card:bySlot){const auto value=values.value(card.value("augment_id").toInt());
        decisionRound|=value.contains("decision_numeric_available")||value.contains("decision_gain_percent")
            ||value.contains("decision_scope")||value.contains("decision_score_basis")||value.contains("decision_trace");}
    const QString basis=decisionRound?QStringLiteral("decision_gain_percent"):QStringLiteral("current_gain_percent");
    const auto validDecisionScope=[](const QJsonObject &scope){
        if(scope.value("target_count")!=QJsonValue(5))return false;
        const auto windows=scope.value("windows").toArray();QSet<int> seen;
        if(windows.size()!=3)return false;
        for(const auto &window:windows){const int seconds=window.toInt(-1);
            if(!window.isDouble()||window.toDouble()!=seconds||!QSet<int>{3,8,20}.contains(seconds)||seen.contains(seconds))return false;
            seen.insert(seconds);}
        return seen.size()==3;
    };
    QString decisionScoreBasis;QJsonObject decisionScope;bool firstDecisionBasis=true;
    const auto quality=decision.value("ranking_quality").toObject();
    const auto scenarioComparison=decision.value("scenario_comparison").toObject();
    const auto ruleBasis=decision.value("rule_basis").toObject();
    const bool scenarioLimited=currentModel&&decision.contains("scenario_comparison")
        &&!scenarioComparison.value("available").toBool();
    const bool comparisonLimited=currentModel&&((quality.contains("ranking_allowed")&&!quality.value("ranking_allowed").toBool())||scenarioLimited);
    QJsonArray out; bool rankable = bySlot.size() == 3&&!comparisonLimited,partialComparison=false;
    for (int slot = 0; slot < 3; ++slot) {
        QJsonObject row{{"slot_index", slot}, {"available", bySlot.contains(slot)},
                        {"ai_explanation", false}, {"numeric_gain_available", false},
                        {"metric", currentModel ? QStringLiteral("暂不可算") : QStringLiteral("计算中")},
                        {"evaluation_status",currentModel?QStringLiteral("unavailable"):QStringLiteral("calculating")},
                        {"evaluation_reason",currentModel?QStringLiteral("当前条件尚不足以计算这张卡的收益"):QStringLiteral("正在按本轮候选和当前状态计算")},
                        {"numeric_source", decisionRound?QStringLiteral("local_decision_gain_percent"):QStringLiteral("local_current_gain_percent")}};
        if (!bySlot.contains(slot)) {
            row["title"] = QStringLiteral("未识别"); row["metric"] = QStringLiteral("未识别");row["evaluation_status"]=QStringLiteral("input_missing");row["evaluation_reason"]=QStringLiteral("尚未识别这个位置的候选卡牌");
            rankable = false; out.append(row); continue;
        }
        const auto card = bySlot[slot]; const int id = card.value("augment_id").toInt(); const auto value = values.value(id);
        row["augment_id"] = id; row["title"] = card.value("augment_name").toString();
        if(currentModel&&!quality.isEmpty()){row["ranking_quality"]=quality;row["comparison_limited"]=comparisonLimited;if(comparisonLimited)row["comparison_reason"]=quality.value("summary");}
        if(currentModel&&!ambiguousValues.contains(id))for(const auto &key:{"evaluation_status","evaluation_reason","evaluation_reason_code","evaluation_scope","evaluation_missing_inputs","evaluation_trigger","evaluation_stat_changes","evaluation_model_gap","evaluation_coverage","evaluation_complete","evaluation_numeric_available","evaluation_numeric_effect_count","evaluation_required_inputs","evaluation_event_actions_consumed"})if(value.contains(key))row[key]=value.value(key);
        if(currentModel&&!ambiguousValues.contains(id)&&decisionRound){
            if(value.contains("decision_scenarios"))row["decision_scenarios"]=value.value("decision_scenarios");
            if(!scenarioLimited)for(const auto &v:scenarioComparison.value("candidates").toArray()){
                const auto sensitivity=v.toObject();if(sensitivity.value("candidate_id").toInt()==id){row["scenario_sensitivity"]=sensitivity;break;}}
            for(const auto &key:{"decision_gain_percent","decision_scope","decision_score_basis","decision_trace","decision_numeric_available","decision_fallback","decision_fallback_reason","decision_fallback_basis","decision_comparable","decision_marginal_comparable","decision_additional_unknown_count","decision_additional_unknowns","decision_baseline_unknown_count","decision_baseline_unknowns","decision_comparability_reasons","matchup_gain_percent","historical_pressure_gain_percent"})if(value.contains(key))row[key]=value.value(key);
            // These remain the source's eight-second observation/model fields,
            // separate from the number presented and ranked below.
            if(value.contains("current_gain_percent"))row["current_gain_percent"]=value.value("current_gain_percent");
            if(value.contains("evaluation_hint"))row["source_evaluation_hint"]=value.value("evaluation_hint");
        }
        if(currentModel&&value.contains("conditional_risk"))row["conditional_risk"]=value.value("conditional_risk");
        const auto gain = value.value(basis); const int unknown = std::max(0, value.value("unknown_count").toInt());
        const bool reportedMarginal=value.value("marginal_comparable").toBool(unknown==0);
        const int additional=std::max(0,value.value("additional_unknown_count").toInt(reportedMarginal?0:std::max(1,unknown)));
        const bool marginal=reportedMarginal&&additional==0&&value.value("additional_unknowns").toArray().isEmpty();
        row["additional_unknown_count"]=additional;row["baseline_unknown_count"]=std::max(0,value.value("baseline_unknown_count").toInt(marginal?unknown:0));
        const bool decisionAvailable=!decisionRound||(value.value("decision_numeric_available").isBool()&&value.value("decision_numeric_available").toBool()
            &&!value.value("decision_score_basis").toString().isEmpty()&&validDecisionScope(value.value("decision_scope").toObject()));
        const bool known = currentModel && !ambiguousValues.contains(id) && value.value("available").toBool() && decisionAvailable
                           && gain.isDouble() && std::isfinite(gain.toDouble())
                           && !(std::abs(gain.toDouble()) <= 1e-9 && unknown > 0 && !marginal);
        row["marginal_comparable"]=known&&marginal;
        row["unknown_count"]=unknown;
        const auto sourceStatus=row.value("evaluation_status").toString();
        const bool sourceGap=sourceStatus==QStringLiteral("trigger_unobserved")||sourceStatus==QStringLiteral("effect_unmodeled")||sourceStatus==QStringLiteral("input_missing");
        const bool sourceIncomplete=value.contains("evaluation_complete")&&!value.value("evaluation_complete").toBool();
        const bool decisionGap=decisionRound&&((value.contains("decision_marginal_comparable")&&!value.value("decision_marginal_comparable").toBool())
            ||(value.contains("decision_comparable")&&!value.value("decision_comparable").toBool())
            ||value.value("decision_additional_unknown_count").toInt()>0||!value.value("decision_additional_unknowns").toArray().isEmpty());
        const bool decisionBaselineGap=decisionRound&&(value.value("decision_baseline_unknown_count").toInt()>0
            ||!value.value("decision_baseline_unknowns").toArray().isEmpty());
        if(sourceGap||sourceIncomplete||value.value("evaluation_model_gap").toBool()||decisionGap)row["marginal_comparable"]=false;
        const bool windowZero=known&&std::abs(gain.toDouble())<=1e-9&&!sourceGap;
        const QString statusKey=decisionRound?QStringLiteral("display_evaluation_status"):QStringLiteral("evaluation_status");
        const QString reasonKey=decisionRound?QStringLiteral("display_evaluation_reason"):QStringLiteral("evaluation_reason");
        if(decisionRound){row[statusKey]=row.value("evaluation_status");row[reasonKey]=row.value("evaluation_reason");}
        if(currentModel&&ambiguousValues.contains(id)){row[statusKey]=QStringLiteral("data_conflict");row[reasonKey]=QStringLiteral("本轮出现重复的计算结果，未采用其中任何一个");}
        else if(windowZero){row[statusKey]=decisionRound?QStringLiteral("decision_windows_zero"):QStringLiteral("current_window_zero");row[reasonKey]=decisionRound?QStringLiteral("3/8/20秒、五名对手的已建模部分未见收益变化；不表示整局没有收益"):QStringLiteral("8秒已建模部分未见收益变化；不表示整局没有收益");}
        else if(known&&decisionRound&&!sourceGap){
            row[statusKey]=QStringLiteral("computed");row[reasonKey]=sourceIncomplete||value.value("evaluation_model_gap").toBool()
                ?QStringLiteral("五名对手在3/8/20秒条件模型中的已知数值收益；机制覆盖尚不完整，暂不排序")
                :QStringLiteral("五名对手在3/8/20秒条件模型中的已知数值收益变化");}
        else if(known&&row.value(statusKey)==QStringLiteral("unavailable")){row[statusKey]=QStringLiteral("computed");row[reasonKey]=QStringLiteral("当前8秒条件模型的收益变化");}
        else if(currentModel&&!known&&(row.value(statusKey)==QStringLiteral("computed")||row.value(statusKey)==QStringLiteral("current_window_zero")||row.value(statusKey)==QStringLiteral("unavailable"))){
            if(additional>0||unknown>0){row[statusKey]=QStringLiteral("effect_unmodeled");row[reasonKey]=QStringLiteral("这张卡新增的机制或触发条件尚未完整计入，不能把零值当作无收益");}
            else if(!value.value("available").toBool()){row[statusKey]=QStringLiteral("input_missing");row[reasonKey]=value.value("reason").toString(decision.value("reason").toString(QStringLiteral("缺少当前英雄、技能等级或有效对局信息")));}
            else if(decisionRound){row[statusKey]=QStringLiteral("unavailable");row[reasonKey]=QStringLiteral("五名对手的3/8/20秒推演尚不完整，暂不能比较这张卡");}
        }
        if(decisionGap&&known){row[statusKey]=QStringLiteral("effect_unmodeled");row[reasonKey]=QStringLiteral("3/8/20秒推演新增机制尚未完整计入；仅展示已知数值，暂不排序");}
        const auto status=row.value(statusKey).toString();
        const QMap<QString,QString> statusLabels{{"calculating",QStringLiteral("计算中")},{"current_window_zero",QStringLiteral("短窗无变化")},{"trigger_unobserved",QStringLiteral("需触发条件")},{"effect_unmodeled",QStringLiteral("机制未计入")},{"input_missing",QStringLiteral("缺局内信息")},{"unavailable",QStringLiteral("暂不可算")},{"not_eligible",QStringLiteral("当前不可选")},{"data_conflict",QStringLiteral("数据冲突")}};
        if(!known)row["metric"]=statusLabels.value(status,QStringLiteral("暂不可算"));
        row["evaluation_hint"]=windowZero?(decisionRound?QStringLiteral("仅限3/8/20秒、五名对手的已建模部分"):QStringLiteral("仅限8秒已建模部分")):row.value(reasonKey).toString();
        const auto changes=row.value("evaluation_stat_changes").toObject();
        if(!decisionRound&&windowZero&&changes.value("r_haste").isDouble()&&std::isfinite(changes.value("r_haste").toDouble()))row["evaluation_hint"]=QStringLiteral("R急速%1%2 · 8秒条件模型").arg(changes.value("r_haste").toDouble()>=0?QStringLiteral("+"):QString()).arg(changes.value("r_haste").toDouble(),0,'f',0);
        else if(known&&comparisonLimited)row["evaluation_hint"]=row.value("comparison_reason");
        if(decisionGap&&known)row["evaluation_hint"]=row.value(reasonKey);
        if (known) {
            const double n = gain.toDouble();
            const auto number = std::abs(n) >= 10000 ? QString::number(n, 'g', 3) : QString::number(n, 'f', 1);
            row["metric"] = QStringLiteral("适配 %1%2%").arg(n >= 0 ? "+" : "", number);
            row[basis] = n; row["numeric_gain_available"] = true;
            if(!ruleBasis.isEmpty())row["rule_basis"]=ruleBasis;
            row["partial"] = unknown > 0 || decisionBaselineGap || !row.value("marginal_comparable").toBool();
            // New unknown effects may invalidate either axis; keep the source
            // values private until the same marginal guard permits comparison.
            if(row.value("marginal_comparable").toBool())for(const auto &key:{"output_gain_percent","effective_health_gain_percent"}){
                const auto axis=value.value(QString::fromLatin1(key));
                if(axis.isDouble()&&std::isfinite(axis.toDouble()))row[QString::fromLatin1(key)]=axis;
            }
            if(row.value("marginal_comparable").toBool())row["axis_gain_semantics"]="same_snapshot_model_estimate";
            if(decisionRound&&row.value("marginal_comparable").toBool()){
                row["axis_gain_source"]="legacy_eight_second_conditional_model";
                if(value.contains("evaluation_scope"))row["axis_gain_scope"]=value.value("evaluation_scope");
            }
        }
        row["directions_disabled"]=true;
        row["needs_current_growth_inputs"]=!known||value.value("needs_current_growth_inputs").toBool();
        // A modeled zero can be shown even with shared baseline gaps. It is
        // still insufficient to order the entire offer set under those gaps.
        if (!known || !marginal || (windowZero&&unknown>0) || sourceGap || sourceIncomplete || value.value("evaluation_model_gap").toBool() || decisionGap) rankable = false;
        if(decisionRound&&known){
            const auto scoreBasis=value.value("decision_score_basis").toString();const auto scope=value.value("decision_scope").toObject();
            if(firstDecisionBasis){decisionScoreBasis=scoreBasis;decisionScope=scope;firstDecisionBasis=false;}
            else if(scoreBasis!=decisionScoreBasis||scope!=decisionScope)rankable=false;
        }
        partialComparison|=unknown>0||decisionBaselineGap;
        out.append(row);
    }
    // Keep data-integrity failures separate from model uncertainty.
    if(!ambiguousValues.isEmpty())return out;
    // No candidate may add unmodeled effects to this comparison. Shared
    // baseline gaps still permit an explicitly limited known-part ordering;
    // they are never asserted to cancel or produce a complete recommendation.
    if(!rankable)return out;
    double best=-std::numeric_limits<double>::infinity();int winners=0;
    for(const auto &v:out)best=std::max(best,v.toObject().value(basis).toDouble());
    for(const auto &v:out)if(std::abs(v.toObject().value(basis).toDouble()-best)<=.05)++winners;
    for(int i=0;i<out.size();++i){auto row=out[i].toObject();const bool top=std::abs(row.value(basis).toDouble()-best)<=.05;
        row["recommendation_basis"]=basis;row["recommended"]=top;row["compared_candidate_count"]=3;row["excluded_augment_ids"]=QJsonArray{};row["comparison_scope"]=partialComparison?"known_model_shared_unknowns":"complete_current_model";
        if(scenarioComparison.value("current_model_duration_tradeoff").toBool()){
            const auto leads=row.value("scenario_sensitivity").toObject().value("current_model_leading_seconds").toArray();
            if(leads.contains(3))row["duration_hint_seconds"]=3;
            else if(leads.contains(20))row["duration_hint_seconds"]=20;
        }
        int rank=1;for(const auto &v:out)if(v.toObject().value(basis).toDouble()>row.value(basis).toDouble()+.05)++rank;row["rank"]=rank;
        row["recommendation"]=partialComparison?(top?(winners>1?QStringLiteral("数值同优"):QStringLiteral("数值领先")):QStringLiteral("部分 #%1").arg(rank)):(top?(winners>1?QStringLiteral("均可"):QStringLiteral("推荐")):QStringLiteral("备选"));out[i]=row;
    }
    return out;
}
bool selectionHintsVisible(const PanelState &state, bool freshKiwi, bool foreground,
                           bool demo, bool hidden, QDateTime now) {
    if (hidden || state.status != "open" || state.candidates.isEmpty() || !freshKiwi) return false;
    if (demo) return true;
    const auto age = state.observed.msecsTo(now);
    return !state.replay && foreground && state.observed.isValid() && age >= 0 && age < 60000;
}
