#include "history_pane.h"
#include "kda_theme.h"
#include "kda_reading_surface.h"
#include "kda_notice.h"
#include "kda_controls.h"
#include "kda_glass_surface.h"
#include "kda_depth_plane.h"
#include "hud_symbols.h"
#include "player_match_view.h"
#include "sgp_history.h"
#include "server_names.h"
#include <QComboBox>
#include <QApplication>
#include <QAction>
#include <QDynamicPropertyChangeEvent>
#include <QElapsedTimer>
#include <QThread>
#include <QTextDocument>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QUuid>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTextOption>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace {
// setSkin repaints widgets, but an owner's local stylesheet needs to be
// regenerated too. Only appearance changes affect this local field styling.
class SkinStyleBinding final : public QObject {
public:
    SkinStyleBinding(QWidget *owner,std::function<void()> refresh):QObject(owner),refreshStyles(std::move(refresh)){
        refreshStyles();if(qApp)qApp->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject *watched,QEvent *event)override{
        if(watched==qApp&&event->type()==QEvent::DynamicPropertyChange){const auto name=static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName();if(name=="lixinSkin"||name=="lixinGlassEnabled")refreshStyles();}
        return QObject::eventFilter(watched,event);
    }
private:std::function<void()> refreshStyles;
};
class CompactInfoLabel final : public QLabel {
public:
    explicit CompactInfoLabel(QWidget *parent=nullptr):QLabel(parent){
        setMinimumWidth(0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        setFont(Kda::font(Kda::FontRole::Body,11));setTextFormat(Qt::PlainText);
    }
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);Kda::configurePainter(painter);painter.setFont(font());painter.setPen(Kda::color("muted"));
        painter.drawText(contentsRect(),Qt::AlignLeft|Qt::AlignVCenter,fontMetrics().elidedText(text(),Qt::ElideRight,contentsRect().width()));
    }
};
class ScorePill final : public QLabel {
public:
    explicit ScorePill(HudSymbols::Symbol symbol):glyph(symbol){
        setFont(Kda::font(Kda::FontRole::Numeric,11));setContentsMargins(18,0,3,0);
        setSizePolicy(QSizePolicy::Minimum,QSizePolicy::Preferred);setWordWrap(false);setTextFormat(Qt::PlainText);
    }
    QSize sizeHint()const override{
        const auto margins=contentsMargins();const auto metrics=fontMetrics();
        return {metrics.horizontalAdvance(text())+margins.left()+margins.right()+2,metrics.height()+margins.top()+margins.bottom()};
    }
    QSize minimumSizeHint()const override{return sizeHint();}
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);Kda::configurePainter(painter);painter.setFont(font());painter.setPen(Kda::color("silver"));
        painter.drawText(contentsRect(),Qt::AlignLeft|Qt::AlignVCenter,text());
        HudSymbols::draw(painter,glyph,QRectF(1,(height()-12)/2.,12,12),Kda::color("muted"),1.3);
    }
private:HudSymbols::Symbol glyph;
};
void symbolAction(QPushButton *button,HudSymbols::Symbol symbol,const QString &description,int size=28,int labelWidth=0){
    auto action=static_cast<Kda::Button*>(button);action->setSymbol(symbol);action->setSymbolOnly(labelWidth==0);
    action->setProperty("toolbarAction",!action->property("primaryAction").toBool());
    action->setToolTip(description);action->setAccessibleName(description);action->setFixedSize(labelWidth?labelWidth:size,size);
}
QAction *searchLeadingIcon(QLineEdit *input){
    return input->addAction(QIcon(HudSymbols::pixmap(HudSymbols::Symbol::Search,14,input->devicePixelRatioF(),Kda::color("muted"))),QLineEdit::LeadingPosition);
}
QString h(const QString &text){return text.toHtmlEscaped();}
QString n(const QJsonValue &value,int precision=0){return value.isDouble()?QString::number(value.toDouble(),'f',precision):QStringLiteral("—");}
bool finite(const QJsonValue &value){return value.isDouble()&&std::isfinite(value.toDouble());}
QString scoreReason(const QString &reason){
    if(reason=="player_budget_exceeded")return QStringLiteral("参考网络超出计算范围");
    if(reason=="core_player_budget_exceeded")return QStringLiteral("核心参考网络超出计算范围");
    if(reason=="core_cannot_support_fixed_reference")return QStringLiteral("当前网络无法支持固定参考");
    if(reason=="insufficient_core_support")return QStringLiteral("可用核心记录不足");
    if(reason=="insufficient_recurrent_reference")return QStringLiteral("反复出现的参考玩家不足");
    if(reason=="unidentifiable_target_contrast")return QStringLiteral("个人与参考差异尚不可分离");
    if(reason=="weak_contrast_information")return QStringLiteral("相对差异的数据支持不足");
    if(reason=="computation_budget_exceeded")return QStringLiteral("本次计算未完成");
    if(reason=="disconnected_groups")return QStringLiteral("参考网络尚未连通");
    if(reason=="target_only_star_sample")return QStringLiteral("缺少其他玩家额外对局");
    if(reason=="unidentifiable_team_design")return QStringLiteral("个人影响尚不能分离");
    if(reason=="persistent_team_confounding")return QStringLiteral("固定同队影响尚不能分离");
    if(reason=="insufficient_independent_matchups")return QStringLiteral("不同完整对阵不足");
    if(reason=="prior_dominated_uncertainty")return QStringLiteral("数据支持不足");
    if(reason=="fit_did_not_converge")return QStringLiteral("模型尚未收敛");
    if(reason=="insufficient_sample")return QStringLiteral("有效样本不足");
    if(reason=="no_eligible_matches"||reason=="no_matches"||reason=="target_not_observed"||reason=="no_completed_valid_matches")return QStringLiteral("暂无有效对局");
    if(reason=="database_unavailable")return QStringLiteral("本地记录暂不可读取");
    if(reason=="invalid_request"||reason=="player_region_mismatch_or_missing")return QStringLiteral("当前玩家资料不完整");
    if(reason=="unsupported_schema")return QStringLiteral("当前资料格式暂不支持");
    if(reason=="unsupported_queue")return QStringLiteral("该模式暂不评分");
    return QStringLiteral("暂不可评分");
}
QString cacheExplanation(const QJsonObject &data,bool paused){
    const auto cache=data.value("cache").toObject();QStringList lines;
    if(paused)lines.append(QStringLiteral("局内暂停更新，当前展示已保存结果。"));
    else if(cache.value("stale").toBool())lines.append(QStringLiteral("当前结果来自较旧缓存。"));
    const auto at=cache.value("computed_at_ms");if(finite(at)&&at.toDouble()>0&&at.toDouble()<9e15)lines.append(QStringLiteral("计算时间：%1").arg(QDateTime::fromMSecsSinceEpoch(qint64(at.toDouble())).toString(QStringLiteral("MM-dd HH:mm"))));
    return lines.join(QChar('\n'));
}
struct ScoreLabel {QString text,tooltip;bool known=false;};
ScoreLabel recentScoreText(const QJsonObject &score){
    const auto value=score.value("value");const auto observations=score.value("observations").toObject();
    const bool known=score.value("status")==QStringLiteral("scored")&&finite(value)&&value.toDouble()>=0&&value.toDouble()<=100;
    QString text=known?QStringLiteral("近期战果分 %1/100").arg(n(value,1)):QStringLiteral("近期战果分 —");
    const auto matches=score.value("n_matches").isDouble()?score.value("n_matches"):observations.value("matches");
    if(finite(matches)&&matches.toDouble()>=0)text+=QStringLiteral(" · 本人%1局").arg(n(matches));
    const auto support=score.value("sample_support").toObject().value("level").toString();
    const auto remaining=score.value("remaining").toObject().value("independent_matchups_to_score");
    const auto reason=score.value("reason").toString();const bool moreMatchups=reason=="insufficient_independent_matchups"||reason=="no_completed_valid_matches"||reason=="no_eligible_matches"||reason=="no_matches";
    if(known&&support=="low")text+=QStringLiteral(" · 少样本");
    else if(!known&&moreMatchups&&finite(remaining)&&remaining.toDouble()>0)text+=QStringLiteral(" · 待补%1组对阵").arg(n(remaining));
    else if(!known)text+=QStringLiteral(" · %1").arg(scoreReason(score.value("reason").toString()));
    QStringList lines{QStringLiteral("近期战果分概括已记录的对局结果，不是官方隐藏分、实力评级或下一场胜负预测。")};
    const auto days=score.value("window").toObject().value("days");if(finite(days))lines.append(QStringLiteral("统计范围：近%1天已完成战果。").arg(n(days)));
    if(finite(observations.value("wins"))&&finite(observations.value("losses")))lines.append(QStringLiteral("本人记录：%1胜 / %2负；不同完整对阵%3组。").arg(n(observations.value("wins")),n(observations.value("losses")),n(observations.value("independent_matchups"))));
    if(finite(observations.value("effective_n")))lines.append(QStringLiteral("有效样本量：%1；按时间衰减和重复对阵权重计算，并不证明各局统计独立。").arg(n(observations.value("effective_n"),1)));
    if(support=="low")lines.append(QStringLiteral("当前有效样本较少；分数仍受中立先验影响，不能据此判定稳定水平。"));
    const auto formula=score.value("formula").toObject();
    if(finite(formula.value("prior_alpha"))&&finite(formula.value("prior_beta")))lines.append(QStringLiteral("计算采用中立先验%1局；时间半衰期%2天；同一完整对阵的总权重上限%3。").arg(QString::number(formula.value("prior_alpha").toDouble()+formula.value("prior_beta").toDouble(),'f',0),n(formula.value("half_life_days")),n(formula.value("matchup_weight_cap"))));
    if(!known){lines.append(QStringLiteral("未出分原因：%1。").arg(scoreReason(reason)));if(moreMatchups&&finite(remaining)&&remaining.toDouble()>0)lines.append(QStringLiteral("还需记录%1组不同完整对阵才达到出分条件；重复同一完整对阵不会增加对阵种类。").arg(n(remaining)));}
    const auto interval=score.value("sample_win_rate_interval").toObject();const auto lower=interval.value("lower"),upper=interval.value("upper");
    if(interval.value("descriptive_only").toBool()&&finite(lower)&&finite(upper)&&lower.toDouble()>=0&&lower.toDouble()<=upper.toDouble()&&upper.toDouble()<=1)lines.append(QStringLiteral("已记录样本的描述性胜率区间：%1%–%2%；这不是战果分区间或未来胜率。").arg(QString::number(lower.toDouble()*100,'f',1),QString::number(upper.toDouble()*100,'f',1)));
    lines.prepend(text);
    QString compact=known?QStringLiteral("近期 %1/100").arg(n(value,1)):QStringLiteral("近期 —");
    if(finite(matches)&&matches.toDouble()>=0)compact+=QStringLiteral(" · %1局").arg(n(matches));
    if(known&&support=="low")compact+=QStringLiteral("*");
    return {compact,lines.join(QChar('\n')),known};
}
ScoreLabel poolScoreText(const QJsonObject &score){
    const auto value=score.value("value");const bool known=score.value("status")==QStringLiteral("estimated")&&finite(value);
    QString text=QStringLiteral("匹配池估计 %1").arg(known?n(value,1):QStringLiteral("—"));
    const auto scale=score.value("scale").toObject();
    if(known&&scale.value("comparable_across_requests")==QJsonValue(false))text+=QStringLiteral(" · 本次样本");
    else if(!known)text+=QStringLiteral(" · %1").arg(scoreReason(score.value("reason").toString()));
    QStringList lines{QStringLiteral("匹配池估计只表示本地参考样本中的相对尺度，并非官方隐藏分或真实匹配MMR，也不是近期战果分。")};
    lines.append(QStringLiteral("参考网络：%1局 / %2人；其中本人%3局。网络局数不等于本人局数。").arg(n(score.value("n_matches")),n(score.value("n_players")),n(score.value("observations").toObject().value("target_matches"))));
    const auto core=score.value("observations").toObject().value("core").toObject();if(!core.isEmpty())lines.append(QStringLiteral("建模核心：%1局 / %2人；固定参考玩家%3人，参考名单在结构筛选前确定且排除本人。").arg(n(core.value("matches")),n(core.value("players")),n(core.value("reference_count"))));
    if(known&&scale.value("comparable_across_requests")==QJsonValue(false))lines.append(QStringLiteral("当前参考人群随本次样本选择变化，不能直接跨玩家、跨次查询比较数值。"));
    if(finite(scale.value("baseline")))lines.append(QStringLiteral("本地参考基准：%1。").arg(n(scale.value("baseline"),1)));
    if(!known)lines.append(QStringLiteral("未出分原因：%1；—不表示0分或仍在加载。").arg(scoreReason(score.value("reason").toString())));
    lines.prepend(text);
    return {QStringLiteral("匹配池 %1").arg(known?n(value,1):QStringLiteral("—")),lines.join(QChar('\n')),known};
}
void compactAction(QPushButton *button){button->setProperty("labelPixels",11);button->setProperty("labelRegular",true);button->setMinimumHeight(Kda::Density::controlHeight);button->setFixedHeight(Kda::Density::controlHeight);}
void compactInput(QWidget *input){
    input->setStyleSheet(QStringLiteral("QLineEdit,QComboBox{background:%2;border:1px solid %3;border-radius:%1px;padding:0 8px;color:%4;font-size:11px;font-weight:400;selection-background-color:%5;}QLineEdit:focus,QComboBox:focus{border-color:%6;}QComboBox{padding-right:24px;}QComboBox::drop-down{border:0;background:transparent;width:22px;}QComboBox::down-arrow{image:none;}").arg(Kda::radius("control")).arg(Kda::glassFieldBackground(),Kda::color("border").name(),Kda::color("text").name(),Kda::color("raised").name(),Kda::color("ice").name()));
    input->setMinimumHeight(28);input->setFixedHeight(28);
}
QString message(const QString &state){
    if(state=="idle")return QStringLiteral("搜索玩家，或打开“我的战绩”");
    if(state=="querying")return QStringLiteral("正在读取玩家档案…");
    if(state=="login_required")return QStringLiteral("登录英雄联盟客户端后可查看本人战绩\n也可以搜索本机已保存的玩家档案");
    if(state=="client_data_unavailable")return QStringLiteral("暂时读不到当前账号，请稍后重试");
    if(state=="ambiguous")return QStringLiteral("存在同名档案，请选择大区");
    if(state=="not_found")return QStringLiteral("本地暂无档案");
    if(state=="cancelled")return QStringLiteral("查询已取消");
    if(state=="timeout")return QStringLiteral("查询超时，请稍后重试");
    if(state=="collector_missing")return QStringLiteral("玩家查询引擎未就绪");
    if(state=="python_unavailable")return QStringLiteral("本机玩家查询组件不可用，请重新启动助手");
    if(state=="engine_unavailable")return QStringLiteral("玩家档案读取失败，请重试；本地战绩不会丢失");
    if(state=="database_unavailable")return QStringLiteral("本地战绩库暂时无法读取，请稍后重试");
    if(state=="unsupported_schema")return QStringLiteral("本地战绩格式需要更新，请重新启动助手后重试");
    if(state=="player_not_found")return QStringLiteral("未匹配到该玩家，请核对完整名称和大区");
    if(state=="invalid_query")return QStringLiteral("请输入完整的 名称#编号，并核对大区");
    if(state=="refresh_failed")return QStringLiteral("战绩更新未完成，请确认客户端已登录后重试");
    if(state=="paused_in_game")return QStringLiteral("局内暂停自动更新 · 可点击更新战绩");
    if(state=="source_stopped")return QStringLiteral("战绩来源已暂停 · 已保存战绩保留");
    if(state=="retry_pending"||state=="waiting_source")return QStringLiteral("连接暂未恢复 · 稍后自动重试，已保存战绩保留");
    if(state=="query_busy"||state=="refresh_busy")return QStringLiteral("战绩正在后台整理 · 稍后自动更新");
    return QStringLiteral("暂时无法查询，已保存的档案保留");
}
QString shortMessage(const QString &state){
    if(state=="idle")return QStringLiteral("搜索玩家，或打开“我的战绩”");
    if(state=="querying")return QStringLiteral("后台更新中…");
    if(state=="login_required")return QStringLiteral("查看本人战绩需要登录客户端");
    if(state=="client_data_unavailable")return QStringLiteral("账号暂不可读 · 请重试");
    if(state=="ambiguous")return QStringLiteral("同名玩家 · 请选择大区");
    if(state=="not_found"||state=="player_not_found")return QStringLiteral("未找到玩家档案");
    if(state=="cancelled")return QStringLiteral("查询已取消");
    if(state=="timeout")return QStringLiteral("查询超时 · 请重试");
    if(state=="database_unavailable")return QStringLiteral("战绩库暂不可读");
    if(state=="unsupported_schema")return QStringLiteral("战绩格式待更新");
    if(state=="invalid_query")return QStringLiteral("请核对名称#编号与大区");
    if(state=="refresh_failed")return QStringLiteral("更新未完成 · 请重试");
    if(state=="paused_in_game")return QStringLiteral("局内可手动更新");
    if(state=="source_stopped")return QStringLiteral("战绩来源已暂停");
    if(state=="retry_pending"||state=="waiting_source")return QStringLiteral("等待连接恢复 · 自动重试");
    if(state=="query_busy"||state=="refresh_busy")return QStringLiteral("后台整理中…");
    return QStringLiteral("查询暂不可用 · 请重试");
}
QString pythonProgram(){const auto configured=qEnvironmentVariable("LIXIN_PYTHON");if(!configured.isEmpty())return QFileInfo::exists(configured)?configured:QStandardPaths::findExecutable(configured);const auto bundled=QDir::homePath()+QStringLiteral("/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe");return QFileInfo::exists(bundled)?bundled:QStandardPaths::findExecutable(QStringLiteral("python.exe"));}
// Bind refresh navigation to the rendered player, not to a row number or a
// mutable display label. No identity is added to reports or sent elsewhere.
QJsonObject profileNavigationScope(const QJsonObject &data){
    const auto identity=data.value("identity").toObject(),player=data.value("profile").toObject().value("player").toObject(),navigation=player.value("navigation").toObject();
    const auto server=navigation.value("server").toString(player.value("server").toString(identity.value("server").toString())).trimmed().toUpper();
    if(server.isEmpty()||server.size()>64)return {};
    const auto ref=navigation.value("player_ref").toString(player.value("player_ref").toString());
    if(QRegularExpression(QStringLiteral("^[a-f0-9]{24}$")).match(ref).hasMatch())return {{"server",server},{"player_ref",ref}};
    const auto riotId=identity.value("riot_id").toString(player.value("riot_id").toString(player.value("display_name").toString())).trimmed();
    if(!QRegularExpression(QStringLiteral("^[^#\\r\\n]{1,128}#[^#\\r\\n]{1,32}$")).match(riotId).hasMatch())return {};
    return {{"server",server},{"riot_id",riotId}};
}
bool navigationMatchExists(const QJsonObject &profile,const QJsonObject &view){
    const auto ref=view.value("match_ref").toString();if(ref.isEmpty())return true;
    int found=0;for(const auto &match:profile.value("matches").toArray())found+=match.toObject().value("match_ref").toString()==ref;
    return found==1;
}
void restoreRefreshScroll(PlayerMatchView *results,const QJsonObject &scope,const QJsonObject &view,quint64 epoch){
    const auto expected=results->navigationState();
    // setProfile/openMatch already queue page layout. Restore offsets after
    // those callbacks, and give any newer render or explicit navigation priority.
    QTimer::singleShot(0,results,[results,scope,view,expected,epoch]{
        if(results->property("historyNavigationEpoch").toULongLong()!=epoch||results->property("historyNavigationScope").value<QJsonObject>()!=scope||results->navigationState()!=expected)return;
        if(auto list=results->findChild<QListWidget*>(QStringLiteral("matchArchiveList")))list->verticalScrollBar()->setValue(view.value("list_scroll").toInt());
        if(auto detail=results->findChild<QScrollArea*>(QStringLiteral("matchDetailScrollArea"))){detail->verticalScrollBar()->setValue(view.value("detail_scroll").toInt());detail->horizontalScrollBar()->setValue(view.value("detail_scroll_x").toInt());}
    });
}
}

HistoryPane::HistoryPane(const Catalog &c,QWidget *parent):QWidget(parent),catalog(c){
    setObjectName(QStringLiteral("playerHome"));setMinimumWidth(0);
    auto layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(Kda::Density::sectionGap);
    auto toolbar=new Kda::GlassSurface(nullptr,Kda::GlassSurfaceRole::Toolbar);toolbar->setObjectName(QStringLiteral("historySearchToolbar"));toolbar->setFixedHeight(28);auto row=new QHBoxLayout(toolbar);row->setContentsMargins(0,0,0,0);row->setSpacing(4);
    name=new Kda::LineEdit;name->setObjectName("summonerSearch");name->setPlaceholderText(QStringLiteral("输入名称#编号"));name->setAccessibleName(QStringLiteral("搜索召唤师"));name->setMaxLength(160);name->setMinimumWidth(0);auto leadingIcon=searchLeadingIcon(name);row->addWidget(name,1);
    region=new Kda::ComboBox;region->setObjectName("summonerRegion");region->setAccessibleName(QStringLiteral("查询大区"));region->addItem(QStringLiteral("自动识别"),QString());for(const auto &entry:GameRegion::regions()){if(!sgpServers().contains(entry.code))continue;region->addItem(entry.name,entry.code);const int index=region->count()-1;region->setItemData(index,GameRegion::description(entry),Qt::ToolTipRole);region->setItemData(index,GameRegion::description(entry),Qt::AccessibleDescriptionRole);}region->setFixedWidth(152);region->setMaxVisibleItems(region->count());row->addWidget(region);
    connect(region,&QComboBox::currentIndexChanged,this,[this](int index){region->setToolTip(region->itemData(index,Qt::ToolTipRole).toString());region->setAccessibleDescription(region->toolTip());});updateDetectedServer({});
    search=new Kda::Button(QStringLiteral("搜索"));search->setObjectName("searchSummoner");row->addWidget(search);
    personalButton=new Kda::Button(QStringLiteral("我的战绩"));personalButton->setObjectName("personalHistory");row->addWidget(personalButton);
    compactInput(name);compactInput(region);compactAction(search);compactAction(personalButton);
    symbolAction(personalButton,HudSymbols::Symbol::User,QStringLiteral("我的战绩"),28,92);
    refresh=new Kda::Button(QStringLiteral("更新战绩"));refresh->setObjectName("refreshPlayerHistory");symbolAction(refresh,HudSymbols::Symbol::Reroll,QStringLiteral("更新当前玩家战绩"));row->addWidget(refresh);auto toolbarPlane=new Kda::DepthPlane(toolbar,Kda::DepthLayer::Hud);toolbarPlane->setObjectName(QStringLiteral("historySearchToolbarPlane"));layout->addWidget(toolbarPlane);
    profileBody=new Kda::ReadingSurface;profileBody->setObjectName(QStringLiteral("historyReadingSurface"));auto body=new QVBoxLayout(profileBody);body->setContentsMargins(0,6,0,6);body->setSpacing(4);
    metadataRow=new QWidget;metadataRow->setObjectName(QStringLiteral("historyMetadata"));metadataRow->setFixedHeight(26);auto heading=new QHBoxLayout(metadataRow);heading->setContentsMargins(18,0,18,0);heading->setSpacing(8);
    previousPlayer=new Kda::Button(QStringLiteral("上位玩家"));previousPlayer->setObjectName(QStringLiteral("backToPreviousPlayer"));symbolAction(previousPlayer,HudSymbols::Symbol::Back,QStringLiteral("返回上一位玩家与原来的对局"),26);previousPlayer->hide();heading->addWidget(previousPlayer);
    status=new CompactInfoLabel;status->setObjectName("dataStatus");heading->addWidget(status,1);
    scoreRow=new QWidget;scoreRow->setObjectName(QStringLiteral("playerScoreSummary"));auto scores=new QHBoxLayout(scoreRow);scores->setContentsMargins(0,0,0,0);scores->setSpacing(14);
    recentScore=new ScorePill(HudSymbols::Symbol::Star);recentScore->setObjectName(QStringLiteral("recentResultsScore"));poolScore=new ScorePill(HudSymbols::Symbol::Team);poolScore->setObjectName(QStringLiteral("matchmakingPoolEstimate"));
    for(auto label:{recentScore,poolScore})scores->addWidget(label);scoreRow->hide();heading->addWidget(scoreRow);auto metadataPlane=new Kda::DepthPlane(metadataRow,Kda::DepthLayer::Hud);metadataPlane->setObjectName(QStringLiteral("historyMetadataPlane"));body->addWidget(metadataPlane);
    results=new PlayerMatchView(catalog);results->setObjectName("playerHistoryResults");results->setMinimumSize(0,240);results->setArchiveHeading(metadataPlane);body->addWidget(results,1);layout->addWidget(profileBody,1);profileBody->show();
    connect(search,&QPushButton::clicked,this,[this]{if(busy()&&phase!="revision")cancel();else start();});
    connect(name,&QLineEdit::returnPressed,this,[this]{if(!busy()||phase=="revision")start();});
    const auto changedIdentity=[this]{if(busy()&&phase=="revision")interruptQuery();riotId.clear();server.clear();playerRef.clear();personalProfile=false;pendingRefresh=false;refreshAfterLookup=false;controls();refreshDecisionReplay();};
    connect(name,&QLineEdit::textEdited,this,[changedIdentity](const QString &){changedIdentity();});
    connect(region,&QComboBox::activated,this,[changedIdentity](int){changedIdentity();});
    connect(personalButton,&QPushButton::clicked,this,[this]{personal();});connect(refresh,&QPushButton::clicked,this,[this]{if(!busy())collect(true);});
    results->onDetailChanged=[this](bool){updateDetailPresentation();};
    results->decisionReplayAvailable=[this](const QString &matchRef){
        return personalProfile&&accountScope.known()&&decisionReplayAvailable&&decisionReplayAvailable(matchRef);
    };
    results->onDecisionReplay=[this](const QString &matchRef){if(onDecisionReplay)onDecisionReplay(matchRef);};
    results->onPlayerRequested=[this](const QJsonObject &value){openPlayer(value);};connect(previousPlayer,&QPushButton::clicked,this,[this]{backToPlayer();});
    deadline.setSingleShot(true);
    connect(&deadline,&QTimer::timeout,this,[this]{
        interruptQuery();
        refreshAfterLookup=false;
        if(phase!="revision"){
            state="timeout";
            controls();render();emit updated();
        }else controls();
        applyPendingPersonalHistory();
        if(lifecycleWork.active())lifecycleDispatchTimer.start(0);
    });
    new SkinStyleBinding(this,[this,leadingIcon]{
        compactInput(name);compactInput(region);
        profileBody->update();
        leadingIcon->setIcon(QIcon(HudSymbols::pixmap(HudSymbols::Symbol::Search,14,name->devicePixelRatioF(),Kda::color("muted"))));
    });
    revisionTimer.setParent(this);revisionTimer.setObjectName(QStringLiteral("playerHistoryRevisionTimer"));revisionTimer.setInterval(25000);connect(&revisionTimer,&QTimer::timeout,this,&HistoryPane::pollRevision);revisionTimer.start();controls();render();
    personalHomeDeadline.setParent(this);personalHomeDeadline.setSingleShot(true);personalHomeDeadline.setObjectName(QStringLiteral("personalHomeDeadline"));
    lifecycleClock.start();
    connect(&personalHomeDeadline,&QTimer::timeout,this,[this]{const auto generation=personalHomeGeneration;if(personalHomeWorker)recordLifecycleCompletion(personalHomeWorker,QJsonObject{{"status","timeout"}});stopPersonalHome();acceptPersonalHome(QJsonObject{{"status","timeout"}},generation);if(lifecycleWork.active())lifecycleDispatchTimer.start(0);});
    lifecycleDispatchTimer.setSingleShot(true);connect(&lifecycleDispatchTimer,&QTimer::timeout,this,&HistoryPane::dispatchLifecycleRefresh);
    lifecycleFollowupTimer.setSingleShot(true);connect(&lifecycleFollowupTimer,&QTimer::timeout,this,&HistoryPane::dispatchLifecycleRefresh);
}
HistoryPane::~HistoryPane(){lifecycleDispatchTimer.stop();lifecycleFollowupTimer.stop();stopPersonalHome();interruptQuery();}
bool HistoryPane::busy()const{return worker&&worker->state()!=QProcess::NotRunning;}
QString HistoryPane::selectedServer()const{const auto manual=region->currentData().toString();return manual.isEmpty()?detectedServer:manual;}
void HistoryPane::updateDetectedServer(const QString &value){
    const auto normalized=GameRegion::normalizedCode(value);detectedServer=sgpServers().contains(normalized)?normalized:QString();
    region->setItemText(0,detectedServer.isEmpty()?QStringLiteral("自动识别"):QStringLiteral("当前 · ")+GameRegion::name(detectedServer));
    region->setItemData(0,detectedServer.isEmpty()?QStringLiteral("登录客户端后识别当前大区，也可手动选择"):QStringLiteral("当前客户端：")+GameRegion::name(detectedServer),Qt::ToolTipRole);
    if(region->currentIndex()==0){region->setToolTip(region->itemData(0,Qt::ToolTipRole).toString());region->setAccessibleDescription(region->toolTip());}
}
void HistoryPane::updateDetailPresentation(){
    const bool detail=results->detailsOpen();
    if(auto toolbar=findChild<QWidget*>(QStringLiteral("historySearchToolbar")))toolbar->setVisible(!detail);
    metadataRow->setVisible(!detail&&(!data.isEmpty()||state!="idle"));
    setMaximumWidth(detail?QWIDGETSIZE_MAX:1100);
    setProperty("matchDetailExpanded",detail);
}
void HistoryPane::showProfile(){profileBody->show();applyPendingPersonalHistory();if(onProfileOpened)onProfileOpened();}
void HistoryPane::openArchive(){
    if(state==QStringLiteral("idle")&&accountScope.known()){
        if(!openPersonalMatch())personal();
        return;
    }
    showProfile();
}
bool HistoryPane::profileVisible()const{return !profileBody->isHidden();}
void HistoryPane::closeProfile(){profileBody->hide();if(onProfileOpened)onProfileOpened();}
void HistoryPane::query(const QString &id,int maxMatches){interruptQuery();limit=std::clamp(maxMatches,20,100);name->setText(id);start();}
void HistoryPane::start(){
    const auto id=name->text().trimmed();if(!QRegularExpression(QStringLiteral("^[^#\\r\\n]{1,128}#[^#\\r\\n]{1,32}$")).match(id).hasMatch()){state="invalid_query";data={};riotId.clear();server.clear();controls();showProfile();render();return;}
    interruptQuery();clearNavigation();personalProfile=false;playerRef.clear();riotId=id;server=selectedServer();data={};results->clear();cancelled=false;refreshAfterLookup=true;pendingRefresh=false;lookup("local");
}
void HistoryPane::personal(){interruptQuery();clearNavigation();personalProfile=true;playerRef.clear();data={};results->clear();riotId.clear();server.clear();cancelled=false;refreshAfterLookup=true;pendingRefresh=false;lookup("self");}
void HistoryPane::stopPersonalHome(){
    personalHomeDeadline.stop();if(!personalHomeWorker)return;auto retired=personalHomeWorker;personalHomeWorker=nullptr;retired->disconnect(this);if(retired->state()!=QProcess::NotRunning)retired->kill();else retired->deleteLater();
}
void HistoryPane::setPersonalAccountName(const QString &value){
    auto hint=value.left(128).trimmed();hint.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));
    if(hint==personalAccountHint)return;
    personalAccountHint=hint;emit personalProfileUpdated();
    // Display names can change or collide. Only canonical setAccountScope
    // invalidates account-bound work; this hint may trigger a cached self read.
    if(!hint.isEmpty())refreshPersonalHome();
}
void HistoryPane::setAccountScope(const QString &value){
    const auto normalized=QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(value).hasMatch()?value:QString();
    const auto change=accountScope.observe(normalized.toStdString());
    if(change==HistoryAccountScope::Change::None)return;
    ++personalHomeGeneration;stopPersonalHome();personalHomeData={};personalHistoryReload.cancel();updateDetectedServer({});
    personalHomeState=normalized.isEmpty()?QStringLiteral("login_required"):QStringLiteral("idle");
    if(change==HistoryAccountScope::Change::Changed){
        if(lifecycleWork.active())++lifecycleCancelled;
        lifecycleDispatchTimer.stop();lifecycleFollowupTimer.stop();lifecycleWork.cancel();lifecycleEventId.clear();lifecycleAccountScope.clear();lifecycleMatchRef.clear();lifecyclePublication={};historyLifecycle={};gamePhase.clear();paused=false;
        clearNavigation();
        if(personalProfile){interruptQuery();data={};riotId.clear();server.clear();playerRef.clear();state=personalHomeState;controls();render();emit updated();}
    }else if(lifecycleWork.active()&&lifecycleAccountScope.isEmpty()){
        lifecycleAccountScope=normalized;lifecycleWork.identityAvailable(lifecycleClock.elapsed());lifecycleDispatchTimer.start(0);
    }
    refreshDecisionReplay();emit personalProfileUpdated();
    if(!normalized.isEmpty()&&!personalAccountHint.isEmpty())refreshPersonalHome();
}
QJsonObject HistoryPane::personalProfileSnapshot()const{return personalHomeData.value("profile").toObject();}
void HistoryPane::refreshPersonalHome(const QString &lifecycleReason,const QString &eventId,int attempt){
    if(personalHomeWorker)return;
    const bool lifecycle=!lifecycleReason.isEmpty();
    if(!lifecycle&&lifecycleWork.active())return;
    if(!lifecycle&&personalAccountHint.isEmpty()){personalHomeData={};personalHomeState="login_required";emit personalProfileUpdated();return;}
    const auto generation=++personalHomeGeneration;
    const auto unavailable=[this,generation,lifecycle](const QString &status){acceptPersonalHome(QJsonObject{{"status",status}},generation);if(lifecycle){++lifecycleFailed;lifecycleWork.complete(HistoryRefreshWork::Outcome::Failed,lifecycleClock.elapsed());lifecycleDispatchTimer.start(0);}};
    const auto python=pythonProgram();if(python.isEmpty()){unavailable("python_unavailable");return;}
    if(!QFileInfo::exists(catalog.root+"/scripts/player_home_bridge.py")){unavailable("collector_missing");return;}
    // The account hint only triggers revalidation. The self bridge obtains the
    // canonical identity from the local client; no search identity is supplied.
    auto process=new QProcess(QCoreApplication::instance());personalHomeWorker=process;personalHomeState="loading";
    if(lifecycle){process->setProperty("lifecycleReason",lifecycleReason);process->setProperty("lifecycleId",eventId);process->setProperty("lifecycleAttempt",attempt);lifecycleLastReason=lifecycleReason;lifecycleLastAttempt=attempt;if(lifecycleReason=="enter_match")++lifecycleEnterRequests;else if(attempt==0)++lifecycleLeaveRequests;else ++lifecycleFollowupRequests;}
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),process,&QObject::deleteLater);
    connect(process,&QProcess::errorOccurred,process,[process](QProcess::ProcessError error){if(error==QProcess::FailedToStart)process->deleteLater();});
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process,generation](int code,QProcess::ExitStatus exit){completePersonalHome(process,generation,code,exit);});
    connect(process,&QProcess::errorOccurred,this,[this,process,generation](QProcess::ProcessError error){if(error!=QProcess::FailedToStart||process!=personalHomeWorker||generation!=personalHomeGeneration)return;recordLifecycleCompletion(process,QJsonObject{{"status","engine_unavailable"}});personalHomeWorker=nullptr;personalHomeDeadline.stop();acceptPersonalHome(QJsonObject{{"status","engine_unavailable"}},generation);if(lifecycleWork.active())lifecycleDispatchTimer.start(0);});
    process->setWorkingDirectory(catalog.root);process->start(python,{"-X","utf8",catalog.root+"/scripts/player_home_bridge.py"});
    if(personalHomeWorker!=process||generation!=personalHomeGeneration)return;
    QJsonObject request=lifecycle?QJsonObject{{"action","lifecycle_refresh"},{"lifecycle_refresh",lifecycleReason},{"lifecycle_id",eventId},{"lifecycle_attempt",attempt},{"limit",100},{"cached_only",false}}:QJsonObject{{"action","self"},{"limit",100},{"cached_only",true}};
    if(lifecycle)request["expected_account_scope"]=lifecycleAccountScope;
    if(lifecycle&&lifecycleReason=="leave_match"){if(!lifecycleMatchRef.isEmpty())request["expected_match_ref"]=lifecycleMatchRef;if(lifecycleBaselineKnown)request["baseline_match_refs"]=lifecycleBaselineRefs;}
    process->write(QJsonDocument(request).toJson(QJsonDocument::Compact));process->closeWriteChannel();personalHomeDeadline.start(lifecycle?int(std::max<qint64>(1,std::min<qint64>(85000,lifecycleWork.remaining(lifecycleClock.elapsed())))):30000);emit personalProfileUpdated();
}
void HistoryPane::completePersonalHome(QProcess *process,quint64 generation,int code,QProcess::ExitStatus exit){
    if(process!=personalHomeWorker||generation!=personalHomeGeneration)return;
    personalHomeDeadline.stop();const auto output=process->readAllStandardOutput();process->readAllStandardError();personalHomeWorker=nullptr;
    if(code!=0||exit!=QProcess::NormalExit||output.size()>8*1024*1024){recordLifecycleCompletion(process,QJsonObject{{"status","engine_unavailable"}});acceptPersonalHome(QJsonObject{{"status","engine_unavailable"}},generation);if(lifecycleWork.active())lifecycleDispatchTimer.start(0);return;}
    QJsonParseError error;const auto document=QJsonDocument::fromJson(output,&error);auto value=error.error==QJsonParseError::NoError&&document.isObject()?document.object():QJsonObject{{"status","engine_unavailable"}};
    const bool lifecycle=!process->property("lifecycleReason").toString().isEmpty();
    const auto returnedName=value.value("identity").toObject().value("riot_id").toString().section('#',0,0).trimmed();
    if(lifecycle&&(lifecycleAccountScope.isEmpty()||lifecycleAccountScope!=QString::fromStdString(accountScope.value()))){value=QJsonObject{{"status","account_changed"}};lifecycleWork.cancel();++lifecycleCancelled;}
    recordLifecycleCompletion(process,value);
    if(lifecycle&&value.value("status")=="ready"&&personalAccountHint.isEmpty())personalAccountHint=returnedName;
    acceptPersonalHome(value,generation);
    if(!process->property("lifecycleReason").toString().isEmpty()&&value.value("status")=="ready"){personalHistoryReload.request();applyPendingPersonalHistory();}
    if(lifecycleWork.active()&&!lifecycleDispatchTimer.isActive())lifecycleDispatchTimer.start(0);
}
void HistoryPane::queueLifecycleRefresh(const QString &reason){
    lifecycleFollowupTimer.stop();lifecycleDispatchTimer.stop();lifecycleEventId=QUuid::createUuid().toString(QUuid::Id128);
    lifecycleReason=reason;lifecycleAccountScope=QString::fromStdString(accountScope.value());lifecycleWork.begin(reason=="enter_match",lifecycleClock.elapsed());lifecyclePublication={};
    if(reason=="enter_match"){lifecycleMatchRef.clear();lifecycleBaselineRefs={};lifecycleBaselineKnown=false;}
    else{lifecycleBaselineKnown=personalHomeState=="ready";lifecycleBaselineRefs={};for(const auto row:personalProfileSnapshot().value("matches").toArray()){const auto ref=row.toObject().value("match_ref").toString();if(QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(ref).hasMatch())lifecycleBaselineRefs.append(ref);}}
    if(personalHomeWorker&&!personalHomeWorker->property("lifecycleReason").toString().isEmpty()){++lifecycleCancelled;++personalHomeGeneration;stopPersonalHome();}
    lifecycleDispatchTimer.start(0);
}
void HistoryPane::dispatchLifecycleRefresh(){
    const auto now=lifecycleClock.elapsed();const bool wasActive=lifecycleWork.active();lifecycleWork.tick(now);
    if(!lifecycleWork.active()){if(wasActive&&lifecycleWork.state()==HistoryRefreshWork::State::Expired)++lifecycleSkipped;return;}
    if(lifecycleWork.entering()!=historyLifecycle.inMatch()){lifecycleWork.cancel();++lifecycleCancelled;return;}
    if(lifecycleWork.state()==HistoryRefreshWork::State::WaitingFollowup){lifecycleFollowupTimer.start(int(std::min(lifecycleWork.followupRemaining(now),lifecycleWork.remaining(now))));return;}
    if(!lifecycleWork.pending()){if(lifecycleWork.state()==HistoryRefreshWork::State::WaitingIdentity)lifecycleDispatchTimer.start(int(lifecycleWork.remaining(now)));return;}
    if(!accountScope.known()){lifecycleDispatchTimer.start(int(lifecycleWork.remaining(now)));return;}
    if(lifecycleAccountScope.isEmpty())lifecycleAccountScope=QString::fromStdString(accountScope.value());
    if(lifecycleAccountScope!=QString::fromStdString(accountScope.value())){lifecycleWork.cancel();++lifecycleCancelled;return;}
    // Wait for existing work's completion, retaining the event until its total
    // deadline. Completion callbacks wake this timer; there is no busy polling.
    if(busy()||personalHomeWorker){lifecycleDispatchTimer.start(int(lifecycleWork.remaining(now)));return;}
    if(lifecycleWork.start(now))refreshPersonalHome(lifecycleReason,lifecycleEventId,lifecycleWork.attempt());
}
void HistoryPane::recordLifecycleCompletion(QProcess *process,const QJsonObject &value){
    const auto reason=process->property("lifecycleReason").toString();if(reason.isEmpty())return;
    if(process->property("lifecycleId").toString()!=lifecycleEventId)return;
    const bool success=value.value("status")=="ready";
    if(success)++lifecycleCompleted;else ++lifecycleFailed;
    if(reason=="enter_match"&&success){const auto ref=value.value("refresh").toObject().value("observed_match_ref").toString();if(QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(ref).hasMatch())lifecycleMatchRef=ref;}
    if(reason=="leave_match")lifecyclePublication=value.value("publication").isObject()?value.value("publication").toObject():QJsonObject{{"state","unknown"},{"evidence","profile_unavailable"},{"just_finished_match_observed",QJsonValue(QJsonValue::Null)}};
    using Outcome=HistoryRefreshWork::Outcome;
    const bool noNetwork=value.contains("network_used")&&value.value("network_used").isBool()&&!value.value("network_used").toBool();
    if(value.value("status")=="account_changed"){lifecycleWork.cancel();++lifecycleCancelled;return;}
    const auto outcome=success?Outcome::Ready:value.value("status")=="refresh_busy"&&noNetwork?Outcome::SourceBusy:value.value("status")=="login_required"&&noNetwork?Outcome::LoginRequired:value.value("status")=="paused_in_game"&&noNetwork?Outcome::RetryBeforeNetwork:Outcome::Failed;
    lifecycleWork.complete(outcome,lifecycleClock.elapsed());
    if(lifecycleWork.active())lifecycleDispatchTimer.start(lifecycleWork.pending()?750:0);
}
void HistoryPane::applyPendingPersonalHistory(){
    if(!personalHistoryReload.take(busy(),personalProfile,profileVisible(),personalHomeState=="ready"))return;
    data=personalHomeData;state="ready";
    const auto identity=data.value("identity").toObject();riotId=identity.value("riot_id").toString();server=identity.value("server").toString();name->setText(riotId);phase="self";controls();render();emit updated();
}
QJsonObject HistoryPane::lifecycleRefreshReport()const{
    using S=HistoryRefreshWork::State;const auto s=lifecycleWork.state();const char *workState=s==S::Idle?"idle":s==S::Pending?"pending":s==S::Running?"running":s==S::WaitingIdentity?"waiting_identity":s==S::WaitingFollowup?"waiting_followup":s==S::Expired?"expired":"finished";
    return {{"event_reason",lifecycleReason},{"event_attempt",lifecycleWork.attempt()},{"last_reason",lifecycleLastReason},{"last_attempt",lifecycleLastAttempt},{"target","authenticated_self"},{"enter_requests",lifecycleEnterRequests},{"leave_requests",lifecycleLeaveRequests},{"followup_requests",lifecycleFollowupRequests},{"completed",lifecycleCompleted},{"completed_means","source_fetch_and_profile_only"},{"failed",lifecycleFailed},{"cancelled",lifecycleCancelled},{"skipped_busy_or_obsolete",lifecycleSkipped},{"pending",lifecycleWork.pending()},{"work_state",workState},{"preflight_retries",lifecycleWork.retryCount()},{"total_deadline_ms",qint64(HistoryRefreshWork::LifetimeMs)},{"publication",lifecyclePublication},{"own_page_reload_pending",personalHistoryReload.pending()},{"followup_pending",s==S::WaitingFollowup},{"maximum_leave_followups",1},{"leave_followup_delay_ms",qint64(HistoryRefreshWork::FollowupMs)},{"identities_exported",false}};
}
void HistoryPane::acceptPersonalHome(const QJsonObject &value,quint64 generation){
    if(generation!=personalHomeGeneration)return;
    if(personalAccountHint.isEmpty()&&value.value("status")=="ready")return;
    auto next=value.value("status").toString(QStringLiteral("engine_unavailable"));
    const QStringList publicStates{"ready","login_required","client_data_unavailable","paused_in_game","cache_missing","query_busy","database_unavailable","unsupported_schema","not_found","player_not_found","ambiguous","invalid_query","python_unavailable","collector_missing","engine_unavailable","timeout"};
    if(!publicStates.contains(next))next="engine_unavailable";
    if(next=="paused_in_game"&&!paused)next="cache_missing";
    const auto identity=value.value("identity").toObject(),profile=value.value("profile").toObject(),player=profile.value("player").toObject();
    if(next=="ready"&&(identity.value("riot_id").toString().isEmpty()||identity.value("server").toString().isEmpty()||profile.isEmpty()||player.isEmpty()||(!player.value("server").toString().isEmpty()&&player.value("server").toString().compare(identity.value("server").toString(),Qt::CaseInsensitive)!=0)))next="engine_unavailable";
    if(next=="login_required")updateDetectedServer({});else if(next!="engine_unavailable"&&!identity.value("riot_id").toString().isEmpty()&&!identity.value("server").toString().isEmpty())updateDetectedServer(identity.value("server").toString());
    personalHomeState=next;personalHomeData=next=="ready"?value:QJsonObject{};emit personalProfileUpdated();
}
bool HistoryPane::openPersonalMatch(const QString &matchRef){
    const auto profile=personalProfileSnapshot();if(profile.isEmpty())return false;
    const auto matches=profile.value("matches").toArray();int index=-1;
    if(!matchRef.isEmpty()){for(int i=0;i<matches.size();++i)if(matches[i].toObject().value("match_ref").toString()==matchRef){index=i;break;}if(index<0)return false;}
    interruptQuery();clearNavigation();data=personalHomeData;personalProfile=true;const auto identity=data.value("identity").toObject();riotId=identity.value("riot_id").toString();server=identity.value("server").toString();playerRef=profile.value("player").toObject().value("navigation").toObject().value("player_ref").toString();
    name->setText(riotId);const int regionIndex=region->findData(server);if(regionIndex>=0)region->setCurrentIndex(regionIndex);
    state="ready";phase="self";cancelled=false;pendingRefresh=false;refreshAfterLookup=false;controls();render();results->closeMatch();if(index>=0)results->openMatch(index);showProfile();emit updated();return true;
}
void HistoryPane::refreshDecisionReplay(){results->refreshDecisionReplay();}
void HistoryPane::returnToDecisionMatch(const QString &matchRef){
    if(matchRef.isEmpty())return;
    if(!personalProfile||results->navigationState().value("match_ref").toString()!=matchRef)
        openPersonalMatch(matchRef);
}
void HistoryPane::clearNavigation(){navigation.clear();previousPlayer->hide();}
bool HistoryPane::interruptQuery(){
    ++queryGeneration;deadline.stop();cancelled=true;
    // Retire the process without waiting on the GUI thread. Each completion
    // is scoped to its own process and generation, so it cannot publish late.
    if(worker){auto retired=worker;worker=nullptr;retired->disconnect(this);if(retired->state()!=QProcess::NotRunning)retired->kill();else retired->deleteLater();}return true;
}
void HistoryPane::openPlayer(const QJsonObject &value){
    const auto ref=value.value("player_ref").toString(),targetServer=value.value("server").toString();if(!value.value("available").toBool()||!QRegularExpression(QStringLiteral("^[a-f0-9]{24}$")).match(ref).hasMatch()||targetServer.isEmpty()||targetServer.size()>64)return;
    if(!interruptQuery())return;if(!data.isEmpty()){
        const auto identity=data.value("identity").toObject(),profile=data.value("profile").toObject().value("player").toObject(),storedNavigation=profile.value("navigation").toObject();
        const auto storedRiotId=identity.value("riot_id").toString(),storedServer=identity.value("server").toString(profile.value("server").toString());const int storedRegion=region->findData(storedServer);
        if(navigation.size()>=12)navigation.removeFirst();navigation.append({data,results->navigationState(),storedRiotId,storedServer,storedNavigation.value("player_ref").toString(),storedRiotId,state=="querying"?QStringLiteral("ready"):state,storedRegion>=0?storedRegion:0,personalProfile});
    }
    personalProfile=false;playerRef=ref;server=targetServer;riotId.clear();name->clear();const int index=region->findData(server);if(index>=0)region->setCurrentIndex(index);data={};results->clear();cancelled=false;refreshAfterLookup=true;pendingRefresh=false;previousPlayer->setVisible(!navigation.isEmpty());lookup("player");
}
void HistoryPane::backToPlayer(){
    if(navigation.isEmpty()||!interruptQuery())return;
    const auto frame=navigation.takeLast();data=frame.data;riotId=frame.riotId;server=frame.server;playerRef=frame.playerRef;personalProfile=frame.personal;name->setText(frame.name);region->setCurrentIndex(frame.region);state=frame.state;phase="local";cancelled=false;pendingRefresh=false;refreshAfterLookup=false;previousPlayer->setVisible(!navigation.isEmpty());controls();render();results->restoreNavigationState(frame.view);
    if(!frame.view.value("match_ref").toString().isEmpty()){
        // Opening details hides the search row and grows the result viewport.
        // Settle that geometry synchronously before restoring the requested
        // page, so a transient capacity cannot re-anchor it on the next layout.
        QList<QWidget*> ancestors;for(QWidget *widget=results;widget;widget=widget->parentWidget())ancestors.prepend(widget);
        for(auto widget:ancestors)if(widget->layout())widget->layout()->activate();
        results->restoreNavigationState(frame.view);
    }
    showProfile();emit updated();
}
void HistoryPane::lookup(const QString &action,bool manualRefresh){
    if(lifecycleWork.active()&&(backgroundLookup||action=="revision"))return;
    if(busy())return;quietNoticeRequest=backgroundLookup||action=="revision";phase=action;if(action!="revision")state="querying";if(!backgroundLookup&&(action=="local"||action=="self"||action=="player"))showProfile();
    const auto python=pythonProgram();if(python.isEmpty()){state="python_unavailable";controls();render();emit updated();return;}if(!QFileInfo::exists(catalog.root+"/scripts/player_home_bridge.py")){state="collector_missing";controls();render();emit updated();return;}
    if(action!="revision"&&data.isEmpty())results->clear(action=="self"?QStringLiteral("正在读取当前账号和本地海克斯战绩…"):QStringLiteral("正在读取本地玩家档案…"));
    const auto generation=++queryGeneration;auto process=new QProcess(QCoreApplication::instance());worker=process;cancelled=false;
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),process,&QObject::deleteLater);
    connect(process,&QProcess::errorOccurred,process,[process](QProcess::ProcessError error){if(error==QProcess::FailedToStart)process->deleteLater();});
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process,generation](int code,QProcess::ExitStatus exit){if(worker==process&&generation==queryGeneration){complete(code,exit);applyPendingPersonalHistory();if(lifecycleWork.active())lifecycleDispatchTimer.start(0);}});
    connect(process,&QProcess::errorOccurred,this,[this,process,generation](QProcess::ProcessError error){if(worker!=process||generation!=queryGeneration||error!=QProcess::FailedToStart)return;worker=nullptr;process->deleteLater();deadline.stop();state="engine_unavailable";controls();render();emit updated();applyPendingPersonalHistory();if(lifecycleWork.active())lifecycleDispatchTimer.start(0);});
    process->setWorkingDirectory(catalog.root);process->start(python,{"-X","utf8",catalog.root+"/scripts/player_home_bridge.py"});
    const bool explicitRefresh=action=="refresh"&&manualRefresh;
    process->write(QJsonDocument(QJsonObject{{"action",action},{"riot_id",riotId},{"server",server},{"player_ref",playerRef},{"limit",limit},{"cached_only",paused&&!explicitRefresh},{"manual_refresh",explicitRefresh}}).toJson(QJsonDocument::Compact));process->closeWriteChannel();deadline.start(action=="refresh"?85000:action=="revision"?15000:30000);controls();if(action!="revision")render();
}
void HistoryPane::collect(bool manualRefresh){
    if((riotId.isEmpty()&&playerRef.isEmpty())||busy())return;
    if(!manualRefresh&&lifecycleWork.active()){pendingRefresh=true;return;}
    if(paused&&!manualRefresh){pendingRefresh=true;state="paused_in_game";controls();render();return;}
    const auto key=server+":"+(playerRef.isEmpty()?riotId:playerRef);const auto now=QDateTime::currentMSecsSinceEpoch();
    if(now<refreshTimes.value(key)){if(!manualRefresh)pendingRefresh=true;return;}
    refreshTimes[key]=now+25000;pendingRefresh=false;lookup("refresh",manualRefresh);
}
void HistoryPane::cancel(){
    interruptQuery();pendingRefresh=false;refreshAfterLookup=false;state="cancelled";controls();render();emit updated();applyPendingPersonalHistory();if(lifecycleWork.active())lifecycleDispatchTimer.start(0);
}
void HistoryPane::complete(int code,QProcess::ExitStatus exit){
    deadline.stop();const auto output=worker->readAllStandardOutput();worker->readAllStandardError();worker=nullptr;
    if(cancelled){controls();render();emit updated();return;}
    if(code!=0||exit!=QProcess::NormalExit||output.size()>8*1024*1024){state="engine_unavailable";controls();render();emit updated();return;}
    const auto value=QJsonDocument::fromJson(output).object();
    if(phase=="revision"){const auto next=value.value("revision").toString();const bool changed=!next.isEmpty()&&next!=revision;revision=next;controls();if(changed||pendingRefresh||personalProfile){refreshAfterLookup=pendingRefresh;reloadCached();}return;}
    state=value.value("status").toString("engine_unavailable");
    const auto identity=value.value("identity").toObject();if(!identity.isEmpty()){riotId=identity.value("riot_id").toString();server=identity.value("server").toString();name->setText(riotId);const int index=region->findData(server);if(index>=0)region->setCurrentIndex(index);}
    if(state=="paused_in_game"||state=="retry_pending"||state=="waiting_source"||state=="query_busy"||state=="refresh_busy")pendingRefresh=true;
    if(phase=="refresh"&&state=="retry_pending"){
        const auto key=server+":"+(playerRef.isEmpty()?riotId:playerRef);
        refreshTimes[key]=std::max(refreshTimes.value(key),value.value("retry_at_ms").toInteger());
    }
    if(state=="ready"){data=value;if(phase=="refresh")pendingRefresh=false;const auto targetNavigation=value.value("profile").toObject().value("player").toObject().value("navigation").toObject();if(targetNavigation.value("available").toBool())playerRef=targetNavigation.value("player_ref").toString();}
    if(personalProfile&&state=="login_required"){data={};riotId.clear();server.clear();playerRef.clear();pendingRefresh=false;}
    controls();render();emit updated();
    if(personalProfile&&state=="ready"&&!personalAccountHint.isEmpty())refreshPersonalHome();
    if(refreshAfterLookup){refreshAfterLookup=false;if(state=="ready"||state=="not_found"||state=="paused_in_game")collect();}
}
void HistoryPane::setGamePhase(const QString &value){
    const auto observed=value.toLatin1().toStdString();gamePhase=value;
    if(!HistoryRefreshLifecycle::insidePhase(observed)&&!HistoryRefreshLifecycle::outsidePhase(observed))return;
    const auto event=historyLifecycle.observe(observed);const bool next=historyLifecycle.inMatch();const bool ended=paused&&!next;
    if(event==HistoryRefreshLifecycle::Event::EnterMatch)queueLifecycleRefresh("enter_match");
    if(event==HistoryRefreshLifecycle::Event::LeaveMatch)queueLifecycleRefresh("leave_match");
    if(next==paused)return;paused=next;
    if(paused){
        if(profileVisible()&&(personalProfile||!riotId.isEmpty()||!playerRef.isEmpty()))pendingRefresh=true;
        if(busy()&&phase!="revision"){refreshTimes.remove(server+":"+(playerRef.isEmpty()?riotId:playerRef));interruptQuery();}
        if(pendingRefresh){state="paused_in_game";controls();render();emit updated();}
    }
    if(ended&&profileVisible()&&(personalProfile||pendingRefresh)){refreshAfterLookup=pendingRefresh;reloadCached();}
    if(ended)refreshPersonalHome();
}
void HistoryPane::reloadCached(){if(busy()||(!personalProfile&&riotId.isEmpty()&&playerRef.isEmpty()))return;backgroundLookup=true;lookup(personalProfile?QStringLiteral("self"):playerRef.isEmpty()?QStringLiteral("local"):QStringLiteral("player"));backgroundLookup=false;}
void HistoryPane::pollRevision(){if(!profileVisible()||busy()||(!personalProfile&&!pendingRefresh))return;lookup("revision");}
void HistoryPane::controls(){
    const bool active=busy()&&phase!="revision";name->setEnabled(!active);region->setEnabled(!active);personalButton->setEnabled(!active);refresh->setEnabled(!active&&(!riotId.isEmpty()||!playerRef.isEmpty()));
    search->setText(active?QStringLiteral("取消"):QStringLiteral("搜索"));symbolAction(search,active?HudSymbols::Symbol::Close:HudSymbols::Symbol::Search,active?QStringLiteral("取消当前查询"):QStringLiteral("搜索玩家（回车）"),28,64);

}
void HistoryPane::publishNotice(){
    if(suppressNotices||state==QStringLiteral("idle")||phase==QStringLiteral("revision"))return;
    if(quietNoticeRequest&&(state==QStringLiteral("querying")||state==QStringLiteral("ready")))return;
    const bool stale=data.value("cache").toObject().value("stale").toBool();
    const auto stamp=state+(state==QStringLiteral("querying")?QStringLiteral(":")+phase:QString())+(state==QStringLiteral("ready")?(stale?QStringLiteral(":saved"):QStringLiteral(":current")):QString());
    if(stamp==lastNoticeState)return;lastNoticeState=stamp;
    Kda::Notice notice;notice.key=QStringLiteral("history.query");notice.detail=message(state);
    if(state==QStringLiteral("querying")){notice.kind=Kda::NoticeKind::Progress;notice.title=phase==QStringLiteral("refresh")?QStringLiteral("正在更新战绩"):QStringLiteral("正在读取战绩");notice.detail=phase==QStringLiteral("refresh")?QStringLiteral("已保存的结果可以继续查看，新记录到达后自动更新。"):QStringLiteral("先读取本机已保存的档案。对局中只使用已保存结果。");notice.durationMs=3500;}
    else if(state==QStringLiteral("ready")){notice.kind=stale?Kda::NoticeKind::Warning:Kda::NoticeKind::Success;notice.title=stale?QStringLiteral("已显示保存的战绩"):QStringLiteral("战绩已就绪");notice.detail=stale?QStringLiteral("当前来自较旧缓存；连接可用时继续更新。详情保留在战绩信息中。"):QStringLiteral("已读取可用的玩家记录。每项数值按有记录的场次计算。");notice.durationMs=stale?5500:3000;}
    else if(state==QStringLiteral("query_busy")||state==QStringLiteral("refresh_busy")){notice.kind=Kda::NoticeKind::Progress;notice.title=QStringLiteral("战绩正在后台整理");notice.durationMs=3500;}
    else if(state==QStringLiteral("paused_in_game")||state==QStringLiteral("source_stopped")||state==QStringLiteral("login_required")||state==QStringLiteral("ambiguous")||state==QStringLiteral("not_found")||state==QStringLiteral("player_not_found")||state==QStringLiteral("invalid_query")){notice.kind=Kda::NoticeKind::Warning;notice.title=shortMessage(state);notice.durationMs=6000;}
    else if(state==QStringLiteral("cancelled")){notice.kind=Kda::NoticeKind::Info;notice.title=QStringLiteral("查询已取消");notice.durationMs=2500;}
    else{notice.kind=Kda::NoticeKind::Error;notice.title=shortMessage(state);notice.durationMs=8500;}
    ++noticeTransitions;Kda::postNotice(notice);
}
void HistoryPane::render(){
    const auto navigationEpoch=results->property("historyNavigationEpoch").toULongLong()+1;results->setProperty("historyNavigationEpoch",QVariant::fromValue(navigationEpoch));
    publishNotice();
    if(data.isEmpty()){results->setProperty("historyNavigationScope",QVariant::fromValue(QJsonObject{}));metadataRow->hide();scoreRow->hide();for(auto label:{recentScore,poolScore}){label->clear();label->setToolTip({});label->setAccessibleName({});label->setAccessibleDescription({});label->setProperty("scoreKnown",false);}status->setText(shortMessage(state));status->setToolTip(message(state));status->setAccessibleName(message(state));results->clear(state=="idle"?QString():message(state));return;}
    metadataRow->show();
    const auto profile=data.value("profile").toObject(),coverage=profile.value("coverage").toObject();
    const auto recent=profile.value("recent_results_score").toObject(),estimate=profile.value("matchmaking_estimate").toObject();const auto cacheNote=cacheExplanation(data,paused);
    const auto showScore=[&](QLabel *label,const QJsonObject &score,const ScoreLabel &view){label->setVisible(!score.isEmpty()&&(label!=poolScore||view.known));label->setText(view.text);const auto explanation=view.tooltip+(cacheNote.isEmpty()?QString():QStringLiteral("\n")+cacheNote);label->setToolTip(explanation);label->setAccessibleName((label==recentScore?QStringLiteral("近期战果分 "):QStringLiteral("匹配池估计 "))+view.text);label->setAccessibleDescription(explanation);label->setProperty("scoreKnown",view.known);};
    showScore(recentScore,recent,recentScoreText(recent));showScore(poolScore,estimate,poolScoreText(estimate));scoreRow->setVisible((!recent.isEmpty()||poolScoreText(estimate).known));
    QStringList missing;for(const auto &v:profile.value("five_axes").toArray()){const auto axis=v.toObject();if(!axis.value("value").isDouble())missing.append(axis.value("label").toString());}QString ready=QStringLiteral("%1 场归档 · 显示最近 %2 场 · 海克斯大乱斗").arg(coverage.value("eligible_matches").toInt()).arg(profile.value("matches").toArray().size());if(!missing.isEmpty())ready+=QStringLiteral(" · %1未记录").arg(missing.join(QStringLiteral("、")));
    if(data.value("cache").toObject().value("stale").toBool())ready+=QStringLiteral(" · 较旧缓存");
    QString compact=QStringLiteral("最近对局 · %1 场 · 海克斯").arg(profile.value("matches").toArray().size());
    if(personalProfile&&lifecycleReason=="leave_match"){
        const auto publication=lifecyclePublication.value("state").toString();
        if(publication=="record_observed")compact+=QStringLiteral(" · 本局已同步");
        else if(publication=="awaiting_publication")compact+=QStringLiteral(" · 待战绩发布");
        else if(publication=="new_record_observed"||publication=="unknown")compact+=QStringLiteral(" · 本局待确认");
    }
    status->setText(compact);
    QString tooltip=ready+QStringLiteral("\n")+(missing.isEmpty()?QStringLiteral("每项数值按有记录的场次计算；鼠标停在数值上可看有效场次。"):QStringLiteral("“未记录”表示已保存对局没有该字段，不是仍在计算；更新战绩后如来源提供该项才会补齐。"));if(state!="ready")tooltip+=QStringLiteral("\n")+message(state);if(!cacheNote.isEmpty())tooltip+=QStringLiteral("\n")+cacheNote;status->setToolTip(tooltip);status->setAccessibleName(compact);status->setAccessibleDescription(tooltip);
    const auto scope=profileNavigationScope(data),view=results->navigationState();
    const bool preserve=!scope.isEmpty()&&scope==results->property("historyNavigationScope").value<QJsonObject>()&&navigationMatchExists(profile,view);
    // PlayerMatchView can retain a matching match_ref across setProfile; clear
    // it when the player is different or unconfirmed, even if refs coincide.
    if(!preserve)results->clear();
    results->setProfile(profile);results->setProperty("historyNavigationScope",QVariant::fromValue(scope));
    if(preserve){
        // An archive refresh must not repaginate the hidden scoreboard. Its
        // viewport is unsettled while switching back from another player.
        if(view.value("match_ref").toString().isEmpty())results->setArchivePage(view.value("archive_page").toInt());
        else results->restoreNavigationState(view);
        restoreRefreshScroll(results,scope,view,navigationEpoch);
    }
    updateDetailPresentation();
}

QJsonObject HistoryPane::report()const{
    const bool scoresVisible=scoreRow->isVisible();bool scoresFit=true;QJsonArray scoreGeometry;
    if(scoresVisible)for(auto label:{recentScore,poolScore})if(label->isVisible()){
        const auto contents=label->contentsRect();const auto metrics=label->fontMetrics();const int textWidth=metrics.horizontalAdvance(label->text());const bool insideRow=scoreRow->rect().contains(label->geometry());
        scoresFit&=contents.width()>0&&textWidth<=contents.width()&&metrics.height()<=contents.height()&&insideRow;
        scoreGeometry.append(QJsonObject{{"name",label->objectName()},{"text_width",textWidth},{"available_width",contents.width()},{"font_height",metrics.height()},{"available_height",contents.height()},{"inside_row",insideRow}});
    }
    const auto toolbar=findChild<QWidget*>(QStringLiteral("historySearchToolbar"));
    return {{"state",state},{"busy",busy()},{"query_phase",phase},{"query_engine_available",!pythonProgram().isEmpty()&&QFileInfo::exists(catalog.root+"/scripts/player_home_bridge.py")},{"missing_fields_are_not_loading",true},{"surface","main_window"},{"depth_composition",QJsonObject{{"fixed_actions_foreground",QJsonArray{"historySearchToolbar","historyMetadata"}},{"native_input",true},{"toolbar_visibility_synced",toolbar&&toolbar->isHidden()==toolbar->parentWidget()->isHidden()},{"metadata_visibility_synced",metadataRow->isHidden()==metadataRow->parentWidget()->isHidden()}}},{"profile_visible",profileVisible()},{"archive",results->report()},{"engine","player_five_dimensions"},{"shared_player_library",true},{"toolbar_height",toolbar?toolbar->height():0},{"search_visible",toolbar&&!toolbar->isHidden()},{"detail_expanded",results->detailsOpen()&&maximumWidth()==QWIDGETSIZE_MAX},{"detail_metadata_hidden",results->detailsOpen()&&metadataRow->isHidden()},{"metadata_height",metadataRow->height()},{"icon_actions",true},{"navigation_depth",navigation.size()},{"previous_player_available",!navigation.isEmpty()},{"navigation_cancels_old_query",true},{"background_refresh",true},{"game_phase",gamePhase},{"in_match",historyLifecycle.inMatch()},{"paused_in_game",paused},{"refresh_pending",pendingRefresh},{"personal_profile",personalProfile},{"personal_home",QJsonObject{{"state",personalHomeState},{"reason",personalHomeState=="cache_missing"?QStringLiteral("cache_missing"):QString()},{"has_profile",!personalProfileSnapshot().isEmpty()},{"busy",personalHomeWorker!=nullptr},{"generation",qint64(personalHomeGeneration)},{"cached_only",true},{"remote_refresh",false},{"identities_exported",false}}},{"lifecycle_refresh",lifecycleRefreshReport()},{"refresh_throttle_ms",25000},{"revision_poll_ms",revisionTimer.interval()},{"query_generation",qint64(queryGeneration)},{"blocking_cancel",false},{"uses_source_lease_bridge",true},{"matchmaking_estimate",data.value("profile").toObject().value("matchmaking_estimate")},{"recent_results_score",data.value("profile").toObject().value("recent_results_score")},{"score_presentation",QJsonObject{{"recent_text",recentScore->text()},{"pool_text",poolScore->text()},{"recent_known",recentScore->property("scoreKnown").toBool()},{"pool_known",poolScore->property("scoreKnown").toBool()},{"visible",scoresVisible},{"fit_evaluated",scoresVisible},{"labels_fit",scoresFit},{"label_geometry",scoreGeometry}}},{"shown_matches",data.value("profile").toObject().value("matches").toArray().size()},{"identities_exported",false},{"sent_to_ai",false}};
}
QJsonObject HistoryPane::selfCheck(const QString &outputDir){
    const bool savedNoticeSuppression=suppressNotices;suppressNotices=true;
    const auto saved=data;const auto oldState=state,oldPhase=phase;const bool visible=profileVisible(),savedPersonal=personalProfile,savedPending=pendingRefresh,savedRefreshAfter=refreshAfterLookup;const auto savedNavigation=navigation;const auto savedPlayerRef=playerRef,savedRiotId=riotId,savedServer=server,savedName=name->text();const int savedRegion=region->currentIndex();
    const auto savedHomeData=personalHomeData;const auto savedAccountHint=personalAccountHint,savedHomeState=personalHomeState,savedDetectedServer=detectedServer;const bool homeWasLoading=personalHomeWorker!=nullptr;
    const auto savedCanonicalScope=accountScope;const auto savedHistoryLifecycle=historyLifecycle;const auto savedGamePhase=gamePhase;const bool savedGamePaused=paused;
    ++personalHomeGeneration;stopPersonalHome();
    const auto actionLabelsFit=[&]{
        for(auto control:findChildren<QPushButton*>())if(control->isVisibleTo(window())){
            if(control->property("elidedPlayerName").toBool()||control->property("elidedAugmentName").toBool())continue;
            if(control->property("symbolOnly").toBool()){if(control->toolTip().isEmpty()||control->accessibleName().isEmpty()||control->width()<26||control->height()<26)return false;continue;}
            const int pixels=control->property("labelPixels").toInt();
            auto font=Kda::font(Kda::FontRole::Label,pixels>=11&&pixels<=16?pixels:13,QFont::Bold);
            if(control->property("labelRegular").toBool())font.setWeight(QFont::Normal);
            const QFontMetrics metrics(font);
            const auto themed=dynamic_cast<Kda::Button*>(control);const int symbolInset=themed&&themed->hasSymbol()?22:0;
            if(metrics.horizontalAdvance(control->text())>control->width()-16-symbolInset||metrics.height()>control->height()||control->height()<Kda::Density::controlHeight)return false;
        }
        return true;
    };
    QJsonArray players;const QJsonArray champions{10,103,145,84,28,1,22,86,12,64};
    const int kills[]{5,11,3,2,4,8,17,10,6,2},deaths[]{0,6,4,2,3,1,1,9,2,9},assists[]{10,6,16,15,14,4,7,9,12,20};
    const int damage[]{0,10400,9380,6750,6340,21400,13200,8300,11700,5200},taken[]{7880,10200,6730,5070,14900,18400,27800,11000,6400,9800};
    for(int i=0;i<10;++i)players.append(QJsonObject{{"navigation",QJsonObject{{"available",i!=9},{"player_ref",QStringLiteral("%1").arg(i+1,24,16,QChar('0'))},{"server","TJ100"}}},{"champion_id",champions[i]},{"team_id",i<5?100:200},{"is_target",i==0},{"display_name",i==0?QStringLiteral("合成示意·用于窄屏验收的长中文召唤师名称"):QStringLiteral("合成玩家 %1").arg(i+1)},{"scores",QJsonObject{{"kills",kills[i]},{"deaths",i==0?QJsonValue():QJsonValue(deaths[i])},{"assists",assists[i]},{"gold_earned",15604+i*100},{"level",18},{"damage_to_champions",damage[i]},{"damage_taken",taken[i]},{"physical_damage_to_champions",damage[i]*.55},{"magic_damage_to_champions",i==2?QJsonValue():QJsonValue(damage[i]*.35)},{"true_damage_to_champions",damage[i]*.1},{"damage_mitigated",i==1?0:16000+i*1200},{"team_damage_share",i==0?QJsonValue():QJsonValue(.18+i*.01)},{"damage_per_minute",1062+i*12},{"heal",2400+i*300},{"gold_per_minute",890+i*7},{"cs",65+i},{"heal_teammates",i==0?QJsonValue():QJsonValue(1200+i*100)},{"shield",i==1?0:2400},{"kill_participation",.65}}},{"items",QJsonArray{3115,3089,3020,3135,3157,4628,3364}},{"augments",i==0?QJsonArray{1001,1002,1003,1004,1001,1002,1003,1004}:i==8?QJsonArray{1001,999999999}:i==9?QJsonArray{}:QJsonArray{1001,1002,1003,1004}}});
    QJsonArray matches;for(int i=0;i<6;++i)matches.append(QJsonObject{{"match_ref",QString::number(i)},{"champion_id",champions[i]},{"started_at_ms",1792584000000.0-i*86400000.},{"duration_seconds",1053},{"patch",i==5?QStringLiteral("0.0"):catalog.patch},{"win",i%2==0},{"scores",QJsonObject{{"kills",8},{"deaths",QJsonValue()},{"assists",12},{"gold_earned",15604}}},{"players",players}});
    QJsonArray axes;const QStringList axisKeys{QStringLiteral("damage_to_champions_per_minute"),QStringLiteral("gold_earned_per_minute"),QStringLiteral("kill_participation"),QStringLiteral("deaths_per_minute"),QStringLiteral("damage_taken_per_minute")};const QStringList axisLabels{QStringLiteral("输出"),QStringLiteral("经济"),QStringLiteral("参团"),QStringLiteral("生存"),QStringLiteral("承伤")};const double axisValues[]{1062.,890.,65.,.35,885.2},axisRanks[]{65.,50.,75.,35.,55.};for(int i=0;i<5;++i)axes.append(QJsonObject{{"key",axisKeys[i]},{"label",axisLabels[i]},{"value",axisValues[i]},{"unit",i==2?QStringLiteral("%"):QStringLiteral("/分")},{"lower_is_better",i==3},{"n",6},{"percentile",axisRanks[i]},{"percentile_n",5}});
    const QJsonObject recentFixture{{"status","scored"},{"value",62.5},{"n_matches",6},{"observations",QJsonObject{{"matches",6},{"wins",4},{"losses",2},{"independent_matchups",6},{"effective_n",5.4}}},{"sample_support",QJsonObject{{"level","low"}}},{"window",QJsonObject{{"days",30}}},{"formula",QJsonObject{{"prior_alpha",2},{"prior_beta",2},{"half_life_days",14},{"matchup_weight_cap",1}}}};
    const QJsonObject poolFixture{{"status","unknown"},{"value",QJsonValue()},{"reason","player_budget_exceeded"},{"n_matches",400},{"n_players",325},{"observations",QJsonObject{{"target_matches",6}}}};
    data={{"profile",QJsonObject{{"player",QJsonObject{{"display_name",QStringLiteral("合成玩家#00000")},{"server","TJ100"}}},{"coverage",QJsonObject{{"eligible_matches",6},{"selected_matches",6}}},{"five_axes",axes},{"matches",matches},{"recent_results_score",recentFixture},{"matchmaking_estimate",poolFixture}}}};state="ready";showProfile();render();
    QJsonObject checks{{"player_profile_uses_main_surface",profileVisible()},{"unknown_deaths_not_zero",matches[0].toObject().value("scores").toObject().value("deaths").isNull()},{"query_not_sent_to_ds",!report().value("sent_to_ai").toBool()},{"player_engine_shared_store",report().value("shared_player_library").toBool()},{"match_archive_uses_icons",results->report().value("icons").toBool()},{"match_archive_has_six_clickable_rows",results->report().value("rows").toInt()==6}};
    {
        HistoryPane offlineArchive(catalog);
        offlineArchive.closeProfile();
        offlineArchive.openArchive();
        const auto emptyReason=offlineArchive.findChild<QLabel*>(QStringLiteral("playerArchiveEmptyReason"));
        checks["archive_offline_entry_leaves_local_search_ready"]=offlineArchive.profileVisible()&&
            offlineArchive.state==QStringLiteral("idle")&&offlineArchive.queryGeneration==0&&
            !offlineArchive.busy()&&offlineArchive.name->isEnabled()&&emptyReason&&
            emptyReason->text().contains(QStringLiteral("本机已保存的战绩"));
    }
    bool regionNamesReadable=true,regionLabelsFit=true;QStringList regionCodes;const int regionBeforeChecks=region->currentIndex();
    for(int i=1;i<region->count();++i){region->setCurrentIndex(i);regionCodes.append(region->itemData(i).toString());regionNamesReadable&=!region->itemText(i).isEmpty()&&region->itemText(i)!=region->itemData(i).toString();regionLabelsFit&=static_cast<Kda::ComboBox*>(region)->currentLabelFits();}region->setCurrentIndex(regionBeforeChecks);regionCodes.sort();
    checks["region_names_are_readable_and_keep_supported_query_codes"]=regionNamesReadable&&regionCodes==sgpServers().keys()&&GameRegion::name(QStringLiteral("TJ100"))==QStringLiteral("联盟四区")&&GameRegion::name(QStringLiteral("TENCENT_NJ100"))==QStringLiteral("联盟一区");
    checks["all_region_labels_fit_native_combo_text_area"]=regionLabelsFit;
    QApplication::processEvents();checks["history_statistics_are_inline_by_default"]=scoreRow->isVisible()&&results->report().value("five_dimension_visual").toObject().value("visible").toBool()&&results->report().value("visible_complete_rows").toInt()>=3;
    auto splitList=results->findChild<QListWidget*>(QStringLiteral("matchArchiveList"));
    const auto splitListOrigin=splitList->mapTo(results,QPoint());const auto splitListSize=splitList->size();
    if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-player-collapsed.png");
    QApplication::processEvents();
    checks["sidebar_statistics_leave_match_list_in_place"]=splitList->mapTo(results,QPoint())==splitListOrigin&&splitList->size()==splitListSize&&results->report().value("layout")==QStringLiteral("player_left_matches_right");
    checks["history_unknown_pool_estimate_is_hidden"]=poolScore->isHidden();checks["profile_header_values_fit_default"]=results->report().value("profile_header_labels_fit").toBool();checks["synthetic_recent_score_value_visible"]=recentScore->text().contains(QStringLiteral("62.5/100"))&&recentScore->text().contains(QStringLiteral("6局"))&&recentScore->toolTip().contains(QStringLiteral("近期战果分"));checks["pool_unknown_reason_not_misreported_as_low_score"]=poolScore->text().contains(QStringLiteral("匹配池 —"))&&poolScore->toolTip().contains(QStringLiteral("参考网络超出计算范围"))&&!poolScore->property("scoreKnown").toBool();const auto defaultScores=report().value("score_presentation").toObject();checks["score_labels_fit_inline"]=defaultScores.value("visible").toBool()&&defaultScores.value("fit_evaluated").toBool()&&defaultScores.value("labels_fit").toBool();checks["history_toolbar_and_metadata_are_compact"]=report().value("toolbar_height").toInt()==28&&metadataRow->height()==26;QElapsedTimer artWait;artWait.start();while(artWait.elapsed()<600&&results->report().value("artwork").toObject().value("pending").toInt()>0){QApplication::processEvents();QThread::msleep(2);}QApplication::processEvents();if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-player.png");
    checks["inline_statistics_keep_three_match_rows"]=results->report().value("visible_complete_rows").toInt()>=2;
    const auto fiveDimensionVisual=results->report().value("five_dimension_visual").toObject();checks["radar_uses_five_recorded_same_match_axes"]=fiveDimensionVisual.value("known_radar_axes").toInt()==5&&fiveDimensionVisual.value("filled_polygon").toBool()&&fiveDimensionVisual.value("radar_scope")==QStringLiteral("mean_same_match_ten_player_percentile");checks["radar_does_not_claim_global_rank"]=!fiveDimensionVisual.value("global_percentile_claimed").toBool()&&!fiveDimensionVisual.value("roles_adjusted").toBool();checks["recent_results_strip_keeps_match_order"]=fiveDimensionVisual.value("trend_matches").toInt()==6&&fiveDimensionVisual.value("trend_order")==QStringLiteral("oldest_to_newest");
    checks["radar_and_metrics_fit_inline_without_expansion"]=results->report().value("profile_header_height").toInt()==330&&results->report().value("rows").toInt()==6&&!results->findChild<QPushButton*>(QStringLiteral("togglePlayerFiveDimensions"))&&results->report().value("five_dimension_visual").toObject().value("inline").toBool();if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-player-five-dimensions.png");
    checks["profile_icon_metrics_keep_explanations"]=results->report().value("axis_tooltip_accessibility").toBool();
    results->openMatch(0);QApplication::processEvents();checks["match_detail_hides_search_and_profile_metadata"]=!report().value("search_visible").toBool()&&report().value("detail_metadata_hidden").toBool();checks["match_detail_uses_full_content_width"]=report().value("detail_expanded").toBool()&&width()>=window()->width()-48;
    checks["match_detail_shows_both_teams"]=results->report().value("detail_players").toInt()==10;checks["history_action_labels_fit_default"]=actionLabelsFit();
    const auto overviewReport=results->report();checks["detail_overview_keeps_ten_complete_players"]=overviewReport.value("detail_overview_complete_players").toInt()==10;const auto leaders=overviewReport.value("leader_rows").toArray();
    checks["match_teams_are_two_non_overlapping_columns"]=overviewReport.value("side_by_side_teams").toBool();
    checks["match_overview_focuses_on_kda_and_participation"]=overviewReport.value("overview_columns")==QJsonArray{QStringLiteral("KDA"),QStringLiteral("参团")}&&overviewReport.value("overview_fields_fit").toBool();
    checks["team_kill_and_assist_leaders_are_distinct"]=leaders.size()==10&&leaders[1].toObject().value("kills").toBool()&&leaders[2].toObject().value("assists").toBool()&&!leaders[1].toObject().value("assists").toBool();
    checks["tied_death_leaders_are_both_highlighted"]=leaders.size()==10&&leaders[7].toObject().value("deaths").toBool()&&leaders[9].toObject().value("deaths").toBool()&&!leaders[6].toObject().value("deaths").toBool();
    checks["damage_and_taken_leaders_use_independent_columns"]=leaders.size()==10&&leaders[1].toObject().value("damage_to_champions").toBool()&&leaders[4].toObject().value("damage_taken").toBool()&&!leaders[0].toObject().value("damage_to_champions").toBool();
    bool noPartialDeathLeader=true;for(int i=0;i<5&&i<leaders.size();++i)noPartialDeathLeader&=!leaders[i].toObject().value("deaths").toBool();
    checks["missing_team_scores_do_not_invent_a_leader"]=noPartialDeathLeader;
    if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-match-detail.png");
    const auto detailWindowSize=window()->size();window()->resize(1280,820);QApplication::processEvents();
    checks["wide_match_detail_expands_beyond_profile_limit"]=width()>=window()->width()-48&&width()>1000&&results->report().value("overview_fields_fit").toBool();
    if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-match-detail-wide.png");window()->resize(detailWindowSize);QApplication::processEvents();
    QJsonObject clickedPlayer;const auto playerCallback=results->onPlayerRequested;results->onPlayerRequested=[&clickedPlayer](const QJsonObject &value){clickedPlayer=value;};auto firstPlayer=results->findChild<QPushButton*>(QStringLiteral("matchPlayerHistory_0"));if(firstPlayer)firstPlayer->click();results->onPlayerRequested=playerCallback;
    checks["player_row_click_dispatches_stable_identity"]=firstPlayer&&clickedPlayer.value("available").toBool()&&clickedPlayer.value("player_ref").toString().size()==24&&clickedPlayer.value("server")==QStringLiteral("TJ100");checks["player_row_keyboard_accessible"]=firstPlayer&&firstPlayer->focusPolicy()==Qt::StrongFocus;checks["missing_player_identity_has_no_navigation_button"]=results->findChild<QPushButton*>(QStringLiteral("matchPlayerHistory_9"))==nullptr;
    bool stableDetailWidths=overviewReport.value("detail_tabs_equal_width").toBool();
    for(int mode=1;mode<=5;++mode){
        auto button=results->findChild<QPushButton*>(QStringLiteral("matchMetricMode_%1").arg(mode));if(button)button->click();QApplication::processEvents();const auto page=results->report();
        checks[QStringLiteral("match_metric_group_%1_switches_and_fits").arg(mode)]=button&&page.value("metric_mode").toInt()==mode&&page.value("detail_columns_fit").toBool();
        stableDetailWidths&=page.value("detail_tabs_equal_width").toBool()&&page.value("detail_gutter_free").toBool()&&page.value("detail_viewport_width")==overviewReport.value("detail_viewport_width")&&page.value("team_column_width")==overviewReport.value("team_column_width");
        const auto fields=page.value("displayed_fields").toArray();
        if(mode==1)checks["battle_breakdown_preserves_damage_and_taken_totals"]=fields.contains("physical_damage_to_champions")&&fields.contains("magic_damage_to_champions")&&fields.contains("true_damage_to_champions")&&fields.contains("damage_mitigated")&&fields.contains("damage_to_champions")&&fields.contains("damage_taken");
        if(mode==3)checks["support_has_three_distinct_heal_and_shield_metrics"]=fields==QJsonArray{"heal","heal_teammates","shield"};
        if(mode==4)checks["hextech_page_excludes_equipment"]=fields==QJsonArray{"augments"};
        if(mode==5)checks["equipment_page_excludes_hextech_and_fits_inventory"]=fields==QJsonArray{"items","gold_earned"}&&page.value("equipment_cells_fit").toBool()&&page.value("hextech_items_presented").toInt()==0;
        if(!outputDir.isEmpty())window()->grab().save(outputDir+QStringLiteral("/dashboard-match-metrics-%1.png").arg(mode));
    }
    checks["all_six_detail_pages_keep_equal_tabs_and_team_widths"]=stableDetailWidths;
    if(auto hextech=results->findChild<QPushButton*>(QStringLiteral("matchMetricMode_4")))hextech->click();QApplication::processEvents();
    const auto hextechReport=results->report();
    checks["match_detail_has_independent_hextech_group"]=hextechReport.value("metric_groups").toArray().contains(QStringLiteral("海克斯"))&&hextechReport.value("metric_mode").toInt()==4;
    checks["hextech_mode_presents_all_eight_slots_and_other_players"]=hextechReport.value("hextech_items_recorded").toInt()==38&&hextechReport.value("hextech_items_presented").toInt()==38&&hextechReport.value("hextech_cells_fit").toBool();
    const auto eighthAugment=results->findChild<QWidget*>(QStringLiteral("matchAugment_0_7")),unknownAugment=results->findChild<QWidget*>(QStringLiteral("matchAugment_8_1"));
    checks["hextech_names_and_full_explanations_are_accessible"]=eighthAugment&&unknownAugment&&!eighthAugment->toolTip().isEmpty()&&!eighthAugment->accessibleName().isEmpty()&&!eighthAugment->accessibleDescription().isEmpty()&&eighthAugment->focusPolicy()==Qt::StrongFocus&&unknownAugment->toolTip().contains(QStringLiteral("999999999"))&&hextechReport.value("hextech_tooltips_complete").toBool();
    const auto secondAugment=results->findChild<QWidget*>(QStringLiteral("matchAugment_0_1"));
    checks["hextech_tooltips_do_not_expose_internal_markers"]=secondAugment&&!secondAugment->toolTip().contains(QStringLiteral("%i:"))&&!secondAugment->toolTip().contains(QChar('@'))&&secondAugment->toolTip().contains(QStringLiteral("资料版本"));
    checks["hextech_missing_record_has_no_fabricated_slots"]=hextechReport.value("hextech_empty_rows").toInt()==1&&!results->findChild<QWidget*>(QStringLiteral("matchAugment_9_0"));
    checks["hextech_controls_fit_default"]=hextechReport.value("detail_controls_one_row").toBool()&&actionLabelsFit();
    checks["detail_uses_shared_kda_finish"]=hextechReport.value("detail_style")==(Kda::skin()==QStringLiteral("kda_red")?QStringLiteral("kda_firestarter_black_chrome"):QStringLiteral("kda_all_out_silver_cut"));
    checks["history_result_colors_remain_distinct"]=hextechReport.value("victory_color")!=hextechReport.value("defeat_color");
    const auto teamColors=hextechReport.value("team_colors").toArray();checks["history_team_colors_remain_distinct"]=teamColors.size()==2&&teamColors[0]!=teamColors[1];
    if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-match-hextech.png");
    results->openMatch(5);QApplication::processEvents();const auto historicalAugment=results->findChild<QWidget*>(QStringLiteral("matchAugment_0_1"));
    checks["hextech_historical_tooltip_identifies_description_version"]=historicalAugment&&historicalAugment->toolTip().contains(QStringLiteral("资料版本"))&&historicalAugment->toolTip().contains(QStringLiteral("本场版本 0.0"));
    results->openMatch(0);QApplication::processEvents();
    checks["match_metric_zero_and_unknown_are_separate"]=results->report().value("metric_zero_preserved").toBool()&&results->report().value("metric_unknown_is_dash").toBool();
    const auto returnGeometry=[&]{
        const auto archive=results->report();const auto detail=results->findChild<QScrollArea*>(QStringLiteral("matchDetailScrollArea"));
        return QJsonObject{{"navigation",results->navigationState()},{"detail_page_capacity",archive.value("detail_page_capacity")},{"detail_pages",archive.value("detail_pages")},{"detail_scroll_max",archive.value("detail_scroll_max")},{"detail_viewport_height",detail?detail->viewport()->height():0},{"detail_canvas_height",detail&&detail->widget()?detail->widget()->height():0},{"result_height",results->height()},{"pane_height",height()}};
    };
    auto scrollView=results->navigationState();scrollView["detail_page"]=1;results->restoreNavigationState(scrollView);QApplication::processEvents();const auto returnView=results->navigationState(),returnBefore=returnGeometry();
    navigation.append({data,returnView,riotId,server,playerRef,name->text(),state,region->currentIndex()});auto other=data;auto otherProfile=other.value("profile").toObject();otherProfile["matches"]=QJsonArray{};other["profile"]=otherProfile;data=other;render();previousPlayer->show();previousPlayer->click();const auto returnImmediate=returnGeometry();QApplication::processEvents();
    checks["previous_player_return_restores_original_match"]=results->report().value("rows").toInt()==6&&results->report().value("details_open").toBool()&&results->navigationState().value("match_ref")==returnView.value("match_ref")&&results->report().value("metric_mode").toInt()==4;
    checks["previous_player_return_restores_detail_page"]=results->navigationState().value("detail_page")==returnView.value("detail_page")&&results->report().value("detail_scroll_max").toInt()==0;
    setProperty("historyReturnNavigationDiagnostics",QVariant::fromValue(QJsonObject{{"before",returnBefore},{"immediate_return",returnImmediate},{"after_events",returnGeometry()},{"page_matches",results->navigationState().value("detail_page")==returnView.value("detail_page")},{"vertical_overflow_zero",results->report().value("detail_scroll_max").toInt()==0}}));
    if(auto overview=results->findChild<QPushButton*>(QStringLiteral("matchMetricMode_0")))overview->click();
    const auto originalSize=window()->size();window()->resize(820,600);QApplication::processEvents();checks["history_action_labels_fit_narrow"]=actionLabelsFit();checks["narrow_detail_eight_augments_columns_fit"]=results->report().value("detail_columns_fit").toBool();if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-match-detail-narrow.png");
    window()->resize(760,500);QApplication::processEvents();checks["history_action_labels_fit_minimum"]=actionLabelsFit();checks["minimum_detail_eight_augments_and_numbers_fit"]=window()->width()==760&&results->report().value("detail_columns_fit").toBool();if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-match-detail-minimum.png");
    const auto minimumOverview=results->report();checks["minimum_detail_overview_keeps_ten_complete_players"]=minimumOverview.value("detail_overview_complete_players").toInt()==10;bool stableMinimumWidths=minimumOverview.value("detail_tabs_equal_width").toBool();
    checks["minimum_clickable_names_stay_in_rows"]=minimumOverview.value("player_navigation_fits").toBool();for(int mode=1;mode<=5;++mode){if(auto button=results->findChild<QPushButton*>(QStringLiteral("matchMetricMode_%1").arg(mode)))button->click();QApplication::processEvents();const auto page=results->report();checks[QStringLiteral("minimum_match_metrics_%1_fit").arg(mode)]=page.value("detail_columns_fit").toBool()&&page.value("player_navigation_fits").toBool();stableMinimumWidths&=page.value("detail_tabs_equal_width").toBool()&&page.value("detail_controls_one_row").toBool()&&page.value("detail_viewport_width")==minimumOverview.value("detail_viewport_width")&&page.value("team_column_width")==minimumOverview.value("team_column_width");}
    checks["minimum_all_six_detail_pages_keep_equal_tabs_and_team_widths"]=stableMinimumWidths;
    bool allDetailPagesFit=true;for(int mode=0;mode<6;++mode){if(auto button=results->findChild<QPushButton*>(QStringLiteral("matchMetricMode_%1").arg(mode)))button->click();QApplication::processEvents();auto previous=results->findChild<QPushButton*>(QStringLiteral("detailPreviousPlayers"));int back=0;while(previous&&previous->isEnabled()&&back++<10){previous->click();QApplication::processEvents();}auto next=results->findChild<QPushButton*>(QStringLiteral("detailNextPlayers"));int visited=0,seen=0;do{const auto page=results->report();allDetailPagesFit&=page.value("detail_scroll_max").toInt()==0&&page.value("player_navigation_fits").toBool()&&page.value("hextech_cells_fit").toBool();++visited;seen+=page.value("detail_visible_players").toInt();if(!next||!next->isEnabled())break;next->click();QApplication::processEvents();}while(visited<10);allDetailPagesFit&=visited==results->report().value("detail_pages").toInt()&&seen==10;}checks["all_six_metric_groups_and_player_pages_need_no_vertical_scroll"]=allDetailPagesFit;

    if(auto hextech=results->findChild<QPushButton*>(QStringLiteral("matchMetricMode_4")))hextech->click();QApplication::processEvents();
    checks["minimum_hextech_names_and_controls_fit"]=results->report().value("hextech_cells_fit").toBool()&&results->report().value("detail_controls_one_row").toBool()&&results->report().value("hextech_items_presented").toInt()==38&&actionLabelsFit();
    if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-match-hextech-minimum.png");
    if(auto overview=results->findChild<QPushButton*>(QStringLiteral("matchMetricMode_0")))overview->click();
    results->closeMatch();QApplication::processEvents();
    QApplication::processEvents();
    const auto minimumScores=report().value("score_presentation").toObject();checks["score_labels_fit_inline_minimum"]=minimumScores.value("visible").toBool()&&minimumScores.value("fit_evaluated").toBool()&&minimumScores.value("labels_fit").toBool();

    // QListView defers its item geometry after a viewport resize. Flush that
    // layout before measuring rows inside this synchronous native inspection.
    results->layout()->activate();QApplication::processEvents();
    auto archiveList=results->findChild<QListWidget*>(QStringLiteral("matchArchiveList"));
    if(archiveList)archiveList->doItemsLayout();QApplication::processEvents();
    const int minimumCompleteRows=results->report().value("visible_complete_rows").toInt();bool remainingRowsReachable=false;
    QJsonObject minimumLayoutEvidence{{"complete_rows",minimumCompleteRows}};
    if(archiveList&&archiveList->count()>0){const int savedPage=results->report().value("archive_page").toInt();auto next=results->findChild<QPushButton*>(QStringLiteral("archiveNextPage"));int steps=0;while(next&&next->isEnabled()&&steps++<100){next->click();QApplication::processEvents();}auto last=archiveList->item(archiveList->count()-1);const auto lastRect=archiveList->visualItemRect(last);
        remainingRowsReachable=archiveList->isVisible()&&!last->isHidden()&&!lastRect.isEmpty()&&archiveList->viewport()->rect().contains(lastRect)&&archiveList->verticalScrollBar()->maximum()==0;
        minimumLayoutEvidence["last_row_reachable_by_paging"]=remainingRowsReachable;minimumLayoutEvidence["scroll_maximum"]=archiveList->verticalScrollBar()->maximum();minimumLayoutEvidence["pages"]=results->report().value("archive_pages");results->setArchivePage(savedPage);QApplication::processEvents();
    }
    if(!outputDir.isEmpty()){QFile evidence(outputDir+QStringLiteral("/minimum-history-layout.json"));if(evidence.open(QIODevice::WriteOnly))evidence.write(QJsonDocument(minimumLayoutEvidence).toJson());}
    checks["compact_history_keeps_three_complete_rows_and_all_pages_reachable_without_scroll"]=minimumCompleteRows>=3&&remainingRowsReachable;
    if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-player-minimum.png");
    results->openMatch(0);window()->resize(820,600);QApplication::processEvents();
    const auto returnButton=results->findChild<QPushButton*>(QStringLiteral("backToMatchArchive"));
    checks["history_has_no_duplicate_home_return"]=findChild<QPushButton*>(QStringLiteral("backFromHistory"))==nullptr;
    checks["detail_has_one_archive_return"]=results->findChildren<QPushButton*>(QStringLiteral("backToMatchArchive")).size()==1&&returnButton&&returnButton->isVisibleTo(window());
    checks["detail_return_geometry_stays_inside_results"]=returnButton&&results->rect().contains(QRect(returnButton->mapTo(results,QPoint()),returnButton->size()));
    if(returnButton)returnButton->click();QApplication::processEvents();
    checks["detail_return_restores_search"]=report().value("search_visible").toBool()&&maximumWidth()==1100;
    checks["detail_return_restores_archive"]=!results->report().value("details_open").toBool()&&results->report().value("rows").toInt()==6&&profileVisible();
    checks["narrow_player_search_fits"]=name->width()>150&&rect().contains(name->geometry())&&search->isVisibleTo(window())&&personalButton->isVisibleTo(window());checks["player_history_text_wraps"]=results->width()<=width();checks["profile_header_values_fit_narrow"]=results->report().value("profile_header_labels_fit").toBool();
    if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-player-narrow.png");
    window()->resize(originalSize);QApplication::processEvents();
    auto partial=data;auto partialProfile=partial.value("profile").toObject();auto partialAxes=partialProfile.value("five_axes").toArray();auto missingAxis=partialAxes[0].toObject();missingAxis["value"]=QJsonValue();missingAxis["n"]=0;partialAxes[0]=missingAxis;partialProfile["five_axes"]=partialAxes;partial["profile"]=partialProfile;data=partial;state="ready";render();QApplication::processEvents();
    checks["missing_profile_field_has_reason_not_loading"]=results->report().value("unrecorded_axes").toInt()==1&&results->report().value("recorded_axes").toInt()==4&&status->toolTip().contains(QStringLiteral("输出未记录"))&&results->report().value("missing_axes_are_not_loading").toBool();
    checks["history_metadata_is_quiet_unframed_context_with_full_source_explanation"]=dynamic_cast<Kda::GlassSurface*>(metadataRow)==nullptr&&status->text().contains(QStringLiteral("海克斯"))&&!status->text().contains(QStringLiteral("较旧缓存"))&&!status->toolTip().isEmpty();
    const auto partialVisual=results->report().value("five_dimension_visual").toObject();checks["missing_radar_axis_is_not_zero_or_filled"]=partialVisual.value("known_radar_axes").toInt()==4&&partialVisual.value("radar_values").toArray()[0].isNull()&&!partialVisual.value("filled_polygon").toBool()&&partialVisual.value("unknown_axes_unconnected").toBool();
    auto zeroProfile=partialProfile;auto zeroAxes=partialAxes;auto zeroAxis=zeroAxes[0].toObject();zeroAxis["value"]=0;zeroAxis["n"]=6;zeroAxis["percentile"]=0;zeroAxis["percentile_n"]=5;zeroAxes[0]=zeroAxis;zeroProfile["five_axes"]=zeroAxes;results->setProfile(zeroProfile);const auto zeroVisual=results->report().value("five_dimension_visual").toObject();checks["recorded_zero_radar_axis_remains_recorded"]=zeroVisual.value("known_radar_axes").toInt()==5&&zeroVisual.value("radar_values").toArray()[0].isDouble()&&zeroVisual.value("radar_values").toArray()[0].toDouble()==0&&zeroVisual.value("filled_polygon").toBool();
    for(int i=0;i<zeroAxes.size();++i){auto noRank=zeroAxes[i].toObject();noRank["percentile"]=QJsonValue();noRank["percentile_n"]=0;zeroAxes[i]=noRank;}zeroProfile["five_axes"]=zeroAxes;results->setProfile(zeroProfile);const auto noRanks=results->report();checks["raw_metrics_do_not_invent_radar_positions"]=noRanks.value("recorded_axes").toInt()==5&&noRanks.value("five_dimension_visual").toObject().value("known_radar_axes").toInt()==0;results->setProfile(partialProfile);
    // Stress the painted fields with full values, rather than accepting a
    // synthetic fit check whose short fixture never reaches the field edges.
    auto stressPlayers=players;auto stressPlayer=stressPlayers[0].toObject();auto stressScores=stressPlayer.value("scores").toObject();
    stressScores["kills"]=9999;stressScores["deaths"]=8888;stressScores["assists"]=7777;stressPlayer["scores"]=stressScores;stressPlayers[0]=stressPlayer;
    auto stressMatch=matches[0].toObject();stressMatch["players"]=stressPlayers;auto stressProfile=partialProfile;stressProfile["matches"]=QJsonArray{stressMatch};stressProfile["five_axes"]=axes;

    window()->resize(760,500);results->setProfile(stressProfile);results->openMatch(0);QApplication::processEvents();const auto fittedKda=results->report();
    checks["long_kda_uses_readable_font_and_full_values"]=fittedKda.value("kda_minimum_pixels").toInt()>=12&&fittedKda.value("kda_minimum_pixels").toInt()<18&&fittedKda.value("kda_full_values_fit").toBool()&&fittedKda.value("detail_header_fields_fit").toBool();
    checks["kda_actual_paint_is_clipped_to_its_column"]=fittedKda.value("kda_paint_clip_verified").toBool();
    stressScores["kills"]=12345678;stressScores["deaths"]=87654321;stressScores["assists"]=23456789;stressScores["physical_damage_to_champions"]=9876543210123.;stressScores["magic_damage_to_champions"]=8765432101234.;stressScores["true_damage_to_champions"]=7654321012345.;stressPlayer["scores"]=stressScores;stressPlayers[0]=stressPlayer;stressMatch["players"]=stressPlayers;stressProfile["matches"]=QJsonArray{stressMatch};
    auto stressAxes=axes;auto stressAxis=stressAxes[0].toObject();stressAxis["value"]=1234567890123.;stressAxes[0]=stressAxis;stressProfile["five_axes"]=stressAxes;results->setProfile(stressProfile);results->closeMatch();
    QApplication::processEvents();const auto compactAxis=results->findChild<QLabel*>(QStringLiteral("playerProfileAxis_0"));
    checks["inline_long_axis_keeps_unit_and_full_tooltip"]=compactAxis&&compactAxis->text().endsWith(QStringLiteral("/分"))&&compactAxis->toolTip().startsWith(QStringLiteral("1234567890123.0/分"))&&results->report().value("profile_header_labels_fit").toBool();
    setProperty("compactAxisDiagnostics",QVariant::fromValue(QJsonObject{{"text",compactAxis?compactAxis->text():QString()},{"tooltip",compactAxis?compactAxis->toolTip():QString()},{"labels_fit",results->report().value("profile_header_labels_fit")}}));
    results->openMatch(0);QApplication::processEvents();bool stressPagesFit=true;int stressCanvasWidth=-1;
    for(int mode=0;mode<6;++mode){if(auto button=results->findChild<QPushButton*>(QStringLiteral("matchMetricMode_%1").arg(mode)))button->click();QApplication::processEvents();const auto page=results->report();
        stressPagesFit&=page.value("detail_columns_fit").toBool()&&page.value("detail_header_fields_fit").toBool()&&page.value("player_navigation_fits").toBool()&&page.value("kda_full_values_fit").toBool()&&page.value("kda_minimum_pixels").toInt()>=12;
        if(stressCanvasWidth<0)stressCanvasWidth=page.value("detail_canvas_width").toInt();else stressPagesFit&=stressCanvasWidth==page.value("detail_canvas_width").toInt();
    }
    checks["six_detail_pages_keep_long_values_in_real_bounds_at_760"]=stressPagesFit&&window()->width()==760;
    const auto stressReport=results->report();checks["long_metric_canvas_scrolls_instead_of_compressing_columns"]=stressReport.value("detail_horizontal_scroll_enabled").toBool()&&stressReport.value("detail_horizontal_scroll_max").toInt()>0&&stressReport.value("detail_canvas_width").toInt()>stressReport.value("detail_viewport_width").toInt();
    setProperty("stressCanvasDiagnostics",QVariant::fromValue(stressReport));
    auto horizontalState=results->navigationState();horizontalState["detail_scroll_x"]=stressReport.value("detail_horizontal_scroll_max");results->restoreNavigationState(horizontalState);QApplication::processEvents();checks["long_detail_horizontal_position_is_restorable"]=results->navigationState().value("detail_scroll_x")==horizontalState.value("detail_scroll_x");
    const auto stressName=results->findChild<QPushButton*>(QStringLiteral("matchPlayerHistory_0"));checks["long_player_name_keeps_full_hover_text_and_own_column"]=stressName&&stressName->toolTip().startsWith(stressPlayer.value("display_name").toString())&&stressReport.value("detail_header_fields_fit").toBool();
    if(!outputDir.isEmpty())window()->grab().save(outputDir+"/dashboard-match-long-values-minimum.png");
    {
        const auto savedRefreshData=data;const auto savedRefreshState=state,savedRefreshPhase=phase;
        auto refreshProfile=stressProfile;auto refreshPlayer=refreshProfile.value("player").toObject();
        refreshPlayer["navigation"]=QJsonObject{{"available",true},{"player_ref",QString(24,QLatin1Char('a'))},{"server","TJ100"}};refreshProfile["player"]=refreshPlayer;
        QJsonArray refreshMatches{stressMatch};for(int i=1;i<matches.size();++i)refreshMatches.append(matches[i]);refreshProfile["matches"]=refreshMatches;
        data=QJsonObject{{"status","ready"},{"identity",QJsonObject{{"riot_id",QStringLiteral("刷新合成#00000")},{"server","TJ100"}}},{"profile",refreshProfile}};state="ready";phase="local";render();results->openMatch(0);
        if(auto mode=results->findChild<QPushButton*>(QStringLiteral("matchMetricMode_1")))mode->click();QApplication::processEvents();
        auto refreshView=results->navigationState();refreshView["detail_page"]=1;refreshView["detail_scroll_x"]=results->report().value("detail_horizontal_scroll_max");results->restoreNavigationState(refreshView);QApplication::processEvents();refreshView=results->navigationState();
        checks["history_refresh_fixture_has_nonzero_detail_page_and_horizontal_offset"]=refreshView.value("detail_page").toInt()>0&&refreshView.value("detail_scroll_x").toInt()>0;
        state="querying";phase="refresh";render();QApplication::processEvents();
        checks["history_refresh_start_keeps_current_match_tab_and_positions"]=results->navigationState()==refreshView;
        auto updatedProfile=refreshProfile;auto updatedPlayer=refreshPlayer;updatedPlayer["display_name"]=QStringLiteral("更新后的显示名#00000");updatedProfile["player"]=updatedPlayer;
        auto newMatch=matches[2].toObject();newMatch["match_ref"]=QStringLiteral("newly-published");QJsonArray reordered{newMatch};for(const auto &match:refreshMatches)reordered.append(match);updatedProfile["matches"]=reordered;data["profile"]=updatedProfile;state="ready";render();QApplication::processEvents();QApplication::processEvents();
        const auto refreshedView=results->navigationState();
        checks["history_refresh_reordered_records_keep_stable_match_tab_page_and_horizontal_offset"]=refreshedView.value("match_ref")==refreshView.value("match_ref")&&refreshedView.value("metric_mode")==refreshView.value("metric_mode")&&refreshedView.value("detail_page")==refreshView.value("detail_page")&&refreshedView.value("detail_scroll_x")==refreshView.value("detail_scroll_x");
        checks["history_refresh_keeps_bounded_vertical_positions"]=refreshedView.value("list_scroll")==refreshView.value("list_scroll")&&refreshedView.value("detail_scroll")==refreshView.value("detail_scroll");
        render();if(auto mode=results->findChild<QPushButton*>(QStringLiteral("matchMetricMode_3")))mode->click();QApplication::processEvents();
        checks["history_refresh_queued_restore_yields_to_explicit_metric_tab"]=results->navigationState().value("metric_mode").toInt()==3;
        render();results->openMatch(2);QApplication::processEvents();
        checks["history_refresh_queued_restore_yields_to_explicit_other_match"]=results->navigationState().value("match_ref")==matches[1].toObject().value("match_ref");
        render();results->closeMatch();QApplication::processEvents();
        checks["history_refresh_queued_restore_yields_to_explicit_return"]=!results->detailsOpen();
        results->setArchivePage(1);QApplication::processEvents();const auto archiveView=results->navigationState();render();QApplication::processEvents();
        checks["history_refresh_keeps_archive_page_without_opening_details"]=!results->detailsOpen()&&results->navigationState().value("archive_page")==archiveView.value("archive_page");
        results->openMatch(1);render();results->openMatch(2);render();QApplication::processEvents();
        checks["history_refresh_newer_render_keeps_latest_explicit_match"]=results->navigationState().value("match_ref")==matches[1].toObject().value("match_ref");
        results->openMatch(1);auto missingProfile=updatedProfile;missingProfile["matches"]=QJsonArray{matches[1]};data["profile"]=missingProfile;render();QApplication::processEvents();
        checks["history_refresh_removed_match_returns_to_archive"]=!results->detailsOpen();
        data["profile"]=updatedProfile;render();results->openMatch(1);auto otherPlayer=updatedPlayer;auto otherNavigation=otherPlayer.value("navigation").toObject();otherNavigation["player_ref"]=QString(24,QLatin1Char('b'));otherPlayer["navigation"]=otherNavigation;auto otherProfile=updatedProfile;otherProfile["player"]=otherPlayer;data["profile"]=otherProfile;render();QApplication::processEvents();
        checks["history_refresh_different_player_cannot_inherit_same_match_ref"]=!results->detailsOpen()&&results->navigationState().value("archive_page").toInt()==0;
        auto unknownPlayer=updatedPlayer;unknownPlayer.remove("navigation");unknownPlayer["display_name"]=QStringLiteral("未确认玩家");auto unknownProfile=updatedProfile;unknownProfile["player"]=unknownPlayer;data=QJsonObject{{"profile",unknownProfile}};render();results->openMatch(1);render();QApplication::processEvents();
        checks["history_refresh_unconfirmed_identity_never_guesses_navigation_scope"]=!results->detailsOpen();
        data=savedRefreshData;state=savedRefreshState;phase=savedRefreshPhase;render();QApplication::processEvents();
    }
    results->closeMatch();window()->resize(originalSize);results->setProfile(partialProfile);results->openMatch(0);if(auto button=results->findChild<QPushButton*>(QStringLiteral("matchMetricMode_0")))button->click();results->closeMatch();QApplication::processEvents();
    data={};state="idle";render();checks["idle_search_hides_empty_numeric_header"]=results->report().value("empty_state_visible").toBool()&&!results->report().value("profile_header_visible").toBool()&&status->text().contains(QStringLiteral("搜索玩家"));
    state="database_unavailable";render();checks["failed_query_has_specific_reason"]=status->text().contains(QStringLiteral("战绩库"))&&!results->report().value("profile_header_visible").toBool();
    state="timeout";render();checks["timed_out_query_does_not_keep_loading"]=status->text().contains(QStringLiteral("超时"))&&results->report().value("empty_state_visible").toBool();
    data=partial;state="ready";render();checks["query_recovers_after_error"]=results->report().value("rows").toInt()==6&&!results->report().value("empty_state_visible").toBool()&&results->report().value("profile_header_visible").toBool();
    auto emptyProfile=partialProfile;emptyProfile["matches"]=QJsonArray{};results->setProfile(emptyProfile);checks["found_player_without_eligible_games_has_empty_reason"]=results->report().value("empty_state_visible").toBool()&&results->report().value("rows").toInt()==0;
    // Exercise the exact publication and navigation paths with synthetic data.
    // No home process is started and no local client or remote source is read.
    personalAccountHint=QStringLiteral("synthetic account hint");++personalHomeGeneration;
    auto homeProfile=partialProfile;homeProfile["matches"]=QJsonArray{matches[2],matches[0],matches[1],matches[3],matches[4],matches[5]};
    const QJsonObject homeFixture{{"status","ready"},{"identity",QJsonObject{{"riot_id",QStringLiteral("本人合成#00000")},{"server","TJ100"}}},{"profile",homeProfile},{"source","local_library"},{"network_used",false}};
    acceptPersonalHome(homeFixture,personalHomeGeneration);const auto ownSnapshot=personalProfileSnapshot();
    const int regionBeforeDetection=region->currentIndex();region->setCurrentIndex(0);
    checks["current_client_region_is_named_and_scopes_automatic_search"]=selectedServer()==QStringLiteral("TJ100")&&region->currentText().contains(QStringLiteral("联盟四区"))&&static_cast<Kda::ComboBox*>(region)->currentLabelFits();
    region->setCurrentIndex(region->findData(QStringLiteral("HN1")));acceptPersonalHome(homeFixture,personalHomeGeneration);
    checks["manual_region_is_not_overridden_by_client_detection"]=selectedServer()==QStringLiteral("HN1")&&region->currentText()==QStringLiteral("艾欧尼亚");
    auto homeCacheMiss=homeFixture;homeCacheMiss["status"]="not_found";homeCacheMiss.remove("profile");region->setCurrentIndex(0);acceptPersonalHome(homeCacheMiss,personalHomeGeneration);
    checks["client_region_is_available_without_cached_matches"]=selectedServer()==QStringLiteral("TJ100")&&personalProfileSnapshot().isEmpty();acceptPersonalHome(homeFixture,personalHomeGeneration);region->setCurrentIndex(regionBeforeDetection);

    auto otherHomeProfile=partialProfile;otherHomeProfile["player"]=QJsonObject{{"display_name",QStringLiteral("其他合成#00001")},{"server","TJ100"}};data=QJsonObject{{"profile",otherHomeProfile}};personalProfile=false;render();
    checks["personal_home_cache_survives_searching_another_player"]=!ownSnapshot.isEmpty()&&personalProfileSnapshot()==ownSnapshot&&data.value("profile").toObject()!=ownSnapshot;
    checks["personal_home_identity_comes_from_self_response"]=personalHomeData.value("identity").toObject().value("riot_id")==homeFixture.value("identity").toObject().value("riot_id")&&personalHomeData.value("identity").toObject().value("riot_id").toString()!=personalAccountHint;
    accountScope.observe(std::string(64,'a'));const auto lateGeneration=personalHomeGeneration;setAccountScope({});acceptPersonalHome(homeFixture,lateGeneration);
    checks["personal_home_account_clear_rejects_late_response"]=personalProfileSnapshot().isEmpty()&&personalHomeState=="login_required"&&detectedServer.isEmpty()&&personalHomeGeneration>lateGeneration&&personalHomeWorker==nullptr;
    personalAccountHint=QStringLiteral("synthetic account hint");++personalHomeGeneration;acceptPersonalHome(homeFixture,personalHomeGeneration);acceptPersonalHome(QJsonObject{{"status","login_required"}},personalHomeGeneration);
    checks["personal_home_failed_revalidation_clears_old_profile"]=personalProfileSnapshot().isEmpty()&&personalHomeState=="login_required";
    const bool savedPaused=paused;paused=false;acceptPersonalHome(QJsonObject{{"status","paused_in_game"}},personalHomeGeneration);paused=savedPaused;
    checks["personal_home_idle_cache_miss_is_not_reported_as_in_game"]=personalProfileSnapshot().isEmpty()&&personalHomeState=="cache_missing";
    acceptPersonalHome(homeFixture,personalHomeGeneration);const auto beforeUnknownData=data,beforeUnknownView=results->navigationState();
    checks["personal_home_unknown_match_does_not_change_navigation"]=!openPersonalMatch(QStringLiteral("missing-synthetic-match"))&&data==beforeUnknownData&&results->navigationState()==beforeUnknownView;
    const bool openedOwnMatch=openPersonalMatch(QStringLiteral("0"));
    checks["personal_home_opens_stable_match_ref_after_reordering"]=openedOwnMatch&&personalProfile&&results->detailsOpen()&&results->navigationState().value("match_ref")==QStringLiteral("0")&&data==personalHomeData;
    const auto originalArchiveAvailable=decisionReplayAvailable;
    const auto originalOpenArchive=onDecisionReplay;
    accountScope.observe(std::string(64,'a'));
    QString openedReplayRef;
    decisionReplayAvailable=[](const QString &ref){return ref==QStringLiteral("0");};
    onDecisionReplay=[&openedReplayRef](const QString &ref){openedReplayRef=ref;};
    auto replayButton=results->findChild<QPushButton*>(QStringLiteral("openArchivedDecisionReplay"));
    refreshDecisionReplay();
    const bool ownReplayVisible=replayButton&&!replayButton->isHidden();
    if(ownReplayVisible)replayButton->click();
    const auto replayMatchView=results->navigationState();
    returnToDecisionMatch(openedReplayRef);
    const bool sameMatchPreserved=results->navigationState()==replayMatchView;
    personalProfile=false;
    refreshDecisionReplay();
    checks["decision_replay_opens_exact_own_match_and_keeps_detail_on_return"]=
        ownReplayVisible&&openedReplayRef==QStringLiteral("0")&&sameMatchPreserved;
    checks["decision_replay_hidden_when_viewing_another_player"]=replayButton&&replayButton->isHidden();
    personalProfile=true;
    decisionReplayAvailable=originalArchiveAvailable;
    onDecisionReplay=originalOpenArchive;
    checks["personal_home_empty_ref_opens_own_archive_without_refresh"]=openPersonalMatch()&&!results->detailsOpen()&&results->report().value("rows").toInt()==matches.size()&&!refreshAfterLookup&&personalHomeWorker==nullptr;
    ++personalHomeGeneration;stopPersonalHome();personalHomeData=savedHomeData;personalAccountHint=savedAccountHint;personalHomeState=homeWasLoading?(savedHomeData.isEmpty()?QStringLiteral("idle"):QStringLiteral("ready")):savedHomeState;accountScope=savedCanonicalScope;historyLifecycle=savedHistoryLifecycle;gamePhase=savedGamePhase;paused=savedGamePaused;emit personalProfileUpdated();
    updateDetectedServer(savedDetectedServer);data=saved;state=oldState;phase=oldPhase;personalProfile=savedPersonal;pendingRefresh=savedPending;refreshAfterLookup=savedRefreshAfter;navigation=savedNavigation;playerRef=savedPlayerRef;riotId=savedRiotId;server=savedServer;name->setText(savedName);region->setCurrentIndex(savedRegion);previousPlayer->setVisible(!navigation.isEmpty());profileBody->setVisible(visible);render();suppressNotices=savedNoticeSuppression;if(onProfileOpened)onProfileOpened();return checks;
}
