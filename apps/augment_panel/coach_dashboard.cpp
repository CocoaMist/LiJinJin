#include "coach_dashboard.h"
#include "dashboard_tutorial.h"
#include "champ_select_view.h"
#include "coach_hud_details.h"
#include "coach_replay.h"
#include "app_version.h"
#include "startup_notice.h"
#include "version_about_view.h"
#include "settings_page_motion.h"
#include "history_pane.h"
#include "personal_home_view.h"
#include "kda_identity_plate.h"
#include "waiting_player_view.h"
#include "kda_home_material.h"
#include "kda_appearance_save.h"
#include "kda_glass_button.h"
#include "kda_glass_surface.h"
#include "kda_reading_surface.h"
#include "catalog.h"
#include "dashboard_match_view.h"
#include "roster_recent_results_view.h"
#include "growth_capture_settings.h"
#include "dashboard_chrome.h"
#include "dashboard_window_frame.h"
#include "kda_theme.h"
#include "kda_notice.h"
#include "kda_hud_style.h"
#include "kda_controls.h"
#include "kda_surface.h"
#include "kda_skin_transition.h"
#include "kda_depth_plane.h"
#include "kda_sparkles.h"
#include "preference_switch.h"
#include "hud_assets.h"
#include "settings.h"
#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDateTime>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsEffect>
#include <QGridLayout>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QImage>
#include <QImageReader>
#include <QBuffer>
#include <QIcon>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QLocale>
#include <QLibrary>
#include <QLineF>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QHideEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPointer>
#include <QPolygonF>
#include <QScreen>
#include <QSet>
#include <QShowEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionButton>
#include <QSystemTrayIcon>
#include <QTextDocument>
#include <QThread>
#include <QTabBar>
#include <QDynamicPropertyChangeEvent>
#include <QRadialGradient>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <utility>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
class InactiveDashboardEffect final : public QGraphicsEffect {
public:
    static constexpr qreal inactiveAmount = .8;
    static constexpr int durationMs = 240;

    explicit InactiveDashboardEffect(QWidget *window) : dashboard(window) {
        setEnabled(false);
        animation.setDuration(durationMs);
        animation.setEasingCurve(QEasingCurve::InOutCubic);
        connect(&animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            setAmount(value.toReal());
        });
    }

    void setInactive(bool inactive, bool animate) {
        const qreal next = inactive ? inactiveAmount : 0;
        if (animate && next == target) return;
        animation.stop();
        target = next;
        if (!animate || qFuzzyCompare(amount + 1, target + 1)) {
            setAmount(target);
            return;
        }
        {
            const QSignalBlocker quiet(&animation);
            animation.setStartValue(amount);
            animation.setEndValue(target);
            animation.setCurrentTime(0);
        }
        animation.start();
    }

    QJsonObject report() const {
        return {{"desaturation", amount}, {"target", target}, {"duration_ms", durationMs},
                {"animating", animation.state() == QAbstractAnimation::Running}};
    }

protected:
    void draw(QPainter *painter) override {
        QPoint offset;
        auto image = sourcePixmap(Qt::LogicalCoordinates, &offset, NoPad).toImage()
                         .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (image.isNull()) return;

        QImage preserved(image.size(), QImage::Format_ARGB32_Premultiplied);
        preserved.setDevicePixelRatio(image.devicePixelRatio());
        preserved.fill(Qt::transparent);
        {
            QPainter mask(&preserved);
            // Use the artwork and text coverage so transparent areas still fade.
            for (const auto *name : {"wordmark", "startupNoticeLogo", "startupNoticeBrand",
                                     "aboutTitle", "aboutSoraka", "aboutCodex", "aboutAuthor"}) {
                auto *widget = dashboard->findChild<QWidget *>(QString::fromLatin1(name));
                if (!widget || !widget->isVisibleTo(dashboard)) continue;
                const QPoint origin = widget->mapTo(dashboard, QPoint()) - offset;
                mask.setClipRegion(widget->visibleRegion().translated(origin));
                widget->render(&mask, origin, QRegion(), QWidget::DrawChildren);
            }
        }

        const auto luminance = [](QRgb pixel) {
            return (54 * qRed(pixel) + 183 * qGreen(pixel) + 19 * qBlue(pixel) + 128) / 256;
        };
        const int muted = qRound(amount * 256);
        const int original = 256 - muted;
        // Fade the composed image, then restore the exempt foreground's chroma.
        // Its premultiplied channels also cover antialiased edges.
        for (int y = 0; y < image.height(); ++y) {
            auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            const auto *coverage = reinterpret_cast<const QRgb *>(preserved.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const auto retained = coverage[x];
                if (qAlpha(retained) == 255) continue;
                const auto pixel = row[x];
                const int alpha = qAlpha(pixel);
                const int gray = luminance(pixel) - luminance(retained);
                const auto mute = [=](int channel, int retainedChannel) {
                    return std::clamp((original * channel + muted * (gray + retainedChannel) + 128) / 256, 0, alpha);
                };
                row[x] = qRgba(mute(qRed(pixel), qRed(retained)), mute(qGreen(pixel), qGreen(retained)),
                               mute(qBlue(pixel), qBlue(retained)), alpha);
            }
        }

        painter->save();
        painter->setCompositionMode(QPainter::CompositionMode_Source);
        painter->drawImage(offset, image);
        painter->restore();
    }

private:
    void setAmount(qreal value) {
        amount = value;
        setEnabled(amount > 0);
        if (isEnabled()) update();
    }

    QWidget *const dashboard;
    QVariantAnimation animation;
    qreal amount = 0, target = 0;
};

constexpr int settingsReadingWidth=600;
constexpr int settingsTwoColumnWidth=740;
constexpr int usageTwoColumnWidth=540;
constexpr int settingsGeneralTab=0,settingsModelsTab=1,settingsSkinsTab=2,settingsAboutTab=3;
const auto closeToTrayKey=QStringLiteral("coach/closeToTray");
constexpr auto firstTutorialKey="onboarding/quickStartShown";
constexpr int settingsAcknowledgmentsTab=4;
bool usageCount(const QJsonValue &value,qint64 &out){
    if(!value.isDouble())return false;const auto number=value.toDouble();
    if(!std::isfinite(number)||number<0||number>1e15||std::floor(number)!=number)return false;
    out=static_cast<qint64>(number);return true;
}
qint64 usageCount(const QJsonObject &object,const char *field){
    qint64 result=0;usageCount(object.value(QString::fromLatin1(field)),result);return result;
}
QString usageNumber(qint64 value){return QLocale(QLocale::English,QLocale::UnitedStates).toString(value);}
QString usageMoney(const QJsonValue &low,const QJsonValue &high){
    if(!low.isDouble()||!high.isDouble())return {};
    const auto a=low.toDouble(),b=high.toDouble();if(!std::isfinite(a)||!std::isfinite(b)||a<0||b<a)return {};
    const auto display=[](double value){
        auto text=QString::number(value,'f',value>=.01?4:6);
        if(value>0&&text.toDouble()==0)text=QString::number(value,'g',4);
        return text;
    };
    auto left=display(a),right=display(b);
    // Equal-looking rounded bounds must not erase an actual price range.
    if(a!=b&&left==right){left=QString::number(a,'g',6);right=QString::number(b,'g',6);}
    return a==b?QStringLiteral("≈ ¥%1").arg(left):QStringLiteral("≈ ¥%1–%2").arg(left,right);
}
QJsonObject usagePresentation(const QJsonObject &usage){
    const auto current=usage.value("current_match").toObject(),past=usage.value("completed_matches").toObject();
    const auto calls=usageCount(current,"call_count"),pending=usageCount(current,"pending_call_count"),known=usageCount(current,"usage_known_call_count"),unknown=usageCount(current,"unknown_cost_call_count");
    QString cost=QStringLiteral("本局已记录 · 尚无记录"),currentAmount=QStringLiteral("尚无记录"),currentStatus=QStringLiteral("等待调用"),tokens=QStringLiteral("输入 / 输出 tokens · 尚无记录"),requests=QStringLiteral("请求 · 尚无记录"),currentState=QStringLiteral("no_calls");
    if(calls>0){
        const auto amount=usageMoney(current.value("cost_min_cny"),current.value("cost_max_cny"));
        const bool complete=current.value("cost_complete")==QJsonValue(true)&&pending==0&&unknown==0;
        if(complete&&!amount.isEmpty()){cost=QStringLiteral("本局已记录 · ")+amount;currentAmount=amount;currentStatus=QStringLiteral("已计价");currentState=QStringLiteral("complete");}
        else if(calls>unknown&&!amount.isEmpty()){cost=QStringLiteral("本局已计价部分 · ")+amount;currentAmount=amount;currentStatus=QStringLiteral("部分计价");currentState=QStringLiteral("partial");}
        else {cost=QStringLiteral("本局已记录 · 未完整");currentAmount=QStringLiteral("未完整");currentStatus=QStringLiteral("等待用量");currentState=QStringLiteral("unknown_cost");}
        qint64 input=0,output=0;
        if(known>0&&usageCount(current.value("prompt_tokens"),input)&&usageCount(current.value("completion_tokens"),output)){
            const bool allTokens=known==calls&&pending==0;
            tokens=QStringLiteral("%1输入 %2 / 输出 %3 tokens").arg(allTokens?QString():QStringLiteral("已记录 ")).arg(usageNumber(input),usageNumber(output));
        }else tokens=QStringLiteral("输入 / 输出 tokens · 未完整");
        requests=QStringLiteral("请求 %1 次").arg(usageNumber(calls));
        if(pending>0)requests+=QStringLiteral(" · %1 次进行中").arg(usageNumber(pending));
        const auto unpriced=std::max(qint64(0),unknown-pending);
        if(unpriced>0)requests+=QStringLiteral(" · %1 次未计价").arg(usageNumber(unpriced));
    }
    const auto sampleCount=usageCount(past,"priced_match_count"),excluded=usageCount(past,"excluded_unknown_cost_match_count");
    QString average=QStringLiteral("平均每局 · 尚无记录"),averageAmount=QStringLiteral("尚无记录"),averageState=QStringLiteral("no_samples");
    if(sampleCount>0){
        const auto amount=usageMoney(past.value("mean_cost_min_cny"),past.value("mean_cost_max_cny"));
        average=amount.isEmpty()?QStringLiteral("平均每局 · 未完整"):QStringLiteral("平均每局 · ")+amount;
        averageAmount=amount.isEmpty()?QStringLiteral("未完整"):amount;
        averageState=amount.isEmpty()?QStringLiteral("unknown_cost"):QStringLiteral("complete_samples");
    }
    QString samples=QStringLiteral("完整样本 %1 局").arg(usageNumber(sampleCount));
    if(excluded>0)samples+=QStringLiteral(" · 未计入 %1 局").arg(usageNumber(excluded));
    const bool persistenceFailed=usage.contains("persistence_ok")&&usage.value("persistence_ok")==QJsonValue(false);
    return {{"current",cost},{"current_amount",currentAmount},{"current_status",currentStatus},{"tokens",tokens},{"requests",requests},{"average",average},{"average_amount",averageAmount},{"samples",samples},
        {"current_state",currentState},{"average_state",averageState},{"priced_match_count",sampleCount},{"excluded_unknown_cost_match_count",excluded},
        {"persistence_warning",persistenceFailed?QStringLiteral("记录暂未保存，重启后可能不完整"):QString()},
        {"note",QStringLiteral("按接口用量估算，非账单；本地网页搜索不另计费")}};
}
QString growthPlanSummary(const QJsonObject &plan){
    for(const auto &key:{QStringLiteral("growth_reasoning"),QStringLiteral("growth_objective")}){
        const auto summary=plan.value(key).toObject().value(QStringLiteral("summary")).toString().trimmed();
        if(!summary.isEmpty())return summary;
    }
    return QStringLiteral("英雄与海克斯成长联动");
}
bool growthCandidatePlan(const QJsonObject &plan){
    return plan.value("growth_conditioned_candidates").toBool()
        ||plan.value("recommendation_kind")==QStringLiteral("growth_conditioned_candidates");
}
QStringList growthCandidateNotes(const QJsonObject &plan){
    const auto objective=plan.value("growth_objective").toObject();const auto held=objective.value("held_growth_inputs").toObject();
    bool heartsteel=false,heartsteelGiven=false,otherMissing=false,progressUnknown=false,utilityUnknown=false;
    const auto inspect=[&](const QString &name,const QJsonObject &supplied){const auto value=supplied.value(name);const bool given=value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0;if((name==QStringLiteral("heartsteel_stacks")||name==QStringLiteral("item:3084:permanent_hp"))){heartsteel=true;heartsteelGiven|=given;}else if(!given)otherMissing=true;};
    for(const auto &v:objective.value("equipment_growth_inputs").toArray()){const auto row=v.toObject();for(const auto &input:row.value("required_inputs").toArray())inspect(input.toString(),row.value("supplied_baseline_inputs").toObject());}
    for(const auto &v:objective.value("selected_augments").toArray())for(const auto &input:v.toObject().value("growth_stack_inputs").toArray())inspect(input.toString(),held);
    for(const auto &v:objective.value("unknowns").toArray()){const auto reason=v.toString();if(reason.startsWith(QStringLiteral("growth_input_unobserved:")))inspect(reason.mid(24),held);progressUnknown|=reason.startsWith(QStringLiteral("growth_dependency_does_not_verify_stacks_or_quest_progress:"));utilityUnknown|=reason==QStringLiteral("ally_heal_shield_control_utility_not_fully_quantified");}
    QStringList notes;if(heartsteel)notes.append(heartsteelGiven?QStringLiteral("心钢层数按给定基线计算，未自动采集"):QStringLiteral("心钢层数未自动采集，当前未知"));if(otherMissing)notes.append(QStringLiteral("部分成长层数或触发计数待补充"));if(progressUnknown)notes.append(QStringLiteral("装备前置可核对，层数与任务进度未核实"));if(utilityUnknown)notes.append(QStringLiteral("治疗、护盾与控制价值未完整量化"));notes.append(QStringLiteral("未来层数、命中与成长用时未预测"));return notes;
}
class DashboardButton final : public Kda::Button {
public:
    explicit DashboardButton(const QString &copy):Kda::Button(copy),response(this,180){setProperty("labelPixels",12);setProperty("labelRegular",true);setMinimumHeight(Kda::Density::controlHeight);connect(this,&QPushButton::toggled,this,[this]{respond();});}
    QJsonObject motionReport()const{auto report=response.report();report["control_easing"]=easingReport();return report;}
protected:
    bool event(QEvent *event)override{const auto type=event->type();const bool accepted=Kda::Button::event(event);if(isEnabled()&&(type==QEvent::Enter||type==QEvent::FocusIn||type==QEvent::MouseButtonPress||type==QEvent::KeyPress))respond();return accepted;}
    void paintEvent(QPaintEvent *event)override{Kda::Button::paintEvent(event);if(!response.running())return;QPainter painter(this);Kda::configurePainter(painter);const qreal strength=1-response.progress();auto echo=Kda::skin()==QStringLiteral("kda_red")?Kda::color("silver"):QColor(218,227,239);echo.setAlpha(qRound(strength*100));painter.setPen(QPen(echo,1));const qreal inset=3+strength*2;painter.drawLine(QPointF(inset,height()-inset),QPointF(width()-inset,height()-inset));}
private:
    void respond(){response.finish();if(!property("toolbarAction").toBool()&&!property("dashboardNavigation").toBool()&&objectName()!=QStringLiteral("quiet"))response.trigger();}
    Kda::Transition response;
};
class NavigationRail final : public QWidget {
public:
    explicit NavigationRail(QWidget *bar):QWidget(bar),transition(this,220){setAttribute(Qt::WA_TransparentForMouseEvents);setAttribute(Qt::WA_NoSystemBackground);setFocusPolicy(Qt::NoFocus);bar->installEventFilter(this);for(auto button:bar->findChildren<QPushButton*>(QString(),Qt::FindDirectChildrenOnly))button->installEventFilter(this);show();}
    void follow(QPushButton *button,bool animate){
        selected=button;const QRectF destination(button->geometry());
        from=currentRect();transition.finish();target=destination;
        if(!animate||from.isEmpty())from=target;else transition.trigger();setGeometry(parentWidget()->rect());lower();update();
    }
    QJsonObject report()const{return transition.report();}
protected:
    void paintEvent(QPaintEvent *)override{
        if(target.isEmpty())return;QPainter painter(this);Kda::configurePainter(painter);
        const auto plate=currentRect().adjusted(.5,.5,-.5,-.5);
        for(auto button:parentWidget()->findChildren<QPushButton*>(QString(),Qt::FindDirectChildrenOnly)){
            if(auto face=dynamic_cast<Kda::Button*>(button)){const QRectF bounds(button->geometry());face->setNavigationCoverage(plate.intersected(bounds).width()/std::max(qreal(1),bounds.width()-1));}
        }
        const auto platinum=Kda::color("silver"),ice=Kda::color("ice");
        QLinearGradient silver(plate.topLeft(),plate.bottomRight());
        silver.setColorAt(0,platinum);silver.setColorAt(.55,platinum.darker(106));silver.setColorAt(1,platinum.darker(119));
        painter.setPen(QPen(platinum.darker(112),.8));painter.setBrush(silver);
        // The full selection plate eases to the destination; each label follows
        // the material underneath it so contrast stays readable while it moves.
        painter.drawRoundedRect(plate,Kda::radius("navigation"),Kda::radius("navigation"));
        painter.save();QPainterPath plateClip;plateClip.addRoundedRect(plate,Kda::radius("navigation"),Kda::radius("navigation"));painter.setClipPath(plateClip);
        auto glint=platinum.lighter(125);glint.setAlpha(150);painter.setPen(QPen(glint,.65));painter.drawLine(plate.topLeft()+QPointF(4,1),plate.topRight()+QPointF(-4,1));
        auto lower=ice.darker(160);lower.setAlpha(95);painter.setPen(QPen(lower,.65));painter.drawLine(plate.bottomLeft()+QPointF(4,-1),plate.bottomRight()+QPointF(-4,-1));painter.restore();
        if(transition.running()){
            const auto accent=currentRect().adjusted(8,0,-8,-1.5);
            painter.setPen(QPen(Kda::skin()==QStringLiteral("kda_red")?Kda::color("silver"):QColor("#d5ebf4"),1));
            painter.drawLine(QPointF(accent.left(),accent.bottom()),QPointF(accent.right(),accent.bottom()));
        }
    }
    bool eventFilter(QObject *object,QEvent *event)override{if(selected&&((object==parentWidget()&&event->type()==QEvent::Resize)||(object==selected&&(event->type()==QEvent::Move||event->type()==QEvent::Resize))))follow(selected,false);return QWidget::eventFilter(object,event);}
private:
    QRectF currentRect()const{const auto phase=transition.progress();return QRectF(from.topLeft()+(target.topLeft()-from.topLeft())*phase,from.size()+(target.size()-from.size())*phase);}
    Kda::Transition transition;QPointer<QPushButton> selected;QRectF from,target;
};
class BrandMark final : public QWidget {
public:
    BrandMark():artwork(QStringLiteral(":/lixin/kda/brand/lijinjin-app.png")){
        setObjectName(QStringLiteral("wordmark"));setAccessibleName(QStringLiteral("李金金"));
        setFixedSize(28,32);setProperty("artworkLoaded",!artwork.isNull());
        setProperty("brandStyle",QStringLiteral("User-selected Jinx icon without visible wordmark"));
        setProperty("logoSampling",QStringLiteral("8x_master_sampled_at_device_ratio"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    bool artworkCoverage(){
        prepareArtwork();const auto image=cached.toImage();
        for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y).alpha()>0)return true;
        return false;
    }
protected:
    void paintEvent(QPaintEvent *)override{
        prepareArtwork();if(cached.isNull())return;QPainter painter(this);Kda::configurePainter(painter);painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const qreal dpr=devicePixelRatioF();
        const auto size=cached.deviceIndependentSize();
        const QPointF position(qRound((28-size.width())*.5*dpr)/dpr,qRound((height()-size.height())*.5*dpr)/dpr);
        painter.drawPixmap(position,cached);
    }
private:
    void prepareArtwork(){
        const qreal dpr=devicePixelRatioF();const QSize physicalBox(qRound(28*dpr),qRound(28*dpr));
        if(cached.isNull()||cachedPhysicalBox!=physicalBox||!qFuzzyCompare(cachedDpr,dpr)){
            if(artwork.isNull()||physicalBox.isEmpty())return;
            cached=QPixmap::fromImage(artwork.scaled(physicalBox,Qt::KeepAspectRatio,Qt::SmoothTransformation));
            cached.setDevicePixelRatio(dpr);cachedPhysicalBox=physicalBox;cachedDpr=dpr;
        }
    }
    const QImage artwork;QPixmap cached;QSize cachedPhysicalBox;qreal cachedDpr=0;
};
// Account typography is live, independent of the atmospheric artwork below it.
class SatinBanner final : public QWidget {
public:
    static constexpr int Height=104;
    static constexpr qreal ArtworkScale=.5;
    SatinBanner():careerArt(QStringLiteral(":/lixin/kda/illustrations/soraka-star-guardian-official-v2.jpg")),reveal(this,650),refraction(this,280),sparkles(this){setObjectName(QStringLiteral("kdaCampaignCover"));setFixedHeight(Height);setMinimumWidth(0);setMouseTracking(true);setAttribute(Qt::WA_Hover);syncCampaignIdentity();}
    void setBackdrop(const QByteArray &bytes){
        if(bytes==backdropBytes)return;backdropBytes=bytes;
        QImage image;
        if(!bytes.isEmpty()&&bytes.size()<=8*1024*1024){QBuffer buffer;buffer.setData(bytes);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer);const auto size=reader.size();if(size.isValid()&&size.width()<=8192&&size.height()<=8192&&qint64(size.width())*size.height()<=16*1024*1024){reader.setScaledSize(size.scaled(1600,800,Qt::KeepAspectRatio));image=reader.read();}}
        customCareerArt=!image.isNull();careerArt=customCareerArt?QPixmap::fromImage(image):QPixmap(QStringLiteral(":/lixin/kda/illustrations/soraka-star-guardian-official-v2.jpg"));careerLayer=QImage();setProperty("careerBackdropSource",customCareerArt?QStringLiteral("local_league_profile"):QStringLiteral("bundled_official_soraka"));update();
    }
    QJsonObject motionReport()const{return {{"entrance",reveal.report()},{"pointer_response",refraction.report()},{"sparkles",sparkles.report()},{"native_facets",0},{"pointer_offset_max",QJsonArray{3,2}},{"wordmark_moves",false},{"looping",false},{"texture_required",true},{"artwork_scale",ArtworkScale},{"composition","unframed_account_identity"}};}
    QJsonObject motionChecks(const QString &outputDir);
    void setPlayerName(const QString &value){if(playerName==value)return;playerName=value;setToolTip(playerName);syncCampaignIdentity();update();}
    QString name()const{return playerName;}
    bool artworkLoaded()const{return !careerArt.isNull();}
    QString artworkName()const{return customCareerArt?QStringLiteral("local_league_profile"):QStringLiteral("bundled_official_soraka");}
    QString titleText()const{const int tag=playerName.lastIndexOf('#');return playerName.isEmpty()?QStringLiteral("等待玩家昵称"):tag>0?playerName.left(tag):playerName;}
    QFont titleFont()const{return Kda::font(Kda::FontRole::Title,32,QFont::DemiBold);}
    QRect titleRect()const{return QRect(18,17,std::max(1,qRound(width()*.56)-30),48);}
    QString displayedTitle()const{return QFontMetrics(titleFont()).elidedText(titleText(),Qt::ElideRight,titleRect().width());}
protected:
    void showEvent(QShowEvent *event)override{QWidget::showEvent(event);pointerFrom=pointerTarget={};refraction.finish();reveal.trigger();}
    void hideEvent(QHideEvent *event)override{reveal.finish();refraction.finish();pointerFrom=pointerTarget={};QWidget::hideEvent(event);}
    void enterEvent(QEnterEvent *event)override{QWidget::enterEvent(event);moveRefraction(event->position());}
    void mouseMoveEvent(QMouseEvent *event)override{QWidget::mouseMoveEvent(event);moveRefraction(event->position());}
    void leaveEvent(QEvent *event)override{QWidget::leaveEvent(event);retarget({});}
    void paintEvent(QPaintEvent *)override{
        syncCampaignIdentity();QPainter p(this);Kda::configurePainter(p);p.fillRect(rect(),QColor(9,10,15));
        // Career artwork belongs behind the account, fading into the reading
        // space. A layer mask keeps the player's name on a calm background.
        if(!careerArt.isNull()){
            const QRectF wing(width()*.40,0,width()*.60,height());
            const qreal imageWidth=wing.width()*ArtworkScale,imageHeight=careerArt.height()*imageWidth/careerArt.width();
            const qreal imageTop=imageHeight<=height()?(height()-imageHeight)*.5:std::clamp(height()*.5-imageHeight*.24,height()-imageHeight,qreal(0));
            const QRectF imageBounds(wing.width()-imageWidth,imageTop,imageWidth,imageHeight);
            const qreal dpr=devicePixelRatioF();const QSize physical(qRound(wing.width()*dpr),qRound(height()*dpr));
            if(careerLayer.size()!=physical||!qFuzzyCompare(careerLayer.devicePixelRatioF(),dpr)){
                careerLayer=QImage(physical,QImage::Format_ARGB32_Premultiplied);careerLayer.fill(Qt::transparent);careerLayer.setDevicePixelRatio(dpr);
                QPainter art(&careerLayer);art.setRenderHint(QPainter::SmoothPixmapTransform);const QRectF bounds(0,0,wing.width(),height());art.drawPixmap(imageBounds,careerArt,QRectF(careerArt.rect()));art.setCompositionMode(QPainter::CompositionMode_DestinationIn);QLinearGradient fade(imageBounds.left(),0,wing.width(),0);fade.setColorAt(0,Qt::transparent);fade.setColorAt(.38,QColor(0,0,0,155));fade.setColorAt(1,QColor(0,0,0,225));art.fillRect(bounds,fade);
            }
            p.save();p.setClipRect(wing,Qt::IntersectClip);p.drawImage(wing.topLeft()+QPointF(pointerPosition().x()*3,pointerPosition().y()*2),careerLayer);p.restore();
        }
        QLinearGradient quiet(0,0,width()*.94,0);quiet.setColorAt(0,QColor(9,10,15));quiet.setColorAt(.50,QColor(9,10,15));quiet.setColorAt(.70,QColor(9,10,15,220));quiet.setColorAt(.86,QColor(9,10,15,110));quiet.setColorAt(1,QColor(9,10,15,0));p.fillRect(rect(),quiet);
        Kda::paintIdentityPlate(p,QRectF(0,0,width()*.62,height()));
        auto accent=Kda::color("ice");auto label=Kda::font(Kda::FontRole::Label,10,QFont::DemiBold);label.setLetterSpacing(QFont::AbsoluteSpacing,1.5);
        p.setFont(titleFont());p.setPen(Kda::identityInk());p.drawText(titleRect(),Qt::AlignLeft|Qt::AlignVCenter,displayedTitle());
        p.setFont(Kda::font(Kda::FontRole::Numeric,12));p.setPen(Kda::identityInk());const int tagAt=playerName.lastIndexOf('#');const auto accountTag=tagAt>0?playerName.mid(tagAt):playerName.isEmpty()?QStringLiteral("登录客户端，回顾你的对局"):QString();p.drawText(QRect(19,68,std::max(1,width()/2),21),Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(p.font()).elidedText(accountTag,Qt::ElideRight,std::max(1,width()/2)));
        p.setFont(label);p.setPen(Kda::color("silver"));const QRect tag(qRound(width()*.68),height()-24,std::max(1,qRound(width()*.30)),18);p.drawText(tag,Qt::AlignRight|Qt::AlignVCenter,QStringLiteral("海克斯大乱斗"));
        sparkles.paint(p,QRectF(width()*.70,5,width()*.29,height()-28),16,.6);
        auto line=accent;line.setAlpha(135);p.setPen(QPen(line,.8));p.drawLine(QPointF(19,height()-2),QPointF(61,height()-2));line.setAlpha(40);p.setPen(QPen(line,.7));p.drawLine(QPointF(67,height()-2),QPointF(width()-17,height()-2));
    }
private:
    void syncCampaignIdentity(){const auto artwork=artworkName();if(property("campaignArtwork").toString()!=artwork){setProperty("campaignArtwork",artwork);setProperty("campaignAssetLoaded",artworkLoaded());}setAccessibleName((playerName.isEmpty()?titleText():playerName)+QStringLiteral(" · 个人战绩主页"));}
    QPointF pointerPosition()const{return Kda::reducedMotion()?QPointF():pointerFrom+(pointerTarget-pointerFrom)*refraction.progress();}
    void moveRefraction(const QPointF &position){retarget({std::clamp(position.x()/std::max(1,width())*2-1,qreal(-1),qreal(1)),std::clamp(position.y()/std::max(1,height())*2-1,qreal(-1),qreal(1))});}
    void retarget(const QPointF &next){if(Kda::reducedMotion()||!isVisible()){pointerFrom=pointerTarget={};refraction.finish();return;}if(QLineF(pointerTarget,next).length()<.035)return;pointerFrom=pointerPosition();refraction.finish();pointerTarget=next;refraction.trigger();update();}
    QPixmap careerArt;QImage careerLayer;QByteArray backdropBytes;bool customCareerArt=false;QString playerName;Kda::Transition reveal,refraction;Kda::SparkleField sparkles;QPointF pointerFrom,pointerTarget;
};

QJsonObject SatinBanner::motionChecks(const QString &outputDir){
    QJsonObject checks;const bool expectedMotion=!Kda::reducedMotion();
    const auto flush=[] {QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);};
    const auto settle=[&](int milliseconds){QElapsedTimer timer;timer.start();while(timer.elapsed()<milliseconds){flush();QThread::msleep(2);}flush();};
    settle(40);const int ambientFrames=sparkles.report().value("frames").toInt();settle(85);
    checks["sparkles_animate_only_in_active_visible_window"]=(sparkles.report().value("frames").toInt()>ambientFrames)==(expectedMotion&&window()->isActiveWindow());
    reveal.finish();refraction.finish();pointerFrom=pointerTarget={};update();flush();const auto before=grab().toImage();
    if(!outputDir.isEmpty())window()->grab().save(outputDir+QStringLiteral("/dashboard-material-before.png"));
    const QPointF local(width()*.9,height()*.25);QMouseEvent move(QEvent::MouseMove,local,QPointF(mapToGlobal(local.toPoint())),Qt::NoButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(this,&move);
    checks["cover_pointer_motion_respects_reduced_motion"]=refraction.running()==expectedMotion;
    settle(90);if(!outputDir.isEmpty())window()->grab().save(outputDir+QStringLiteral("/dashboard-material-middle.png"));settle(215);const auto after=grab().toImage();
    checks["cover_optical_signature_responds_without_moving_content"]=expectedMotion?before!=after:before==after;
    const qreal ratio=devicePixelRatioF();const QRect stable(qRound(14*ratio),qRound(25*ratio),qRound(width()*.38*ratio),qRound(52*ratio));
    checks["cover_wordmark_region_stays_stable"]=before.copy(stable)==after.copy(stable);
    checks["cover_pointer_motion_finishes_without_a_loop"]=!refraction.running()&&!reveal.running();
    if(!outputDir.isEmpty())window()->grab().save(outputDir+QStringLiteral("/dashboard-material-after.png"));
    hide();const int hiddenParticleFrames=sparkles.report().value("frames").toInt();const int frames=reveal.report().value("frames").toInt()+refraction.report().value("frames").toInt();settle(85);
    checks["hidden_cover_stops_particle_timer"]=!sparkles.report().value("running").toBool()&&hiddenParticleFrames==sparkles.report().value("frames").toInt();
    checks["hidden_cover_has_no_animation_frames"]=!refraction.running()&&!reveal.running()&&frames==reveal.report().value("frames").toInt()+refraction.report().value("frames").toInt();
    show();checks["cover_show_starts_a_finite_native_entrance"]=reveal.running()==expectedMotion;reveal.finish();refraction.finish();pointerFrom=pointerTarget={};update();flush();
    const bool preference=qApp->property("lixinReducedMotion").toBool();Kda::setReducedMotion(true);flush();
    const auto reducedImage=grab().toImage();const int reducedFrames=sparkles.report().value("frames").toInt();settle(85);
    checks["reduced_motion_removes_sparkles_and_stops_timer"]=!sparkles.report().value("running").toBool()&&reducedFrames==sparkles.report().value("frames").toInt()&&reducedImage==grab().toImage();
    Kda::setReducedMotion(preference);flush();return checks;
}
class ArchiveDivider final : public QWidget {
public:
    ArchiveDivider(){setFixedSize(12,34);setAttribute(Qt::WA_TransparentForMouseEvents);}
protected:
    void paintEvent(QPaintEvent *)override{QPainter painter(this);Kda::configurePainter(painter);painter.setPen(QPen(QColor("#353943"),.8));painter.drawLine(QPointF(6,1),QPointF(6,33));}
};
class SkinCard final : public QPushButton {
public:
    SkinCard(QString value,QString title,QString detail):id(std::move(value)),caption(std::move(title)),subtitle(std::move(detail)){
        setObjectName(QStringLiteral("skin_")+id);setAccessibleName(caption);setCheckable(true);setMinimumWidth(150);setFixedHeight(110);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);setToolTip(subtitle);campaign.load(id==QStringLiteral("kda_red")?QStringLiteral(":/lixin/kda/textures/home-fire-ribbon-v2.png"):QStringLiteral(":/lixin/kda/textures/home-silver-ribbon-v2.png"));baseMaterial.load(id==QStringLiteral("kda_red")?QStringLiteral(":/lixin/kda/textures/home-fire-mirror-v2.png"):QStringLiteral(":/lixin/kda/textures/home-platinum-stage-v1.jpg"));
    }
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter p(this);Kda::configurePainter(p);const auto r=QRectF(rect()).adjusted(.5,.5,-.5,-.5);const auto accent=Kda::skinColor(id,"ice");
        QPainterPath plate;plate.addRoundedRect(r,Kda::radius("section"),Kda::radius("section"));Kda::paintReadingSurface(p,QRectF(rect()),this);
        p.setPen(QPen(isChecked()||hasFocus()?accent:Kda::color("border"),isChecked()||hasFocus()?1.3:.7));p.setBrush(Qt::NoBrush);p.drawPath(plate);
        const QRectF sample(8,8,width()-16,42);QPainterPath clip;clip.addRoundedRect(sample,3,3);p.save();p.setClipPath(clip);
        QLinearGradient base(sample.topLeft(),sample.bottomRight());base.setColorAt(0,Kda::skinColor(id,"raised"));base.setColorAt(1,Kda::skinColor(id,"bg"));p.fillRect(sample,base);
        auto light=accent;light.setAlpha(38);QPainterPath plane;plane.moveTo(sample.left()+sample.width()*.44,sample.top());plane.lineTo(sample.right(),sample.top());plane.lineTo(sample.right(),sample.bottom());plane.lineTo(sample.left()+sample.width()*.18,sample.bottom());plane.closeSubpath();p.fillPath(plane,light);
        const auto materialPreview=[&](const QPixmap &art,qreal opacity){if(art.isNull())return;const qreal h=std::min(qreal(art.height()),art.width()*sample.height()/sample.width());p.setOpacity(opacity);p.drawPixmap(sample,art,QRectF(0,(art.height()-h)*.5,art.width(),h));p.setOpacity(1);};
        materialPreview(baseMaterial,id==QStringLiteral("kda_red")?.65:.34);materialPreview(campaign,id==QStringLiteral("kda_red")?.74:.47);
        p.setPen(QPen(accent,1));p.drawLine(QPointF(17,19),QPointF(width()-30,19));auto front=Kda::skinColor(id,"surface");front.setAlpha(245);p.setPen(Qt::NoPen);p.setBrush(front);p.drawRoundedRect(QRectF(16,26,width()-50,17),3,3);p.setPen(QPen(Kda::skinColor(id,"silver"),2));p.drawLine(QPointF(23,34),QPointF(width()*.47,34));p.restore();
        p.setFont(Kda::font(Kda::FontRole::Body,12,QFont::DemiBold));p.setPen(Kda::color("text"));p.drawText(QRect(10,56,width()-35,22),Qt::AlignVCenter,caption);
        p.setFont(Kda::font(Kda::FontRole::Body,12));p.setPen(Kda::color("muted"));p.drawText(QRect(10,80,width()-20,20),Qt::AlignVCenter,subtitle);
        if(isChecked()){p.setPen(Qt::NoPen);p.setBrush(accent);const QPointF c(width()-18,67);p.drawPolygon(QPolygonF{c+QPointF(0,-4),c+QPointF(4,0),c+QPointF(0,4),c+QPointF(-4,0)});}
    }
private:QString id,caption,subtitle;QPixmap campaign,baseMaterial;
};
constexpr int modelThumbnailHeight=50;
constexpr int modelPreviewFadeDuration=190;
constexpr int modelPageInset=Kda::Density::pageInset;

class ModelSlotCard final : public QPushButton {
public:
    ModelSlotCard(QString slot,QString title,QString detail,const QString &asset)
        :caption(std::move(title)),subtitle(std::move(detail)),artwork(asset),hover(this,140){
        const bool local=slot==QStringLiteral("local_openai");
        const int side=qRound(artwork.height()*.9);
        sourceCrop=QRect(qRound(artwork.width()*(local?.675:.68)),
                         qRound(artwork.height()*(local?0.:.018)),side,side).intersected(artwork.rect());
        croppedArtwork=artwork.copy(sourceCrop);
        setProperty("slotId",slot);
        setProperty("assetLoaded",!croppedArtwork.isNull());
        setProperty("assetPath",asset);
        setProperty("captionText",caption);
        setProperty("subtitleText",subtitle);
        setProperty("artworkSourceSize",artwork.size());
        setProperty("posterIndex",local?QStringLiteral("02"):QStringLiteral("01"));
        setProperty("expanded",false);
        setProperty("backgroundLayout",QStringLiteral("model_choice_thumbnail"));
        setAccessibleName(caption+QStringLiteral("模型位"));
        setToolTip(QStringLiteral("预览 %1 · %2").arg(caption,subtitle));
        setCheckable(true);
        setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
    }

    void setExpanded(bool selected){
        setProperty("expanded",selected);
        update();
    }

    QJsonObject visualReport()const{
        const auto array=[](const QRect &rect){return QJsonArray{rect.x(),rect.y(),rect.width(),rect.height()};};
        const auto status=property("statusText").toString();
        return {{"title_rect",array(titleRect())},{"subtitle_rect",array(detailRect())},
                {"status_rect",array(detailRect())},{"artwork_rect",array(portraitRect())},
                {"artwork_source_size",QJsonArray{artwork.width(),artwork.height()}},
                {"portrait_source_rect",array(sourceCrop)},{"background_rect",array(rect())},
                {"background_layout",property("backgroundLayout").toString()},
                {"background_preparations",portraitPreparations},
                {"caption_width",QFontMetrics(titleFont()).horizontalAdvance(caption)},
                {"subtitle_width",QFontMetrics(detailFont()).horizontalAdvance(subtitle)},
                {"status_width",QFontMetrics(detailFont()).horizontalAdvance(status)},
                {"poster_index",property("posterIndex").toString()},
                {"hover_level",hoverLevel()},{"hover_motion",hover.report()},
                {"expanded",property("expanded").toBool()},
                {"collapsed_height",modelThumbnailHeight},{"expanded_height",modelThumbnailHeight},
                {"layout_response_ms",0},
                {"selected_underline",isChecked()},{"active_status_emphasized",property("currentEnabled").toBool()},
                {"focus_ring_distinct",true},{"artwork_overlay_animated",false}};
    }

protected:
    bool event(QEvent *event)override{
        const auto type=event->type();
        const bool accepted=QPushButton::event(event);
        if(type==QEvent::Enter&&isEnabled())retargetHover(1.);
        else if(type==QEvent::Leave)retargetHover(0.);
        else if(type==QEvent::Hide||type==QEvent::Close||(type==QEvent::EnabledChange&&!isEnabled())){
            hover.finish();
            hoverFrom=hoverTarget=0.;
        }
        return accepted;
    }

    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);
        Kda::configurePainter(painter);
        const auto bounds=QRectF(rect()).adjusted(.5,.5,-.5,-.5);
        const auto accent=Kda::color("ice");
        painter.drawPixmap(portraitRect().topLeft(),portrait());

        const auto status=property("statusText").toString();
        const bool active=property("currentEnabled").toBool();
        const auto detail=status.isEmpty()?subtitle:status;
        painter.setFont(titleFont());
        painter.setPen(Kda::mixColor(Kda::color(isChecked()?"text":"muted"),accent,hoverLevel()));
        painter.drawText(titleRect(),Qt::AlignVCenter,QFontMetrics(titleFont()).elidedText(caption,Qt::ElideRight,titleRect().width()));
        painter.setFont(detailFont());
        painter.setPen(active?accent:Kda::color("muted"));
        painter.drawText(detailRect(),Qt::AlignVCenter,QFontMetrics(detailFont()).elidedText(detail,Qt::ElideRight,detailRect().width()));

        if(isChecked()){
            painter.setPen(QPen(accent,2,Qt::SolidLine,Qt::RoundCap));
            painter.drawLine(QPointF(titleRect().left(),height()-4),QPointF(titleRect().left()+22,height()-4));
        }
        if(hasFocus()&&window()->testAttribute(Qt::WA_KeyboardFocusChange)){
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(Kda::color("silver"),.8,Qt::DashLine));
            painter.drawRoundedRect(bounds.adjusted(3,3,-3,-3),2,2);
        }
    }

private:
    QRect portraitRect()const{return {0,(height()-28)/2,28,28};}
    static QFont titleFont(){return Kda::font(Kda::FontRole::Body,14,QFont::DemiBold);}
    static QFont detailFont(){return Kda::font(Kda::FontRole::Body,11);}
    QRect titleRect()const{return {38,6,width()-48,21};}
    QRect detailRect()const{return {38,27,width()-48,16};}
    qreal hoverLevel()const{return hoverFrom+(hoverTarget-hoverFrom)*hover.progress();}

    void retargetHover(qreal target){
        if(qFuzzyCompare(hoverTarget+1.,target+1.))return;
        hoverFrom=hoverLevel();
        hover.finish();
        hoverTarget=target;
        hover.trigger();
        update();
    }

    const QPixmap &portrait(){
        const auto dpr=devicePixelRatioF();
        if(cachedPortrait.isNull()||!qFuzzyCompare(cachedPortrait.devicePixelRatio(),dpr)){
            cachedPortrait=Kda::roundedPixmap(croppedArtwork,portraitRect().size(),3,dpr);
            setProperty("backgroundPreparations",++portraitPreparations);
        }
        return cachedPortrait;
    }

    QString caption,subtitle;
    QPixmap artwork,croppedArtwork,cachedPortrait;
    QRect sourceCrop;
    Kda::Transition hover;
    qreal hoverFrom=0.,hoverTarget=0.;
    int portraitPreparations=0;
};

class ModelSlotPreview final : public QWidget {
public:
    ModelSlotPreview():fade(this,modelPreviewFadeDuration){
        setObjectName(QStringLiteral("modelSlotPreview"));
        setMinimumWidth(0);
        setFixedHeight(84);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    }

    void setSlot(const QString &slot,const QString &state){
        const int next=slot==QStringLiteral("local_openai")?1:0;
        if(current==next&&status==state)return;
        if(current!=next){
            const bool hasPrevious=current>=0;
            fadeFrom=hasPrevious?1.-opacity():0.;
            fade.finish();
            current=next;
            if(hasPrevious)fade.trigger();
        }
        status=state;
        setAccessibleName(title()+QStringLiteral("立绘预览"));
        setAccessibleDescription(status);
        update();
    }

protected:
    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);
        Kda::configurePainter(painter);
        const qreal incomingOpacity=opacity();
        if(incomingOpacity<1.){
            painter.drawPixmap(QPointF(),background(1-current));
            painter.setOpacity(incomingOpacity);
        }
        painter.drawPixmap(QPointF(),background(std::max(0,current)));
        painter.setOpacity(1.);

    }

private:
    QString title()const{return current==1?QStringLiteral("本地模型"):QStringLiteral("DeepSeek");}
    qreal opacity()const{return fadeFrom+(1.-fadeFrom)*fade.progress();}

    const QPixmap &background(int index){
        auto &cache=backgrounds[index];
        const auto dpr=devicePixelRatioF();
        const auto plane=Kda::color("surface");
        if(!cache.isNull()&&backgroundSizes[index]==size()&&backgroundPlanes[index]==plane
            &&qFuzzyCompare(cache.devicePixelRatio(),dpr))return cache;
        const QSize physical(std::max(1,qRound(width()*dpr)),std::max(1,qRound(height()*dpr)));
        QImage surface(physical,QImage::Format_ARGB32_Premultiplied);
        surface.setDevicePixelRatio(dpr);
        const auto base=index==1?QColor("#101c19"):QColor("#101524");
        surface.fill(base);
        QPainter painter(&surface);
        Kda::configurePainter(painter);
        const auto &image=artworks[index];
        if(!image.isNull()){
            auto scaled=image.scaledToHeight(qRound(physical.height()*1.14),Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_ARGB32_Premultiplied);
            {
                QPainter mask(&scaled);
                mask.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                QLinearGradient feather(0,0,scaled.width(),0);
                feather.setColorAt(0,Qt::transparent);
                feather.setColorAt(.30,Qt::white);
                feather.setColorAt(.92,Qt::white);
                feather.setColorAt(1,Qt::transparent);
                mask.fillRect(scaled.rect(),feather);
            }
            const qreal artWidth=scaled.width()/dpr;
            const qreal face=index==1?.817:.823;
            const qreal faceX=width()*.88;
            const QRectF art(faceX-artWidth*face,-3,artWidth,scaled.height()/dpr);
            painter.drawImage(art,scaled);
        }
        QLinearGradient veil(0,0,width(),0);
        auto clear=base;
        clear.setAlpha(0);
        veil.setColorAt(0,base);
        veil.setColorAt(.54,base);
        veil.setColorAt(.84,clear);
        veil.setColorAt(1,clear);
        painter.fillRect(rect(),veil);
        QLinearGradient dissolve(0,height()*.58,0,height());
        auto transparentPlane=plane;
        transparentPlane.setAlpha(0);
        dissolve.setColorAt(0,transparentPlane);
        dissolve.setColorAt(1,plane);
        painter.fillRect(rect(),dissolve);
        painter.end();
        cache=Kda::roundedPixmap(QPixmap::fromImage(surface),size(),Kda::radius("section"),dpr);
        backgroundSizes[index]=size();
        backgroundPlanes[index]=plane;
        return cache;
    }

    const QImage artworks[2]{
        QImage(QStringLiteral(":/lixin/kda/model-slots/deepseek-maid-strip-v1.png")),
        QImage(QStringLiteral(":/lixin/kda/model-slots/local-mint-strip-v1.png"))
    };
    QPixmap backgrounds[2];
    QSize backgroundSizes[2];
    QColor backgroundPlanes[2];
    Kda::Transition fade;
    qreal fadeFrom=0.;
    int current=-1;
    QString status;
};
class FacetedWorkspace final : public QFrame {
public:
    FacetedWorkspace():atmosphere(new Kda::HomeAtmosphere(this)){}
    void setDepthScene(Kda::DepthScene *value){scene=value;}
    bool roundedCoverage()const{
        if(surface.isNull())return false;
        return surface.pixelColor(0,0).alpha()==0&&surface.pixelColor(surface.width()-1,0).alpha()==0
            &&surface.pixelColor(0,surface.height()-1).alpha()==0&&surface.pixelColor(surface.width()-1,surface.height()-1).alpha()==0
            &&surface.pixelColor(surface.width()/2,0).alpha()==cachedAlpha;
    }
    QJsonObject depthReport()const{
        auto result=scene?scene->report():QJsonObject{};
        result["display_stage_frame"]=false;
        result["backdrop_is_static"]=true;
        result["titlebar_fixed"]=true;
        result["material_midground"]=atmosphere->report();
        return result;
    }
protected:
    void resizeEvent(QResizeEvent *event)override{QFrame::resizeEvent(event);atmosphere->setGeometry(rect());atmosphere->lower();}
    void paintEvent(QPaintEvent *)override{
        const qreal dpr=devicePixelRatioF();const QSize physicalSize(qRound(width()*dpr),qRound(height()*dpr));
        const int tint=window()->property("dashboardBackdropTintAlpha").toInt();
        const bool glass=Kda::glassEnabled();const auto skin=Kda::skin();
        const int alpha=glass&&tint>0&&tint<255?232:255;
        const bool matchingGeometry=surface.size()==physicalSize&&qFuzzyCompare(cachedDpr,dpr);
        const bool reuseGeometry=!matchingGeometry&&window()->property("dashboardInteractiveMoveSize").toBool();
        if(surface.isNull()||(!matchingGeometry&&!reuseGeometry)||cachedSkin!=skin||cachedGlass!=glass||cachedAlpha!=alpha){
            // Resolve the complete static background and its rounded mask once.
            // Content depth comes from live native planes, without a glass frame.
            // Text is drawn by native controls. This bitmap contains only the
            // background, so render it directly at the display's physical size.
            QImage coverage(physicalSize,QImage::Format_ARGB32_Premultiplied);coverage.fill(Qt::transparent);coverage.setDevicePixelRatio(dpr);
            {QPainter painter(&coverage);Kda::configurePainter(painter);QPainterPath clip;clip.addRoundedRect(QRectF(rect()),Kda::radius("workspace"),Kda::radius("workspace"));painter.setClipPath(clip);painter.fillPath(clip,Kda::color("bg"));
                Kda::paintHomeBackdrop(painter,QRectF(rect()));
                painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);painter.fillRect(rect(),QColor(0,0,0,alpha));
            }
            surface=std::move(coverage);cachedDpr=dpr;cachedSkin=skin;cachedGlass=glass;cachedAlpha=alpha;
        }
        QPainter painter(this);Kda::configurePainter(painter);QPainterPath clip;clip.addRoundedRect(QRectF(rect()),Kda::radius("workspace"),Kda::radius("workspace"));painter.setClipPath(clip);painter.setCompositionMode(QPainter::CompositionMode_Source);
        if(reuseGeometry)painter.drawImage(rect(),surface);
        else painter.drawImage(QPointF(),surface);
    }
private:
    Kda::HomeAtmosphere *atmosphere;QImage surface;qreal cachedDpr=0;QString cachedSkin;bool cachedGlass=false;int cachedAlpha=255;QPointer<Kda::DepthScene> scene;
};
Kda::DepthPlane *depthPlane(QWidget *content,Kda::DepthLayer layer,const char *name){
    auto plane=new Kda::DepthPlane(content,layer);plane->setObjectName(QString::fromLatin1(name));return plane;
}
class AmbientStage final : public QWidget {
public:
    AmbientStage():sparkles(this){}
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);Kda::configurePainter(painter);
        // The central content is reserved: only the surrounding negative space
        // carries drifting light, including at the compact window size.
        painter.setClipRegion(QRegion(rect()).subtracted(QRegion(childrenRect().adjusted(-14,-14,14,14))));
        sparkles.paint(painter,QRectF(rect()).adjusted(12,12,-12,-12),32,.72);
        sparkles.paintFlow(painter,QRectF(rect()).adjusted(1,1,-1,-1),.65,Kda::radius("workspace"));
    }
private:Kda::SparkleField sparkles;
};
class EditorialMasthead final : public QWidget {
public:
    explicit EditorialMasthead(QString title):copy(std::move(title)),sparkles(this){setFixedHeight(40);setMinimumWidth(0);setAccessibleName(copy);}
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);Kda::configurePainter(painter);
        const auto area=QRectF(rect()).adjusted(.5,.5,-.5,-.5);QPainterPath shape;shape.addRoundedRect(area,Kda::radius("section"),Kda::radius("section"));
        painter.save();painter.setClipPath(shape);painter.fillPath(shape,Kda::color("surface"));
        // A fixed title slab and a separate material wing carry the campaign
        // identity. The title never sits directly over the busy foil texture.
        const qreal cut=width()*.54;QPainterPath wing;wing.moveTo(cut+24,0);wing.lineTo(width(),0);wing.lineTo(width(),height());wing.lineTo(cut-15,height());wing.closeSubpath();
        painter.save();painter.setClipPath(wing,Qt::IntersectClip);Kda::paintFoil(painter,rect(),.82);auto tint=Kda::color("ice");tint.setAlpha(30);painter.fillPath(wing,tint);painter.restore();
        auto edge=Kda::color("silver");edge.setAlpha(100);painter.setPen(QPen(edge,.75));painter.drawLine(QPointF(10,.8),QPointF(cut+21,.8));
        QPainterPath heel;heel.moveTo(width()-84,0);heel.lineTo(width(),0);heel.lineTo(width(),height());heel.lineTo(width()-109,height());heel.closeSubpath();painter.fillPath(heel,Kda::color("bg"));
        sparkles.paint(painter,QRectF(width()-94,2,90,height()-4),9,.65);painter.restore();
        auto border=Kda::color("border");painter.setBrush(Qt::NoBrush);painter.setPen(QPen(border,.75));painter.drawPath(shape);
        sparkles.paintFlow(painter,area,.8,Kda::radius("section"));
        painter.setPen(Kda::color("text"));painter.setFont(Kda::font(Kda::FontRole::Title,20,QFont::DemiBold));painter.drawText(QRect(15,0,qRound(cut)-16,height()),Qt::AlignLeft|Qt::AlignVCenter,copy);
        const bool firestarter=Kda::skin()==QStringLiteral("kda_red");auto signature=Kda::font(Kda::FontRole::Label,11,QFont::DemiBold);signature.setLetterSpacing(QFont::AbsoluteSpacing,firestarter?.65:1.3);painter.setFont(signature);painter.setPen(firestarter?Kda::color("text"):QColor("#18212b"));painter.drawText(QRect(qRound(cut)+24,0,std::max(0,width()-qRound(cut)-145),height()),Qt::AlignRight|Qt::AlignVCenter,firestarter?QStringLiteral("FIRESTARTER"):QStringLiteral("ALL OUT"));
        painter.setPen(QPen(Kda::color("gold"),1.3));painter.drawLine(QPointF(width()-58,11),QPointF(width()-39,30));painter.setPen(QPen(Kda::color("ice"),.8));painter.drawLine(QPointF(width()-47,11),QPointF(width()-28,30));
    }
private:QString copy;Kda::SparkleField sparkles;
};
class EditorialPanel final : public QFrame {
protected:
    void paintEvent(QPaintEvent *event)override{
        QFrame::paintEvent(event);const auto name=objectName();
        const bool blue=name==QStringLiteral("teamBluePanel"),red=name==QStringLiteral("teamRedPanel"),settings=name==QStringLiteral("settingsPanel");
        const bool recommendation=name==QStringLiteral("scoreBar");
        if(!blue&&!red&&!recommendation&&!settings&&name!=QStringLiteral("archiveStrip"))return;
        QPainter painter(this);Kda::configurePainter(painter);
        Kda::paintReadingSurface(painter,QRectF(rect()),settings?this:nullptr);
        const auto area=QRectF(rect()).adjusted(.6,.6,-.6,-.6);QPainterPath clip;clip.addRoundedRect(area,Kda::radius("section"),Kda::radius("section"));painter.setClipPath(clip);
        const auto panelInk=[&](const char *token){return (blue||red)&&Kda::skin()==QStringLiteral("kda_red")?Kda::skinColor("kda",token):Kda::color(token);};
        auto ice=panelInk(red?"danger":blue?"ice":"silver");auto clear=ice;ice.setAlpha(name==QStringLiteral("scoreBar")?95:50);clear.setAlpha(0);
        QLinearGradient rim(8,0,width()-8,0);rim.setColorAt(0,ice);rim.setColorAt(.52,clear);rim.setColorAt(1,clear);painter.setPen(QPen(rim,.7));painter.drawLine(QPointF(8,1.1),QPointF(width()-8,1.1));
        auto shadow=Kda::color("bg");shadow.setAlpha(80);painter.setPen(QPen(shadow,.6));painter.drawLine(QPointF(8,height()-1.2),QPointF(width()-8,height()-1.2));
        if(blue||red){auto facetInk=panelInk(red?"danger":"ice");facetInk.setAlpha(16);QPainterPath facet;facet.moveTo(width()-33,1);facet.lineTo(width()-8,1);facet.lineTo(width()-16,10);facet.lineTo(width()-41,10);facet.closeSubpath();painter.fillPath(facet,facetInk);}
    }
};
QPixmap roundedArtwork(const QPixmap &source,QSize logicalSize,qreal ratio,qreal radius){
    return Kda::roundedPixmap(source,logicalSize,radius,ratio);
}
class ElidedLabel final : public QLabel {
public:
    // Ignored causes QBoxLayout to reserve zero width even for a fixed-width
    // label. Keep a real size hint and allow only the text column to shrink.
    explicit ElidedLabel(QWidget *parent=nullptr):QLabel(parent){setTextFormat(Qt::PlainText);setMinimumWidth(0);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);}
    QSize minimumSizeHint()const override{return {0,QLabel::minimumSizeHint().height()};}
    void present(const QString &value){setProperty("fullText",value);setText(fontMetrics().elidedText(value,Qt::ElideRight,std::max(0,contentsRect().width())));}
protected:
    void resizeEvent(QResizeEvent *e)override{QLabel::resizeEvent(e);present(property("fullText").toString());}
    void changeEvent(QEvent *event)override{QLabel::changeEvent(event);if(event->type()==QEvent::FontChange||event->type()==QEvent::ApplicationFontChange||event->type()==QEvent::StyleChange)present(property("fullText").toString());}
};
ElidedLabel *line(const QString &name,const QString &value={}){auto label=new ElidedLabel;label->setObjectName(name);label->present(value);return label;}
void present(QLabel *label,const QString &value){static_cast<ElidedLabel*>(label)->present(value);}
QLabel *wrapped(const QString &name,const QString &value={}){auto label=new QLabel(value);label->setObjectName(name);label->setWordWrap(true);label->setMinimumWidth(0);label->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);label->setTextFormat(Qt::PlainText);return label;}
QFrame *section(const QString &name){auto frame=new EditorialPanel;frame->setObjectName(name);frame->setMinimumWidth(0);frame->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);if(name==QStringLiteral("settingsPanel"))Kda::markSharedGlassSurface(frame);return frame;}
class CurrentSettingsPageStack final : public QStackedWidget {
public:
    CurrentSettingsPageStack(){
        layout()->setSizeConstraint(QLayout::SetNoConstraint);
        connect(this,&QStackedWidget::currentChanged,this,[this]{layout()->invalidate();syncCurrentMinimum();updateGeometry();});
    }
    QSize sizeHint()const override{return currentWidget()?currentWidget()->sizeHint():QSize();}
    QSize minimumSizeHint()const override{
        auto hint=currentWidget()?currentWidget()->minimumSizeHint():QSize();hint.setWidth(0);return hint;
    }
    bool hasHeightForWidth()const override{return currentWidget()&&currentWidget()->hasHeightForWidth();}
    int heightForWidth(int width)const override{
        if(!currentWidget())return 0;
        const auto page=currentWidget();return std::max(page->minimumSizeHint().height(),page->hasHeightForWidth()?page->heightForWidth(width):page->sizeHint().height());
    }
protected:
    bool event(QEvent *event)override{
        const auto type=event->type();const bool accepted=QStackedWidget::event(event);
        if(type==QEvent::LayoutRequest)syncCurrentMinimum();return accepted;
    }
    void resizeEvent(QResizeEvent *event)override{
        QStackedWidget::resizeEvent(event);if(currentWidget()&&currentWidget()->layout())currentWidget()->layout()->activate();syncCurrentMinimum();
    }
private:
    void syncCurrentMinimum(){
        const auto page=currentWidget();
        const int currentWidth=contentsRect().width();if(currentWidth<=0)return;
        const int wrappedHeight=page&&page->hasHeightForWidth()?page->heightForWidth(currentWidth):-1;
        const int pageHeight=page?std::max({0,page->minimumSizeHint().height(),wrappedHeight>=0?wrappedHeight:page->sizeHint().height()}):0;
        // QStackedLayout can retain another page's height-for-width. Bound the
        // stack to the current page's natural height, including wrapped copy.
        if(minimumHeight()!=pageHeight||maximumHeight()!=pageHeight)setFixedHeight(pageHeight);
    }
};
class CenteredSettingsScroll final : public QScrollArea {
public:
    explicit CenteredSettingsScroll(QWidget *page):content(page){
        setObjectName(QStringLiteral("pageScroll"));setWidgetResizable(true);setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setWidget(content);viewport()->installEventFilter(this);
        connect(verticalScrollBar(),&QScrollBar::rangeChanged,this,[this]{syncContentMargins();});
    }
protected:
    bool eventFilter(QObject *watched,QEvent *event)override{
        const bool handled=QScrollArea::eventFilter(watched,event);
        if(watched==viewport()&&event->type()==QEvent::Resize)syncContentMargins();return handled;
    }
    void resizeEvent(QResizeEvent *event)override{QScrollArea::resizeEvent(event);syncContentMargins();}
private:
    QWidget *content=nullptr;
    void syncContentMargins(){
        if(!content||!content->layout())return;
        // Center on the whole settings shell, including the scrollbar gutter,
        // so fixed tabs and every scrolling page keep the same reading edges.
        const int leftGutter=std::max(0,viewport()->geometry().left()-contentsRect().left());
        const int rightGutter=std::max(0,contentsRect().right()-viewport()->geometry().right());
        const QMargins margins(Kda::Density::pageInset+std::max(0,rightGutter-leftGutter),Kda::Density::pageInset,
                               Kda::Density::pageInset+std::max(0,leftGutter-rightGutter),Kda::Density::pageInset);
        if(content->layout()->contentsMargins()!=margins)content->layout()->setContentsMargins(margins);
    }
};
class ResponsiveSettingsPage final : public Kda::ReadingSurface {
public:
    void setPanels(QWidget *floating,QWidget *ds,QWidget *shortcuts){left=floating;right=ds;footer=shortcuts;arrange();}
protected:
    void resizeEvent(QResizeEvent *event)override{QWidget::resizeEvent(event);arrange();}
private:
    QWidget *left=nullptr,*right=nullptr,*footer=nullptr;int columns=0;
    void arrange(){
        if(!left||!footer)return;
        const int next=right&&width()>=settingsTwoColumnWidth?2:1;if(next==columns)return;columns=next;
        auto grid=static_cast<QGridLayout*>(layout());for(auto panel:{left,right,footer})if(panel)grid->removeWidget(panel);grid->setHorizontalSpacing(right?12:0);
        for(int row=0;row<4;++row)grid->setRowStretch(row,0);
        grid->setColumnStretch(0,1);grid->setColumnStretch(1,columns==2?1:0);
        grid->addWidget(left,right&&columns==1?1:0,0,Qt::AlignTop);if(right)grid->addWidget(right,0,columns==2?1:0,Qt::AlignTop);
        const int footerRow=right&&columns==1?2:1;grid->addWidget(footer,footerRow,0,1,columns,Qt::AlignTop);grid->setRowStretch(footerRow+1,1);
        setProperty("settingsColumns",columns);grid->invalidate();updateGeometry();
    }
};
class UsageCostSummary final : public QWidget {
public:
    UsageCostSummary(){
        setMinimumWidth(0);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
        root=new QVBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(8);
        metrics=new QGridLayout;metrics->setContentsMargins(0,0,0,0);metrics->setHorizontalSpacing(20);metrics->setVerticalSpacing(12);root->addLayout(metrics);
    }
    void setContents(QWidget *current,QWidget *average,QWidget *details){
        currentBlock=current;averageBlock=average;
        for(auto block:{currentBlock,averageBlock}){block->setMinimumWidth(0);block->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);}
        root->addWidget(details);arrange();
    }
protected:
    void resizeEvent(QResizeEvent *event)override{QWidget::resizeEvent(event);arrange();}
private:
    QVBoxLayout *root=nullptr;QGridLayout *metrics=nullptr;QWidget *currentBlock=nullptr,*averageBlock=nullptr;int columns=0;
    void arrange(){
        if(!currentBlock||!averageBlock)return;
        const int next=width()>=usageTwoColumnWidth?2:1;if(next==columns)return;columns=next;
        metrics->removeWidget(currentBlock);metrics->removeWidget(averageBlock);
        metrics->setColumnStretch(0,1);metrics->setColumnStretch(1,columns==2?1:0);
        metrics->addWidget(currentBlock,0,0,Qt::AlignTop);metrics->addWidget(averageBlock,columns==2?0:1,columns==2?1:0,Qt::AlignTop);
        setProperty("usageMetricColumns",columns);metrics->invalidate();updateGeometry();update();
    }
};
QVBoxLayout *column(QWidget *widget,int margin=20,int spacing=12){auto layout=new QVBoxLayout(widget);layout->setContentsMargins(margin,margin,margin,margin);layout->setSpacing(spacing);return layout;}
QPushButton *button(const QString &text,const QString &name={}){auto result=new DashboardButton(text);result->setObjectName(name);if(name==QStringLiteral("quiet"))result->setProperty("toolbarAction",true);result->setCursor(Qt::PointingHandCursor);result->setMinimumHeight(Kda::Density::controlHeight);result->setFocusPolicy(Qt::StrongFocus);return result;}
QScrollArea *scrollPage(QWidget *content){auto scroll=new QScrollArea;scroll->setObjectName(QStringLiteral("pageScroll"));scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);scroll->setWidget(content);return scroll;}
bool observedInt(const QJsonValue &v,int &result){if(!v.isDouble()||!std::isfinite(v.toDouble())||v.toDouble()!=v.toInt()||v.toInt()<0)return false;result=v.toInt();return true;}
QString roleSummary(const QJsonObject &champion){
    const QMap<QString,QString> names{{"tank",QStringLiteral("坦克")},{"mage",QStringLiteral("法师")},{"marksman",QStringLiteral("射手")},{"fighter",QStringLiteral("战士")},{"assassin",QStringLiteral("刺客")},{"support",QStringLiteral("辅助")}};
    QStringList roles;for(const auto &v:champion.value("roles").toArray())if(names.contains(v.toString()))roles.append(names.value(v.toString()));return roles.join(QStringLiteral(" / "));
}
QString numberText(const QJsonValue &value,int precision=0){return value.isDouble()&&std::isfinite(value.toDouble())?QString::number(value.toDouble(),'f',precision):QStringLiteral("—");}
QString comparisonText(const QJsonObject &comparison){
    const auto direction=comparison.value("direction").toString();const auto label=comparison.value("label").toString();
    if(label.isEmpty())return {};if(direction=="above_reference")return QStringLiteral("↑ ")+label;if(direction=="below_reference")return QStringLiteral("↓ ")+label;if(direction=="similar_reference")return QStringLiteral("↔ ")+label;return {};
}
double opacityPreference(const QSettings &preferences){const auto value=preferences.value("right/opacity",.88).toDouble();return std::isfinite(value)?std::clamp(value,.45,1.):.88;}
QRect relativeRect(const QWidget *widget,const QWidget *ancestor){return {widget->mapTo(ancestor,QPoint()),widget->size()};}
QJsonArray rectJson(const QRect &rect){return {rect.x(),rect.y(),rect.width(),rect.height()};}
}

CoachDashboard::CoachDashboard(const Catalog &data,QSettings &shared,QWidget *parent,bool startupGated):QMainWindow(parent),catalog(data),preferences(shared),matchProjection(data),startupNoticeActive(startupGated){
    auto bodyFont=Kda::font(Kda::FontRole::Body,12);bodyFont.setWeight(QFont::Normal);setFont(bodyFont);
    setObjectName(QStringLiteral("coachDashboard"));setWindowTitle(QStringLiteral("李金金 · 对局助手"));resize(840,520);setMinimumSize(760,500);
    assets=new HudAssets(catalog,this);assets->onChanged=[this]{updateArtwork();if(champSelectView)champSelectView->update();};
    dashboardBaseStyle=QStringLiteral(R"(
      QMainWindow,QWidget#dashboardRoot,QWidget#pageContent{background:transparent;color:#e7e9ef;}
      QWidget{font-family:'Inter','Noto Sans SC','Segoe UI';font-size:12px;font-weight:400;color:#e7e9ef;}
      QWidget#topbar{background:transparent;border:0;border-bottom:1px solid #252832;}
      QWidget#dashboardStatusBar{background:transparent;}
      QLabel{background:transparent;border:0;} QLabel#wordmark{font-family:'Inter','Noto Sans SC','Segoe UI';font-size:16px;font-style:normal;font-weight:500;color:#e7e4ef;}
      QLabel#pageTitle{font-size:16px;font-weight:600;color:#eceef3;}
      QLabel#opacityCaption,QLabel#closeWindowBehaviorCaption{font-size:12px;color:#d4d8e0;}
      QLabel#sectionTitle{font-size:14px;font-weight:500;color:#eceef3;}
      QLabel#settingsTitle{font-size:20px;font-weight:600;color:#eceef3;}
      QLabel#settingsHint{font-size:11px;color:#a09aaa;}
      QLabel#acknowledgmentGroupTitle{font-size:11px;font-weight:500;color:#a09aaa;}
      QLabel#acknowledgmentProjectLink{font-size:11px;}
      QLabel#aboutAcknowledgmentLogo[lightBadge="true"]{background:#ece8ff;border-radius:5px;padding:2px;}
      QLabel#aboutAcknowledgmentLogo[fontSpecimen="true"]{background:#252832;border-radius:5px;font-family:'Noto Sans SC';font-size:16px;color:#e7e9ef;}
      QLabel#dsUsageCaption{font-size:11px;font-weight:500;color:#b7b0c2;}
      QLabel#dsUsageCurrent,QLabel#dsUsageAverage{font-family:'Inter','Noto Sans SC';font-size:18px;font-weight:500;color:#eeeaf7;}
      QLabel#dsUsageCurrent[usageState="no_calls"],QLabel#dsUsageCurrent[usageState="unknown_cost"],QLabel#dsUsageAverage[usageState="no_samples"],QLabel#dsUsageAverage[usageState="unknown_cost"]{font-size:16px;font-weight:400;color:#a9a2b4;}
      QLabel#dsUsageState{font-size:10px;color:#a9a2b4;}
      QLabel#dsUsageTokens,QLabel#dsUsageCalls,QLabel#dsUsageSamples{font-size:11px;color:#a09aaa;}
      QLabel#dsUsageNote{font-size:10px;color:#8e879a;}
      QLabel#dsUsagePersistence{font-size:11px;color:#c9b0ed;}
      QLabel#modelCurrentName,QLabel#modelCurrentProvider,QLabel#modelCurrentState,QLabel#modelDraftState,QLabel#modelSaveStatus,QLabel#modelUsageScope,QLabel#modelLockStatus{font-size:11px;color:#a9a2b4;}
      QLabel#teamBlue{font-size:12px;font-weight:600;color:#acc3d3;} QLabel#teamRed{font-size:12px;font-weight:600;color:#c9a7b1;}
      QLabel#seatName{font-size:12px;font-weight:600;color:#ecebf2;} QLabel#seatName[compact="true"]{font-size:12px;}
      QLabel#seatDetail{font-size:12px;color:#aab1bd;}
      QLabel#seatMarker{font-family:'Inter','Noto Sans SC';font-size:12px;color:#c4cad4;}
      QLabel#portrait{background:#24262e;border:0;border-radius:%4px;color:#8f97a5;font-size:17px;}
      QLabel#score{font-family:'Inter','Noto Sans SC','Segoe UI';font-size:15px;font-weight:600;color:#d4d8e0;}
      QLabel#clock{font-family:'Inter','Noto Sans SC','Segoe UI';font-size:12px;font-weight:600;color:#acb4c2;}
      QLabel#actionTitle{font-size:21px;font-weight:600;color:#efedf6;}
      QLabel#actionDetail{font-size:12px;color:#b9c0cd;} QLabel#route{font-size:12px;color:#a5adba;}
      QLabel#purchasePrice{font-size:14px;font-weight:700;color:#c5b58e;}
      QLabel#rosterSummary{font-size:12px;color:#a8b0bf;}
      QLabel#saveStatus{font-size:12px;color:#a7afbd;}
      QLabel#idleTitle{font-size:15px;font-weight:600;color:#eceef4;}
      QLabel#idleDetail{font-size:12px;color:#a5adba;}
      QLabel#archiveValue{font-family:'Inter','Noto Sans SC','Segoe UI';font-size:23px;font-weight:500;color:#d8dde7;}
      QLabel#archiveLabel{font-size:12px;color:#a5adba;}
      QLabel#archiveUpdated{font-size:12px;color:#a0a8b5;}
      QLabel#phase{font-size:12px;color:#a5adba;padding:3px 6px;border-radius:%5px;}
      QLabel#phase[active="true"]{color:#151a23;background:#d0d8e4;}
      QLabel#phase[complete="true"]{color:#a1b1c4;}
      QFrame#seat{background:transparent;border:0;border-top:1px solid #2a2d36;}
      QFrame#seat[self="true"]{background:#20242d;border:0;border-radius:%2px;}
      QFrame#scoreBar,QFrame#teamBluePanel,QFrame#teamRedPanel,QFrame#archiveStrip,QFrame#settingsPanel{background:transparent;border:0;border-radius:%2px;}
      QFrame#navigationGroup{background:transparent;border:0;}
      QPushButton{background:#26222d;border:1px solid #39333f;border-radius:%3px;padding:3px 6px;color:#d3cdda;font-size:12px;}
      QPushButton:hover{background:#332e3c;border-color:#6e647b;} QPushButton:pressed{background:#1a1720;}
      QPushButton:focus{border-color:#accdd9;}
      QPushButton[dashboardNavigation="true"]{background:transparent;border:1px solid transparent;border-radius:%7px;padding:2px 12px;color:#a09aaa;}
      QPushButton[dashboardNavigation="true"]:checked{color:#211b2b;font-weight:600;}
      QPushButton[dashboardNavigation="true"]:focus{border-color:#accdd9;}
      QPushButton[toolbarAction="true"]{background:transparent;border-color:transparent;padding:3px 8px;color:#aaa3b5;}
      QPushButton[toolbarAction="true"]:hover{background:#26212d;color:#ece8f2;}
      QPushButton[toolbarAction="true"]:focus{border-color:#accdd9;}
      QPushButton[primaryAction="true"]{background:#e4deed;border-color:#e4deed;color:#201a28;font-weight:600;} QPushButton[primaryAction="true"]:hover{background:#f3eff8;} QPushButton[primaryAction="true"]:focus{border-color:#675775;}
      QPushButton#quiet{background:transparent;border-color:transparent;color:#a09aa9;}
      QPushButton#quiet:focus{border-color:#accdd9;}
      QScrollArea,QScrollArea>QWidget>QWidget{background:transparent;border:0;}
      QFrame#dashboardWorkspace{background:transparent;border:0;border-radius:%1px;}
      QStackedWidget#dashboardPages{background:transparent;border:0;}
      QCheckBox{padding:0;background:transparent;}
      QLineEdit,QComboBox{background:#101218;border:1px solid #3d424f;border-radius:%3px;padding:3px 6px;font-size:12px;selection-background-color:#48586e;min-height:18px;}
      QLineEdit:focus,QComboBox:focus{border:1px solid #8e9fb5;}QComboBox::drop-down{border:0;width:24px;background:transparent;}QComboBox::down-arrow{image:none;width:10px;height:7px;} QComboBox QAbstractItemView{background:#191c24;color:#e8edf5;selection-background-color:#394558;}
      QSlider::groove:horizontal{height:4px;background:#343b49;border-radius:2px;} QSlider::sub-page:horizontal{background:#b8c4d4;}
      QSlider::handle:horizontal{width:15px;height:15px;margin:-6px 0;background:transparent;border:0;border-radius:0;} QSlider:focus{border:1px solid #b4a0c7;}
      QScrollBar:vertical{width:8px;background:transparent;margin:12px 0;}QScrollBar::handle:vertical{background:#454c5a;min-height:24px;border-radius:%8px;}QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}
    )").arg(Kda::radius("workspace")).arg(Kda::radius("section")).arg(Kda::radius("control")).arg(Kda::radius("artwork")).arg(Kda::radius("tag")).arg(Kda::radius("navigation_group")).arg(Kda::radius("navigation")).arg(Kda::radius("small"));
    Kda::bindThemeStyle(this,[this]{
        auto sheet=Kda::overlayStyleSheet()+Kda::themedStyleSheet(dashboardBaseStyle);
        sheet+=QStringLiteral("QLabel#muted,QLabel#dataStatus,QLabel#dsApiKeyCaption,QLabel#matchRefreshStatus,QLabel#subTitle{font-size:12px;color:%1;}")
            .arg(Kda::color("muted").name());
        sheet+=QStringLiteral("QLineEdit#dsApiKey,QComboBox#dsModel,QComboBox#modelSlot,QLineEdit#localModelBaseUrl,QLineEdit#localModelName,QLineEdit#localModelApiKey{background:%1;}QComboBox#dsModel QLineEdit{background:transparent;}").arg(Kda::glassFieldBackground());
        return sheet;
    });
    auto root=new QWidget;root->setObjectName(QStringLiteral("dashboardRoot"));setCentralWidget(root);auto shell=new QVBoxLayout(root);const int surfaceGap=qRound(Kda::radius("gap"));shell->setContentsMargins(surfaceGap,0,surfaceGap,surfaceGap);shell->setSpacing(0);
    // Each stationary HUD host adds 2 px on both sides: 12 + 4 gives the same
    // visible 16 px gap as the navigation buttons use within their shared host.
    auto topbar=new QWidget;topbar->setObjectName(QStringLiteral("topbar"));topbar->setFixedHeight(DashboardChrome::TitleBarHeight);auto top=new QHBoxLayout(topbar);top->setContentsMargins(20,0,8,0);top->setSpacing(12);auto brand=new BrandMark;top->addWidget(depthPlane(brand,Kda::DepthLayer::Hud,"brandHudPlane"));
    auto navigationGroup=new QFrame;navigationGroup->setObjectName(QStringLiteral("navigationGroup"));navigationGroup->setFixedHeight(34);auto navigationLayout=new QHBoxLayout(navigationGroup);navigationLayout->setContentsMargins(0,3,0,3);navigationLayout->setSpacing(16);
    matchNavigation=button(QStringLiteral("主页"),"navigation");settingsNavigation=button(QStringLiteral("设置"),"navigation");historyNavigation=button(QStringLiteral("战绩"),"navHistory");historyNavigation->setProperty("dashboardNavigation",true);for(auto nav:{matchNavigation,historyNavigation,settingsNavigation}){nav->setCheckable(true);nav->setFixedSize(64,28);nav->setProperty("labelPixels",13);nav->setProperty("labelRegular",false);navigationLayout->addWidget(nav);}top->addWidget(depthPlane(navigationGroup,Kda::DepthLayer::Hud,"navigationHudPlane"));top->addStretch(1);
    chrome=new DashboardChrome(this,topbar);top->addWidget(depthPlane(chrome->controls(),Kda::DepthLayer::Hud,"chromeHudPlane"));navigationRail=new NavigationRail(navigationGroup);shell->addWidget(topbar);
    auto statusBar=new QWidget;statusBar->setObjectName(QStringLiteral("dashboardStatusBar"));statusBar->setFixedHeight(26);Kda::markSharedGlassSurface(statusBar);auto statusRow=new QHBoxLayout(statusBar);statusRow->setContentsMargins(10,0,4,0);statusRow->setSpacing(6);
    connectionLabel=line("dataStatus",QStringLiteral("○ 等待客户端"));connectionLabel->setMinimumWidth(124);statusRow->addWidget(connectionLabel,1);
    auto tutorialAction=button(QStringLiteral("教程"),QStringLiteral("openTutorial"));
    tutorialAction->setAccessibleName(QStringLiteral("教程"));
    tutorialAction->setToolTip(QStringLiteral("打开图解教程"));
    tutorialAction->setProperty("toolbarAction",true);
    tutorialAction->setFixedHeight(Kda::Density::controlHeight);
    statusRow->addWidget(tutorialAction);
    auto hudLayoutAction=button(QStringLiteral("HUD 管理"));hudLayoutAction->setObjectName(QStringLiteral("openHudLayout"));hudLayoutAction->setProperty("toolbarAction",true);hudLayoutAction->setToolTip(QStringLiteral("全屏调整浮层的位置与大小"));hudLayoutAction->setFixedHeight(Kda::Density::controlHeight);statusRow->addWidget(hudLayoutAction);connect(hudLayoutAction,&QPushButton::clicked,this,[this]{if(onOpenHudEditor)onOpenHudEditor();});
    auto replay=button(QStringLiteral("本局记录"));replay->setObjectName(QStringLiteral("openReplay"));replay->setToolTip(QStringLiteral("查看本局已记录的信息"));visibilityAction=button(QStringLiteral("浮层显隐"));visibilityAction->setObjectName(QStringLiteral("toggleOverlay"));visibilityAction->setToolTip(QStringLiteral("显示或隐藏游戏浮层 · Ctrl + Alt + F8"));for(auto action:{replay,visibilityAction}){action->setFixedHeight(Kda::Density::controlHeight);action->setProperty("toolbarAction",true);statusRow->addWidget(action);}
    auto workspace=new FacetedWorkspace;workspace->setObjectName(QStringLiteral("dashboardWorkspace"));auto workspaceLayout=new QVBoxLayout(workspace);workspaceLayout->setContentsMargins(0,0,0,0);workspaceLayout->setSpacing(0);shell->addWidget(workspace,1);
    pages=new QStackedWidget;
    pages->setObjectName(QStringLiteral("dashboardPages"));
    workspaceLayout->addWidget(pages,1);
    workspaceLayout->addWidget(depthPlane(statusBar,Kda::DepthLayer::Hud,"statusHudPlane"));
    matchPage=new QWidget;matchPage->setObjectName(QStringLiteral("pageContent"));auto matchLayout=column(matchPage,Kda::Density::pageInset,Kda::Density::sectionGap);playerHistory=new HistoryPane(catalog);playerHistory->onProfileOpened=[this]{selectPage(playerHistory->profileVisible()?2:0);};matchBoard=new QWidget;auto main=column(matchBoard,0,4);matchLayout->addWidget(depthPlane(matchBoard,Kda::DepthLayer::Readout,"homeReadoutPlane"),1);
    auto heading=new QHBoxLayout;heading->setSpacing(8);matchTitle=line("pageTitle",QStringLiteral("等待对局"));matchTitle->setFixedWidth(160);matchSubtitle=line("subTitle",QStringLiteral("进入选人后自动分析"));heading->addWidget(matchTitle);heading->addWidget(matchSubtitle,1);
    scoreLabel=line("score",QStringLiteral("—  :  —"));scoreLabel->setAlignment(Qt::AlignRight|Qt::AlignVCenter);scoreLabel->setFixedWidth(90);scoreLabel->setToolTip(QStringLiteral("我方击杀 : 对手击杀"));heading->addWidget(scoreLabel);
    timeLabel=line("clock",QStringLiteral("等待开始"));timeLabel->setAlignment(Qt::AlignCenter);timeLabel->setFixedWidth(64);heading->addWidget(timeLabel);
    for(const auto &title:{QStringLiteral("选人"),QStringLiteral("加载"),QStringLiteral("局内")}){auto label=line("phase",title);label->setFixedWidth(48);label->setAlignment(Qt::AlignCenter);phases.append(label);heading->addWidget(label);}main->addLayout(heading);
    champSelectView=new ChampSelectView(catalog,assets);champSelectView->setFixedHeight(140);main->addWidget(champSelectView);champSelectView->hide();
    auto scoreBar=section("scoreBar");purchaseOverview=scoreBar;scoreBar->setAccessibleName(QStringLiteral("装备推荐"));scoreBar->setFixedHeight(82);auto scoreRow=new QHBoxLayout(scoreBar);scoreRow->setContentsMargins(12,8,12,8);scoreRow->setSpacing(12);
    actionIcon=new QLabel(QStringLiteral("◇"));actionIcon->setObjectName(QStringLiteral("portrait"));actionIcon->setFixedSize(56,56);actionIcon->setAlignment(Qt::AlignCenter);scoreRow->addWidget(actionIcon);auto actionCopy=new QVBoxLayout;actionCopy->setContentsMargins(0,0,0,0);actionCopy->setSpacing(1);actionTitle=line("actionTitle",QStringLiteral("等待局内推荐"));actionDetail=line("actionDetail",QStringLiteral("组件估价 · —"));routeLabel=line("route",QStringLiteral("成长联动 · —"));actionCopy->addWidget(actionTitle);actionCopy->addWidget(actionDetail);actionCopy->addWidget(routeLabel);scoreRow->addLayout(actionCopy,1);priceLabel=line("purchasePrice",QStringLiteral("—"));priceLabel->setAlignment(Qt::AlignRight|Qt::AlignVCenter);priceLabel->setMaximumWidth(120);scoreRow->addWidget(priceLabel);main->addWidget(scoreBar);
    auto coverageRow=new QHBoxLayout;coverageRow->setSpacing(8);
    coverageLabel=line("dataStatus",QStringLiteral("对局资料 · 等待阵容公开"));coverageLabel->setMinimumHeight(16);coverageRow->addWidget(coverageLabel,1);
    auto refreshMatch=button(QStringLiteral("刷新数据"));refreshMatch->setObjectName(QStringLiteral("refreshMatchData"));refreshMatch->setProperty("toolbarAction",true);refreshMatch->setToolTip(QStringLiteral("重新读取客户端阵容与本局实时数据；读取结果以页面来源与时效提示为准"));coverageRow->addWidget(refreshMatch);
    connect(refreshMatch,&QPushButton::clicked,this,[this]{setMatchRefreshStatus(onRefreshMatchData?onRefreshMatchData():QStringLiteral("当前无法刷新"));});
    matchInputsAction=button(QStringLiteral("记录本局信息"));matchInputsAction->setObjectName(QStringLiteral("completeMatchInputs"));matchInputsAction->setAccessibleName(QStringLiteral("记录本局信息"));matchInputsAction->setToolTip(QStringLiteral("可选：手动记录已选海克斯；不影响查看已观测到的阵容、金币与配方参考"));matchInputsAction->setProperty("toolbarAction",true);matchInputsAction->setFixedHeight(Kda::Density::controlHeight);coverageRow->addWidget(matchInputsAction);
    connect(matchInputsAction,&QPushButton::clicked,this,[this]{if(onCompleteMatchInputs)onCompleteMatchInputs();});
    auto teams=new QHBoxLayout;teams->setSpacing(Kda::Density::sectionGap);
    for(bool ally:{true,false}){auto panel=section(ally?"teamBluePanel":"teamRedPanel");auto team=column(panel,8,0);team->setContentsMargins(8,4,8,4);auto teamTitle=line(ally?"teamBlue":"teamRed",ally?QStringLiteral("我方阵容"):QStringLiteral("对手阵容"));teamTitle->setFixedHeight(20);team->addWidget(teamTitle);
        for(int index=0;index<5;++index){Seat seat;seat.body=section("seat");seat.body->setFixedHeight(38);auto row=new QHBoxLayout(seat.body);row->setContentsMargins(0,1,0,1);row->setSpacing(6);seat.icon=new QLabel(QStringLiteral("·"));seat.icon->setObjectName(QStringLiteral("portrait"));seat.icon->setFixedSize(30,30);seat.icon->setAlignment(Qt::AlignCenter);row->addWidget(seat.icon);auto copy=new QVBoxLayout;copy->setContentsMargins(0,0,0,0);copy->setSpacing(0);copy->addStretch(1);auto titleRow=new QHBoxLayout;titleRow->setSpacing(4);seat.name=line("seatName",QStringLiteral("等待公开"));seat.marker=line("seatMarker");seat.marker->setAlignment(Qt::AlignRight);seat.marker->setFixedWidth(75);titleRow->addWidget(seat.name,1);titleRow->addWidget(seat.marker);copy->addLayout(titleRow);seat.detail=line("seatDetail",QStringLiteral("—"));copy->addWidget(seat.detail);copy->addStretch(1);row->addLayout(copy,1);team->addWidget(seat.body,1);(ally?allies:enemies).append(seat);}teams->addWidget(panel,1);
    }main->addLayout(teams);
    main->addLayout(coverageRow);
    rosterSummary=line("rosterSummary",QStringLiteral("阵容摘要 · 等待选人"));rosterSummary->setMinimumHeight(16);main->addWidget(rosterSummary);
    auto recentSummary=line("rosterRecentResultsSummary");recentSummary->setMinimumHeight(16);recentSummary->hide();main->addWidget(recentSummary);
    auto sourceRow=new QHBoxLayout;sourceRow->setSpacing(12);
    sourceLabel=line("dataStatus",QStringLiteral("客户端 等待 · 实时 等待 · 海克斯 等待 · 版本 —"));sourceLabel->setMinimumHeight(16);sourceRow->addWidget(sourceLabel,1);
    matchRefreshStatus=line("matchRefreshStatus");matchRefreshStatus->setAccessibleName(QStringLiteral("对局数据刷新状态"));matchRefreshStatus->setAlignment(Qt::AlignRight|Qt::AlignVCenter);matchRefreshStatus->setMaximumWidth(192);matchRefreshStatus->setMinimumHeight(16);matchRefreshStatus->hide();sourceRow->addWidget(matchRefreshStatus);main->addLayout(sourceRow);main->addStretch(1);
    waitingPlayer=new WaitingPlayerView;matchLayout->addWidget(waitingPlayer,1);
    idleState=new AmbientStage;idleState->setObjectName(QStringLiteral("idleState"));auto idleLayout=column(idleState,0,0);
    auto idleContent=new QWidget;idleContent->setObjectName(QStringLiteral("personalHomeContent"));idleContent->setMinimumWidth(0);idleContent->setMaximumWidth(1100);auto idleCopy=column(idleContent,0,5);idleCopy->addWidget(new SatinBanner);
    personalHome=new PersonalHomeView(catalog);idleCopy->addWidget(personalHome,1);
    archiveUpdated=line("archiveUpdated");archiveUpdated->hide();
    auto personalPlane=depthPlane(idleContent,Kda::DepthLayer::Readout,"personalHomePlane");personalPlane->setProperty("kdaDepthStationary",true);
    auto idleRow=new QHBoxLayout;idleRow->setContentsMargins(0,0,0,0);idleRow->setSpacing(0);idleRow->addStretch(1);idleRow->addWidget(personalPlane,1000);idleRow->addStretch(1);idleLayout->addLayout(idleRow,1);matchLayout->addWidget(idleState,1);matchScroll=scrollPage(matchPage);homePages=new QStackedWidget;homePages->setObjectName(QStringLiteral("homeContentPages"));homePages->addWidget(matchScroll);pages->addWidget(homePages);
    connect(playerHistory,&HistoryPane::personalProfileUpdated,this,[this]{updatePersonalHome();});
    personalHome->onOpenAll=[this]{if(inSelfCheck||startupNoticeActive)return;if(!playerHistory->openPersonalMatch())playerHistory->personal();selectPage(2);};
    personalHome->onMatchRequested=[this](const QString &ref){if(inSelfCheck||startupNoticeActive)return;if(!playerHistory->openPersonalMatch(ref))playerHistory->personal();selectPage(2);};
    auto settingsShell=new QWidget;
    auto settingsShellLayout=new QHBoxLayout(settingsShell);
    settingsShellLayout->setContentsMargins(0,0,0,0);
    settingsShellLayout->setSpacing(Kda::Density::sectionGap);
    settingsPage=new QWidget;settingsPage->setObjectName(QStringLiteral("pageContent"));auto settingsRow=new QHBoxLayout(settingsPage);settingsRow->setContentsMargins(Kda::Density::pageInset,Kda::Density::pageInset,Kda::Density::pageInset,Kda::Density::pageInset);settingsRow->setSpacing(0);
    auto settingsContent=new QWidget;settingsContent->setObjectName(QStringLiteral("settingsContent"));settingsContent->setMaximumWidth(settingsReadingWidth);settingsContent->setMinimumWidth(0);auto settings=column(settingsContent,0,10);settingsRow->addStretch();settingsRow->addWidget(settingsContent,1);settingsRow->addStretch();
    auto settingsHeader=new QWidget;
    settingsHeader->setObjectName(QStringLiteral("settingsHeader"));
    settingsHeader->setFixedWidth(88);
    auto settingsHeading=new QVBoxLayout(settingsHeader);
    settingsHeading->setContentsMargins(Kda::Density::pageInset,Kda::Density::pageInset,0,0);
    settingsHeading->setSpacing(0);
    settingsShellLayout->addWidget(settingsHeader);
    settingsTabs=new Kda::SettingsTabBar;
    settingsTabs->setObjectName(QStringLiteral("settingsTabs"));
    settingsTabs->setAccessibleName(QStringLiteral("设置分类"));
    settingsTabs->addTab(QStringLiteral("常规"));
    settingsTabs->addTab(QStringLiteral("模型"));
    settingsTabs->addTab(QStringLiteral("皮肤"));
    settingsTabs->addTab(QStringLiteral("关于"));
    settingsTabs->addTab(QStringLiteral("鸣谢"));
    settingsTabs->setExpanding(false);
    settingsTabs->setDrawBase(false);
    settingsHeading->addWidget(settingsTabs);
    settingsHeading->addStretch(1);
    auto settingsSections=new CurrentSettingsPageStack;settingsSections->setObjectName(QStringLiteral("settingsSections"));settings->addWidget(settingsSections);settings->addStretch(1);
    auto generalPage=new ResponsiveSettingsPage;generalPage->setObjectName(QStringLiteral("settingsGeneralPage"));settingsSections->addWidget(generalPage);
    auto modelsPage=new Kda::ReadingSurface;
    modelsPage->setObjectName(QStringLiteral("settingsModelsPage"));
    auto modelsLayout=column(modelsPage,0,0);
    settingsSections->addWidget(modelsPage);
    auto selection=new QWidget;
    selection->setObjectName(QStringLiteral("modelSlotSelection"));
    auto selectionLayout=new QGridLayout(selection);
    selectionLayout->setContentsMargins(0,0,0,0);selectionLayout->setSpacing(0);
    modelSlotPreview=new ModelSlotPreview;
    selectionLayout->addWidget(modelSlotPreview,0,0);
    modelSlotRoster=new QWidget;
    modelSlotRoster->setObjectName(QStringLiteral("modelSlotRoster"));
    modelSlotRoster->setFixedHeight(modelThumbnailHeight);
    auto modelChoices=new QHBoxLayout(modelSlotRoster);
    modelChoices->setContentsMargins(modelPageInset,0,modelPageInset,0);
    modelChoices->setSpacing(8);
    selectionLayout->addWidget(modelSlotRoster,0,0,Qt::AlignVCenter);

    modelLockStatus=wrapped("modelLockStatus");
    modelLockButton=button(QStringLiteral("使用已保存配置"),"activateModelSlot");
    modelsLayout->addWidget(selection);

    auto deepseekCard=new ModelSlotCard(QStringLiteral("deepseek"),QStringLiteral("DeepSeek"),QStringLiteral("云端接口"),QStringLiteral(":/lixin/kda/model-slots/deepseek-maid-strip-v1.png"));deepseekCard->setObjectName(QStringLiteral("modelSlotDeepSeekCard"));
    auto localCard=new ModelSlotCard(QStringLiteral("local_openai"),QStringLiteral("本地模型"),QStringLiteral("本地兼容服务"),QStringLiteral(":/lixin/kda/model-slots/local-mint-strip-v1.png"));localCard->setObjectName(QStringLiteral("modelSlotLocalCard"));
    for(auto card:{deepseekCard,localCard}){
        const auto prefix=card==deepseekCard?QStringLiteral("modelSlotDeepSeek"):QStringLiteral("modelSlotLocal");
        auto entry=new QWidget;entry->setObjectName(prefix+QStringLiteral("Entry"));entry->setMaximumWidth(158);
        auto entryLayout=column(entry,0,0);entryLayout->addWidget(card);modelChoices->addWidget(entry,1);
        auto body=new QWidget;body->setObjectName(prefix+QStringLiteral("Body"));body->setProperty("slotId",card->property("slotId"));body->setMinimumWidth(0);column(body,0,0);body->hide();modelSlotBodies.append(body);
        modelSlotCards.append(card);
        connect(card,&QPushButton::clicked,this,[this,card]{
            const auto index=modelSlot->findData(card->property("slotId"));
            if(index<0)return;
            if(index==modelSlot->currentIndex())updateModelConfiguration();
            else modelSlot->setCurrentIndex(index);
        });
    }
    modelChoices->addStretch();
    auto modelDetails=new QWidget;modelDetails->setObjectName(QStringLiteral("modelDetails"));
    auto detailsGrid=new QGridLayout(modelDetails);detailsGrid->setContentsMargins(modelPageInset,modelPageInset,modelPageInset,modelPageInset);
    detailsGrid->setHorizontalSpacing(28);detailsGrid->setVerticalSpacing(10);detailsGrid->setColumnStretch(0,3);detailsGrid->setColumnStretch(1,2);
    auto connectionColumn=new QWidget;connectionColumn->setObjectName(QStringLiteral("modelConnectionColumn"));
    connectionColumn->setMinimumWidth(0);connectionColumn->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    auto connectionLayout=column(connectionColumn,0,0);
    for(auto body:modelSlotBodies)connectionLayout->addWidget(body);
    connectionLayout->addStretch();detailsGrid->addWidget(connectionColumn,0,0,Qt::AlignTop);
    modelsLayout->addWidget(modelDetails);
    auto skinsPage=new Kda::ReadingSurface;skinsPage->setObjectName(QStringLiteral("settingsSkinsPage"));settingsSections->addWidget(skinsPage);auto aboutPage=new VersionAbout::View;settingsSections->addWidget(aboutPage);
    auto acknowledgmentsPage=new Kda::ReadingSurface;
    acknowledgmentsPage->setObjectName(QStringLiteral("settingsAcknowledgmentsPage"));
    auto acknowledgments=column(acknowledgmentsPage,Kda::Density::pageInset,10);
    acknowledgments->addWidget(wrapped("sectionTitle",QStringLiteral("鸣谢")));
    struct Acknowledgment {
        QString name,url,mark;
        bool lightBadge=false;
    };
    const auto addAcknowledgmentGroup=[&](const QString &title,std::initializer_list<Acknowledgment> projects){
        auto group=new QVBoxLayout;
        group->setSpacing(4);
        group->addWidget(wrapped("acknowledgmentGroupTitle",title));
        for(const auto &project:projects){
            auto row=new QHBoxLayout;
            row->setSpacing(10);
            auto logo=new QLabel;
            logo->setObjectName(QStringLiteral("aboutAcknowledgmentLogo"));
            logo->setFixedSize(28,28);
            logo->setAlignment(Qt::AlignCenter);
            if(project.mark.isEmpty()){
                logo->setText(QStringLiteral("字"));
                logo->setProperty("fontSpecimen",true);
                logo->setAccessibleName(project.name+QStringLiteral("字体示例"));
            }else{
                logo->setPixmap(QPixmap(QStringLiteral(":/lixin/kda/brand/")+project.mark));
                logo->setScaledContents(true);
                logo->setProperty("lightBadge",project.lightBadge);
                logo->setAccessibleName(project.name+QStringLiteral("标志"));
            }
            row->addWidget(logo);
            row->addWidget(wrapped("aboutAcknowledgmentName",project.name),1,Qt::AlignVCenter);
            auto address=wrapped("acknowledgmentProjectLink");
            address->setTextFormat(Qt::RichText);
            address->setWordWrap(false);
            const auto displayUrl=project.url.sliced(QStringView(u"https://").size());
            address->setText(QStringLiteral("<a href=\"%1\" style=\"color:#b9c8df\">%2</a>")
                .arg(project.url.toHtmlEscaped(),displayUrl.toHtmlEscaped()));
            address->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
            address->setTextInteractionFlags(Qt::LinksAccessibleByMouse|Qt::LinksAccessibleByKeyboard);
            address->setOpenExternalLinks(true);
            address->setCursor(Qt::PointingHandCursor);
            address->setToolTip(project.url);
            row->addWidget(address,0,Qt::AlignVCenter);
            group->addLayout(row);
        }
        acknowledgments->addLayout(group);
    };
    addAcknowledgmentGroup(QStringLiteral("开源海克斯参考"),{
        {QStringLiteral("RankPeek"),QStringLiteral("https://github.com/wxl11071123/rankpeek"),QStringLiteral("rankpeek-mark.png")},
        {QStringLiteral("ARAM Oracle"),QStringLiteral("https://github.com/fabs133/league-aram-builder"),QStringLiteral("aram-oracle-mark.png")}
    });
    addAcknowledgmentGroup(QStringLiteral("游戏资料工具"),{
        {QStringLiteral("CommunityDragon"),QStringLiteral("https://github.com/CommunityDragon/CDTB"),QStringLiteral("communitydragon-mark.png"),true}
    });
    addAcknowledgmentGroup(QStringLiteral("核心支持"),{
        {QStringLiteral("Qt"),QStringLiteral("https://github.com/qt/qtbase"),QStringLiteral("qt-mark.png")}
    });
    addAcknowledgmentGroup(QStringLiteral("开源字体"),{
        {QStringLiteral("Noto Sans SC"),QStringLiteral("https://fonts.google.com/noto/specimen/Noto+Sans+SC"),{}},
        {QStringLiteral("Inter"),QStringLiteral("https://rsms.me/inter/"),QStringLiteral("inter-mark.png")}
    });
    settingsSections->addWidget(acknowledgmentsPage);
    auto skinLayout=column(skinsPage,Kda::Density::pageInset,10);auto skinHeading=new QHBoxLayout;skinHeading->setSpacing(10);skinHeading->addWidget(wrapped("sectionTitle",QStringLiteral("K/DA 主题")));skinHeading->addStretch(1);skinHeading->addWidget(wrapped("settingsHint",QStringLiteral("游戏浮层")));skinLayout->addLayout(skinHeading);
    auto skinRow=new QGridLayout;skinRow->setSpacing(8);skinRow->setColumnStretch(0,1);skinRow->setColumnStretch(1,1);
    const QStringList skinIds{QStringLiteral("kda"),QStringLiteral("kda_red")};
    const QStringList skinNames{QStringLiteral("K/DA · 银棱"),QStringLiteral("K/DA · 绯焰")};
    const QStringList skinDetails{QStringLiteral("水晶切面 · 银色流光"),QStringLiteral("熔黑镜面 · 绯焰流光")};
    for(int i=0;i<skinIds.size();++i){auto card=new SkinCard(skinIds[i],skinNames[i],skinDetails[i]);card->setProperty("skinId",skinIds[i]);skinChoices.append(card);skinRow->addWidget(card,i/2,i%2);connect(card,&QPushButton::clicked,this,[this,card]{
        QElapsedTimer switchClock;switchClock.start();
        const auto id=card->property("skinId").toString();const bool skinChanged=id!=Kda::skin();const bool reveal=skinTransition&&!inSelfCheck&&id!=Kda::skin();
        if(reveal)static_cast<Kda::SkinTransition*>(skinTransition)->captureBeforeChange();
        const qreal captureMs=switchClock.nsecsElapsed()/1e6;
        Kda::setSkin(id);for(auto choice:skinChoices)choice->setChecked(choice==card);
        const qreal themeMs=switchClock.nsecsElapsed()/1e6-captureMs;
        savePreference("appearance/skin",id);update();if(skinChanged&&!inSelfCheck)Kda::postNotice({QStringLiteral("skin-applied"),(id==QStringLiteral("kda_red")?QStringLiteral("绯焰已应用"):QStringLiteral("银棱已应用")),QStringLiteral("主窗口、HUD 与提醒已同步"),Kda::NoticeKind::Success,3200});
        const qreal preferenceMs=switchClock.nsecsElapsed()/1e6-captureMs-themeMs;
        if(reveal)static_cast<Kda::SkinTransition*>(skinTransition)->revealAfterChange();
        const qreal totalMs=switchClock.nsecsElapsed()/1e6;
        skinSwitchTiming={{"capture_ms",captureMs},{"theme_ms",themeMs},{"preference_ms",preferenceMs},{"prepare_ms",totalMs-captureMs-themeMs-preferenceMs},{"click_handler_ms",totalMs},{"keyed_preference_dispatch",bool(onPreferenceChanged)},{"scope","local_gui_thread_not_display_latency"}};
    });}skinLayout->addLayout(skinRow);
    auto depthPanel=new QWidget;depthPanel->setObjectName(QStringLiteral("settingsMaterialSection"));auto depthLayout=column(depthPanel,0,6);depthLayout->addWidget(wrapped("sectionTitle",QStringLiteral("空间质感")));auto materialOptions=new QHBoxLayout;materialOptions->setContentsMargins(0,0,0,0);materialOptions->setSpacing(10);parallaxEnabled=new PreferenceSwitch(QStringLiteral("三层景深"));parallaxEnabled->setObjectName(QStringLiteral("parallaxEnabled"));materialOptions->addWidget(parallaxEnabled,1);depthLayout->addLayout(materialOptions);depthLayout->addWidget(wrapped("settingsHint",QStringLiteral("背景固定，内容同向响应；文字与操作保持清晰。")));skinLayout->addWidget(depthPanel);skinLayout->addWidget(wrapped("settingsHint",QStringLiteral("需要静止界面时，可在常规中开启「减少动态效果」。")));skinLayout->addStretch(1);
    auto finishRow=new QHBoxLayout;finishRow->setContentsMargins(0,0,0,0);finishRow->setSpacing(10);
    auto finishCaption=wrapped("settingsHint",QStringLiteral("HUD 皮肤浓度"));finishCaption->setWordWrap(false);finishRow->addWidget(finishCaption);
    hudSkinStrength=new Kda::Slider(Qt::Horizontal);hudSkinStrength->setObjectName(QStringLiteral("hudSkinStrength"));hudSkinStrength->setAccessibleName(QStringLiteral("HUD 皮肤浓度"));hudSkinStrength->setRange(0,100);hudSkinStrength->setMinimumWidth(120);hudSkinStrength->setMaximumWidth(360);finishRow->addWidget(hudSkinStrength,1);
    hudSkinStrengthValue=new QLabel;hudSkinStrengthValue->setMinimumWidth(42);hudSkinStrengthValue->setAlignment(Qt::AlignRight|Qt::AlignVCenter);finishRow->addWidget(hudSkinStrengthValue);depthLayout->addLayout(finishRow);
    hudSkinStrength->setToolTip(QStringLiteral("调整所有 HUD 的皮肤光泽与边缘装饰；正文对比度保持稳定。浮层右上角也可快速切换。"));
    connect(hudSkinStrength,&QSlider::valueChanged,this,[this](int value){hudSkinStrengthValue->setText(QString::number(value)+QChar(u'%'));if(savingPreferences)return;Kda::HudStyle::setStrength(value/100.);savePreference("appearance/hudSkinStrength",value/100.);});
    connect(parallaxEnabled,&QCheckBox::toggled,this,[this](bool value){if(savingPreferences)return;Kda::setDepthEnabled(value);savePreference("appearance/parallax",value);});
    frostedGlassEnabled=new PreferenceSwitch(QStringLiteral("柔焦玻璃"));frostedGlassEnabled->setObjectName(QStringLiteral("frostedGlassEnabled"));frostedGlassEnabled->setToolTip(QStringLiteral("轻透背景与柔焦控件；关闭后使用实色背景，文字和图标保持清晰"));materialOptions->addWidget(frostedGlassEnabled,1);setTabOrder(parallaxEnabled,frostedGlassEnabled);
    connect(frostedGlassEnabled,&QCheckBox::toggled,this,[this](bool value){if(savingPreferences)return;Kda::setGlassEnabled(value);savePreference("appearance/frostedGlass",value);});
    overlayBlur=new PreferenceSwitch(QStringLiteral("HUD 与网页背景虚化"));overlayBlur->setObjectName(QStringLiteral("overlayBlurEnabled"));overlayBlur->setChecked(Kda::overlayBlurEnabled());depthLayout->addWidget(overlayBlur);
    overlayBlur->setToolTip(QStringLiteral("联动所有 HUD 和浏览器，只虚化背后的桌面，文字与图标保持清晰。"));
    auto backdropRow=new QHBoxLayout;backdropRow->setSpacing(10);backdropRow->addWidget(wrapped("settingsHint",QStringLiteral("背景浓度")));
    overlayBackdrop=new Kda::Slider(Qt::Horizontal);overlayBackdrop->setObjectName(QStringLiteral("overlayBackdropOpacity"));overlayBackdrop->setAccessibleName(QStringLiteral("HUD 与网页背景浓度"));overlayBackdrop->setRange(22,88);overlayBackdrop->setValue(qRound(Kda::overlayBackdropOpacity()*100));backdropRow->addWidget(overlayBackdrop,1);
    overlayBackdropValue=new QLabel(QString::number(overlayBackdrop->value())+QChar(u'%'));overlayBackdropValue->setMinimumWidth(42);overlayBackdropValue->setAlignment(Qt::AlignRight|Qt::AlignVCenter);backdropRow->addWidget(overlayBackdropValue);depthLayout->addLayout(backdropRow);
    connect(overlayBlur,&QCheckBox::toggled,this,[this](bool value){if(savingPreferences)return;Kda::setOverlayBlurEnabled(value);savePreference("appearance/overlayBlur",value);});
    connect(overlayBackdrop,&QSlider::valueChanged,this,[this](int value){overlayBackdropValue->setText(QString::number(value)+QChar(u'%'));if(savingPreferences)return;Kda::setOverlayBackdropOpacity(value/100.);savePreference("appearance/overlayBackdropOpacity",value/100.);});
    auto generalColumns=new QGridLayout(generalPage);generalColumns->setContentsMargins(Kda::Density::pageInset,Kda::Density::pageInset,Kda::Density::pageInset,Kda::Density::pageInset);generalColumns->setHorizontalSpacing(12);generalColumns->setVerticalSpacing(12);generalColumns->setColumnStretch(0,1);generalColumns->setColumnStretch(1,1);generalColumns->setRowStretch(2,1);

    auto floating=new QWidget;floating->setObjectName(QStringLiteral("settingsFloatingSection"));auto floatingLayout=column(floating,0,6);
    auto floatingHeading=new QHBoxLayout;floatingHeading->setSpacing(10);floatingHeading->addWidget(wrapped("sectionTitle",QStringLiteral("游戏浮层")));floatingHeading->addStretch(1);floatingHeading->addWidget(wrapped("settingsHint",QStringLiteral("局内提示与窗口行为")));floatingLayout->addLayout(floatingHeading);
    shopRecommendationsEnabled=new PreferenceSwitch(QStringLiteral("游戏商店出装页"));shopRecommendationsEnabled->setObjectName(QStringLiteral("shopRecommendationsEnabled"));shopRecommendationsEnabled->setToolTip(QStringLiteral("写入本英雄的已核验出装。对局内打开商店，在装备方案中查找「李金金 · 当前出装」。装备由你购买。"));connect(shopRecommendationsEnabled,&QCheckBox::toggled,this,[this](bool value){savePreference(QStringLiteral("coach/shopRecommendationsEnabled"),value);});
    overlayEnabled=new PreferenceSwitch(QStringLiteral("教练浮层"));cardHintsEnabled=new PreferenceSwitch(QStringLiteral("选牌提示"));autoMinimize=new PreferenceSwitch(QStringLiteral("对局时收起主窗口"));reduceMotion=new PreferenceSwitch(QStringLiteral("减少动态效果"));reduceMotion->setObjectName(QStringLiteral("reduceMotion"));
    auto toggles=new QGridLayout;toggles->setContentsMargins(0,0,0,0);toggles->setHorizontalSpacing(10);toggles->setVerticalSpacing(0);int toggleIndex=0;for(auto control:{overlayEnabled,cardHintsEnabled,shopRecommendationsEnabled,autoMinimize,reduceMotion}){toggles->addWidget(control,toggleIndex,0);++toggleIndex;}toggles->setColumnStretch(0,1);floatingLayout->addLayout(toggles);shopRecommendationsStatus=line("settingsHint",QStringLiteral("进入选人后准备出装资料"));floatingLayout->addWidget(shopRecommendationsStatus);
    auto closeRow=new QHBoxLayout;
    auto closeCaption=wrapped("closeWindowBehaviorCaption",QStringLiteral("关闭主窗口时"));
    closeCaption->setWordWrap(false);
    closeCaption->setContentsMargins(6,0,0,0);
    closeWindowBehavior=new Kda::ComboBox;
    closeWindowBehavior->setObjectName(QStringLiteral("closeWindowBehavior"));
    closeWindowBehavior->setAccessibleName(closeCaption->text());
    closeWindowBehavior->setToolTip(QStringLiteral("选择关闭主窗口后退出助手，或在系统托盘可用时继续运行"));
    closeWindowBehavior->setMinimumWidth(144);
    closeWindowBehavior->addItem(QStringLiteral("退出助手"),false);
    closeWindowBehavior->addItem(QStringLiteral("收起到托盘"),true);
    closeCaption->setBuddy(closeWindowBehavior);
    closeRow->addWidget(closeCaption);
    closeRow->addStretch(1);
    closeRow->addWidget(closeWindowBehavior);
    floatingLayout->addLayout(closeRow);
    connect(closeWindowBehavior,&QComboBox::currentIndexChanged,this,[this]{savePreference(closeToTrayKey,closeWindowBehavior->currentData().toBool());});
    auto opacityWidget=new QWidget;opacityWidget->setFixedHeight(Kda::Density::switchHeight);auto opacityRow=new QHBoxLayout(opacityWidget);opacityRow->setContentsMargins(0,0,0,0);opacityRow->setSpacing(10);opacityCaption=wrapped("opacityCaption",QStringLiteral("不透明度"));opacityCaption->setWordWrap(false);opacityCaption->setSizePolicy(QSizePolicy::Minimum,QSizePolicy::Preferred);opacityRow->addWidget(opacityCaption);opacity=new Kda::Slider(Qt::Horizontal);opacity->setRange(45,100);opacity->setMinimumWidth(100);opacity->setMaximumWidth(360);opacity->setFocusPolicy(Qt::StrongFocus);opacity->setAccessibleName(QStringLiteral("浮层不透明度"));opacityRow->addWidget(opacity,1);opacityValue=new QLabel;opacityValue->setFixedWidth(42);opacityValue->setAlignment(Qt::AlignRight|Qt::AlignVCenter);opacityRow->addWidget(opacityValue);floatingLayout->addWidget(opacityWidget);
    auto floatingActions=new QHBoxLayout;floatingActions->setContentsMargins(0,0,0,0);floatingActions->setSpacing(6);
    auto hudManager=button(QStringLiteral("HUD 管理"));hudManager->setObjectName(QStringLiteral("hudLayoutAction"));hudManager->setAccessibleName(QStringLiteral("HUD 管理"));hudManager->setToolTip(QStringLiteral("全屏调整浮层的位置与大小"));floatingActions->addWidget(hudManager);connect(hudManager,&QPushButton::clicked,this,[this]{if(onOpenHudEditor)onOpenHudEditor();});
    auto reset=button(QStringLiteral("浮层归位"));floatingActions->addWidget(reset);
    uiBrowserAction=button(QStringLiteral("界面浏览"));uiBrowserAction->setObjectName(QStringLiteral("uiBrowserAction"));uiBrowserAction->setFocusPolicy(Qt::StrongFocus);uiBrowserAction->setAccessibleName(QStringLiteral("界面浏览"));uiBrowserAction->setToolTip(QStringLiteral("逐个查看加载、出装、阵容、选牌和阵亡提示"));floatingActions->addWidget(uiBrowserAction);floatingActions->addStretch(1);floatingLayout->addLayout(floatingActions);
    auto readingRow=new QHBoxLayout;readingRow->setContentsMargins(0,2,0,0);readingRow->setSpacing(10);
    growthCaptureAction=button(QStringLiteral("自动读取设置"));growthCaptureAction->setObjectName(QStringLiteral("growthCaptureSettingsAction"));growthCaptureAction->setAccessibleName(QStringLiteral("自动读取设置"));growthCaptureAction->setToolTip(QStringLiteral("定位自己的海克斯与装备图标栏；定位后，正常悬停图标即可读取可见提示"));readingRow->addWidget(growthCaptureAction);
    growthCaptureStatus=line("growthCaptureStatus",QStringLiteral("等候结构化来源"));readingRow->addWidget(growthCaptureStatus,1);floatingLayout->addLayout(readingRow);generalColumns->addWidget(floating,0,0,Qt::AlignTop);

    auto ds=new QWidget;
    ds->setObjectName(QStringLiteral("modelConfigurationEditor"));
    ds->setProperty("modelConfigurationPanel",true);
    modelConfigurationEditor=ds;
    auto dsLayout=column(ds,0,10);
    auto dsHeading=new QHBoxLayout;
    dsHeading->setContentsMargins(0,0,0,0);
    dsHeading->setSpacing(12);
    dsHeading->addWidget(wrapped("sectionTitle",QStringLiteral("连接配置")),1);
    modelDraftState=wrapped("modelDraftState");
    dsHeading->addWidget(modelDraftState);
    rosterDsEnabled=new PreferenceSwitch(QStringLiteral("启用规划"));
    rosterDsEnabled->setMaximumWidth(112);
    dsHeading->addWidget(rosterDsEnabled);
    dsLayout->addLayout(dsHeading);
    auto modelSummary=new QWidget;
    modelSummary->setObjectName(QStringLiteral("modelCurrentSummary"));
    auto summaryRow=new QHBoxLayout(modelSummary);
    summaryRow->setContentsMargins(0,0,0,0);
    summaryRow->setSpacing(12);
    modelCurrentName=wrapped("modelCurrentName");
    modelCurrentProvider=wrapped("modelCurrentProvider");
    modelCurrentState=wrapped("modelCurrentState");
    summaryRow->addWidget(modelCurrentName,1);
    summaryRow->addWidget(modelCurrentProvider);
    summaryRow->addWidget(modelCurrentState);
    dsLayout->addWidget(modelSummary);
    modelSlot=new Kda::ComboBox(modelsPage);modelSlot->setObjectName(QStringLiteral("modelSlot"));modelSlot->setAccessibleName(QStringLiteral("配置模型位"));modelSlot->addItem(QStringLiteral("DeepSeek · 云端接口"),QStringLiteral("deepseek"));modelSlot->addItem(QStringLiteral("本地兼容服务"),QStringLiteral("local_openai"));modelSlot->setMinimumWidth(0);modelSlot->setFocusPolicy(Qt::NoFocus);modelSlot->hide();
    const auto field=[&](QVBoxLayout *layout,QLabel *label,QWidget *control){auto row=new QHBoxLayout;row->setContentsMargins(0,0,0,0);row->setSpacing(10);label->setFixedWidth(42);label->setWordWrap(false);label->setContentsMargins(6,0,0,0);label->setBuddy(control);row->addWidget(label);row->addWidget(control,1);layout->addLayout(row);};
    modelConnectionPages=new CurrentSettingsPageStack;modelConnectionPages->setObjectName(QStringLiteral("modelConnectionPages"));dsLayout->addWidget(modelConnectionPages);
    auto dsFields=new QWidget;dsFields->setObjectName(QStringLiteral("dsConfigurationFields"));auto dsFieldsLayout=column(dsFields,0,6);modelConnectionPages->addWidget(dsFields);
    opacityCaption->setContentsMargins(6,0,0,0);
    apiKey=new Kda::LineEdit;apiKey->setObjectName(QStringLiteral("dsApiKey"));apiKey->setMinimumWidth(0);apiKey->setEchoMode(QLineEdit::Password);apiKey->setMaxLength(4096);apiKey->setPlaceholderText(QStringLiteral("DeepSeek API key，仅保存在本机"));apiKey->setAccessibleName(QStringLiteral("DS API key"));auto apiKeyCaption=wrapped("muted",QStringLiteral("密钥"));apiKeyCaption->setObjectName(QStringLiteral("dsApiKeyCaption"));apiKeyCaption->setToolTip(QStringLiteral("DS API key"));field(dsFieldsLayout,apiKeyCaption,apiKey);
    model=new Kda::ComboBox;model->setObjectName(QStringLiteral("dsModel"));model->setEditable(true);model->setMinimumWidth(0);model->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);model->setMinimumContentsLength(0);model->addItems({"deepseek-flash","deepseek-v4-pro"});model->setAccessibleName(QStringLiteral("DS 模型"));field(dsFieldsLayout,wrapped("muted",QStringLiteral("模型")),model);
    auto localFields=new QWidget;localFields->setObjectName(QStringLiteral("localModelConfigurationFields"));auto localFieldsLayout=column(localFields,0,6);modelConnectionPages->addWidget(localFields);
    localModelBaseUrl=new Kda::LineEdit;localModelBaseUrl->setObjectName(QStringLiteral("localModelBaseUrl"));localModelBaseUrl->setAccessibleName(QStringLiteral("本地服务地址"));localModelBaseUrl->setMinimumWidth(0);localModelBaseUrl->setMaxLength(2048);localModelBaseUrl->setPlaceholderText(QStringLiteral("http://127.0.0.1:1234/v1"));localModelBaseUrl->setToolTip(QStringLiteral("仅连接本机的 HTTP 或 HTTPS 兼容服务，例如 localhost、127.0.0.1 或 ::1。"));field(localFieldsLayout,wrapped("muted",QStringLiteral("地址")),localModelBaseUrl);
    localModelName=new Kda::LineEdit;localModelName->setObjectName(QStringLiteral("localModelName"));localModelName->setAccessibleName(QStringLiteral("本地模型名称"));localModelName->setMinimumWidth(0);localModelName->setMaxLength(256);localModelName->setPlaceholderText(QStringLiteral("填写本地服务提供的模型名称"));field(localFieldsLayout,wrapped("muted",QStringLiteral("模型")),localModelName);
    localModelApiKey=new Kda::LineEdit;localModelApiKey->setObjectName(QStringLiteral("localModelApiKey"));localModelApiKey->setAccessibleName(QStringLiteral("本地服务独立密钥"));localModelApiKey->setMinimumWidth(0);localModelApiKey->setEchoMode(QLineEdit::Password);localModelApiKey->setMaxLength(4096);localModelApiKey->setPlaceholderText(QStringLiteral("可选；留空时不发送密钥"));field(localFieldsLayout,wrapped("muted",QStringLiteral("密钥")),localModelApiKey);
    dsLayout->addWidget(modelLockStatus);
    auto saveRow=new QHBoxLayout;saveRow->setContentsMargins(0,2,0,0);saveRow->setSpacing(8);auto save=button(QStringLiteral("保存模型配置"),"primary");modelSaveButton=save;save->setFixedHeight(Kda::Density::controlHeight);save->setProperty("primaryAction",true);saveRow->addWidget(save);saveRow->addWidget(modelLockButton);saveRow->addStretch();dsLayout->addLayout(saveRow);
    modelSaveStatus=wrapped("modelSaveStatus");modelSaveStatus->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);dsLayout->addWidget(modelSaveStatus);
    saveStatus=wrapped("saveStatus");saveStatus->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);floatingLayout->addWidget(saveStatus);
    auto usageSection=new QWidget;
    usageSection->setObjectName(QStringLiteral("modelUsageSection"));
    auto usageLayout=column(usageSection,0,8);
    usageSection->setMinimumWidth(0);usageSection->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    detailsGrid->addWidget(usageSection,0,1,Qt::AlignTop);
    auto usageHeading=wrapped("sectionTitle",QStringLiteral("DeepSeek 用量"));usageHeading->setFixedHeight(Kda::Density::switchHeight);usageLayout->addWidget(usageHeading);modelUsageScope=wrapped("modelUsageScope");usageLayout->addWidget(modelUsageScope);
    auto usage=new UsageCostSummary;usage->setObjectName(QStringLiteral("dsUsagePanel"));
    dsUsageCurrent=wrapped("dsUsageCurrent");dsUsageTokens=wrapped("dsUsageTokens");dsUsageCalls=wrapped("dsUsageCalls");
    dsUsageAverage=wrapped("dsUsageAverage");dsUsageSamples=wrapped("dsUsageSamples");dsUsagePersistence=wrapped("dsUsagePersistence");
    auto currentMetric=new QWidget;currentMetric->setObjectName(QStringLiteral("dsUsageCurrentMetric"));auto currentMetricLayout=column(currentMetric,0,3);auto currentCaption=new QHBoxLayout;currentCaption->setContentsMargins(0,0,0,0);currentCaption->addWidget(wrapped("dsUsageCaption",QStringLiteral("本局已记录")),1);
    dsUsageState=wrapped("dsUsageState");dsUsageState->setWordWrap(false);dsUsageState->setFixedHeight(16);dsUsageState->setAlignment(Qt::AlignCenter);currentCaption->addWidget(dsUsageState);currentMetricLayout->addLayout(currentCaption);
    dsUsageCurrent->setAccessibleName(QStringLiteral("本局已记录费用"));currentMetricLayout->addWidget(dsUsageCurrent);currentMetricLayout->addWidget(dsUsageTokens);currentMetricLayout->addWidget(dsUsageCalls);
    auto averageMetric=new QWidget;averageMetric->setObjectName(QStringLiteral("dsUsageAverageMetric"));auto averageMetricLayout=column(averageMetric,0,3);auto averageCaption=wrapped("dsUsageCaption",QStringLiteral("平均每局"));averageCaption->setMinimumHeight(16);averageMetricLayout->addWidget(averageCaption);
    dsUsageAverage->setAccessibleName(QStringLiteral("平均每局费用"));averageMetricLayout->addWidget(dsUsageAverage);averageMetricLayout->addWidget(dsUsageSamples);
    auto usageDetails=new QWidget;auto usageDetailsLayout=column(usageDetails,0,3);auto usageNote=wrapped("dsUsageNote",QStringLiteral("费用为接口用量估算，非账单"));usageNote->setToolTip(QStringLiteral("按接口用量估算，非账单；本地网页搜索不另计费"));usageDetailsLayout->addWidget(usageNote);
    usageDetailsLayout->addWidget(dsUsagePersistence);dsUsagePersistence->hide();usage->setContents(currentMetric,averageMetric,usageDetails);usageLayout->addWidget(usage);updateDsUsage();
    auto researchHint=wrapped("settingsHint",QStringLiteral("自研专用模型 · 研究中"));researchHint->setToolTip(QStringLiteral("自研专用模型暂不参与对局规划"));detailsGrid->addWidget(researchHint,1,0,1,2);modelsLayout->addStretch(1);
    connect(modelSlot,&QComboBox::currentIndexChanged,this,[this]{if(!savingPreferences)modelSaveStatus->clear();updateModelConfiguration();});
    connect(modelLockButton,&QPushButton::clicked,this,[this]{activateModelSlot(modelSlot->currentData().toString());});
    for(auto edit:{apiKey,localModelBaseUrl,localModelName,localModelApiKey})connect(edit,&QLineEdit::textChanged,this,[this]{if(!savingPreferences)modelSaveStatus->clear();updateModelConfiguration();});
    connect(model,&QComboBox::currentTextChanged,this,[this]{if(!savingPreferences)modelSaveStatus->clear();updateModelConfiguration();});connect(rosterDsEnabled,&QCheckBox::toggled,this,[this]{updateModelConfiguration();});

    auto shortcuts=new QWidget;auto shortcutLayout=column(shortcuts,0,2);auto toolsHeading=new QHBoxLayout;toolsHeading->setContentsMargins(0,0,0,0);toolsHeading->addWidget(wrapped("sectionTitle",QStringLiteral("快捷键")));toolsHeading->addStretch(1);auto quit=button(QStringLiteral("退出助手"),"quiet");toolsHeading->addWidget(quit);shortcutLayout->addLayout(toolsHeading);auto keyGrid=new QGridLayout;keyGrid->setContentsMargins(0,0,0,0);keyGrid->setHorizontalSpacing(24);keyGrid->setVerticalSpacing(2);int shortcutIndex=0;for(const auto &label:{QStringLiteral("打开主窗口"),QStringLiteral("浮层归位"),QStringLiteral("浮层显隐"),QStringLiteral("鼠标穿透")}){auto row=new QHBoxLayout;row->setSpacing(8);auto caption=wrapped("muted",label);caption->setFixedHeight(20);row->addWidget(caption,1);auto key=wrapped("muted",QStringLiteral("Ctrl + Alt + F%1").arg(6+shortcutIndex));key->setAlignment(Qt::AlignRight|Qt::AlignVCenter);key->setWordWrap(false);row->addWidget(key);keyGrid->addLayout(row,shortcutIndex/2,shortcutIndex%2);++shortcutIndex;}keyGrid->setColumnStretch(0,1);keyGrid->setColumnStretch(1,1);shortcutLayout->addLayout(keyGrid);generalColumns->addWidget(shortcuts,1,0,1,2,Qt::AlignTop);settingsScroll=new CenteredSettingsScroll(settingsPage);settingsShellLayout->addWidget(settingsScroll,1);pages->addWidget(depthPlane(settingsShell,Kda::DepthLayer::Hud,"settingsHudPlane"));auto settingsMotion=new Kda::SettingsPageMotion(static_cast<Kda::SettingsTabBar*>(settingsTabs),settingsSections,settingsScroll);settingsMotion->aboutEntrance=[about=QPointer<VersionAbout::View>(aboutPage)](bool play){if(about){if(play)about->playEntrance();else about->finishEntrance();}};settingsMotion->aboutReport=[about=QPointer<VersionAbout::View>(aboutPage)]{return about?about->entranceReport():QJsonObject{};};
    generalPage->setPanels(floating,nullptr,shortcuts);
    connect(settingsTabs,&QTabBar::currentChanged,this,[this](int index){
        if(index==settingsModelsTab&&pages->currentIndex()==1&&!inSelfCheck&&!startupNoticeActive)
            loadModelConfiguration(false);
    });

    auto historyPage=new QWidget;historyPage->setObjectName("historyPage");auto historyLayout=column(historyPage,Kda::Density::pageInset,Kda::Density::sectionGap);playerHistory->setMaximumWidth(1100);auto historyRow=new QHBoxLayout;historyRow->setContentsMargins(0,0,0,0);historyRow->setSpacing(0);historyRow->addStretch(1);historyRow->addWidget(playerHistory,1000);historyRow->addStretch(1);historyLayout->addLayout(historyRow,1);pages->addWidget(historyPage);
    connect(tutorialAction,&QPushButton::clicked,this,&CoachDashboard::showTutorial);
    connect(historyNavigation,&QPushButton::clicked,playerHistory,&HistoryPane::openArchive);
    connect(matchNavigation,&QPushButton::clicked,this,[this]{dismissDecisionReplay();playerHistory->closeProfile();if(homePages)homePages->setCurrentIndex(0);selectPage(0);});connect(settingsNavigation,&QPushButton::clicked,this,[this]{selectPage(1);});connect(quit,&QPushButton::clicked,this,[this]{if(onQuit)onQuit();});connect(visibilityAction,&QPushButton::clicked,this,[this]{if(onVisibilityToggle)onVisibilityToggle();});connect(reset,&QPushButton::clicked,this,[this]{if(onResetOverlay)onResetOverlay();});
    connect(replay,&QPushButton::clicked,this,[this]{if(onOpenReplay)onOpenReplay();});
    connect(uiBrowserAction,&QPushButton::clicked,this,[this]{if(onOpenUiBrowser)onOpenUiBrowser();});
    connect(growthCaptureAction,&QPushButton::clicked,this,[this]{if(onGrowthCaptureSettings)onGrowthCaptureSettings();});
    chrome->setPinned(preferences.value("appearance/alwaysOnTop",false).toBool());
    connect(chrome->controls()->findChild<QAbstractButton*>(QStringLiteral("chromePin")), &QAbstractButton::clicked,
            this, [this](bool pinned) { if (chrome->setPinned(pinned)) savePreference("appearance/alwaysOnTop",pinned); });
    connect(overlayEnabled,&QCheckBox::toggled,this,[this](bool value){savePreference("coach/overlayEnabled",value);});connect(cardHintsEnabled,&QCheckBox::toggled,this,[this](bool value){savePreference("coach/cardHintsEnabled",value);});connect(rosterDsEnabled,&QCheckBox::toggled,this,[this](bool value){savePreference("coach/rosterDsEnabled",value);});connect(autoMinimize,&QCheckBox::toggled,this,[this](bool value){savePreference("coach/autoMinimize",value);});connect(opacity,&QSlider::valueChanged,this,[this](int value){opacityValue->setText(QString::number(value)+QChar(u'%'));savePreference("right/opacity",value/100.);});connect(save,&QPushButton::clicked,this,[this]{saveDsSettings();});
    setTabOrder(matchNavigation,historyNavigation);setTabOrder(historyNavigation,settingsNavigation);setTabOrder(settingsNavigation,tutorialAction);setTabOrder(tutorialAction,hudLayoutAction);setTabOrder(hudLayoutAction,replay);setTabOrder(replay,visibilityAction);setTabOrder(visibilityAction,overlayEnabled);setTabOrder(overlayEnabled,cardHintsEnabled);setTabOrder(cardHintsEnabled,autoMinimize);setTabOrder(autoMinimize,reduceMotion);setTabOrder(reduceMotion,opacity);setTabOrder(opacity,reset);setTabOrder(reset,uiBrowserAction);setTabOrder(uiBrowserAction,growthCaptureAction);
    setTabOrder(growthCaptureAction,deepseekCard);setTabOrder(deepseekCard,localCard);setTabOrder(localCard,modelLockButton);setTabOrder(modelLockButton,rosterDsEnabled);setTabOrder(rosterDsEnabled,apiKey);
    setTabOrder(apiKey,model);setTabOrder(model,localModelBaseUrl);setTabOrder(localModelBaseUrl,localModelName);setTabOrder(localModelName,localModelApiKey);setTabOrder(localModelApiKey,save);setTabOrder(save,quit);
    matchNavigation->setObjectName(QStringLiteral("navMatch"));settingsNavigation->setObjectName(QStringLiteral("navSettings"));matchNavigation->setProperty("dashboardNavigation",true);settingsNavigation->setProperty("dashboardNavigation",true);
    overlayEnabled->setObjectName(QStringLiteral("overlayEnabled"));cardHintsEnabled->setObjectName(QStringLiteral("cardHintsEnabled"));rosterDsEnabled->setObjectName(QStringLiteral("rosterDsEnabled"));autoMinimize->setObjectName(QStringLiteral("autoMinimize"));opacity->setObjectName(QStringLiteral("overlayOpacity"));apiKey->setObjectName(QStringLiteral("dsApiKey"));model->setObjectName(QStringLiteral("dsModel"));save->setObjectName(QStringLiteral("saveDs"));save->setProperty("primaryAction",true);for(auto control:{save,matchNavigation,settingsNavigation}){control->style()->unpolish(control);control->style()->polish(control);control->updateGeometry();}
    auto depthScene=new Kda::DepthScene(this);
    // Preserve titlebar layout hosts, but keep all window chrome outside pointer depth.
    for(auto widget:findChildren<QWidget*>())if(auto plane=dynamic_cast<Kda::DepthPlane*>(widget))if(!topbar->isAncestorOf(plane))depthScene->add(plane);
    workspace->setDepthScene(depthScene);
    skinTransition=new Kda::SkinTransition(this);
    appearanceSave=new Kda::AppearanceSave(preferences,this,[this](bool saved){saveStatus->setText(saved?QStringLiteral("已保存"):QStringLiteral("偏好保存失败"));});
    connect(reduceMotion,&QCheckBox::toggled,this,[this](bool value){if(savingPreferences)return;Kda::setReducedMotion(value);savePreference("appearance/reducedMotion",value);});loadControls(true);selectPage(0);
    if(startupNoticeActive){navigationGroup->hide();workspace->hide();if(auto timer=playerHistory->findChild<QTimer*>(QStringLiteral("playerHistoryRevisionTimer"))){startupRevisionTimerWasActive=timer->isActive();timer->stop();}}
    else updateView();
    for(auto widget:findChildren<QWidget*>())widget->setMouseTracking(true);
    Kda::installNoticeHost(this,10);
    if(qApp)qApp->installEventFilter(this);
    connect(Kda::overlayAppearanceEvents(),&Kda::OverlayAppearanceEvents::changed,this,[this]{
        const QSignalBlocker quietBlur(overlayBlur),quietOpacity(overlayBackdrop);
        overlayBlur->setChecked(Kda::overlayBlurEnabled());overlayBackdrop->setValue(qRound(Kda::overlayBackdropOpacity()*100));overlayBackdropValue->setText(QString::number(overlayBackdrop->value())+QChar(u'%'));
    });
    auto *inactiveEffect = new InactiveDashboardEffect(this);
    inactiveEffect->setInactive(!isActiveWindow(), false);
    setGraphicsEffect(inactiveEffect);
#ifdef Q_OS_WIN
    QLibrary dwm(QStringLiteral("dwmapi"));using SetAttribute=HRESULT(WINAPI *)(HWND,DWORD,LPCVOID,DWORD);if(const auto setAttribute=reinterpret_cast<SetAttribute>(dwm.resolve("DwmSetWindowAttribute"))){const BOOL dark=TRUE;setAttribute(reinterpret_cast<HWND>(winId()),20,&dark,sizeof(dark));}
#endif
}

bool CoachDashboard::startupNoticePending()const{return startupNoticeActive;}

void CoachDashboard::beginStartupNotice(std::function<void()> accepted,std::function<void()> rejected){
    if(startupNoticePage){startupNoticePage->focusExit();return;}
    auto navigation=findChild<QWidget*>(QStringLiteral("navigationGroup"));auto workspace=findChild<QWidget*>(QStringLiteral("dashboardWorkspace"));
    if(!startupNoticeActive){startupNavigationWasVisible=navigation&&!navigation->isHidden();startupWorkspaceWasVisible=workspace&&!workspace->isHidden();}
    startupNoticeActive=true;startupAccepted=std::move(accepted);startupRejected=std::move(rejected);
    if(navigation)navigation->hide();if(workspace)workspace->hide();
    if(auto timer=playerHistory->findChild<QTimer*>(QStringLiteral("playerHistoryRevisionTimer"))){startupRevisionTimerWasActive|=timer->isActive();timer->stop();}
    startupNoticePage=new StartupNoticePage(centralWidget());
    startupNoticePage->onAccepted=[this]{finishStartupNotice(true);};startupNoticePage->onRejected=[this]{finishStartupNotice(false);};
    static_cast<QVBoxLayout*>(centralWidget()->layout())->addWidget(startupNoticePage,1);startupNoticePage->show();startupNoticePage->focusExit();
}

void CoachDashboard::finishStartupNotice(bool accepted){
    if(!startupNoticeActive)return;
    startupNoticeActive=false;auto callback=accepted?std::move(startupAccepted):std::move(startupRejected);startupAccepted={};startupRejected={};
    if(!accepted)hide();
    if(startupNoticePage){auto retired=startupNoticePage;startupNoticePage=nullptr;retired->onAccepted={};retired->onRejected={};centralWidget()->layout()->removeWidget(retired);retired->hide();retired->setObjectName(QStringLiteral("retiredStartupNoticePage"));retired->deleteLater();}
    if(auto navigation=findChild<QWidget*>(QStringLiteral("navigationGroup")))navigation->setVisible(startupNavigationWasVisible);
    if(auto workspace=findChild<QWidget*>(QStringLiteral("dashboardWorkspace")))workspace->setVisible(startupWorkspaceWasVisible);
    if(accepted&&startupRevisionTimerWasActive)if(auto timer=playerHistory->findChild<QTimer*>(QStringLiteral("playerHistoryRevisionTimer")))timer->start();
    startupRevisionTimerWasActive=false;
    if(accepted){
        playerHistory->setAccountScope(localAccountScope);
        firstTutorialPending=!preferences.value(firstTutorialKey,false).toBool();
        if(firstTutorialPending)QTimer::singleShot(0,this,&CoachDashboard::showFirstTutorial);
    }
    if(callback)callback();
}

void CoachDashboard::showTutorial(){
    if(startupNoticeActive)return;
    auto *navigation=findChild<QWidget*>(QStringLiteral("navigationGroup"));
    if(!tutorial){
        auto *workspace=findChild<QWidget*>(QStringLiteral("dashboardWorkspace"));
        if(!workspace)return;
        tutorial=new DashboardTutorial(workspace);
        tutorial->onDismiss=[this,navigation]{
            if(navigation)navigation->setEnabled(true);
            if(auto entry=findChild<QPushButton*>(QStringLiteral("openTutorial")))entry->setFocus(Qt::OtherFocusReason);
        };
        tutorial->onOpenHudEditor=[this]{
            if(!onOpenHudEditor)return;
            const QPointer<DashboardTutorial> guide(tutorial);
            guide->hide();
            onOpenHudEditor();
            if(guide)guide->present();
        };
        tutorial->onOpenPreview=[this]{if(onOpenUiBrowser)onOpenUiBrowser();};
    }
    if(navigation)navigation->setEnabled(false);
    tutorial->present();
}

void CoachDashboard::showFirstTutorial(){
    if(!firstTutorialPending||startupNoticeActive||!isVisible()||isMinimized())return;
    firstTutorialPending=false;
    // The first visible presentation consumes the prompt; closing early still
    // leaves the same guide available through the toolbar.
    showTutorial();
    preferences.setValue(firstTutorialKey,true);
    preferences.sync();
}

void CoachDashboard::selectPage(int index){
    index=std::clamp(index,0,2);
    if(index!=0)dismissDecisionReplay();
    const bool archive=!archivedReplayMatch.isEmpty();
    const bool enteringSettings=index==1&&pages->currentIndex()!=1;pages->setCurrentIndex(index);
    matchNavigation->setChecked(index==0&&!archive);settingsNavigation->setChecked(index==1);historyNavigation->setChecked(index==2||archive);
    if(navigationRail)static_cast<NavigationRail*>(navigationRail)->follow(index==1?settingsNavigation:index==2||archive?historyNavigation:matchNavigation,true);
    if(enteringSettings&&!inSelfCheck&&!startupNoticeActive)loadControls();
    if(enteringSettings&&!inSelfCheck&&!startupNoticeActive)if(auto motion=findChild<QWidget*>(QStringLiteral("settingsPageMotion")))static_cast<Kda::SettingsPageMotion*>(motion)->replayAboutEntrance();
    if(index==0&&homeViewPending&&!startupNoticeActive)updateView();
    if(index==0&&!archive&&!inSelfCheck&&!startupNoticeActive&&personalHome){playerHistory->refreshPersonalHome();updatePersonalHome();}
}

QString CoachDashboard::decisionArchiveDirectory(const QString &root,const QString &accountScope){
    static const QRegularExpression scopePattern(QStringLiteral("^[a-f0-9]{64}$"));
    if(root.isEmpty()||!scopePattern.match(accountScope).hasMatch())return {};
    return QDir(root).filePath(QStringLiteral(".local/augment-panel/decision-replay/")+accountScope);
}
void CoachDashboard::decisionArchiveChanged(){playerHistory->refreshDecisionReplay();}
void CoachDashboard::setReplayWidget(CoachReplay *widget){
    if(!widget||replayPage)return;
    replayView=widget;
    replayPage=new QWidget;replayPage->setObjectName(QStringLiteral("inlineMatchRecord"));
    auto layout=column(replayPage,Kda::Density::pageInset,Kda::Density::sectionGap);auto heading=new QHBoxLayout;
    replayTitle=wrapped("pageTitle",QStringLiteral("本局记录"));heading->addWidget(replayTitle,1);
    replayBack=button(QStringLiteral("返回主页"));replayBack->setObjectName(QStringLiteral("closeInlineReplay"));replayBack->setProperty("toolbarAction",true);heading->addWidget(replayBack);layout->addLayout(heading);
    widget->setParent(replayPage);widget->setWindowFlags(Qt::Widget);widget->setMinimumSize(0,0);layout->addWidget(widget,1);homePages->addWidget(replayPage);
    connect(replayBack,&QPushButton::clicked,this,[this]{
        const auto matchRef=archivedReplayMatch;
        dismissDecisionReplay();
        homePages->setCurrentIndex(0);
        if(matchRef.isEmpty())selectPage(0);
        else{playerHistory->returnToDecisionMatch(matchRef);selectPage(2);}
    });
    playerHistory->decisionReplayAvailable=[this](const QString &matchRef){
        const auto directory=decisionArchiveDirectory(catalog.root,localAccountScope);
        return !directory.isEmpty()&&CoachReplay::archiveAvailable(directory,matchRef);
    };
    playerHistory->onDecisionReplay=[this](const QString &matchRef){showDecisionReplay(matchRef);};
    decisionArchiveChanged();
}
void CoachDashboard::showDecisionReplay(const QString &matchRef){
    if(!replayView||startupNoticeActive)return;
    const auto directory=decisionArchiveDirectory(catalog.root,localAccountScope);
    if(directory.isEmpty()||!replayView->openArchive(directory,matchRef)){
        decisionArchiveChanged();
        return;
    }
    archivedReplayMatch=matchRef;
    replayTitle->setText(QStringLiteral("决策回看"));
    replayBack->setText(QStringLiteral("返回对局"));
    homePages->setCurrentWidget(replayPage);
    selectPage(0);
}
void CoachDashboard::dismissDecisionReplay(){
    if(archivedReplayMatch.isEmpty())return;
    archivedReplayMatch.clear();
    replayView->showLive();
    replayTitle->setText(QStringLiteral("本局记录"));
    replayBack->setText(QStringLiteral("返回主页"));
    homePages->setCurrentIndex(0);
}
void CoachDashboard::showReplay(){
    if(!replayPage||startupNoticeActive)return;
    dismissDecisionReplay();
    const auto phase=snapshot.value("live_phase").toString();
    replaySawMatch=replaySawMatch||phase==QStringLiteral("InProgress")||phase==QStringLiteral("GameStart")||phase==QStringLiteral("Reconnect")||!matchView.value("stage").toString().isEmpty();
    homePages->setCurrentWidget(replayPage);selectPage(0);
}
bool CoachDashboard::eventFilter(QObject *object,QEvent *event){
    if(startupNoticeActive&&event->type()==QEvent::KeyPress&&static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape){
        const auto target=qobject_cast<QWidget*>(object);if(target&&(target==this||isAncestorOf(target))){if(startupNoticePage)startupNoticePage->reject();else finishStartupNotice(false);event->accept();return true;}
    }
    if(object==qApp&&event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinHudSkinStrength"&&hudSkinStrength){
        const QSignalBlocker quiet(hudSkinStrength);hudSkinStrength->setValue(qRound(Kda::HudStyle::strength()*100));hudSkinStrengthValue->setText(QString::number(hudSkinStrength->value())+QChar(u'%'));
    }
    const auto type = event->type();
    const bool focusChanged = object == this && (type == QEvent::ActivationChange || type == QEvent::Show);
    const bool windowHidden = object == this && (type == QEvent::Hide || (type == QEvent::WindowStateChange && isMinimized()));
    const bool reduceMotionNow = object == qApp && type == QEvent::DynamicPropertyChange &&
        static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "lixinReducedMotion" && Kda::reducedMotion();
    if (focusChanged || windowHidden || reduceMotionNow) {
        if (auto *effect = static_cast<InactiveDashboardEffect*>(graphicsEffect()))
            effect->setInactive(windowHidden || !isActiveWindow(),
                                !windowHidden && !reduceMotionNow && !Kda::reducedMotion() && isVisible() && !isMinimized());
    }
    if(object==this&&event->type()==QEvent::Show&&homeViewPending&&!startupNoticeActive)updateView();
    if(object==this&&firstTutorialPending&&(type==QEvent::Show||type==QEvent::WindowStateChange))
        QTimer::singleShot(0,this,&CoachDashboard::showFirstTutorial);
    if(object==this&&event->type()==QEvent::WindowStateChange)QTimer::singleShot(0,this,[this]{if(homeViewPending&&!startupNoticeActive)updateView();});
    if(object==this&&event->type()==QEvent::DevicePixelRatioChange)QTimer::singleShot(0,this,[this]{updateArtwork();});
    return QMainWindow::eventFilter(object,event);
}
bool CoachDashboard::closesToTray() const {
    return preferences.value(closeToTrayKey,false).toBool()&&QSystemTrayIcon::isSystemTrayAvailable();
}
void CoachDashboard::closeEvent(QCloseEvent *event){
    if(startupNoticeActive){
        event->ignore();
        if(startupNoticePage)startupNoticePage->reject();else finishStartupNotice(false);
        return;
    }
    if(closesToTray()){
        event->ignore();
        hide();
        return;
    }
    event->accept();
    if(onQuit)onQuit();
}
void CoachDashboard::paintEvent(QPaintEvent *){
    QPainter painter(this);painter.setCompositionMode(QPainter::CompositionMode_Source);painter.fillRect(rect(),Qt::transparent);painter.setCompositionMode(QPainter::CompositionMode_SourceOver);const qreal radius=isMaximized()||isFullScreen()?0:Kda::radius("window");
    const int alpha=property("dashboardBackdropTintAlpha").isValid()?property("dashboardBackdropTintAlpha").toInt():255;
    DashboardFrame::paint(painter,size(),devicePixelRatioF(),radius,QColor(Kda::color("bg").red(),Kda::color("bg").green(),Kda::color("bg").blue(),std::clamp(alpha,0,255)),QColor(240,243,248,42));
}
void CoachDashboard::resizeEvent(QResizeEvent *event){
    QMainWindow::resizeEvent(event);const bool compact=width()<960;if(property("compactDashboard").isValid()&&property("compactDashboard").toBool()==compact)return;setProperty("compactDashboard",compact);
    for(const auto *side:{&allies,&enemies})for(const auto &seat:*side)for(auto label:{seat.name,seat.detail}){label->setProperty("compact",compact);label->style()->unpolish(label);label->style()->polish(label);label->updateGeometry();present(label,label->property("fullText").toString());}
}
void CoachDashboard::loadControls(bool loadSecrets){
    savingPreferences=true;
    closeWindowBehavior->setCurrentIndex(preferences.value(closeToTrayKey,false).toBool()?1:0);
    const auto finish=preferences.value("appearance/hudSkinStrength",.65).toDouble();Kda::HudStyle::setStrength(finish);hudSkinStrength->setValue(qRound(Kda::HudStyle::strength()*100));hudSkinStrengthValue->setText(QString::number(hudSkinStrength->value())+QChar(u'%'));Kda::setSkin(preferences.value("appearance/skin",QStringLiteral("kda")).toString());for(auto choice:skinChoices)choice->setChecked(choice->property("skinId").toString()==Kda::skin());const bool depth=preferences.value("appearance/parallax",true).toBool();parallaxEnabled->setChecked(depth);Kda::setDepthEnabled(depth);const bool glass=preferences.value("appearance/frostedGlass",true).toBool();frostedGlassEnabled->setChecked(glass);Kda::setGlassEnabled(glass);const bool reduced=preferences.value("appearance/reducedMotion",false).toBool();reduceMotion->setChecked(reduced);Kda::setReducedMotion(reduced);shopRecommendationsEnabled->setChecked(preferences.value("coach/shopRecommendationsEnabled",true).toBool());overlayEnabled->setChecked(preferences.value("coach/overlayEnabled",true).toBool());cardHintsEnabled->setChecked(preferences.value("coach/cardHintsEnabled",true).toBool());rosterDsEnabled->setChecked(preferences.value("coach/rosterDsEnabled",true).toBool());autoMinimize->setChecked(preferences.value("coach/autoMinimize",true).toBool());opacity->setValue(qRound(opacityPreference(preferences)*100));opacityValue->setText(QString::number(opacity->value())+QChar(u'%'));loadModelConfiguration(loadSecrets);savingPreferences=false;
}
void CoachDashboard::savePreference(const QString &key,const QVariant &value){
    if(savingPreferences||inSelfCheck)return;
    if(key.startsWith(QStringLiteral("appearance/"))&&appearanceSave){
        appearanceSave->defer();preferences.setValue(key,value);saveStatus->setText(QStringLiteral("外观已更新"));
        if(onPreferenceChanged)onPreferenceChanged(key);else if(onPreferencesChanged)onPreferencesChanged();return;
    }
    if(appearanceSave)appearanceSave->flush();preferences.setValue(key,value);preferences.sync();saveStatus->setText(preferences.status()==QSettings::NoError?QStringLiteral("已保存"):QStringLiteral("偏好保存失败"));
    if(preferences.status()==QSettings::NoError){if(onPreferenceChanged)onPreferenceChanged(key);else if(onPreferencesChanged)onPreferencesChanged();}
}
void CoachDashboard::saveDsSettings(){
    if(inSelfCheck)return;
    QString error;auto local=loadLocalSettings(catalog.root,&error);
    if(!error.isEmpty()){modelSaveStatus->setText(error);return;}
    const auto slot=modelSlot->currentData().toString();local.activeModelSlot=slot;
    if(slot==QStringLiteral("deepseek")){
        local.apiKey=apiKey->text().trimmed();local.model=model->currentText().trimmed();if(local.model.isEmpty())local.model=QStringLiteral("deepseek-flash");
    }else if(slot==QStringLiteral("local_openai")){
        local.localBaseUrl=localModelBaseUrl->text().trimmed();local.localModel=localModelName->text().trimmed();local.localApiKey=localModelApiKey->text().trimmed();
    }else {modelSaveStatus->setText(QStringLiteral("请选择模型位"));return;}
    const bool saved=saveLocalSettings(catalog.root,local,&error);
    if(saved){refreshSavedModelConfiguration();updateModelConfiguration();}
    modelSaveStatus->setText(saved?(savedModelConfigured?QStringLiteral("已保存，当前使用此模型位"):QStringLiteral("已保存，请补全配置后启用")):error.isEmpty()?QStringLiteral("模型配置保存失败"):error);
    Kda::postNotice({QStringLiteral("settings-save"),saved?QStringLiteral("模型配置已保存"):QStringLiteral("配置保存失败"),saved?QString():error,saved?Kda::NoticeKind::Success:Kda::NoticeKind::Error,saved?3000:7000});
    if(saved&&onPreferencesChanged)onPreferencesChanged();
}
void CoachDashboard::activateModelSlot(const QString &slot){
    if(inSelfCheck||slot==savedModelSlot)return;
    const auto previous=savedModelSlot;QString error;
    if(!activateLocalModelSlot(catalog.root,slot,&error)){
        modelLockStatus->setText(error.isEmpty()?QStringLiteral("锁定失败，仍使用当前模型"):error);return;
    }
    refreshSavedModelConfiguration();updateModelConfiguration();
    const bool changed=savedModelSlot!=previous;
    if(changed&&onPreferencesChanged)onPreferencesChanged();
}
QString CoachDashboard::refreshSavedModelConfiguration(){
    QString error;const auto local=loadLocalSettings(catalog.root,&error);
    savedModelSlot=local.activeModelSlot;savedDsModel=local.model;savedDsApiKey=local.apiKey;
    savedLocalBaseUrl=local.localBaseUrl;savedLocalModel=local.localModel;savedLocalApiKey=local.localApiKey;
    savedModelConfigured=error.isEmpty()&&local.modelConnection().isConfigured();return error;
}
void CoachDashboard::loadModelConfiguration(bool loadSecrets){
    if(!loadSecrets&&settingsTabs->currentIndex()!=settingsModelsTab)return;
    const auto state=modelConfigurationReport();
    const bool preserveDrafts=modelControlsLoaded&&state.value("has_unsaved_changes").toBool();
    const bool preservePreview=modelControlsLoaded&&state.value("preview_pending").toBool();
    const auto error=refreshSavedModelConfiguration();
    if(loadSecrets&&(!modelControlsLoaded||!preserveDrafts)){
        const QSignalBlocker slotSignals(modelSlot),keySignals(apiKey),modelSignals(model),urlSignals(localModelBaseUrl),localModelSignals(localModelName),localKeySignals(localModelApiKey);
        apiKey->setText(savedDsApiKey);if(model->findText(savedDsModel)<0)model->addItem(savedDsModel);model->setCurrentText(savedDsModel);
        localModelBaseUrl->setText(savedLocalBaseUrl);localModelName->setText(savedLocalModel);localModelApiKey->setText(savedLocalApiKey);
        if(!preservePreview)modelSlot->setCurrentIndex(std::max(0,modelSlot->findData(savedModelSlot)));
    }
    modelControlsLoaded=true;if(!error.isEmpty())modelSaveStatus->setText(error);updateModelConfiguration();
}
QJsonObject CoachDashboard::modelConfigurationReport()const{
    if(!modelSlot)return {};
    const auto slot=modelSlot->currentData().toString();auto dsModel=model->currentText().trimmed();if(dsModel.isEmpty())dsModel=QStringLiteral("deepseek-flash");
    const bool dsDirty=apiKey->text().trimmed()!=savedDsApiKey||dsModel!=savedDsModel;
    const bool localDirty=localModelBaseUrl->text().trimmed()!=savedLocalBaseUrl||localModelName->text().trimmed()!=savedLocalModel||localModelApiKey->text().trimmed()!=savedLocalApiKey;
    QJsonArray cards;for(auto card:modelSlotCards)cards.append(QJsonObject{{"slot_id",card->property("slotId").toString()},{"selected",card->isChecked()},{"expanded",card->property("expanded").toBool()},{"saved_active",card->property("savedActive").toBool()},{"current_enabled",card->property("currentEnabled").toBool()},{"draft_pending",card->property("draftPending").toBool()},{"artwork_loaded",card->property("assetLoaded").toBool()},{"artwork_asset",card->property("assetPath").toString()},{"visual",static_cast<ModelSlotCard*>(card)->visualReport()}});
    const auto editorOwner=modelConfigurationEditor&&modelConfigurationEditor->parentWidget()?modelConfigurationEditor->parentWidget()->property("slotId").toString():QString();
    return {{"active_slot",savedModelSlot},{"editing_slot",slot},{"expanded_slot",slot},{"editor_owner",editorOwner},{"selection_layout","choices_on_shared_header"},{"selection_headers_grouped",true},{"slot_controller_visible",modelSlot->isVisibleTo(this)},{"preview_pending",slot!=savedModelSlot},{"has_unsaved_changes",dsDirty||localDirty},{"editing_has_unsaved_changes",slot==QStringLiteral("local_openai")?localDirty:dsDirty},{"other_slot_has_unsaved_changes",slot==QStringLiteral("local_openai")?dsDirty:localDirty},{"active_configured",savedModelConfigured},{"planning_enabled",rosterDsEnabled->isChecked()},{"local_usage_priced",false},{"reports_connection_verified",false},{"cards",cards}};
}
void CoachDashboard::updateModelConfiguration(){
    if(!modelSlot||!modelCurrentName)return;
    const auto state=modelConfigurationReport();const bool local=savedModelSlot==QStringLiteral("local_openai"),known=local||savedModelSlot==QStringLiteral("deepseek");
    const auto editingSlot=modelSlot->currentData().toString();
    const bool editingLocal=editingSlot==QStringLiteral("local_openai");
    const bool previewing=editingSlot!=savedModelSlot;
    const bool planningEnabled=rosterDsEnabled->isChecked();
    LocalSettings saved;
    saved.apiKey=savedDsApiKey;
    saved.model=savedDsModel;
    saved.localBaseUrl=savedLocalBaseUrl;
    saved.localModel=savedLocalModel;
    saved.localApiKey=savedLocalApiKey;
    saved.activeModelSlot=editingSlot;
    const bool previewConfigured=saved.modelConnection().isConfigured();
    modelConnectionPages->setCurrentIndex(editingLocal?1:0);
    const auto currentStatus=!planningEnabled?QStringLiteral("规划已关闭"):savedModelConfigured?QStringLiteral("当前使用"):QStringLiteral("待配置");
    static_cast<ModelSlotPreview*>(modelSlotPreview)->setSlot(editingSlot,previewing?QStringLiteral("预览中"):currentStatus);
    const bool canUseSavedProfile=previewing&&previewConfigured;
    modelLockButton->setEnabled(canUseSavedProfile);
    modelLockButton->setVisible(canUseSavedProfile);
    modelLockStatus->setVisible(previewing);
    modelLockStatus->setText(previewConfigured?QStringLiteral("尚未切换"):QStringLiteral("填写后保存并启用"));
    modelSaveButton->setText(previewing?QStringLiteral("保存并启用"):QStringLiteral("保存模型配置"));
    const auto savedName=!known?QStringLiteral("未识别的模型位"):local?(savedLocalModel.isEmpty()?QStringLiteral("尚未设置本地模型"):savedLocalModel):savedDsModel;
    modelCurrentName->setText(QStringLiteral("当前 · %1").arg(savedName));
    modelCurrentProvider->setText(!known?QStringLiteral("请选择模型位并重新保存"):local?QStringLiteral("本地兼容服务"):QStringLiteral("DeepSeek 云端接口"));
    modelCurrentState->setText(!rosterDsEnabled->isChecked()?QStringLiteral("规划已关闭"):savedModelConfigured?QStringLiteral("已配置"):QStringLiteral("待配置"));
    modelCurrentState->setToolTip(QStringLiteral("显示已保存的配置状态，不代表服务已连接或模型已通过对局验证。"));
    modelCurrentProvider->setVisible(previewing);
    modelCurrentState->setVisible(previewing&&(!savedModelConfigured||!planningEnabled));
    const bool dirty=state.value("editing_has_unsaved_changes").toBool(),otherDirty=state.value("other_slot_has_unsaved_changes").toBool();
    modelDraftState->setText(dirty?QStringLiteral("有未保存修改"):otherDirty?QStringLiteral("另一模型有未保存修改"):previewConfigured?QString():QStringLiteral("待配置"));
    modelUsageScope->setText(local?QStringLiteral("以下为 DeepSeek 已记录用量；本地服务调用未计价。"):QStringLiteral("仅统计 DeepSeek 官方接口调用。"));
    for(int i=0;i<modelSlotCards.size();++i){auto card=modelSlotCards[i];
        const auto id=card->property("slotId").toString();const bool selected=id==editingSlot,savedActive=id==savedModelSlot,current=savedActive&&savedModelConfigured&&planningEnabled,pending=selected&&dirty;
        card->setChecked(selected);static_cast<ModelSlotCard*>(card)->setExpanded(selected);card->setProperty("savedActive",savedActive);card->setProperty("currentEnabled",current);card->setProperty("draftPending",pending);
        auto body=modelSlotBodies[i];body->setProperty("expanded",selected);if(selected){if(modelConfigurationEditor->parentWidget()!=body)body->layout()->addWidget(modelConfigurationEditor);modelConfigurationEditor->show();}body->setVisible(selected);
        const auto status=savedActive?currentStatus:pending?QStringLiteral("待保存"):selected?QStringLiteral("预览中"):QStringLiteral("未启用");
        card->setProperty("statusText",status);
        card->setAccessibleDescription(selected?QStringLiteral("已选中预览 · %1").arg(status):status);
        card->update();
    }
}
void CoachDashboard::setSnapshot(const QJsonObject &value){if(!inSelfCheck&&!startupNoticeActive){playerHistory->setGamePhase(value.value("live_phase").toString());applySnapshot(value);}}
void CoachDashboard::setMatchRefreshStatus(const QString &value){
    if(!matchRefreshStatus)return;
    const auto text=value.simplified().left(80);present(matchRefreshStatus,text);matchRefreshStatus->setToolTip(text);matchRefreshStatus->setVisible(!text.isEmpty());
    setProperty("matchRefreshStatus",text);
}
void CoachDashboard::setLocalPlayerDisplayName(const QString &value){
    localPlayerDisplayName=value.left(128).trimmed();localPlayerDisplayName.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));
    if(auto cover=findChild<QWidget*>(QStringLiteral("kdaCampaignCover")))static_cast<SatinBanner*>(cover)->setPlayerName(localPlayerDisplayName);
    updateIdlePresentation();
    if(!inSelfCheck&&!startupNoticeActive){playerHistory->setPersonalAccountName(localPlayerDisplayName);updatePersonalHome();}
}
void CoachDashboard::setLocalAccountScope(const QString &value){
    const auto next=QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(value).hasMatch()?value:QString();
    const bool exitArchive=next!=localAccountScope&&!archivedReplayMatch.isEmpty();
    localAccountScope=next;
    if(exitArchive)dismissDecisionReplay();
    if(!inSelfCheck&&!startupNoticeActive)playerHistory->setAccountScope(localAccountScope);
    if(exitArchive)selectPage(2);
}
void CoachDashboard::setLocalPlayerBackdrop(const QByteArray &bytes){if(auto cover=findChild<QWidget*>(QStringLiteral("kdaCampaignCover")))static_cast<SatinBanner*>(cover)->setBackdrop(bytes);}
void CoachDashboard::updatePersonalHome(){
    if(inSelfCheck||startupNoticeActive||!personalHome)return;
    const auto state=playerHistory->report().value("personal_home").toObject();const auto current=playerHistory->personalProfileSnapshot();
    personalHome->setProfile(current);
    const QMap<QString,QString> explanations{{"login_required",QStringLiteral("登录客户端后显示本人战绩")},{"cache_missing",QStringLiteral("暂无已保存的本人战绩")},{"paused_in_game",QStringLiteral("对局中暂缓更新本人战绩")},{"querying",QStringLiteral("正在读取本地本人战绩…")},{"source_stopped",QStringLiteral("本人战绩来源尚未开启")},{"database_unavailable",QStringLiteral("本地战绩库暂不可用")}};
    const auto status=state.value("state").toString();personalHome->setState(state.value("busy").toBool(),explanations.value(status,current.isEmpty()?QStringLiteral("暂无已保存的本人战绩"):QString()));
}
void CoachDashboard::updateIdlePresentation(){
    if(!waitingPlayer)return;
    const bool idle=matchView.value("stage").toString().isEmpty();
    const bool waiting=idle&&localPlayerDisplayName.isEmpty();
    matchBoard->setVisible(!idle);idleState->setVisible(idle&&!waiting);waitingPlayer->setVisible(waiting);
    const auto connection=snapshot.value("connection").toString();
    waitingPlayer->setConnected(connection==QStringLiteral("客户端已连接")||connection.startsWith(QStringLiteral("对局已连接")));
}
void CoachDashboard::applySnapshot(const QJsonObject &value){snapshot=value;const auto growth=value.value("growth_capture").toObject();present(growthCaptureStatus,growthCaptureStatusText(growth));growthCaptureStatus->setToolTip(QStringLiteral("自动读取需先定位自己的图标栏，再正常悬停提示；勾选开关不代表已读到海克斯。仅核实的输入参与计算，缺失层数保持未知。"));updateDsUsage();updateView();}
void CoachDashboard::updateDsUsage(){
    if(!dsUsageCurrent)return;
    dsUsagePresentation=usagePresentation(snapshot.value("deepseek_usage").toObject());
    const auto assign=[&](QLabel *label,const char *field){const auto text=dsUsagePresentation.value(QString::fromLatin1(field)).toString();if(label->text()!=text)label->setText(text);};
    assign(dsUsageCurrent,"current_amount");assign(dsUsageState,"current_status");assign(dsUsageTokens,"tokens");assign(dsUsageCalls,"requests");
    assign(dsUsageAverage,"average_amount");assign(dsUsageSamples,"samples");assign(dsUsagePersistence,"persistence_warning");
    for(auto label:{dsUsageCurrent,dsUsageState,dsUsageAverage}){const auto state=dsUsagePresentation.value(label==dsUsageAverage?"average_state":"current_state").toString();if(label->property("usageState").toString()!=state){label->setProperty("usageState",state);label->style()->unpolish(label);label->style()->polish(label);label->updateGeometry();}}
    dsUsagePersistence->setVisible(!dsUsagePersistence->text().isEmpty());
    const auto incomplete=dsUsagePresentation.value("current_state")!=QStringLiteral("complete")&&dsUsagePresentation.value("current_state")!=QStringLiteral("no_calls");
    dsUsageCurrent->setToolTip(incomplete?QStringLiteral("尚有请求未完成或缺少可用计价信息；显示的已计价部分不代表本局全部费用。"):QStringLiteral("按接口返回用量估算；缓存和计价时段不确定时保留上下限。"));
    dsUsageTokens->setToolTip(QStringLiteral("输入与输出tokens仅合计接口实际返回的有效用量；缺失用量不按零计算。"));
    const auto cohort=QStringLiteral("仅统计有DS调用、完整观测并计价的已结束对局；中途启动、分段记录或费用未完整的对局不计入均值。");
    dsUsageAverage->setToolTip(cohort);dsUsageSamples->setToolTip(cohort);
}
void CoachDashboard::renderSeats(QList<Seat> &seats,const QJsonArray &roster,const QString &side){
    QList<QJsonObject> visible;for(const auto &v:roster){const auto p=v.toObject();if(p.value("team")==side)visible.append(p);}std::stable_sort(visible.begin(),visible.end(),[](const QJsonObject &a,const QJsonObject &b){const auto first=a.value("cell"),second=b.value("cell");if(first.isDouble()!=second.isDouble())return first.isDouble();return first.isDouble()&&first.toInt()<second.toInt();});
    for(int index=0;index<seats.size();++index){
        const auto p=index<visible.size()?visible[index]:QJsonObject{};const int champion=p.value("champion_id").toInt();auto &seat=seats[index];const bool own=p.value("self").toBool();
        QString title=champion>0?catalog.championName(champion):QStringLiteral("等待公开");int level=0;if(champion>0&&observedInt(p.value("level"),level)&&level>0)title+=QStringLiteral(" · %1级").arg(level);if(own)title.prepend(QStringLiteral("你 · "));present(seat.name,title);
        int kills=0,deaths=0,assists=0;const auto scores=p.value("scores").toObject();const bool kda=champion>0&&observedInt(scores.value("kills"),kills)&&observedInt(scores.value("deaths"),deaths)&&observedInt(scores.value("assists"),assists);present(seat.marker,kda?QStringLiteral("%1 / %2 / %3").arg(kills).arg(deaths).arg(assists):QStringLiteral("—"));
        const auto history=p.value("history").toObject();const int samples=history.value("sampled_games").toInt();const bool available=champion>0&&history.value("available").toBool()&&samples>0;QString detail;QStringList tooltip;
        const auto refreshStatus=history.value("refresh_status").toString();const auto refreshText=matchView.value("stage")!="champ_select"?QString():refreshStatus=="active"?QStringLiteral("正在读取"):refreshStatus=="queued"||refreshStatus=="pending"?QStringLiteral("排队更新"):refreshStatus=="failed"||refreshStatus=="error"?QStringLiteral("更新失败"):refreshStatus=="cooldown"?QStringLiteral("稍后重试"):QString();
        if(available){
            const auto metrics=history.value("metrics").toObject();const bool sameChampion=history.value("scope")=="same_patch_champion";detail=QStringLiteral("%1场%2").arg(samples).arg(sameChampion?QStringLiteral("同英雄"):QStringLiteral("同版"));
            tooltip.append(QStringLiteral("历史参考 · %1场 · %2").arg(samples).arg(sameChampion?QStringLiteral("同版同英雄"):QStringLiteral("该玩家同版")));
            const auto damage=metrics.value("damage_per_minute"),gold=metrics.value("gold_per_minute");
            if(damage.isDouble())detail+=QStringLiteral(" · 输出 %1/分").arg(numberText(damage));else if(gold.isDouble())detail+=QStringLiteral(" · 经济 %1/分").arg(numberText(gold));else if(metrics.value("kills").isDouble()&&metrics.value("deaths").isDouble()&&metrics.value("assists").isDouble())detail+=QStringLiteral(" · 均 %1/%2/%3").arg(numberText(metrics.value("kills"),1),numberText(metrics.value("deaths"),1),numberText(metrics.value("assists"),1));
            for(const auto &key:{"damage_per_minute","gold_per_minute","taken_per_minute"})if(metrics.value(key).isDouble()){const auto label=QString::fromLatin1(key)=="damage_per_minute"?QStringLiteral("历史伤害/分"):QString::fromLatin1(key)=="gold_per_minute"?QStringLiteral("历史金币/分"):QStringLiteral("历史承伤/分");tooltip.append(label+QStringLiteral(" · ")+numberText(metrics.value(key)));}
            bool first=true;for(const auto &v:history.value("comparison").toArray()){const auto comparison=v.toObject();const auto text=comparisonText(comparison);if(text.isEmpty())continue;if(first){detail+=QStringLiteral(" · ")+text;first=false;}tooltip.append(QStringLiteral("%1 · %2场 / 对照%3场").arg(text).arg(comparison.value("samples").toInt()).arg(comparison.value("reference_samples").toInt()));}
            if(!refreshText.isEmpty())detail+=QStringLiteral(" · ")+refreshText;else if(history.value("freshness")=="stale")detail+=QStringLiteral(" · 历史已过期");
        }else if(champion>0)detail=refreshText.isEmpty()?QStringLiteral("暂无同版历史样本"):QStringLiteral("历史资料 · ")+refreshText;else detail=QStringLiteral("公开后自动匹配资料");
        if(!refreshText.isEmpty())tooltip.append(QStringLiteral("历史资料状态：")+refreshText+QStringLiteral("；不代表本局实时伤害或操作水平"));
        const auto recent=p.value("recent_results").toObject();const bool hasRecent=champion>0&&matchView.value("recent_results").toObject().value("present").toBool()&&!recent.isEmpty();
        if(hasRecent){if(!kda&&matchView.value("stage")!="in_game")present(seat.marker,recent.value("marker").toString());if(!detail.isEmpty())tooltip.prepend(detail);detail=recent.value("detail").toString();tooltip.prepend(recent.value("tooltip").toString());}
        const auto previousSelf=seat.body->property("self");
        seat.body->setProperty("recentScoreKnown",hasRecent&&recent.value("known").toBool());
        present(seat.detail,detail);seat.detail->setToolTip(tooltip.join(QChar(u'\n')));seat.name->setToolTip(champion>0?catalog.championName(champion)+QStringLiteral(" · ")+roleSummary(catalog.champions.value(champion)):QString());seat.body->setProperty("championVisible",champion>0);seat.body->setProperty("championId",champion);seat.body->setProperty("historyAvailable",available);seat.body->setProperty("historySamples",available?samples:0);seat.body->setProperty("self",own);if(!previousSelf.isValid()||previousSelf.toBool()!=own){seat.body->style()->unpolish(seat.body);seat.body->style()->polish(seat.body);}
    }
}
bool CoachDashboard::shouldDeferHomeView() const{
    return !inSelfCheck&&pages&&(pages->currentIndex()!=0||(homePages&&homePages->currentIndex()!=0)||!isVisible()||isMinimized());
}
void CoachDashboard::updateConnectionState(){
    const bool liveFresh=matchView.value("live_fresh").toBool();
    const auto connection=snapshot.value("connection").toString();
    const bool connected=connection==QStringLiteral("客户端已连接")||connection.startsWith(QStringLiteral("对局已连接"));
    present(connectionLabel,inSelfCheck?QStringLiteral("合成界面检查"):connected?QStringLiteral("● 客户端已连接"):QStringLiteral("○ 等待客户端"));
    connectionLabel->setProperty("connected",connected);
    const auto status=snapshot.value("augment_source").toObject().value("status").toString();
    const QMap<QString,QString> healthNames{{"logs_active",QStringLiteral("已读取")},{"logs_idle",QStringLiteral("待候选")},{"read_error",QStringLiteral("读取失败")},{"stopped",QStringLiteral("未开启")},{"collector_unresponsive",QStringLiteral("未响应")},{"synthetic_demo",QStringLiteral("合成")},{"historical_replay",QStringLiteral("回放")}};
    const auto patch=snapshot.value("observed_patch").toString();
    const auto rules=snapshot.value("rule_basis").toObject();
    const auto rulePatch=rules.value("source_patch").toString();
    const auto gameVersion=patch.isEmpty()?QStringLiteral("游戏版本待确认"):QStringLiteral("游戏 %1").arg(patch);
    auto versions=gameVersion;
    QString versionDetail;
    if(!rules.isEmpty()){
        if(!rules.value("rules_available").toBool()||rulePatch.isEmpty())versions+=QStringLiteral(" / 规则未就绪");
        else if(patch==rulePatch)versions=QStringLiteral("游戏与规则 %1").arg(patch);
        else{
            versions+=QStringLiteral(" / 规则 %1").arg(rulePatch);
            versionDetail=patch.isEmpty()?QStringLiteral("读取到游戏版本后会核对规则版本。")
                :QStringLiteral("游戏与规则版本不同。局内数值按 %1 规则估算。").arg(rulePatch);
        }
    }
    const auto text=QStringLiteral("%1  ·  客户端 %2  ·  实时 %3  ·  海克斯 %4")
        .arg(versions,connected?QStringLiteral("已连接"):QStringLiteral("等待"),liveFresh?QStringLiteral("已接入"):QStringLiteral("等待"),healthNames.value(status,QStringLiteral("等待")));
    present(sourceLabel,text);
    sourceLabel->setToolTip(versionDetail.isEmpty()?text:text+QLatin1Char('\n')+versionDetail);
    sourceLabel->setProperty("liveFresh",liveFresh);
    sourceLabel->setProperty("augmentStatus",status);
}

void CoachDashboard::updateView(){
    // An explicit lifecycle phase closes the record view. Missing/stale live
    // data alone is not evidence that a match has ended.
    const auto phase=snapshot.value("live_phase").toString();
    if(phase==QStringLiteral("InProgress")||phase==QStringLiteral("GameStart")||phase==QStringLiteral("Reconnect"))replaySawMatch=true;
    const QStringList ended{QStringLiteral("PreEndOfGame"),QStringLiteral("WaitingForStats"),QStringLiteral("EndOfGame"),QStringLiteral("Lobby"),QStringLiteral("None")};
    if(replaySawMatch&&ended.contains(phase)){if(homePages&&archivedReplayMatch.isEmpty())homePages->setCurrentIndex(0);replaySawMatch=false;}
    matchView=matchProjection.project(snapshot);updateConnectionState();
    const auto shop=snapshot.value("shop_recommendations").toObject();
    const auto shopState=shop.value("state").toString();
    const bool shopEnabled=preferences.value("coach/shopRecommendationsEnabled",true).toBool();
    const bool shopFailure=!shopState.isEmpty()&&shopState!=QStringLiteral("ready")&&shopState!=QStringLiteral("inactive")&&!shopState.startsWith(QStringLiteral("waiting_"));
    QString shopStatusText=QStringLiteral("进入选人后准备出装资料");
    QString shopHelp;
    if(shopFailure){
        shopStatusText=QStringLiteral("商店页暂未更新");
        shopHelp=QStringLiteral("更新或清理尚未完成。可在李金金继续查看本局信息。");
    }else if(!shopEnabled){
        shopStatusText=QStringLiteral("商店出装页已关闭");
    }else if(shopState==QStringLiteral("ready")){
        shopStatusText=QStringLiteral("已写入「李金金 · 当前出装」");
        QString title=QStringLiteral("李金金 · 当前出装");
        const auto version=shop.value("source_version").toString();
        if(!version.isEmpty())title+=QStringLiteral(" · ")+version;
        shopHelp=QStringLiteral("对局内打开商店，在装备方案中查找「%1」。方案显示时机由游戏决定。").arg(title);
    }else if(shopState==QStringLiteral("waiting_champion")){
        shopStatusText=QStringLiteral("等待确认当前英雄");
    }else if(shopState==QStringLiteral("waiting_verified_recommendation")){
        shopStatusText=QStringLiteral("当前英雄暂无已核验出装");
    }else if(shopState==QStringLiteral("waiting_client_directory")){
        shopStatusText=QStringLiteral("等待客户端提供游戏目录");
    }
    present(shopRecommendationsStatus,shopStatusText);
    shopRecommendationsStatus->setToolTip(shopHelp);
    shopRecommendationsStatus->setAccessibleDescription(shopHelp);
    homeViewPending=true;setProperty("homeViewPending",true);
    if(shouldDeferHomeView())return;
    homeViewPending=false;setProperty("homeViewPending",false);setProperty("homeViewRenderCount",++homeViewRenderCount);
    const auto stage=matchView.value("stage").toString();const auto coach=snapshot.value("coach").toObject();const int active=stage=="champ_select"?0:stage=="loading"?1:stage=="in_game"?2:-1;const bool liveFresh=matchView.value("live_fresh").toBool();
    updateIdlePresentation();
    champSelectView->setState(snapshot.value("champ_select").toObject());champSelectView->setVisible(active==0);purchaseOverview->setVisible(active!=0);
    for(auto *side:{&allies,&enemies})for(auto &seat:*side){
        seat.body->setFixedHeight(active==0?30:38);seat.icon->setFixedSize(active==0?24:30,active==0?24:30);seat.detail->setVisible(active!=0);
    }
    matchInputsAction->setVisible(active==2&&liveFresh);matchInputsAction->setEnabled(snapshot.value("current_players").toArray().size()>0);

    present(matchTitle,active==0?QStringLiteral("选人中"):active==1?QStringLiteral("载入对局"):active==2?QStringLiteral("对局进行中"):QStringLiteral("等待对局"));const int champion=snapshot.value("current_champion_id").toInt(),level=snapshot.value("current_level").toInt();QStringList subtitle;if(inSelfCheck)subtitle.append(QStringLiteral("合成示意"));if(champion>0)subtitle.append(catalog.championName(champion));if(active==2&&liveFresh&&level>0)subtitle.append(QStringLiteral("Lv. %1").arg(level));const auto time=snapshot.value("game_time");if(active==2&&liveFresh&&time.isDouble()&&std::isfinite(time.toDouble())&&time.toDouble()>=0){const int seconds=int(std::min(time.toDouble(),86400.));subtitle.append(QStringLiteral("%1:%2").arg(seconds/60,2,10,QChar(u'0')).arg(seconds%60,2,10,QChar(u'0')));}present(matchSubtitle,subtitle.isEmpty()?active<0?QStringLiteral("进入选人后自动分析"):QStringLiteral("等待英雄公开"):subtitle.join(QStringLiteral("  /  ")));
    for(int i=0;i<phases.size();++i){const bool selected=i==active,complete=active>=0&&i<active;if(phases[i]->property("active").toBool()==selected&&phases[i]->property("complete").toBool()==complete)continue;phases[i]->setProperty("active",selected);phases[i]->setProperty("complete",complete);phases[i]->style()->unpolish(phases[i]);phases[i]->style()->polish(phases[i]);}
    const auto roster=matchView.value("players").toArray();renderSeats(allies,roster,"ALLY");renderSeats(enemies,roster,"ENEMY");const auto economy=coach.value("economy").toObject();
    int allyKills=0,enemyKills=0,allyCount=0,enemyCount=0;bool completeScores=roster.size()==10;
    for(const auto &value:roster){const auto player=value.toObject();int kills=0;const auto side=player.value("team").toString();
        if(player.value("champion_id").toInt()<=0||!observedInt(player.value("scores").toObject().value("kills"),kills)||kills>100000){completeScores=false;continue;}
        if(side=="ALLY"){allyKills+=kills;++allyCount;}else if(side=="ENEMY"){enemyKills+=kills;++enemyCount;}else completeScores=false;
    }
    const bool scoreKnown=active==2&&liveFresh&&completeScores&&allyCount==5&&enemyCount==5;
    present(scoreLabel,scoreKnown?QStringLiteral("%1  :  %2").arg(allyKills).arg(enemyKills):QStringLiteral("—  :  —"));scoreLabel->setProperty("scoreKnown",scoreKnown);
    scoreLabel->setToolTip(scoreKnown?QStringLiteral("当前双方击杀合计 · 直接读取实时记录"):active==2?QStringLiteral("实时记录未完整提供双方击杀数"):QStringLiteral("进入对局后显示双方击杀数"));
    if(active==2&&liveFresh&&time.isDouble()&&std::isfinite(time.toDouble())&&time.toDouble()>=0){const int seconds=int(std::min(time.toDouble(),86400.));present(timeLabel,QStringLiteral("%1:%2").arg(seconds/60,2,10,QChar(u'0')).arg(seconds%60,2,10,QChar(u'0')));}else present(timeLabel,active==0?QStringLiteral("选人中"):active==1?QStringLiteral("加载中"):QStringLiteral("等待开始"));
    const auto purchase=economy.value("next_purchase").toObject();const auto quality=coach.value("recommendation_quality").toObject();
    const auto plan=coach.value("plan").toObject();const bool growthCandidates=growthCandidatePlan(plan);
    const bool liveShopping=active==2&&liveFresh&&(phase==QStringLiteral("InProgress")||phase==QStringLiteral("Reconnect"));
    const auto adaptive=liveShopping?CoachAdaptivePurchase::fromPlan(plan,time.toDouble(-1),[this](int id){return catalog.itemName(id);},quality):CoachAdaptivePurchase::View{};
    const bool timedOut=active==2&&liveFresh&&snapshot.value("model_state")==QStringLiteral("time_budget_exceeded");
    const bool rankingBlocked=coach.value("status")==QStringLiteral("input_required")||growthCandidates||adaptive.active||timedOut||quality.value("primary_purchase_visible")!=QJsonValue(true);
    const int next=active==2&&liveFresh&&!rankingBlocked?purchase.value("item_id").toInt():0;actionIcon->setProperty("itemId",next);
    QString displaySource=QStringLiteral("computed");
    const auto budgetLabel=[](const QJsonValue &gap,bool component){
        if(!gap.isDouble()||!std::isfinite(gap.toDouble())||gap.toDouble()<0)return QStringLiteral("金币待同步");
        if(gap.toDouble()==0)return component?QStringLiteral("组件预算内"):QStringLiteral("目录预算内");
        return (component?QStringLiteral("组件差≈%1G"):QStringLiteral("目录差≈%1G")).arg(std::ceil(gap.toDouble()),0,'f',0);
    };
    const auto priceNote=QStringLiteral("按目录估价核对当前金币；实际售价及购买条件以商店为准。");
    priceLabel->setToolTip(QString());
    const int candidateCount=plan.value("candidate_loadouts").toArray().size();
    present(actionTitle,active==2&&liveFresh&&growthCandidates?(candidateCount>0?QStringLiteral("成长联动候选 · %1组").arg(candidateCount):QStringLiteral("成长联动候选 · 待补输入")):next>0?QStringLiteral("下一件 · ")+catalog.itemName(next):active==2&&rankingBlocked?QStringLiteral("出装依据不完整"):active==2&&!liveFresh?QStringLiteral("实时连接中断"):active==2?QStringLiteral("出装计算中"):QStringLiteral("进入对局后分析出装"));
    QStringList components;if(active==2&&liveFresh&&!rankingBlocked)for(const auto &v:purchase.value("buyable_components").toArray()){const auto p=v.toObject();int price=0;const int id=p.value("item_id").toInt();if(id>0&&observedInt(p.value("remaining_cost"),price))components.append(catalog.itemName(id)+QStringLiteral(" %1G").arg(price));}
    const auto qualityReason=quality.value("summary").toString();
    const auto candidateNotes=growthCandidates?growthCandidateNotes(plan):QStringList{};
    const auto candidateExplanation=growthCandidates?candidateNotes.join(QStringLiteral("；"))+QStringLiteral("。组合并列展示、未排序；购买条件与顺序尚需核实。"):QString();
    present(actionDetail,growthCandidates?candidateNotes.first():rankingBlocked?qualityReason:components.isEmpty()?next>0?QStringLiteral("暂无预算内组件"):QStringLiteral("尚无购买候选"):QStringLiteral("组件估价 · ")+components.join(QStringLiteral(" / ")));
    actionDetail->setToolTip(growthCandidates?candidateExplanation:rankingBlocked?qualityReason:components.join(QStringLiteral(" / "))+QStringLiteral("\n")+priceNote);
    const auto facts=snapshot.contains("observed_purchase_facts")?snapshot.value("observed_purchase_facts").toObject():coach.value("observed_purchase_facts").toObject();
    const auto reference=snapshot.contains("observed_purchase_facts")?facts.value("source_reference").toObject():coach.value("source_purchase_reference").toObject();
    const bool recipeFactsOnly=liveShopping&&!adaptive.ranking&&(coach.value("status")==QStringLiteral("input_required")||(next<=0&&reference.value("available").toBool()));
    if(recipeFactsOnly){
        const bool budget=facts.value("available").toBool()&&facts.value("budget_known").toBool();
        present(actionTitle,budget?QStringLiteral("商店参考 · 当前 %1G").arg(int(facts.value("current_gold").toDouble())):QStringLiteral("商店参考 · 金币待同步"));
        QStringList recipes;for(const auto &v:facts.value("upgrades").toArray()){const auto row=v.toObject();if(row.value("within_catalog_budget")==QJsonValue(true)&&recipes.size()<3)recipes.append(catalog.itemName(row.value("item_id").toInt())+QStringLiteral(" %1G").arg(row.value("remaining_cost").toInt()));}
        present(actionDetail,recipes.isEmpty()?QStringLiteral("强化未识别，暂不排序；阵容与金币仍可查看"):QStringLiteral("目录配方 · ")+recipes.join(QStringLiteral(" / ")));
        actionDetail->setToolTip(actionDetail->property("fullText").toString()+QStringLiteral("\n按合成价展示，不是出装排名；未计强化改价，请核对实际商店。"));
    }
    else if(timedOut){present(actionTitle,QStringLiteral("本轮比较超时"));present(actionDetail,QStringLiteral("稍后用最新局面重试 · 未输出排名"));}
    const auto growth=growthPlanSummary(plan);present(routeLabel,QStringLiteral("成长联动 · ")+(active==2&&liveFresh?growth:active==2?QStringLiteral("计算完成后显示"):QStringLiteral("尚未进入对局")));routeLabel->setToolTip(growthCandidates?growth+QStringLiteral("；")+candidateExplanation:rankingBlocked?growth+QStringLiteral("；")+qualityReason:growth);
    present(priceLabel,next>0?budgetLabel(purchase.value("gold_gap"),false):QString());
    if(next>0)priceLabel->setToolTip(priceNote);
    if(recipeFactsOnly){
        present(routeLabel,QStringLiteral("自动读取 · ")+growthCaptureStatusText(snapshot.value("growth_capture").toObject()));routeLabel->setToolTip(QStringLiteral("设置 → 自动读取设置；先定位自己的图标栏，正常悬停即可读取可见提示。手动记录为可选操作。"));present(priceLabel,QString());
        if(reference.value("available").toBool()){
            const int target=reference.value("target_item_id").toInt(),action=reference.value("action_item_id").toInt();actionIcon->setProperty("itemId",target);
            displaySource=QStringLiteral("reference");
            present(actionTitle,QStringLiteral("资料路线 · ")+catalog.itemName(target));
            present(actionDetail,action!=target?QStringLiteral("组件参考 %1 · ≈%2G").arg(catalog.itemName(action),numberText(reference.value("action_cost"))):QStringLiteral("合成参考价 ≈%1G").arg(numberText(reference.value("target_remaining_cost"))));
            present(routeLabel,facts.value("inputs_ready").toBool()?QStringLiteral("未作本局收益排序 · 同版本出装参考"):QStringLiteral("未计已选强化 · 同版本公开出装参考"));
            routeLabel->setToolTip(QStringLiteral("资料组合不代表本局最优购买顺序。优先衔接已有材料，未核实强化改价与实际商店价格。\n来源：")+reference.value("source_url").toString());actionDetail->setToolTip(routeLabel->toolTip());
            const double cost=reference.value("action_cost").toDouble(-1),gold=reference.value("current_gold").toDouble(-1);
            const bool budget=reference.value("budget_known").toBool()&&std::isfinite(cost)&&std::isfinite(gold)&&cost>=0&&gold>=0;
            present(priceLabel,budgetLabel(budget?QJsonValue(std::max(0.,cost-gold)):QJsonValue(),action!=target));priceLabel->setToolTip(priceNote);
        }
    }
    else if(adaptive.active&&!timedOut){
        displaySource=QStringLiteral("explicit_chain_condition_comparison");
        actionIcon->setProperty("itemId",adaptive.itemId);
        present(actionTitle,QStringLiteral("条件比较 · ")+adaptive.title);
        present(actionDetail,adaptive.selected?QStringLiteral("目录估价 · 请核对商店"):adaptive.ranking?QStringLiteral("当前没有合适的购买候选"):adaptive.status);
        present(routeLabel,QStringLiteral("按给定动作估算 · 条件参考"));
        actionDetail->setToolTip(adaptive.selected?priceNote:adaptive.tooltip);routeLabel->setToolTip(adaptive.selected?priceNote:adaptive.tooltip);
        present(priceLabel,adaptive.selected?budgetLabel(QJsonValue(0),false):QString());if(adaptive.selected)priceLabel->setToolTip(priceNote);
    }
    const auto checklist=snapshot.value("purchase_checklist").toObject();
    const auto rows=checklist.value("rows").toArray();
    const int target=checklist.value("target_item_id").toInt();
    const auto matchId=snapshot.value("match_context").toObject().value("context_id").toString();
    const auto checklistSource=checklist.value("display_source").toString();
    const bool checklistBound=liveShopping&&checklist.value("available").toBool()&&!rows.isEmpty()&&!matchId.isEmpty()
        &&checklist.value("context_id").toString()==matchId&&target>0;
    const auto role=coach.value("role_aware_recommendation").toObject();
    if(checklistBound&&!timedOut&&!adaptive.active&&checklistSource==QStringLiteral("role_guidance")
        &&target==role.value("next_purchase_reference").toObject().value("item_id").toInt()
        &&(role.value("prefer_source_route_for_display").toBool()||role.value("prefer_mechanism_reference_for_display").toBool())){
        displaySource=checklistSource;
        actionIcon->setProperty("itemId",target);
        present(actionTitle,QStringLiteral("辅助参考 · ")+catalog.itemName(target));
        actionDetail->setToolTip(priceNote);routeLabel->setToolTip(priceNote);
    }
    if(checklistBound&&target==actionIcon->property("itemId").toInt()
        &&(checklistSource.isEmpty()||checklistSource==displaySource)){
        QStringList steps;for(int i=0;i<rows.size();++i){const auto item=rows[i].toObject();steps.append(QStringLiteral("%1  %2  ≈%3G").arg(i+1).arg(catalog.itemName(item.value("item_id").toInt()),numberText(item.value("estimated_cost"))));}
        present(actionDetail,steps.value(0));present(routeLabel,steps.mid(1).join(QStringLiteral("  /  ")));const auto first=rows.first().toObject();
        const bool component=first.value("step_kind")==QStringLiteral("component")||first.value("item_id").toInt()!=target;
        present(priceLabel,budgetLabel(first.value("gold_gap"),component));priceLabel->setToolTip(priceNote);
    }
    const auto summary=matchView.value("summary").toString();present(rosterSummary,summary.isEmpty()?QStringLiteral("阵容摘要 · 等待选人"):summary);rosterSummary->setToolTip(summary);
    const auto outlook=snapshot.value("match_outlook").toObject();
    if(active==2&&liveFresh&&outlook.value("available").toBool()&&outlook.value("context_id")==snapshot.value("match_context").toObject().value("context_id")){
        present(rosterSummary,outlook.value("summary").toString());rosterSummary->setToolTip(outlook.value("detail").toString()+QStringLiteral("\n")+summary);
    }
    if(auto recentSummary=findChild<QLabel*>(QStringLiteral("rosterRecentResultsSummary"))){const auto recent=matchView.value("recent_results").toObject();recentSummary->setVisible(recent.value("present").toBool());present(recentSummary,recent.value("summary").toString());recentSummary->setToolTip(QStringLiteral("双方只显示已收录的近期战果。仅十人同一缓存批次、模型、窗口和截止时显示等权均值；缺人不补分。均值不是双方实力或本局胜率。"));}
    const auto coverage=matchView.value("coverage").toObject();const int playerCount=coverage.value("players").toInt(),matched=coverage.value("matched").toInt(),sameChampion=coverage.value("same_champion").toInt();QString coverageText=playerCount>0?QStringLiteral("历史资料 %1/%2  ·  同英雄 %3人").arg(matched).arg(playerCount).arg(sameChampion):QStringLiteral("对局资料 · 等待阵容公开");
    QStringList refreshCounts;if(stage=="champ_select"){if(coverage.value("reading").toInt()>0)refreshCounts.append(QStringLiteral("%1人读取").arg(coverage.value("reading").toInt()));if(coverage.value("queued").toInt()>0)refreshCounts.append(QStringLiteral("%1人排队").arg(coverage.value("queued").toInt()));if(coverage.value("failed").toInt()>0)refreshCounts.append(QStringLiteral("%1人失败").arg(coverage.value("failed").toInt()));}else if(playerCount>0)refreshCounts.append(QStringLiteral("已存资料"));if(!refreshCounts.isEmpty())coverageText+=QStringLiteral("  ·  ")+refreshCounts.join(QStringLiteral(" / "));present(coverageLabel,coverageText);coverageLabel->setToolTip(coverage.value("history_label").toString()+QStringLiteral("；")+refreshCounts.join(QStringLiteral("，")));
    updateArtwork();
}

void CoachDashboard::updateArtwork(){
    if(shouldDeferHomeView()){homeViewPending=true;setProperty("homeViewPending",true);return;}
    if(!assets||!actionIcon)return;for(const auto *side:{&allies,&enemies})for(const auto &seat:*side){const int id=seat.body->property("championId").toInt();const auto icon=id>0?assets->champion(id):QPixmap();const auto rounded=icon.isNull()?QPixmap():roundedArtwork(icon,{30,30},seat.icon->devicePixelRatioF(),Kda::radius("artwork"));if(seat.icon->pixmap().cacheKey()!=rounded.cacheKey())seat.icon->setPixmap(rounded);if(icon.isNull())seat.icon->setText(id>0?QStringLiteral("◇"):QStringLiteral("·"));seat.icon->setProperty("artworkReady",!icon.isNull());}
    const int item=actionIcon->property("itemId").toInt();const auto image=item>0?assets->item(item):QPixmap();const auto rounded=image.isNull()?QPixmap():roundedArtwork(image,actionIcon->size(),actionIcon->devicePixelRatioF(),Kda::radius("artwork"));if(actionIcon->pixmap().cacheKey()!=rounded.cacheKey())actionIcon->setPixmap(rounded);if(image.isNull())actionIcon->setText(QStringLiteral("◇"));actionIcon->setProperty("artworkReady",!image.isNull());
}

QJsonObject CoachDashboard::report()const{
    const QJsonObject startupState{{"pending",startupNoticeActive},{"page_present",startupNoticePage!=nullptr},{"page_is_child",startupNoticePage&&!startupNoticePage->isWindow()&&startupNoticePage->window()==this},{"callbacks_retained",bool(startupAccepted)||bool(startupRejected)}};
    const auto topbar=findChild<QWidget*>(QStringLiteral("topbar"));
    const auto campaignCover=findChild<QWidget*>(QStringLiteral("kdaCampaignCover"));
    const auto settingsForm=findChild<QWidget*>(QStringLiteral("settingsContent")),generalSettings=findChild<QWidget*>(QStringLiteral("settingsGeneralPage"));
    QJsonObject settingsResponsive=property("settingsResponsiveDiagnostics").value<QJsonObject>();
    settingsResponsive["columns"]=generalSettings?generalSettings->property("settingsColumns").toInt():0;
    settingsResponsive["content_width"]=settingsForm?settingsForm->width():0;
    settingsResponsive["vertical_scroll_maximum"]=settingsScroll->verticalScrollBar()->maximum();
    settingsResponsive["metallic_finish"]=Kda::metallicFinishReport(this);
    settingsResponsive["match_projection"]=matchProjection.report();
    const auto usageCard=findChild<QWidget*>(QStringLiteral("dsUsagePanel"));settingsResponsive["usage_metric_columns"]=usageCard?usageCard->property("usageMetricColumns").toInt():0;
    settingsResponsive["usage_card_width"]=usageCard?usageCard->width():0;
    if(const auto keyCaption=findChild<QLabel*>(QStringLiteral("dsApiKeyCaption")))settingsResponsive["api_key_caption"]=QJsonObject{{"text",keyCaption->text()},{"contents_width",keyCaption->contentsRect().width()},{"text_width",keyCaption->fontMetrics().horizontalAdvance(keyCaption->text())},{"legacy_english_text_width",keyCaption->fontMetrics().horizontalAdvance(QStringLiteral("API key"))}};
    int ally=0,enemy=0,artwork=0,references=0;for(const auto *side:{&allies,&enemies})for(const auto &seat:*side){if(side==&allies)ally+=seat.body->property("championVisible").toBool();else enemy+=seat.body->property("championVisible").toBool();artwork+=seat.icon->property("artworkReady").toBool();references+=seat.body->property("historyAvailable").toBool();}
    QJsonArray seatGeometry;for(const auto &seat:allies)seatGeometry.append(QJsonObject{{"icon",rectJson(relativeRect(seat.icon,seat.body))},{"marker",rectJson(relativeRect(seat.marker,seat.body))},{"name",rectJson(relativeRect(seat.name,seat.body))},{"detail",rectJson(relativeRect(seat.detail,seat.body))}});
    const QJsonObject layout{{"personal_home",rectJson(relativeRect(personalHome,this))},{"personal_home_plane_stationary",findChild<QWidget*>(QStringLiteral("personalHomePlane"))->property("kdaDepthStationary").toBool()},{"ally_seats",seatGeometry},{"opacity_caption",rectJson(relativeRect(opacityCaption,this))},{"opacity_slider",rectJson(relativeRect(opacity,this))},{"summary",rectJson(relativeRect(rosterSummary,matchScroll->viewport()))},{"topbar_height",topbar?topbar->height():0},{"topbar_top_inset",topbar?relativeRect(topbar,this).top():-1},{"workspace_top",relativeRect(pages,this).top()},{"idle_cover_height",campaignCover?campaignCover->height():0},{"idle_cover_asset_loaded",campaignCover&&static_cast<SatinBanner*>(campaignCover)->artworkLoaded()},{"idle_cover_artwork",campaignCover?static_cast<SatinBanner*>(campaignCover)->artworkName():QString()},{"chrome_state_diagnostics",property("chromeStateDiagnostics").value<QJsonObject>()},{"notice_diagnostics",QJsonObject{{"kda",property("kdaNoticeDiagnostics").value<QJsonObject>()},{"kda_red",property("kda_redNoticeDiagnostics").value<QJsonObject>()}}},{"compact_axis_diagnostics",playerHistory->property("compactAxisDiagnostics").value<QJsonObject>()},{"stress_canvas_diagnostics",playerHistory->property("stressCanvasDiagnostics").value<QJsonObject>()},{"campaign_motion",campaignCover?static_cast<SatinBanner*>(campaignCover)->motionReport():QJsonObject{}},{"navigation_button_motion",static_cast<DashboardButton*>(matchNavigation)->motionReport()},{"settings_responsive",settingsResponsive},{"settings_column_diagnostics",property("settingsColumnDiagnostics").value<QJsonObject>()},{"settings_reachability_diagnostics",property("settingsReachabilityDiagnostics").value<QJsonArray>()},{"settings_minimum_page_diagnostics",property("settingsMinimumPageDiagnostics").value<QJsonObject>()},{"coordinates",QStringLiteral("Qt_logical_pixels")}};
    return {{"has_startup_notice",startupNoticePage!=nullptr},{"startup_notice",startupState},{"waiting_player",waitingPlayer?waitingPlayer->report():QJsonObject{}},{"appearance_save",QJsonObject{{"pending",appearanceSave&&appearanceSave->isPending()},{"flush_count",appearanceSave?appearanceSave->flushes():0},{"delay_ms",1400},{"flush_on_destroy",true}}},{"skin_switch_timing",skinSwitchTiming},{"personal_home",personalHome?personalHome->report():QJsonObject{}},{"settings_motion",static_cast<Kda::SettingsPageMotion*>(findChild<QWidget*>(QStringLiteral("settingsPageMotion")))->report()},{"app_version",QString::fromLatin1(LIXIN_APP_VERSION)},{"settings_tab",settingsTabs->currentIndex()==settingsAcknowledgmentsTab?QStringLiteral("acknowledgments"):settingsTabs->currentIndex()==settingsAboutTab?QStringLiteral("version"):settingsTabs->currentIndex()==settingsSkinsTab?QStringLiteral("skins"):settingsTabs->currentIndex()==settingsModelsTab?QStringLiteral("models"):QStringLiteral("general")},{"workspace_depth",static_cast<FacetedWorkspace*>(findChild<QWidget*>(QStringLiteral("dashboardWorkspace")))->depthReport()},{"skin_transition",skinTransition?static_cast<Kda::SkinTransition*>(skinTransition)->report():QJsonObject{}},{"theme_pack",Kda::themeReport()},{"navigation_motion",navigationRail?static_cast<NavigationRail*>(navigationRail)->report():QJsonObject{}},{"visible",isVisible()},{"page",pages->currentIndex()==0?QStringLiteral("match"):pages->currentIndex()==2?QStringLiteral("history"):QStringLiteral("settings")},{"presentation",matchView.value("stage").toString().isEmpty()?QStringLiteral("idle"):QStringLiteral("match")},{"geometry",QJsonArray{x(),y(),width(),height()}},{"minimum_size",QJsonArray{minimumWidth(),minimumHeight()}},{"single_runtime",true},{"local_network_clients",0},{"close_hides",closesToTray()},{"close_window_behavior",closeWindowBehavior->currentData().toBool()?QStringLiteral("hide_to_tray"):QStringLiteral("exit_assistant")},{"player_history",playerHistory->report()},{"navigation_count",3},{"replay_inline",replayPage!=nullptr},{"replay_visible",replayPage&&replayPage->isVisibleTo(this)},{"design","kda_editorial_double_radius"},{"navigation_layout","topbar"},{"window_chrome",chrome?chrome->report():QJsonObject{}},{"outer_radius",Kda::radius("window")},{"inner_radius",Kda::radius("workspace")},{"surface_gap",Kda::radius("gap")},{"theme","black_silver_pearl"},{"home_view_pending",homeViewPending},{"home_view_render_count",homeViewRenderCount},{"visible_allies",ally},{"visible_enemies",enemy},{"portraits_ready",artwork},{"history_references",references},{"history_context_valid",matchView.value("history_context_valid")},{"history_is_live_damage",false},{"coverage",matchView.value("coverage")},{ "roster_recent_results",matchView.value("recent_results")},{"score_known",scoreLabel->property("scoreKnown").toBool()},{"unknown_scores_are_null",!scoreLabel->property("scoreKnown").toBool()},{"model_configuration",modelConfigurationReport()},{"deepseek_usage_display",dsUsagePresentation},{"api_key_masked",apiKey->echoMode()==QLineEdit::Password},{"private_identities_exported",false},{"api_key_exported",false},{"snapshot_scope",inSelfCheck?QStringLiteral("synthetic_dashboard_fixture"):QStringLiteral("anonymous_view_of_existing_runtime")},{"self_check",QJsonObject{{"snapshot_scope",QStringLiteral("synthetic_Qt_widgets_not_live_game")},{"focus_motion_scope",property("focusMotionScope").toString()},{"settings_write_performed",false}}},{"growth_capture_status",growthCaptureStatus->property("fullText").toString()},{"growth_capture_settings_action",growthCaptureAction->isVisibleTo(this)},{"layout",layout},{"self_checks",checks}};
}

void CoachDashboard::selfCheck(const QString &outputDir){
    setProperty("focusMotionScope",QStringLiteral("synthetic_effect_transition_not_OS_focus"));
    const int originalHomePage=homePages?homePages->currentIndex():0;
    const bool originalReplaySawMatch=replaySawMatch;
    const auto original=snapshot;const auto originalGeometry=geometry();const auto originalWindowState=windowState();const bool wasVisible=isVisible();const int originalPage=pages->currentIndex();const auto originalKey=apiKey->text(),originalModel=model->currentText(),originalStatus=saveStatus->text();const int originalSettingsTabAtStart=settingsTabs->currentIndex(),originalEditingModelSlot=modelSlot->currentIndex();const auto originalModelSaveStatus=modelSaveStatus->text();QWidget *originalFocus=focusWidget();QMap<QString,QVariant> beforePreferences;for(const auto &key:preferences.allKeys())beforePreferences[key]=preferences.value(key);inSelfCheck=true;checks={};setWindowState(Qt::WindowNoState);modelSlot->setCurrentIndex(0);
    if(homePages)homePages->setCurrentIndex(0);
    const auto flush=[] {
        // A responsive grid changes its page hint, then the scroll area's
        // range. Drain these queued layout passes before inspecting geometry.
        for(int pass=0;pass<3;++pass){QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);}
    };
    const auto save=[&](const QString &name){if(outputDir.isEmpty())return;QElapsedTimer settle;settle.start();while(navigationRail&&static_cast<NavigationRail*>(navigationRail)->report().value("running").toBool()&&settle.elapsed()<1200){flush();QThread::msleep(2);}flush();QDir().mkpath(outputDir);grab().save(QDir(outputDir).filePath(name+QStringLiteral(".png")));};
    const auto fit=[&]{bool okay=true;for(auto area:{matchScroll,settingsScroll})if(area->isVisibleTo(this))okay&=area->horizontalScrollBar()->maximum()==0&&area->widget()->width()<=area->viewport()->width();for(auto label:findChildren<QLabel*>())if(label->isVisibleTo(this)&&label->property("fullText").isValid())okay&=label->fontMetrics().horizontalAdvance(label->text())<=label->contentsRect().width()+1;return okay;};
    const auto seatColumnsFit=[&]{
        bool okay=true;
        for(const auto *side:{&allies,&enemies})for(const auto &seat:*side){
            const auto icon=relativeRect(seat.icon,seat.body);QList<QRect> occupied;
            for(auto control:QList<QWidget*>{seat.icon,seat.marker,seat.name,seat.detail}){
                if(!control->isVisibleTo(seat.body))continue;
                const auto box=relativeRect(control,seat.body);
                okay&=seat.body->contentsRect().contains(box)&&!box.isEmpty();
                for(const auto &other:occupied)okay&=!box.intersects(other);
                occupied.append(box);
                if(control==seat.name||control==seat.detail)
                    okay&=box.left()>=icon.right()+6&&box.height()>=control->fontMetrics().height();
            }
        }
        return okay;
    };
    const auto firstScreen=[&]{bool okay=matchScroll->viewport()->rect().contains(relativeRect(scoreLabel,matchScroll->viewport()))&&matchScroll->viewport()->rect().contains(relativeRect(actionTitle,matchScroll->viewport()));for(const auto *side:{&allies,&enemies})for(const auto &seat:*side)okay&=matchScroll->viewport()->rect().contains(relativeRect(seat.body,matchScroll->viewport()));return okay;};
    const auto keyLabelsFit=[&]{return matchTitle->text()==matchTitle->property("fullText").toString()&&connectionLabel->text()==connectionLabel->property("fullText").toString();};
    const auto idleLabelsFit=[&]{auto coverWidget=findChild<QWidget*>(QStringLiteral("kdaCampaignCover"));if(!coverWidget||!personalHome)return false;const auto visible=matchScroll->viewport()->rect();return personalHome->report().value("content_fits").toBool()&&visible.contains(relativeRect(coverWidget,matchScroll->viewport()))&&visible.contains(relativeRect(personalHome,matchScroll->viewport()))&&!relativeRect(coverWidget,matchScroll->viewport()).intersects(relativeRect(personalHome,matchScroll->viewport()));};
    const auto homeHasNoSearch=[&]{return !findChild<QWidget*>(QStringLiteral("homePlayerLauncher"))&&!findChild<QWidget*>(QStringLiteral("homeHistorySearch"))&&!findChild<QLineEdit*>(QStringLiteral("homeSummonerSearch"));};
    const auto waitForArtwork=[&]{QElapsedTimer timer;timer.start();while(timer.elapsed()<500){flush();if(assets->report().value("pending").toInt()==0)break;QThread::msleep(2);}updateArtwork();flush();};
    setProperty("settingsReachabilityDiagnostics",QVariant::fromValue(QJsonArray{}));
    const auto settingsWidgetIdentity=[&](QWidget *widget){
        QStringList path;for(auto node=widget;node&&node!=settingsPage;node=node->parentWidget()){
            const int ordinal=node->parentWidget()?node->parentWidget()->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly).indexOf(node):-1;
            path.prepend(QString::fromLatin1(node->metaObject()->className())+QChar(u':')+node->objectName()+QChar(u'#')+QString::number(ordinal));
        }return path.join(QChar(u'/'));
    };
    const auto recordSettingsFailure=[&](QJsonObject failure){
        auto failures=property("settingsReachabilityDiagnostics").value<QJsonArray>();if(failures.size()>=64)return;
        failure["window_size"]=QJsonArray{width(),height()};failure["settings_tab"]=settingsTabs->currentIndex();failure["editing_slot"]=modelSlot->currentData().toString();
        failures.append(failure);setProperty("settingsReachabilityDiagnostics",QVariant::fromValue(failures));
    };
    const auto settingsWidgetReachable=[&](QWidget *widget){
        if(!widget||!widget->isVisibleTo(this))return false;
        const int before=settingsScroll->verticalScrollBar()->value();settingsScroll->ensureWidgetVisible(widget,0,0);flush();
        // Qt's helper targets an editor's IME cursor rectangle. Require the
        // complete control border here, adjusting by its measured difference.
        auto completeBounds=relativeRect(widget,settingsScroll->viewport());const auto viewportBounds=settingsScroll->viewport()->rect();
        if(completeBounds.height()<=viewportBounds.height()){
            const int delta=completeBounds.top()<viewportBounds.top()?completeBounds.top()-viewportBounds.top():completeBounds.bottom()>viewportBounds.bottom()?completeBounds.bottom()-viewportBounds.bottom():0;
            if(delta){auto bar=settingsScroll->verticalScrollBar();bar->setValue(bar->value()+delta);flush();}
        }
        const auto bounds=relativeRect(widget,settingsScroll->viewport());const bool fits=settingsScroll->viewport()->rect().contains(bounds);
        if(!fits)recordSettingsFailure({{"reason","readable_not_scroll_reachable"},{"widget",settingsWidgetIdentity(widget)},{"widget_rect_in_viewport",rectJson(bounds)},{"viewport_rect",rectJson(settingsScroll->viewport()->rect())},{"scroll_before",before},{"scroll_after",settingsScroll->verticalScrollBar()->value()},{"scroll_maximum",settingsScroll->verticalScrollBar()->maximum()},{"widget_minimum_hint",QJsonArray{widget->minimumSizeHint().width(),widget->minimumSizeHint().height()}}});
        settingsScroll->verticalScrollBar()->setValue(before);flush();return fits;
    };
    const auto inspectSettingsTab=[&](int index,const auto &inspect){const int previous=settingsTabs->currentIndex(),scrollBefore=settingsScroll->verticalScrollBar()->value();settingsTabs->setCurrentIndex(index);flush();const bool okay=inspect();settingsTabs->setCurrentIndex(previous);settingsScroll->verticalScrollBar()->setValue(scrollBefore);flush();return okay;};
    const auto opacityCaptionFits=[&]{return inspectSettingsTab(settingsGeneralTab,[&]{const auto caption=relativeRect(opacityCaption,this),slider=relativeRect(opacity,this);return opacityCaption->isVisibleTo(this)&&opacityCaption->text()==QStringLiteral("不透明度")&&opacityCaption->contentsRect().width()>=opacityCaption->fontMetrics().horizontalAdvance(opacityCaption->text())&&opacityCaption->height()>=opacityCaption->fontMetrics().height()&&!caption.intersects(slider)&&settingsWidgetReachable(opacityCaption);});};
    const auto uiBrowserFits=[&]{return inspectSettingsTab(settingsGeneralTab,[&]{const auto font=Kda::font(Kda::FontRole::Label,12,QFont::Normal);return uiBrowserAction&&uiBrowserAction->isVisibleTo(this)&&uiBrowserAction->focusPolicy()==Qt::StrongFocus&&uiBrowserAction->width()>=QFontMetrics(font).horizontalAdvance(uiBrowserAction->text())+16&&uiBrowserAction->height()>=QFontMetrics(font).height()&&settingsWidgetReachable(uiBrowserAction);});};
    const auto growthSourceFits=[&]{return inspectSettingsTab(settingsGeneralTab,[&]{const auto font=Kda::font(Kda::FontRole::Label,12,QFont::Normal);return growthCaptureAction&&growthCaptureStatus&&growthCaptureAction->isVisibleTo(this)&&growthCaptureAction->focusPolicy()==Qt::StrongFocus&&growthCaptureAction->width()>=QFontMetrics(font).horizontalAdvance(growthCaptureAction->text())+16&&settingsWidgetReachable(growthCaptureAction)&&!relativeRect(growthCaptureAction,settingsPage).intersects(relativeRect(growthCaptureStatus,settingsPage))&&growthCaptureStatus->text()==growthCaptureStatus->property("fullText").toString();});};
    const auto preferenceRowsFit=[&]{auto textFont=Kda::font(Kda::FontRole::Body,12);textFont.setWeight(QFont::Normal);const auto fits=[&](QCheckBox *control){return control->isVisibleTo(this)&&control->height()==Kda::Density::switchHeight&&QFontMetrics(textFont).horizontalAdvance(control->text())<=control->width()-64&&control->focusPolicy()==Qt::StrongFocus;};return inspectSettingsTab(settingsGeneralTab,[&]{bool okay=true;for(auto control:{overlayEnabled,cardHintsEnabled,shopRecommendationsEnabled,autoMinimize,reduceMotion})okay&=fits(control);return okay;})&&inspectSettingsTab(settingsModelsTab,[&]{return fits(rosterDsEnabled);});};
    const auto titlebarFits=[&]{
        const auto bar=findChild<QWidget*>(QStringLiteral("topbar"));const auto navigation=findChild<QWidget*>(QStringLiteral("navigationGroup"));
        if(!bar||!navigation||bar->height()!=DashboardChrome::TitleBarHeight||relativeRect(bar,this).top()!=0||relativeRect(pages,this).top()!=DashboardChrome::TitleBarHeight||navigation->height()!=34||matchNavigation->height()!=28||settingsNavigation->height()!=28)return false;
        QList<QRect> occupied;for(auto child:bar->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly)){if(!child->isVisibleTo(this))continue;const auto geometry=child->geometry();if(!bar->rect().contains(geometry))return false;for(const auto &other:occupied)if(geometry.intersects(other))return false;occupied.append(geometry);}
        return occupied.size()==3&&navigation->rect().contains(matchNavigation->geometry())&&navigation->rect().contains(settingsNavigation->geometry())&&!matchNavigation->geometry().intersects(settingsNavigation->geometry())&&matchNavigation->focusPolicy()==Qt::StrongFocus&&settingsNavigation->focusPolicy()==Qt::StrongFocus;
    };
    const auto statusbarFits=[&]{
        const auto bar=findChild<QWidget*>(QStringLiteral("dashboardStatusBar"));if(!bar||!bar->isVisibleTo(this))return false;
        const auto barRect=relativeRect(bar,this),pageRect=relativeRect(pages,this);
        const auto workspace=findChild<QWidget*>(QStringLiteral("dashboardWorkspace"));
        if(!workspace||!workspace->isAncestorOf(bar)||!workspace->rect().contains(relativeRect(bar,workspace))||!rect().contains(barRect)||barRect.top()<=pageRect.bottom()||barRect.intersects(pageRect))return false;
        const QList<QWidget*> expected{connectionLabel,bar->findChild<QWidget*>(QStringLiteral("openTutorial")),bar->findChild<QWidget*>(QStringLiteral("openHudLayout")),bar->findChild<QWidget*>(QStringLiteral("openReplay")),visibilityAction};
        for(auto control:expected)if(!control||control->parentWidget()!=bar||!control->isVisibleTo(this))return false;
        QList<QRect> bounds;
        for(auto child:bar->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly)){
            if(!child->isVisibleTo(this))continue;
            const auto box=child->geometry();if(!expected.contains(child)||box.isEmpty()||!bar->rect().contains(box))return false;
            for(const auto &other:bounds)if(box.intersects(other))return false;
            bounds.append(box);
        }
        return bounds.size()==expected.size()&&connectionLabel->text()==connectionLabel->property("fullText").toString();
    };
    QJsonArray rows;for(int i=0;i<5;++i){rows.append(QJsonObject{{"champion_id",QJsonArray{1,10,22,86,12}[i]},{"team","ALLY"},{"self",i==0},{"cell",i}});rows.append(QJsonObject{{"champion_id",QJsonValue()},{"team","ENEMY"},{"self",false},{"cell",i+5}});}
    QJsonObject fixture{{"connection",QStringLiteral("客户端已连接")},{"live_phase","ChampSelect"},{"observed_patch",catalog.patch},{"current_champion_id",1},{"match_context",QJsonObject{{"active",true},{"context_id","synthetic-dashboard"},{"stage","champ_select"},{"roster",rows},{"composition",QJsonObject{{"summary",QStringLiteral("队友已公开 · 对手待公开")}}}}}};
    fixture["champ_select"]=QJsonObject{{"active",true},{"stage","champ_select"},{"self_champion_id",1},{"roster",rows},{"bench_known",true},{"bench_champion_ids",QJsonArray{516,51,25,223}}};
    resize(840,520);show();selectPage(1);
    const int shopFixturePreviousTab=settingsTabs->currentIndex();
    settingsTabs->setCurrentIndex(settingsGeneralTab);
    auto shopFixture=fixture;
    shopFixture["shop_recommendations"]=QJsonObject{{"state","ready"},{"source_version","16.22"},{"hot_reload_verified",false}};
    applySnapshot(shopFixture);flush();
    const auto writtenShopTitle=QStringLiteral("已写入「李金金 · 当前出装」");
    const auto shopHelpText=shopRecommendationsStatus->toolTip();
    checks["shop_status_updates_while_settings_are_visible"]=shopRecommendationsStatus->isVisibleTo(this)&&shopRecommendationsStatus->text()==writtenShopTitle
        &&shopHelpText.contains(QStringLiteral("装备方案"))&&shopHelpText.contains(QStringLiteral("李金金 · 当前出装 · 16.22"))
        &&shopHelpText.contains(QStringLiteral("显示时机由游戏决定"))&&shopRecommendationsStatus->accessibleDescription()==shopHelpText;
    resize(minimumSize());flush();
    checks["shop_status_survives_resize_and_fits_minimum_settings"]=shopRecommendationsStatus->text()==writtenShopTitle
        &&shopRecommendationsStatus->property("fullText").toString()==writtenShopTitle
        &&settingsWidgetReachable(shopRecommendationsStatus);
    shopFixture["shop_recommendations"]=QJsonObject{{"state","waiting_champion"}};applySnapshot(shopFixture);
    checks["shop_waits_for_current_champion_in_selection"]=shopRecommendationsStatus->text()==QStringLiteral("等待确认当前英雄")
        &&shopRecommendationsStatus->toolTip().isEmpty()&&shopRecommendationsStatus->accessibleDescription().isEmpty();
    shopFixture["shop_recommendations"]=QJsonObject{{"state","waiting_verified_recommendation"}};applySnapshot(shopFixture);
    checks["shop_names_missing_verified_equipment"]=shopRecommendationsStatus->text()==QStringLiteral("当前英雄暂无已核验出装");
    shopFixture["shop_recommendations"]=QJsonObject{{"state","waiting_client_directory"}};applySnapshot(shopFixture);
    checks["shop_names_missing_client_directory"]=shopRecommendationsStatus->text()==QStringLiteral("等待客户端提供游戏目录");
    shopFixture["shop_recommendations"]=QJsonObject{{"state","registration_changed"}};applySnapshot(shopFixture);
    checks["shop_failure_is_visible_without_rebuilding_home"]=shopRecommendationsStatus->text().contains(QStringLiteral("暂未更新"));
    settingsTabs->setCurrentIndex(shopFixturePreviousTab);
    resize(840,520);show();selectPage(0);applySnapshot(fixture);waitForArtwork();checks["default_main_window_is_compact"]=size()==QSize(840,520);checks["stable_three_destination_navigation"]=pages->count()==3&&matchNavigation->isChecked()&&!settingsNavigation->isChecked();checks["unknown_scores_use_dash"]=!scoreLabel->property("scoreKnown").toBool()&&scoreLabel->text().contains(QStringLiteral("—"));checks["five_real_ally_slots_and_hidden_enemy_placeholders"]=report().value("visible_allies").toInt()==5&&report().value("visible_enemies").toInt()==0;checks["home_content_fits"]=fit();checks["home_seat_labels_do_not_overlap"]=seatColumnsFit();checks["summary_visible_on_default_home"]=matchScroll->viewport()->rect().contains(relativeRect(rosterSummary,matchScroll->viewport()));checks["default_match_information_on_first_screen"]=firstScreen();
    const auto chromeChecks=chrome->geometryChecks();for(auto it=chromeChecks.begin();it!=chromeChecks.end();++it)checks[it.key()]=it.value();
    checks["wide_titlebar_controls_inside_and_separate"]=titlebarFits();checks["status_actions_separate_from_navigation"]=statusbarFits();checks["product_name_is_lijinjin"]=windowTitle()==QStringLiteral("李金金 · 对局助手")&&findChild<QWidget*>(QStringLiteral("wordmark"))->accessibleName()==QStringLiteral("李金金");
    auto brandMark=static_cast<BrandMark*>(findChild<QWidget*>(QStringLiteral("wordmark")));
    checks["approved_logo_asset_loaded"]=brandMark->property("artworkLoaded").toBool()&&brandMark->artworkCoverage();
    checks["brand_assets_embedded"]=!QImage(QStringLiteral(":/lixin/kda/brand/lijinjin-wordmark.png")).isNull()&&QFileInfo::exists(QStringLiteral(":/lixin/kda/brand/lijinjin-app.ico"))&&!QImage(QStringLiteral(":/lixin/kda/brand/lijinjin-app.png")).isNull();
    const QIcon brandIcon(QStringLiteral(":/lixin/kda/brand/lijinjin-app.ico"));const auto brandIconSizes=brandIcon.availableSizes();bool brandIconDpiSizes=true;for(int pixels:{16,20,24,32,40,48,64,96,128,256})brandIconDpiSizes&=brandIconSizes.contains(QSize(pixels,pixels));
    checks["jinx_application_icon_has_small_and_high_dpi_sizes"]=brandIconDpiSizes&&!brandIcon.pixmap(QSize(16,16)).isNull()&&!brandIcon.pixmap(QSize(64,64)).isNull();
    checks["jinx_header_logo_fits_titlebar"]=brandMark->size()==QSize(28,32)&&brandMark->height()<DashboardChrome::TitleBarHeight;
    checks["workspace_material_retains_four_transparent_rounded_corners"]=static_cast<FacetedWorkspace*>(findChild<QWidget*>(QStringLiteral("dashboardWorkspace")))->roundedCoverage();
    checks["workspace_has_no_inset_glass_display_frame"]=!report().value("workspace_depth").toObject().value("display_stage_frame").toBool(true);
    checks["background_stays_behind_live_native_depth_planes"]=report().value("workspace_depth").toObject().value("backdrop_is_static").toBool();
    const auto originalPlayerName=localPlayerDisplayName;auto cover=static_cast<SatinBanner*>(findChild<QWidget*>(QStringLiteral("kdaCampaignCover")));
    const auto beforeWaiting=snapshot;setLocalPlayerDisplayName({});applySnapshot({});flush();
    checks["waiting_page_replaces_unknown_metrics_and_profile"]=waitingPlayer->isVisibleTo(this)&&!idleState->isVisibleTo(this)&&!matchBoard->isVisibleTo(this)&&!waitingPlayer->report().value("fake_progress").toBool();waitingPlayer->setInspectionTime(1.4);flush();save("dashboard-waiting");waitingPlayer->setInspectionTime(-1);
    const auto waitingLook=waitingPlayer->report();
    const bool crimsonWaiting=Kda::HomeMaterialDetail::fire();
    checks["waiting_poster_uses_native_text_over_the_selected_backdrop"]=waitingLook.value("display_font_loaded").toBool()
        &&!waitingLook.value("text_rasterized").toBool()&&waitingLook.value("backdrops_ready").toBool()
        &&waitingLook.value("background_rendering")==QString::fromLatin1(crimsonWaiting?"native_vector":"cached_bitmap");
    activateWindow();QApplication::setActiveWindow(this);flush();
    const auto waitMotion=[&](int ms){QElapsedTimer timer;timer.start();while(timer.elapsed()<ms){flush();QThread::msleep(2);}flush();};
    const int beforeFrames=waitingPlayer->report().value("motion").toObject().value("frames").toInt();const auto beforeWaitingFrame=waitingPlayer->grab().toImage();waitMotion(150);
    checks["waiting_animation_advances_only_in_active_window"]=((waitingPlayer->report().value("motion").toObject().value("frames").toInt()>beforeFrames)&&beforeWaitingFrame!=waitingPlayer->grab().toImage())==(!Kda::reducedMotion()&&isActiveWindow());save("dashboard-waiting-motion");
    waitingPlayer->setInspectionTime(0);flush();const auto firstLoopFrame=waitingPlayer->grab().toImage();waitingPlayer->setInspectionTime(6.4);flush();checks["waiting_type_loop_joins_identical_frames"]=firstLoopFrame==waitingPlayer->grab().toImage();
    waitingPlayer->setInspectionTime(1.4);flush();const auto stableTypeFrame=waitingPlayer->grab().toImage();waitingPlayer->setInspectionTime(2.14);flush();const auto sliceTypeFrame=waitingPlayer->grab().toImage();save("dashboard-waiting-slice");waitingPlayer->setInspectionTime(3.45);flush();const auto wipeTypeFrame=waitingPlayer->grab().toImage();save("dashboard-waiting-wipe");
    const QRect staticCopy(0,qRound(stableTypeFrame.height()*.73),stableTypeFrame.width(),qFloor(stableTypeFrame.height()*.27));checks["waiting_type_slices_and_matte_change_only_display_lettering"]=stableTypeFrame!=sliceTypeFrame&&stableTypeFrame!=wipeTypeFrame&&stableTypeFrame.copy(staticCopy)==sliceTypeFrame.copy(staticCopy)&&stableTypeFrame.copy(staticCopy)==wipeTypeFrame.copy(staticCopy);
    const auto waitingSkin=Kda::skin();Kda::setSkin(QStringLiteral("kda_red"));waitingPlayer->setInspectionTime(1.4);flush();save("dashboard-waiting-red");Kda::setSkin(waitingSkin);flush();
    resize(760,500);flush();checks["waiting_page_fits_minimum_window"]=waitingPlayer->report().value("content_fits").toBool()&&matchScroll->viewport()->rect().contains(relativeRect(waitingPlayer,matchScroll->viewport()))&&fit();save("dashboard-waiting-minimum");resize(840,520);
    waitingPlayer->setInspectionTime(-1);const bool beforeReduced=Kda::reducedMotion();Kda::setReducedMotion(true);flush();const int frozenFrames=waitingPlayer->report().value("motion").toObject().value("frames").toInt();const auto reducedTypeFrame=waitingPlayer->grab().toImage();waitMotion(100);checks["waiting_animation_respects_reduced_motion"]=!waitingPlayer->report().value("motion").toObject().value("running").toBool()&&waitingPlayer->report().value("motion").toObject().value("frames").toInt()==frozenFrames&&reducedTypeFrame==waitingPlayer->grab().toImage();Kda::setReducedMotion(beforeReduced);
    setLocalPlayerDisplayName(QStringLiteral("星河测试玩家"));flush();checks["player_identity_replaces_waiting_with_personal_home"]=!waitingPlayer->isVisibleTo(this)&&idleState->isVisibleTo(this);
    setLocalPlayerDisplayName({});applySnapshot(beforeWaiting);flush();checks["match_stage_does_not_wait_for_account_name"]=!waitingPlayer->isVisibleTo(this)&&matchBoard->isVisibleTo(this)&&!waitingPlayer->report().value("motion").toObject().value("running").toBool();
    setLocalPlayerDisplayName({});checks["idle_cover_waits_for_current_account_name"]=cover->titleText()==QStringLiteral("等待玩家昵称");
    setLocalPlayerDisplayName(QStringLiteral("星河测试玩家"));checks["idle_cover_shows_current_account_name"]=cover->titleText()==QStringLiteral("星河测试玩家");
    setLocalPlayerDisplayName(QStringLiteral("换号测试玩家"));checks["idle_cover_account_switch_replaces_old_name"]=cover->titleText()==QStringLiteral("换号测试玩家");
    setLocalPlayerDisplayName(QStringLiteral("超长玩家昵称ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"));checks["idle_cover_long_account_name_is_elided"]=cover->displayedTitle()!=cover->titleText()&&QFontMetrics(cover->titleFont()).horizontalAdvance(cover->displayedTitle())<=cover->titleRect().width();
    checks["local_account_name_not_in_public_report"]=!QJsonDocument(report()).toJson().contains(localPlayerDisplayName.toUtf8());
    setLocalPlayerDisplayName({});checks["idle_cover_disconnect_clears_old_account_name"]=cover->name().isEmpty()&&cover->titleText()==QStringLiteral("等待玩家昵称");
    setLocalPlayerDisplayName(QStringLiteral("星河测试玩家"));
    const auto rounded=grab().toImage();checks["outer_corners_are_transparent"]=rounded.hasAlphaChannel()&&rounded.pixelColor(0,0).alpha()==0&&rounded.pixelColor(rounded.width()-1,0).alpha()==0&&rounded.pixelColor(0,rounded.height()-1).alpha()==0&&rounded.pixelColor(rounded.width()-1,rounded.height()-1).alpha()==0;checks["outer_center_retains_reading_opacity"]=rounded.pixelColor(rounded.width()/2,rounded.height()/2).alpha()>=232;checks["dual_radius_content_inset"]=centralWidget()->layout()->contentsMargins()==QMargins(qRound(Kda::radius("gap")),0,qRound(Kda::radius("gap")),qRound(Kda::radius("gap")))&&pages->objectName()==QStringLiteral("dashboardPages");save("dashboard-home");
    bool smoothCorners=true;const int cornerSpan=qCeil(Kda::radius("window")*rounded.devicePixelRatio())+2;
    for(bool right:{false,true})for(bool bottom:{false,true}){QSet<int> coverage;
        for(int y=0;y<cornerSpan;++y)for(int x=0;x<cornerSpan;++x){const int alpha=rounded.pixelColor(right?rounded.width()-1-x:x,bottom?rounded.height()-1-y:y).alpha();if(alpha>0&&alpha<255)coverage.insert(alpha);}
        smoothCorners&=coverage.size()>=6;
    }
    checks["all_four_outer_corners_keep_fractional_alpha_coverage"]=smoothCorners;
    const auto nativeShape=chrome->report().value("shape").toObject();
    checks["native_blur_clip_preserves_smooth_edges"]=nativeShape.value("preserves_fractional_alpha").toBool()&&(!nativeShape.value("native_clip_applied").toBool()||nativeShape.value("clip_excludes_only_zero_alpha").toBool());
    QJsonArray fullRows,currentRows,historyRows;const QJsonArray champions{1,10,22,86,12,64,99,51,54,40};
    for(int i=0;i<10;++i){
        const QJsonObject row{{"champion_id",champions[i]},{"team",i<5?"ALLY":"ENEMY"},{"self",i==0},{"cell",i}};fullRows.append(row);auto observed=row;observed["level"]=i==0?13:12;observed["scores"]=QJsonObject{{"kills",i==0?8:4+i%4},{"deaths",3+i%3},{"assists",11+i%4}};observed["items"]=QJsonArray{3020,3100};currentRows.append(observed);
        const int samples=12+i;QJsonObject metrics{{"kills",7.2+i*.2},{"deaths",5.1},{"assists",12.4},{"damage_per_minute",1160+i*47},{"gold_per_minute",570+i*8},{"taken_per_minute",820+i*38}};QJsonObject fieldSamples;for(auto it=metrics.begin();it!=metrics.end();++it)fieldSamples[it.key()]=samples;const QJsonObject support{{"available",true},{"sampled_games",samples},{"metrics",metrics},{"field_samples",fieldSamples}};
        auto reference=row;reference["status"]="ready";reference["same_patch_champion"]=support;reference["same_patch"]=support;reference["profile_summary"]=QJsonObject{{"available",true},{"sampled_games",48+i*3}};reference["champion_references"]=QJsonObject{{"available",true},{"same_patch_sampled_games",samples}};reference["freshness"]=QJsonObject{{"status","fresh"},{"refresh_status",i==6?"active":"complete"}};
        const QJsonObject comparison{{"status","supported_comparison"},{"direction",i%3==0?"above_reference":i%3==1?"similar_reference":"below_reference"},{"own_participant_rows",samples},{"other_participant_rows",89},{"other_distinct_players",11}};reference["multidimensional_profile"]=QJsonObject{{"status","known"},{"overall",QJsonObject{{"games",48+i*3}}},{"same_patch_champion",QJsonObject{{"games",samples}}},{"conditional_performance_reference",QJsonObject{{"scope","same_champion_same_patch_leave_one_player_out"},{"components",QJsonObject{{"damage_to_champions_per_minute",comparison}}}}}};historyRows.append(reference);
    }
    auto liveFixture=fixture;liveFixture["live_phase"]="InProgress";liveFixture["live_data_fresh"]=true;liveFixture["current_game_mode"]="KIWI";liveFixture["current_queue_id"]=2400;liveFixture["game_time"]=729;liveFixture["current_level"]=13;liveFixture["current_players"]=currentRows;liveFixture["augment_source"]=QJsonObject{{"status","synthetic_demo"}};auto context=fixture.value("match_context").toObject();context["stage"]="in_game";context["started_at_ms"]=2000;context["roster"]=fullRows;context["composition"]=QJsonObject{{"summary",QStringLiteral("前排接团 · 后排保持距离，结合已选海克斯成长")}};liveFixture["match_context"]=context;
    liveFixture["anonymous_player_history"]=QJsonObject{{"context_id","synthetic-dashboard"},{"patch",catalog.patch},{"mode","KIWI"},{"queue_id",2400},{"historical_reference_only",true},{"status","complete"},{"as_of_ms",1000},{"players",historyRows},{"multidimensional_context",QJsonObject{{"status","ready"},{"as_of_ms",1000},{"materialized_at_ms",1000}}}};
    liveFixture["coach"]=QJsonObject{{"recommendation_quality",QJsonObject{{"primary_purchase_visible",true},{"ranking_available",true}}},{"economy",QJsonObject{{"scores_complete",true},{"ally_kills",33},{"enemy_kills",26},{"next_purchase",QJsonObject{{"item_id",3100},{"gold_gap",1640},{"buyable_components",QJsonArray{QJsonObject{{"item_id",1052},{"remaining_cost",400}},QJsonObject{{"item_id",1026},{"remaining_cost",850}}}}}}}},{"plan",QJsonObject{{"resolved_preference",QStringLiteral("技能爆发")},{"growth_reasoning",QJsonObject{{"summary",QStringLiteral("钢之心 · 生命成长联动")}}}}}};
    if(replayPage){
        applySnapshot(liveFixture);showReplay();flush();
        const auto replayWidget=replayPage->findChild<QWidget*>(QStringLiteral("coachReplay"));
        const auto replayList=replayPage->findChild<QWidget*>(QStringLiteral("replayEntries"));
        const auto replayDetail=replayPage->findChild<QWidget*>(QStringLiteral("replayDetail"));
        checks["inline_replay_uses_dashboard_window_and_keeps_both_reading_panes_visible"]=pages->currentIndex()==0&&replayPage->isVisibleTo(this)&&!matchScroll->isVisibleTo(this)&&replayWidget&&!replayWidget->isWindow()&&replayWidget->window()==this&&!replayWidget->findChild<QWidget*>(QStringLiteral("replayTitlebar"))&&replayList&&replayDetail&&replayList->isVisibleTo(this)&&replayDetail->isVisibleTo(this)&&replayPage->rect().contains(relativeRect(replayList,replayPage))&&replayPage->rect().contains(relativeRect(replayDetail,replayPage));
        save("dashboard-inline-replay");
        const auto replaySize=size();resize(760,500);flush();
        checks["minimum_inline_replay_keeps_reading_panes_separate"]=replayList&&replayDetail&&replayPage->rect().contains(relativeRect(replayList,replayPage))&&replayPage->rect().contains(relativeRect(replayDetail,replayPage))&&!relativeRect(replayList,replayPage).intersects(relativeRect(replayDetail,replayPage));
        save("dashboard-inline-replay-minimum");resize(replaySize);flush();
        auto staleRecord=liveFixture;staleRecord["live_data_fresh"]=false;staleRecord["match_context"]=QJsonObject{};applySnapshot(staleRecord);flush();
        const bool staleStayed=replayPage->isVisibleTo(this);
        staleRecord.remove("live_phase");applySnapshot(staleRecord);flush();
        checks["inline_replay_survives_stale_data_and_missing_phase"]=staleStayed&&replayPage->isVisibleTo(this);
        // Reopening while observation is stale must retain the known match
        // lifecycle so its later explicit end still restores the homepage.
        showReplay();flush();
        auto endedRecord=staleRecord;endedRecord["live_phase"]=QStringLiteral("EndOfGame");applySnapshot(endedRecord);flush();
        checks["inline_replay_returns_to_personal_home_on_explicit_game_end"]=homePages->currentIndex()==0&&idleState->isVisibleTo(this)&&!replayPage->isVisibleTo(this);
        applySnapshot(liveFixture);showReplay();flush();
        auto back=findChild<QPushButton*>(QStringLiteral("closeInlineReplay"));if(back)back->click();flush();
        checks["inline_replay_back_action_restores_home_in_same_window"]=back&&homePages->currentIndex()==0&&matchScroll->isVisibleTo(this)&&matchNavigation->isChecked();
        showReplay();matchNavigation->click();flush();
        checks["home_navigation_leaves_inline_replay"]=homePages->currentIndex()==0&&matchScroll->isVisibleTo(this)&&!replayPage->isVisibleTo(this);
    }
    auto calculatingFixture=liveFixture;calculatingFixture.remove("coach");applySnapshot(calculatingFixture);
    checks["fresh_score_survives_pending_purchase_model"]=scoreLabel->property("scoreKnown").toBool()&&scoreLabel->property("fullText")==QStringLiteral("30  :  27");
    auto incompleteFixture=calculatingFixture;auto incompleteRows=currentRows;auto incompletePlayer=incompleteRows[0].toObject();incompletePlayer["scores"]=QJsonObject{{"deaths",0},{"assists",1}};incompleteRows[0]=incompletePlayer;incompleteFixture["current_players"]=incompleteRows;applySnapshot(incompleteFixture);
    checks["missing_live_kills_never_use_model_score_fallback"]=!scoreLabel->property("scoreKnown").toBool();
    auto blockedFixture=liveFixture;auto blockedCoach=blockedFixture.value("coach").toObject();blockedCoach["recommendation_quality"]=QJsonObject{{"primary_purchase_visible",false},{"summary",QStringLiteral("队友增益未计入，暂不排序")}};blockedFixture["coach"]=blockedCoach;applySnapshot(blockedFixture);
    checks["dashboard_obeys_same_support_recommendation_gate"]=actionIcon->property("itemId").toInt()==0&&actionTitle->property("fullText")==QStringLiteral("出装依据不完整")&&actionDetail->property("fullText")==QStringLiteral("队友增益未计入，暂不排序")&&priceLabel->property("fullText").toString().isEmpty();
    for(const auto &gate:QList<QJsonValue>{QJsonValue(QJsonValue::Undefined),QJsonValue(QJsonValue::Null),QJsonValue("true")}){
        auto malformedCoach=liveFixture.value("coach").toObject();malformedCoach["recommendation_quality"]=QJsonObject{{"primary_purchase_visible",gate}};
        auto malformedFixture=liveFixture;malformedFixture["coach"]=malformedCoach;applySnapshot(malformedFixture);
        checks[QStringLiteral("dashboard_unverified_purchase_gate_%1").arg(int(gate.type()))]=actionIcon->property("itemId").toInt()==0&&priceLabel->property("fullText").toString().isEmpty();
    }
    auto growthFallbackFixture=liveFixture;auto growthFallbackCoach=growthFallbackFixture.value("coach").toObject();auto growthFallbackPlan=growthFallbackCoach.value("plan").toObject();growthFallbackPlan.remove("growth_reasoning");growthFallbackPlan["growth_objective"]=QJsonObject{{"summary",QStringLiteral("大招工具人 · 大招循环联动")}};growthFallbackCoach["plan"]=growthFallbackPlan;growthFallbackFixture["coach"]=growthFallbackCoach;applySnapshot(growthFallbackFixture);
    checks["growth_objective_summary_replaces_legacy_direction"]=routeLabel->property("fullText").toString().contains(QStringLiteral("大招工具人 · 大招循环联动"))&&!routeLabel->property("fullText").toString().contains(QStringLiteral("技能爆发"));
    growthFallbackPlan.remove("growth_objective");growthFallbackCoach["plan"]=growthFallbackPlan;growthFallbackFixture["coach"]=growthFallbackCoach;applySnapshot(growthFallbackFixture);
    checks["missing_growth_summary_never_restores_fixed_direction"]=routeLabel->property("fullText").toString().contains(QStringLiteral("英雄与海克斯成长联动"))&&!routeLabel->property("fullText").toString().contains(QStringLiteral("技能爆发"));
    auto candidateFixture=liveFixture;auto candidateCoach=candidateFixture.value("coach").toObject();QJsonObject candidatePlan{{"growth_conditioned_candidates",true},{"reference_only",true},{"ranking_available",false},{"items",QJsonArray{}},{"candidate_loadouts",QJsonArray{QJsonObject{{"item_ids",QJsonArray{3084,3068,2502,3075,3065,3047}}},QJsonObject{{"item_ids",QJsonArray{3084,3110,3065,3083,3190,3158}}}}},{"growth_objective",QJsonObject{{"summary",QStringLiteral("成长入口已知，层数待核实")},{"equipment_growth_inputs",QJsonArray{QJsonObject{{"item_id",3084},{"required_inputs",QJsonArray{"item:3084:permanent_hp"}},{"supplied_baseline_inputs",QJsonObject{}}}}}}}};candidateCoach["plan"]=candidatePlan;candidateFixture["coach"]=candidateCoach;applySnapshot(candidateFixture);
    checks["growth_candidates_with_empty_inventory_are_visible_without_purchase_instruction"]=actionTitle->property("fullText")==QStringLiteral("成长联动候选 · 2组")&&actionDetail->property("fullText")==QStringLiteral("心钢层数未自动采集，当前未知")&&actionIcon->property("itemId").toInt()==0&&priceLabel->property("fullText").toString().isEmpty()&&actionDetail->toolTip().contains(QStringLiteral("未排序"))&&!actionTitle->property("fullText").toString().contains(QStringLiteral("治疗"));
    candidatePlan["candidate_loadouts"]=QJsonArray{};candidateCoach["plan"]=candidatePlan;candidateFixture["coach"]=candidateCoach;applySnapshot(candidateFixture);
    checks["growth_candidates_missing_inputs_do_not_show_computing_or_full_build"]=actionTitle->property("fullText")==QStringLiteral("成长联动候选 · 待补输入")&&!actionDetail->property("fullText").toString().isEmpty()&&actionIcon->property("itemId").toInt()==0;
    applySnapshot(liveFixture);waitForArtwork();checks["live_ten_champions_on_first_screen"]=report().value("visible_allies").toInt()==5&&report().value("visible_enemies").toInt()==5&&firstScreen();checks["matched_history_uses_positive_same_patch_samples"]=report().value("history_context_valid").toBool()&&report().value("history_references").toInt()==10&&allies.first().detail->property("fullText").toString().contains(QStringLiteral("12场同英雄"));checks["historical_metric_and_direction_are_labelled"]=allies.first().detail->property("fullText").toString().contains(QStringLiteral("输出 1160/分"))&&allies.first().detail->property("fullText").toString().contains(QStringLiteral("↑ 历史输出"))&&allies.first().detail->toolTip().contains(QStringLiteral("历史参考"));checks["live_kda_stays_separate_from_history"]=allies.first().marker->property("fullText")==QStringLiteral("8 / 3 / 11");checks["history_does_not_replace_local_purchase_route"]=actionTitle->property("fullText").toString().contains(catalog.itemName(3100))&&routeLabel->property("fullText").toString().contains(QStringLiteral("钢之心 · 生命成长联动"))&&!routeLabel->property("fullText").toString().contains(QStringLiteral("技能爆发"));checks["live_content_fits"]=fit()&&seatColumnsFit();checks["wide_title_and_connection_are_not_elided"]=keyLabelsFit();checks["score_digits_not_vertically_clipped"]=scoreLabel->height()>=scoreLabel->fontMetrics().height();save("dashboard-live");
    selectPage(1);settingsTabs->setCurrentIndex(settingsModelsTab);flush();checks["settings_navigation"]=settingsNavigation->isChecked()&&!matchNavigation->isChecked()&&pages->currentIndex()==1;checks["shared_preferences_loaded"]=overlayEnabled->isChecked()==preferences.value("coach/overlayEnabled",true).toBool()&&cardHintsEnabled->isChecked()==preferences.value("coach/cardHintsEnabled",true).toBool()&&rosterDsEnabled->isChecked()==preferences.value("coach/rosterDsEnabled",true).toBool()&&autoMinimize->isChecked()==preferences.value("coach/autoMinimize",true).toBool()&&opacity->value()==qRound(opacityPreference(preferences)*100);checks["settings_configuration_visible_without_disclosure"]=apiKey->isVisibleTo(this)&&model->isVisibleTo(this);
    checks["automatic_reader_setup_is_visible_without_more_tools"]=inspectSettingsTab(settingsGeneralTab,[&]{return growthCaptureAction->isVisibleTo(this)&&growthCaptureStatus->isVisibleTo(this)&&uiBrowserAction->isVisibleTo(this)&&settingsWidgetReachable(growthCaptureAction)&&settingsWidgetReachable(growthCaptureStatus)&&settingsWidgetReachable(uiBrowserAction);});
    auto recipeFixture=liveFixture;auto recipeCoach=recipeFixture.value("coach").toObject();recipeCoach["status"]="input_required";recipeFixture["coach"]=recipeCoach;
    recipeFixture["observed_purchase_facts"]=QJsonObject{{"available",true},{"budget_known",true},{"current_gold",1200},{"ranking_available",false},{"upgrades",QJsonArray{QJsonObject{{"item_id",3133},{"remaining_cost",750},{"within_catalog_budget",true}}}}};
    recipeFixture["growth_capture"]=QJsonObject{{"visible_reader",QJsonObject{{"enabled",false},{"settings",QJsonObject{{"enabled",true}}}}}};
    applySnapshot(recipeFixture);selectPage(0);flush();
    checks["unconfirmed_augments_leave_gold_recipe_facts_and_roster_visible"]=actionTitle->property("fullText").toString().contains(QStringLiteral("1200G"))&&actionDetail->property("fullText").toString().contains(QStringLiteral("750G"))&&actionDetail->toolTip().contains(QStringLiteral("不是出装排名"))&&actionIcon->property("itemId").toInt()==0&&firstScreen();
    checks["calibration_needed_is_named_without_mandatory_manual_input"]=routeLabel->property("fullText").toString().contains(QStringLiteral("等待定位"))&&!actionDetail->property("fullText").toString().contains(QStringLiteral("补全"));save("dashboard-unconfirmed-augments");
    resize(760,500);flush();checks["minimum_unconfirmed_recipe_reference_fits"]=fit()&&firstScreen();save("dashboard-unconfirmed-augments-minimum");resize(840,520);
    auto routeFacts=recipeFixture.value("observed_purchase_facts").toObject();routeFacts["source_reference"]=QJsonObject{{"available",true},{"target_item_id",3031},{"target_remaining_cost",2800},{"action_item_id",1038},{"action_cost",1300},{"budget_known",true},{"current_gold",1400},{"within_catalog_budget",true},{"source_url","https://op.gg/lol/modes/aram-mayhem/ashe/build"}};recipeFixture["observed_purchase_facts"]=routeFacts;applySnapshot(recipeFixture);flush();
    checks["source_route_names_target_component_and_unknown_augments"]=actionIcon->property("itemId").toInt()==3031&&actionTitle->property("fullText").toString().startsWith(QStringLiteral("资料路线"))&&actionDetail->property("fullText").toString().contains(catalog.itemName(1038))&&routeLabel->property("fullText").toString().contains(QStringLiteral("未计已选强化"))&&actionDetail->toolTip().contains(QStringLiteral("https://op.gg"));save("dashboard-source-purchase-reference");
    resize(760,500);flush();checks["source_route_fits_minimum_without_full_rank"]=fit()&&firstScreen()&&!actionTitle->property("fullText").toString().contains(QStringLiteral("下一件"));save("dashboard-source-purchase-reference-minimum");resize(840,520);
    auto listFixture=recipeFixture;listFixture["purchase_checklist"]=QJsonObject{{"available",true},{"target_item_id",3031},{"context_id",listFixture.value("match_context").toObject().value("context_id")},{"rows",QJsonArray{QJsonObject{{"item_id",1038},{"estimated_cost",1300},{"gold_gap",0}},QJsonObject{{"item_id",3031},{"estimated_cost",2300},{"gold_gap",QJsonValue::Null}},QJsonObject{{"item_id",3085},{"estimated_cost",2650},{"gold_gap",QJsonValue::Null}}}}};applySnapshot(listFixture);flush();
    checks["home_purchase_list_shows_three_steps_without_prose"]=actionIcon->property("itemId").toInt()==3031&&actionTitle->property("fullText")==QStringLiteral("资料路线 · ")+catalog.itemName(3031)&&actionDetail->property("fullText").toString().contains(QStringLiteral("≈1300G"))&&routeLabel->property("fullText").toString().contains(catalog.itemName(3085));
    checks["home_purchase_list_preserves_source_and_component_budget"]=routeLabel->toolTip().contains(QStringLiteral("https://op.gg"))&&priceLabel->property("fullText")==QStringLiteral("组件预算内")&&priceLabel->toolTip().contains(QStringLiteral("以商店为准"));save("dashboard-purchase-list");
    resize(760,500);flush();checks["home_purchase_list_fits_small_window"]=fit()&&firstScreen();save("dashboard-purchase-list-minimum");resize(840,520);auto invalidList=listFixture.value("purchase_checklist").toObject();invalidList["context_id"]="different-match";listFixture["purchase_checklist"]=invalidList;applySnapshot(listFixture);flush();checks["home_purchase_list_rejects_other_context"]=actionTitle->property("fullText").toString().startsWith(QStringLiteral("资料路线"))&&actionDetail->property("fullText").toString().startsWith(QStringLiteral("组件参考"));
    auto budgetFixture=liveFixture;auto budgetCoach=budgetFixture.value("coach").toObject();auto budgetEconomy=budgetCoach.value("economy").toObject();auto budgetPurchase=budgetEconomy.value("next_purchase").toObject();budgetPurchase["gold_gap"]=0;budgetEconomy["next_purchase"]=budgetPurchase;budgetCoach["economy"]=budgetEconomy;budgetFixture["coach"]=budgetCoach;applySnapshot(budgetFixture);flush();
    checks["home_purchase_budget_uses_catalogue_estimates"]=priceLabel->property("fullText")==QStringLiteral("目录预算内")&&actionDetail->property("fullText").toString().startsWith(QStringLiteral("组件估价"))&&priceLabel->toolTip().contains(QStringLiteral("以商店为准"));
    const QJsonObject conditionalRow{{"item_id",3031},{"remaining_cost",2800},{"transaction",QJsonObject{{"remaining_cost",2800}}},
        {"affordable_now",true},{"score_gain",0.2},{"ranking_eligible",true},{"event_admission_changes",QJsonArray{}},{"mechanism_admission_changed",false}};
    const QJsonObject conditionalComparison{{"complete_candidate_universe",true},{"selection_available",true},{"ranking_available",true},
        {"source",QJsonObject{{"kind","caller_declared_scenario"},{"valid_from_game_time",720},{"valid_until_game_time",730}}}};
    const QJsonObject conditionalSandbox{{"policy","explicit_chain_next_purchase_v1"},{"status","conditional_chain_purchase_available"},
        {"score_basis","explicit_chain_condition_comparison"},{"enumeration_complete",true},{"ranking_available",true},{"selected_item_id",3031},
        {"critical_unknowns",QJsonArray{}},{"candidates",QJsonArray{conditionalRow}},{"explicit_chain_condition_comparison",conditionalComparison}};
    auto conditionalFixture=listFixture;auto conditionalCoach=liveFixture.value("coach").toObject();conditionalCoach["plan"]=QJsonObject{{"purchase_sandbox",conditionalSandbox}};
    conditionalFixture["coach"]=conditionalCoach;invalidList["context_id"]=context.value("context_id");invalidList["display_source"]="reference";
    conditionalFixture["purchase_checklist"]=invalidList;applySnapshot(conditionalFixture);flush();
    checks["home_conditional_comparison_rejects_reference_checklist"]=actionTitle->property("fullText")==QStringLiteral("条件比较 · ")+catalog.itemName(3031)
        &&actionDetail->property("fullText")==QStringLiteral("目录估价 · 请核对商店")&&priceLabel->property("fullText")==QStringLiteral("目录预算内");
    auto diagnosticFixture=recipeFixture;auto diagnosticCoach=recipeCoach;diagnosticCoach["status"]=QStringLiteral("ready");diagnosticCoach["economy"]=QJsonObject{{"next_purchase",QJsonObject{}}};diagnosticCoach["recommendation_quality"]=QJsonObject{{"primary_purchase_visible",false}};diagnosticCoach["source_purchase_reference"]=routeFacts.value("source_reference");diagnosticFixture["coach"]=diagnosticCoach;routeFacts["inputs_ready"]=true;diagnosticFixture["observed_purchase_facts"]=routeFacts;applySnapshot(diagnosticFixture);flush();
    checks["completed_diagnostic_model_keeps_source_route_on_home"]=actionIcon->property("itemId").toInt()==3031&&actionTitle->property("fullText").toString().startsWith(QStringLiteral("资料路线"))&&routeLabel->property("fullText").toString().contains(QStringLiteral("未作本局收益排序"));save("dashboard-completed-diagnostic-reference");
    diagnosticFixture["observed_purchase_facts"]=QJsonObject{};applySnapshot(diagnosticFixture);flush();checks["empty_authoritative_facts_never_recover_stale_worker_reference"]=actionIcon->property("itemId").toInt()==0;
    diagnosticFixture["observed_purchase_facts"]=routeFacts;diagnosticFixture["live_phase"]=QStringLiteral("EndOfGame");applySnapshot(diagnosticFixture);flush();checks["ended_phase_never_shows_cached_source_purchase"]=actionIcon->property("itemId").toInt()==0;
    applySnapshot(liveFixture);selectPage(1);flush();
    checks["settings_tools_and_configuration_reachable_without_disclosures"]=inspectSettingsTab(settingsModelsTab,[&]{return apiKey->isVisibleTo(this)&&model->isVisibleTo(this)&&settingsWidgetReachable(apiKey)&&settingsWidgetReachable(model);})&&inspectSettingsTab(settingsGeneralTab,[&]{return growthCaptureAction->isVisibleTo(this)&&uiBrowserAction->isVisibleTo(this)&&settingsWidgetReachable(growthCaptureAction)&&settingsWidgetReachable(uiBrowserAction);});
    checks["password_field_masked"]=apiKey->echoMode()==QLineEdit::Password;apiKey->setText(QStringLiteral("synthetic-dashboard-key-do-not-save"));checks["report_contains_no_key"]=!QJsonDocument(report()).toJson().contains("synthetic-dashboard-key-do-not-save")&&(originalKey.size()<8||!QJsonDocument(report()).toJson().contains(originalKey.toUtf8()));checks["settings_content_fits"]=fit();checks["ui_browser_entry_visible_with_focus_and_full_label"]=uiBrowserFits();checks["opacity_caption_visible_and_separate"]=opacityCaptionFits();apiKey->setFocus(Qt::TabFocusReason);flush();const bool tabMoved=focusNextChild();checks["keyboard_focus_available"]=tabMoved&&(focusWidget()==model||model->isAncestorOf(focusWidget()))&&overlayEnabled->focusPolicy()!=Qt::NoFocus;saveStatus->setText(QStringLiteral("合成界面检查 · 未保存"));save("dashboard-settings");
    bool duplicateSettingsTitle=false;for(const auto label:settingsPage->findChildren<QLabel*>())duplicateSettingsTitle|=label->isVisibleTo(this)&&label->text().trimmed()==QStringLiteral("设置");
    checks["settings_uses_navigation_title_without_duplicate_content_heading"]=settingsTabs->isVisibleTo(this)&&!duplicateSettingsTitle;
    const auto keyCaption=findChild<QLabel*>(QStringLiteral("dsApiKeyCaption"));checks["ds_api_key_caption_is_fully_visible"]=keyCaption&&keyCaption->isVisibleTo(this)&&keyCaption->contentsRect().width()>=keyCaption->fontMetrics().horizontalAdvance(keyCaption->text())&&keyCaption->height()>=keyCaption->fontMetrics().height()&&!relativeRect(keyCaption,settingsPage).intersects(relativeRect(apiKey,settingsPage));
    checks["ds_official_model_presets_preserve_current_custom_or_legacy_value"]=model->findText(QStringLiteral("deepseek-flash"))>=0&&model->findText(QStringLiteral("deepseek-v4-pro"))>=0&&model->currentText()==originalModel;
    const auto usageLabelsFit=[&]{
        const auto panel=findChild<QWidget*>(QStringLiteral("dsUsagePanel"));if(!panel)return false;
        for(auto label:panel->findChildren<QLabel*>()){
            if(!label->isVisibleTo(this))continue;
            if(label->width()<=0||label->height()<=0||label->height()+2<label->heightForWidth(label->width()))return false;
        }
        return settingsScroll->horizontalScrollBar()->maximum()==0;
    };
    const auto usageCardFits=[&]{
        const auto card=findChild<QWidget*>(QStringLiteral("dsUsagePanel")),current=findChild<QWidget*>(QStringLiteral("dsUsageCurrentMetric")),average=findChild<QWidget*>(QStringLiteral("dsUsageAverageMetric"));if(!card||!current||!average)return false;
        const int columns=card->property("usageMetricColumns").toInt();const bool horizontal=card->width()>=usageTwoColumnWidth;
        return columns==(horizontal?2:1)&&!current->geometry().intersects(average->geometry())&&(horizontal?(current->y()==average->y()&&current->geometry().right()<average->x()):(current->geometry().bottom()<average->y()))&&usageLabelsFit();
    };
    checks["ds_no_calls_remain_no_records_instead_of_zero_cost"]=dsUsageCurrent->text().contains(QStringLiteral("尚无记录"))&&dsUsageTokens->text().contains(QStringLiteral("尚无记录"))&&!dsUsageCurrent->text().contains(QChar(u'¥'));
    settingsScroll->verticalScrollBar()->setValue(0);flush();save("dashboard-settings-compact-empty");
    auto usageFixture=liveFixture;
    QJsonObject currentUsage{{"call_count",8},{"pending_call_count",0},{"usage_known_call_count",8},{"unknown_cost_call_count",0},
        {"prompt_tokens",125600},{"completion_tokens",8400},{"cost_min_cny",.001234},{"cost_max_cny",.002468},{"cost_complete",true},{"completed",false}};
    QJsonObject completedUsage{{"completed_match_count",3},{"priced_match_count",2},{"excluded_unknown_cost_match_count",1},{"mean_cost_min_cny",.003456},{"mean_cost_max_cny",.006912}};
    QJsonObject usage{{"current_match",currentUsage},{"completed_matches",completedUsage},{"persistence_ok",true}};
    usageFixture["deepseek_usage"]=usage;applySnapshot(usageFixture);flush();
    checks["ds_estimated_current_and_mean_cost_keep_both_range_bounds"]=dsUsageCurrent->text().contains(QStringLiteral("0.001234–0.002468"))&&dsUsageAverage->text().contains(QStringLiteral("0.003456–0.006912"));
    checks["ds_tokens_and_request_counts_are_displayed"]=dsUsageTokens->text().contains(QStringLiteral("125,600"))&&dsUsageTokens->text().contains(QStringLiteral("8,400"))&&dsUsageCalls->text().contains(QStringLiteral("8 次"));
    checks["ds_mean_displays_complete_sample_count_and_exclusion"]=dsUsageSamples->text().contains(QStringLiteral("2 局"))&&dsUsageSamples->text().contains(QStringLiteral("未计入 1 局"))&&dsUsageAverage->toolTip().contains(QStringLiteral("完整观测并计价"));
    settingsScroll->verticalScrollBar()->setValue(0);flush();
    checks["default_settings_use_about_width_and_clear_single_column"]=size()==QSize(840,520)&&findChild<QWidget*>(QStringLiteral("settingsGeneralPage"))->property("settingsColumns").toInt()==1&&findChild<QWidget*>(QStringLiteral("settingsContent"))->width()==settingsReadingWidth&&fit();
    checks["default_core_ds_configuration_and_both_amounts_are_scroll_reachable"]=settingsWidgetReachable(apiKey)&&settingsWidgetReachable(model)&&settingsWidgetReachable(dsUsageCurrent)&&settingsWidgetReachable(dsUsageAverage);settingsScroll->verticalScrollBar()->setValue(0);flush();save("dashboard-settings-compact-normal");
    checks["ds_usage_wrapped_copy_fits"]=usageLabelsFit();checks["ds_cost_card_metrics_follow_actual_available_width"]=usageCardFits()&&dsUsageState->text()==QStringLiteral("已计价");settingsScroll->ensureWidgetVisible(findChild<QWidget*>(QStringLiteral("dsUsagePanel")),0,0);flush();save("dashboard-ds-usage-range");
    resize(1280,760);flush();settingsScroll->verticalScrollBar()->setValue(0);flush();
    checks["wide_settings_keep_about_reading_width_and_single_column"]=findChild<QWidget*>(QStringLiteral("settingsGeneralPage"))->property("settingsColumns").toInt()==1&&findChild<QWidget*>(QStringLiteral("settingsContent"))->width()==settingsReadingWidth&&fit();checks["ds_cost_sidebar_uses_readable_stacked_metrics"]=usageCardFits()&&findChild<QWidget*>(QStringLiteral("dsUsagePanel"))->property("usageMetricColumns").toInt()==1;save("dashboard-settings-wide");
    resize(760,500);flush();settingsScroll->verticalScrollBar()->setValue(0);flush();
    checks["minimum_settings_use_single_column"]=findChild<QWidget*>(QStringLiteral("settingsGeneralPage"))->property("settingsColumns").toInt()==1&&fit();save("dashboard-settings-responsive-minimum");
    settingsScroll->ensureWidgetVisible(findChild<QWidget*>(QStringLiteral("dsUsagePanel")),0,0);flush();save("dashboard-ds-usage-minimum");
    int dsActionsFound=0;bool dsActionsReachable=true;for(auto action:settingsPage->findChildren<QAbstractButton*>())if(action->objectName()==QStringLiteral("saveDs")||action->text()==QStringLiteral("退出助手")){++dsActionsFound;const auto tab=action->objectName()==QStringLiteral("saveDs")?settingsModelsTab:settingsGeneralTab;dsActionsReachable&=inspectSettingsTab(tab,[&]{return action->isVisibleTo(this)&&settingsWidgetReachable(action);});}
    checks["minimum_ds_cost_ranges_and_actions_are_reachable"]=usageCardFits()&&settingsWidgetReachable(dsUsageCurrent)&&settingsWidgetReachable(dsUsageAverage)&&dsActionsFound==2&&dsActionsReachable&&dsUsageCurrent->text().contains(QStringLiteral("0.001234–0.002468"));
    settingsScroll->verticalScrollBar()->setValue(settingsScroll->verticalScrollBar()->maximum());flush();save("dashboard-settings-responsive-minimum-bottom");
    resize(840,520);flush();settingsScroll->ensureWidgetVisible(findChild<QWidget*>(QStringLiteral("dsUsagePanel")),0,0);flush();save("dashboard-settings-responsive-normal");
    currentUsage["call_count"]=9;currentUsage["pending_call_count"]=1;currentUsage["usage_known_call_count"]=7;currentUsage["unknown_cost_call_count"]=2;currentUsage["cost_complete"]=false;
    usage["current_match"]=currentUsage;usage["persistence_ok"]=false;usageFixture["deepseek_usage"]=usage;applySnapshot(usageFixture);flush();
    checks["ds_partial_usage_is_not_reported_as_complete_bill"]=dsUsageState->text()==QStringLiteral("部分计价")&&dsUsagePresentation.value("current").toString().contains(QStringLiteral("已计价部分"))&&dsUsageTokens->text().startsWith(QStringLiteral("已记录"))&&dsUsageCalls->text().contains(QStringLiteral("进行中"))&&dsUsageCalls->text().contains(QStringLiteral("未计价"));
    checks["ds_persistence_failure_is_visible_without_claiming_saved"]=dsUsagePersistence->isVisibleTo(this)&&dsUsagePersistence->text().contains(QStringLiteral("暂未保存"));
    settingsScroll->ensureWidgetVisible(findChild<QWidget*>(QStringLiteral("dsUsagePanel")),0,0);flush();save("dashboard-ds-usage-partial");
    currentUsage=QJsonObject{{"call_count",2},{"pending_call_count",0},{"usage_known_call_count",0},{"unknown_cost_call_count",2},
        {"prompt_tokens",0},{"completion_tokens",0},{"cost_min_cny",0},{"cost_max_cny",0},{"cost_complete",false}};
    usage["current_match"]=currentUsage;usage["persistence_ok"]=true;usageFixture["deepseek_usage"]=usage;applySnapshot(usageFixture);flush();
    checks["ds_missing_usage_does_not_become_zero_fee_or_zero_tokens"]=dsUsageCurrent->text().contains(QStringLiteral("未完整"))&&!dsUsageCurrent->text().contains(QChar(u'¥'))&&dsUsageTokens->text().contains(QStringLiteral("未完整"))&&!dsUsageTokens->text().contains(QStringLiteral("输入 0"))&&!dsUsagePersistence->isVisibleTo(this);
    settingsScroll->ensureWidgetVisible(findChild<QWidget*>(QStringLiteral("dsUsagePanel")),0,0);flush();save("dashboard-ds-usage-missing");
    checks["ds_tiny_fee_never_rounds_to_apparent_zero"]=!usageMoney(1e-8,1e-8).contains(QStringLiteral("0.000000"))&&usageMoney(.0000011,.0000014).contains(QChar(u'–'));
    applySnapshot(liveFixture);flush();
    const auto savedGrowthAction=onGrowthCaptureSettings;int growthActionCalls=0;onGrowthCaptureSettings=[&]{++growthActionCalls;};growthCaptureAction->click();onGrowthCaptureSettings=savedGrowthAction;
    checks["growth_source_action_has_focus_full_label_and_callback"]=growthSourceFits()&&growthCaptureAction->text()==QStringLiteral("自动读取设置")&&growthActionCalls==1;
    auto sourceFixture=liveFixture;sourceFixture["growth_capture"]=QJsonObject{};applySnapshot(sourceFixture);checks["growth_source_missing_waits_for_structured_data"]=growthCaptureStatus->property("fullText")==QStringLiteral("等候结构化来源");
    sourceFixture["growth_capture"]=QJsonObject{{"status","connected"}};applySnapshot(sourceFixture);checks["growth_source_connected_is_explicit"]=growthCaptureStatus->property("fullText")==QStringLiteral("已连接");
    sourceFixture["growth_capture"]=QJsonObject{{"status","unknown"},{"source","structured_provider_unavailable"},{"unknowns",QJsonArray{"equipment_growth_counters_not_exposed","augment_task_progress_not_exposed"}},{"capabilities",QJsonObject{{"ocr_enabled",false},{"equipment_growth_counters",false}}}};applySnapshot(sourceFixture);checks["connected_interface_does_not_invent_unprovided_growth_layers"]=growthCaptureStatus->property("fullText")==QStringLiteral("未提供层数");
    sourceFixture["growth_capture"]=QJsonObject{{"status","observed"},{"connected",true},{"unknowns",QJsonArray{"heartsteel_permanent_hp_not_exposed","augment_task_progress_not_exposed"}},{"input_coverage",QJsonObject{{"glory_stacks",QJsonObject{{"available",true},{"source_kind","event_derived_counter"}}}}}};applySnapshot(sourceFixture);checks["validated_glory_remains_visible_despite_other_growth_gaps"]=growthCaptureStatus->property("fullText")==QStringLiteral("部分成长可用");
    sourceFixture["growth_capture"]=QJsonObject{{"status","observed"},{"expired",true}};applySnapshot(sourceFixture);checks["expired_growth_source_never_shows_connected"]=growthCaptureStatus->property("fullText")==QStringLiteral("数据过期");applySnapshot(liveFixture);
    bool sourceDialogObserved=false,manualSourceMarked=false,derivedSourceMarked=false,missingSourceKeptUnknown=false,partialSourceExplanationShown=false;QTimer inspectSourceDialog;inspectSourceDialog.setInterval(0);
    connect(&inspectSourceDialog,&QTimer::timeout,this,[&]{
        auto dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog||dialog->objectName()!=QStringLiteral("growthDataSourceDialog"))return;
        sourceDialogObserved=true;const auto manualLabel=dialog->findChild<QLabel*>(QStringLiteral("growthCoverage_selected_augments")),gloryLabel=dialog->findChild<QLabel*>(QStringLiteral("growthCoverage_glory_stacks")),missingLabel=dialog->findChild<QLabel*>(QStringLiteral("growthCoverage_current_health"));
        manualSourceMarked=manualLabel&&manualLabel->text()==QStringLiteral("手动记录 · 非自动实测");derivedSourceMarked=gloryLabel&&gloryLabel->text()==QStringLiteral("事件推导 · 有条件")&&!gloryLabel->text().contains(QStringLiteral("实测"));missingSourceKeptUnknown=missingLabel&&missingLabel->text()==QStringLiteral("未提供");for(const auto label:dialog->findChildren<QLabel*>())partialSourceExplanationShown|=label->text()==QStringLiteral("已有部分成长记录；未提供项保持未知。");inspectSourceDialog.stop();dialog->accept();
    });
    inspectSourceDialog.start();const auto sourceDialogResult=editGrowthCaptureSettings(this,QJsonObject{{"status","connected"},{"input_coverage",QJsonObject{{"selected_augments",QJsonObject{{"available",true},{"source_kind","manual_confirmation"}}},{"glory_stacks",QJsonObject{{"available",true},{"source_kind","event_derived_counter"}}},{"current_health",QJsonObject{{"available",false}}}}}});inspectSourceDialog.stop();
    checks["manual_confirmation_is_labelled_as_non_automatic_in_source_dialog"]=sourceDialogObserved&&manualSourceMarked&&sourceDialogResult.isEmpty();checks["glory_event_counter_is_conditional_and_missing_input_stays_unknown"]=sourceDialogObserved&&derivedSourceMarked&&missingSourceKeptUnknown&&partialSourceExplanationShown&&sourceDialogResult.isEmpty();
    bool modelLayersObserved=false,modelLayersValid=true;
    const auto modelSlotBackgroundLayer=[&](QWidget *parent,QWidget *first,QWidget *second){
        const auto grid=qobject_cast<QGridLayout*>(parent->layout());
        if(parent->objectName()!=QStringLiteral("modelSlotSelection")||!grid||
           !((first==modelSlotPreview&&second==modelSlotRoster)||(second==modelSlotPreview&&first==modelSlotRoster)))return false;
        int previewRow,previewColumn,previewRowSpan,previewColumnSpan,rosterRow,rosterColumn,rosterRowSpan,rosterColumnSpan;
        grid->getItemPosition(grid->indexOf(modelSlotPreview),&previewRow,&previewColumn,&previewRowSpan,&previewColumnSpan);
        grid->getItemPosition(grid->indexOf(modelSlotRoster),&rosterRow,&rosterColumn,&rosterRowSpan,&rosterColumnSpan);
        const bool valid=modelSlotPreview->parentWidget()==parent&&modelSlotRoster->parentWidget()==parent&&
            previewRow==rosterRow&&previewColumn==rosterColumn&&previewRowSpan==rosterRowSpan&&previewColumnSpan==rosterColumnSpan&&
            modelSlotPreview->geometry().contains(modelSlotRoster->geometry())&&!modelSlotRoster->geometry().isEmpty()&&
            parent->children().indexOf(modelSlotPreview)<parent->children().indexOf(modelSlotRoster);
        modelLayersObserved=true;modelLayersValid&=valid;
        checks["model_slot_preview_is_background_behind_same_cell_roster"]=modelLayersObserved&&modelLayersValid;
        return valid;
    };
    checks["model_slot_preview_is_background_behind_same_cell_roster"]=false;
    const auto settingsContentReachable=[&]{
        bool okay=fit();if(!okay)recordSettingsFailure({{"reason","page_horizontal_fit_failed"},{"page_rect",rectJson(settingsPage->rect())},{"viewport_rect",rectJson(settingsScroll->viewport()->rect())},{"horizontal_scroll_maximum",settingsScroll->horizontalScrollBar()->maximum()}});
        for(auto control:settingsPage->findChildren<QWidget*>()){
            if(!control->isVisibleTo(this)||control->isWindow())continue;
            const bool reading=qobject_cast<QLabel*>(control)||qobject_cast<QAbstractButton*>(control)||qobject_cast<QLineEdit*>(control)||qobject_cast<QComboBox*>(control)||qobject_cast<QSlider*>(control)||dynamic_cast<VersionAbout::Portrait*>(control);
            if(reading)okay&=settingsWidgetReachable(control);
            const auto children=control->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly);QList<QPair<QRect,QWidget*>> occupied;
            if(!control->layout())continue;
            for(auto child:children){if(!child->isVisibleTo(this)||child->isWindow())continue;
                if(control->layout()->indexOf(child)<0)continue;
                const auto box=child->geometry();const bool contained=control->rect().contains(box);okay&=contained;
                if(!contained)recordSettingsFailure({{"reason","layout_child_outside_parent"},{"parent",settingsWidgetIdentity(control)},{"child",settingsWidgetIdentity(child)},{"parent_rect",rectJson(control->rect())},{"layout_contents_rect",rectJson(control->layout()->contentsRect())},{"child_rect",rectJson(box)}});
                for(const auto &other:occupied)if(box.intersects(other.first)&&!modelSlotBackgroundLayer(control,child,other.second)){okay=false;recordSettingsFailure({{"reason","layout_children_overlap"},{"parent",settingsWidgetIdentity(control)},{"child",settingsWidgetIdentity(child)},{"other_child",settingsWidgetIdentity(other.second)},{"child_rect",rectJson(box)},{"other_child_rect",rectJson(other.first)}});}occupied.append({box,child});
            }
        }
        return okay;
    };
    const auto form=findChild<QWidget*>(QStringLiteral("settingsContent"));const auto settingsColumnFits=[&]{
        if(!form)return false;
        const auto bounds=relativeRect(form,settingsPage),available=settingsPage->layout()->contentsRect();
        const int expectedWidth=std::min(form->maximumWidth(),available.width()),centerDifference=std::abs(bounds.center().x()-available.center().x());
        const bool belowLimit=form->width()<=form->maximumWidth(),fillsWidth=form->width()==expectedWidth,leftInside=bounds.left()>=available.left(),rightInside=bounds.right()<=available.right(),centered=centerDifference<=1,noHorizontalOverflow=fit();
        const auto rail=findChild<QWidget*>(QStringLiteral("settingsHeader"));const auto formInWindow=relativeRect(form,this),railInWindow=rail?relativeRect(rail,this):QRect();
        const auto scrollInWindow=relativeRect(settingsScroll,this);
        const bool railAlongside=rail&&railInWindow.right()<scrollInWindow.left()&&railInWindow.top()==scrollInWindow.top();
        QJsonObject diagnostic{{"window_size",QJsonArray{width(),height()}},{"viewport_size",QJsonArray{settingsScroll->viewport()->width(),settingsScroll->viewport()->height()}},{"page_rect",rectJson(settingsPage->rect())},{"form_rect_in_page",rectJson(bounds)},{"form_rect_in_window",rectJson(formInWindow)},{"rail_rect_in_window",rectJson(railInWindow)},{"rail_alongside_viewport",railAlongside},{"layout_available_rect",rectJson(available)},{"expected_width",expectedWidth},{"form_maximum_width",form->maximumWidth()},{"center_difference",centerDifference},{"horizontal_scroll_maximum",settingsScroll->horizontalScrollBar()->maximum()},{"below_width_limit",belowLimit},{"fills_expected_width",fillsWidth},{"left_inside",leftInside},{"right_inside",rightInside},{"centered",centered},{"no_horizontal_overflow",noHorizontalOverflow}};
        auto diagnostics=property("settingsColumnDiagnostics").value<QJsonObject>();diagnostics[QStringLiteral("%1x%2").arg(width()).arg(height())]=diagnostic;setProperty("settingsColumnDiagnostics",QVariant::fromValue(diagnostics));
        return belowLimit&&fillsWidth&&leftInside&&rightInside&&centered&&noHorizontalOverflow&&railAlongside;
    };checks["settings_form_uses_available_width_with_readable_limit"]=settingsColumnFits();checks["settings_switch_rows_have_readable_labels_and_focus"]=preferenceRowsFit();
    resize(820,600);flush();checks["narrow_titlebar_35px_controls_inside_and_separate"]=titlebarFits();checks["narrow_settings_no_horizontal_overflow"]=fit();checks["narrow_ui_browser_entry_fits"]=uiBrowserFits();checks["narrow_growth_source_entry_fits"]=growthSourceFits();checks["narrow_opacity_caption_visible_and_separate"]=opacityCaptionFits();checks["narrow_settings_switch_labels_fit"]=preferenceRowsFit();checks["narrow_settings_column_adapts"]=settingsColumnFits();save("dashboard-narrow-settings");selectPage(0);flush();checks["narrow_home_no_horizontal_overflow"]=fit();checks["narrow_seat_labels_do_not_overlap"]=seatColumnsFit();checks["narrow_score_purchase_and_ten_seats_visible"]=firstScreen();checks["narrow_title_and_connection_are_not_elided"]=keyLabelsFit();save("dashboard-narrow");
    resize(760,500);selectPage(1);settingsScroll->verticalScrollBar()->setValue(0);flush();checks["minimum_window_has_real_compact_size"]=size()==QSize(760,500)&&minimumSize()==QSize(760,500);checks["minimum_titlebar_controls_fit"]=titlebarFits();const QRect settingsTabsBeforeScroll=relativeRect(settingsTabs,this);const int settingsScrollBefore=settingsScroll->verticalScrollBar()->value();settingsScroll->verticalScrollBar()->setValue(settingsScroll->verticalScrollBar()->maximum());flush();
    const bool minimumContentReachable=settingsContentReachable(),minimumTabsStationary=relativeRect(settingsTabs,this)==settingsTabsBeforeScroll,minimumTabsInWindow=rect().contains(settingsTabsBeforeScroll),minimumTabsAlongsideViewport=settingsTabsBeforeScroll.right()<relativeRect(settingsScroll->viewport(),this).left();
    bool minimumTabHeadersFit=settingsTabs->shape()==QTabBar::RoundedWest;
    for(int index=0;index<settingsTabs->count();++index){
        const auto tab=settingsTabs->tabRect(index);
        minimumTabHeadersFit&=settingsTabs->rect().contains(tab)&&(index==0||settingsTabs->tabRect(index-1).bottom()<tab.top());
    }
    setProperty("settingsMinimumPageDiagnostics",QVariant::fromValue(QJsonObject{{"content_reachable",minimumContentReachable},{"tabs_stationary",minimumTabsStationary},{"tabs_in_window",minimumTabsInWindow},{"tabs_alongside_viewport",minimumTabsAlongsideViewport},{"tab_headers_fit",minimumTabHeadersFit},{"tabs_before",rectJson(settingsTabsBeforeScroll)},{"tabs_after",rectJson(relativeRect(settingsTabs,this))},{"viewport_in_window",rectJson(relativeRect(settingsScroll->viewport(),this))},{"vertical_scroll_value",settingsScroll->verticalScrollBar()->value()},{"vertical_scroll_maximum",settingsScroll->verticalScrollBar()->maximum()}}));
    checks["minimum_settings_content_scrolls_and_tabs_remain_fixed"]=minimumContentReachable&&minimumTabsStationary&&minimumTabsInWindow&&minimumTabsAlongsideViewport&&minimumTabHeadersFit;checks["minimum_general_settings_are_scroll_reachable"]=inspectSettingsTab(settingsGeneralTab,[&]{const bool reachable=settingsContentReachable();save("dashboard-general-minimum");return reachable;});settingsScroll->verticalScrollBar()->setValue(settingsScrollBefore);flush();checks["minimum_status_actions_fit"]=statusbarFits();checks["minimum_settings_no_overflow_and_labels_fit"]=settingsColumnFits()&&preferenceRowsFit()&&opacityCaptionFits();checks["minimum_ui_browser_entry_fits"]=uiBrowserFits();checks["minimum_growth_source_entry_fits"]=growthSourceFits();save("dashboard-minimum-settings");selectPage(0);matchScroll->verticalScrollBar()->setValue(0);flush();checks["minimum_ten_champions_on_first_screen"]=report().value("visible_allies").toInt()==5&&report().value("visible_enemies").toInt()==5&&firstScreen();checks["minimum_match_text_and_columns_fit"]=fit()&&seatColumnsFit()&&keyLabelsFit();save("dashboard-minimum-live");resize(840,520);flush();
    auto zeroSamples=liveFixture;auto zeroHistory=zeroSamples.value("anonymous_player_history").toObject();auto zeroRows=zeroHistory.value("players").toArray();auto zeroPlayer=zeroRows.first().toObject();const QJsonObject zeroSupport{{"available",true},{"sampled_games",0},{"metrics",QJsonObject{{"damage_per_minute",99999}}},{"field_samples",QJsonObject{{"damage_per_minute",0}}}};zeroPlayer["same_patch_champion"]=zeroSupport;zeroPlayer["same_patch"]=zeroSupport;zeroRows[0]=zeroPlayer;zeroHistory["players"]=zeroRows;zeroSamples["anonymous_player_history"]=zeroHistory;applySnapshot(zeroSamples);flush();checks["zero_samples_clear_old_historical_metric"]=!allies.first().body->property("historyAvailable").toBool()&&allies.first().detail->property("fullText")==QStringLiteral("暂无同版历史样本")&&allies.first().detail->toolTip().isEmpty();
    auto nextContext=liveFixture;auto changedContext=context;changedContext["context_id"]="synthetic-dashboard-next";nextContext["match_context"]=changedContext;applySnapshot(nextContext);flush();checks["new_context_clears_old_history_reference"]=!report().value("history_context_valid").toBool()&&report().value("history_references").toInt()==0&&!allies.first().detail->property("fullText").toString().contains(QStringLiteral("1160"));
    auto unverified=fixture;unverified["live_phase"]="InProgress";unverified["live_data_fresh"]=false;unverified["match_context"]=QJsonObject{};unverified["connection"]=QStringLiteral("读取失败");unverified["augment_source"]=QJsonObject{{"status","read_error"}};applySnapshot(unverified);checks["phase_alone_does_not_claim_live_connection"]=!sourceLabel->property("liveFresh").toBool()&&!connectionLabel->property("connected").toBool()&&!scoreLabel->property("scoreKnown").toBool();checks["log_error_has_explicit_status"]=sourceLabel->property("fullText").toString().contains(QStringLiteral("读取失败"));unverified["augment_source"]=QJsonObject{{"status","logs_idle"}};applySnapshot(unverified);checks["idle_log_waits_for_candidates"]=sourceLabel->property("fullText").toString().contains(QStringLiteral("待候选"));unverified["augment_source"]=QJsonObject{{"status","logs_active"}};applySnapshot(unverified);checks["active_log_uses_actual_status"]=sourceLabel->property("fullText").toString().contains(QStringLiteral("已读取"));
    auto versionFixture=liveFixture;
    versionFixture["observed_patch"]="16.22";
    versionFixture["rule_basis"]=QJsonObject{{"source_patch","16.21"},{"rules_available",true}};
    applySnapshot(versionFixture);flush();
    checks["game_and_rule_versions_are_visible_when_they_differ"]=sourceLabel->isVisibleTo(this)
        &&sourceLabel->text().contains(QStringLiteral("游戏 16.22 / 规则 16.21"))
        &&sourceLabel->toolTip().contains(QStringLiteral("按 16.21 规则估算"));
    const int versionScrollPosition=matchScroll->verticalScrollBar()->value();
    matchScroll->ensureWidgetVisible(sourceLabel,0,0);save("dashboard-rule-version-mismatch");
    matchScroll->verticalScrollBar()->setValue(versionScrollPosition);
    versionFixture["observed_patch"]=QString();
    applySnapshot(versionFixture);
    checks["missing_game_version_does_not_hide_the_loaded_rule_version"]=sourceLabel->text().contains(QStringLiteral("游戏版本待确认 / 规则 16.21"));
    versionFixture["observed_patch"]="16.21";
    applySnapshot(versionFixture);
    checks["matching_versions_collapse_to_one_label"]=sourceLabel->text().contains(QStringLiteral("游戏与规则 16.21"))
        &&!sourceLabel->toolTip().contains(QStringLiteral("版本不同"));
    applySnapshot({});resize(840,520);selectPage(0);matchScroll->verticalScrollBar()->setValue(0);flush();checks["clearing_snapshot_removes_old_roster"]=report().value("visible_allies").toInt()==0&&report().value("visible_enemies").toInt()==0&&!scoreLabel->property("scoreKnown").toBool();checks["idle_compact"]=idleState->isVisibleTo(this)&&!matchBoard->isVisibleTo(this)&&personalHome&&idleLabelsFit();bool emptyGridHidden=true;for(const auto *side:{&allies,&enemies})for(const auto &seat:*side)emptyGridHidden&=!seat.body->isVisibleTo(this);checks["idle_no_empty_ten_grid"]=emptyGridHidden&&!scoreLabel->isVisibleTo(this)&&!coverageLabel->isVisibleTo(this);checks["home_has_no_archival_explainer"]=archiveUpdated->isHidden()&&archiveUpdated->text().isEmpty();checks["idle_title_and_values_not_elided"]=idleLabelsFit();resize(820,600);flush();checks["idle_narrow_title_and_values_not_elided"]=idleLabelsFit()&&fit();resize(840,520);flush();save("dashboard-idle");
    applySnapshot(fixture);flush();const bool selectingRestored=matchBoard->isVisibleTo(this)&&!idleState->isVisibleTo(this)&&report().value("visible_allies").toInt()==5;auto loadingFixture=fixture;auto loadingContext=fixture.value("match_context").toObject();loadingContext["stage"]="loading";loadingFixture["match_context"]=loadingContext;applySnapshot(loadingFixture);flush();const bool loadingRestored=matchBoard->isVisibleTo(this)&&!idleState->isVisibleTo(this);applySnapshot(liveFixture);flush();checks["known_match_stages_restore_match_layout"]=selectingRestored&&loadingRestored&&matchBoard->isVisibleTo(this)&&!idleState->isVisibleTo(this)&&firstScreen()&&scoreLabel->property("scoreKnown").toBool();applySnapshot({});flush();QMap<QString,QVariant> afterPreferences;for(const auto &key:preferences.allKeys())afterPreferences[key]=preferences.value(key);checks["self_check_does_not_save_preferences"]=beforePreferences==afterPreferences;
    checks["main_page_has_no_search_launcher"]=homeHasNoSearch();resize(820,600);flush();checks["narrow_main_page_has_no_search_launcher"]=homeHasNoSearch();resize(760,500);flush();checks["minimum_idle_without_search_fits"]=idleLabelsFit()&&homeHasNoSearch()&&fit()&&titlebarFits();save("dashboard-minimum-idle");
    const auto compactAccountName=localPlayerDisplayName;const auto compactCoverGeometry=cover->geometry();setLocalPlayerDisplayName(QStringLiteral("超长玩家昵称ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789").repeated(3));flush();checks["minimum_long_account_name_preserves_cover_and_separate_reading_rows"]=cover->isVisibleTo(this)&&cover->height()==SatinBanner::Height&&cover->geometry()==compactCoverGeometry&&cover->rect().contains(cover->titleRect())&&cover->displayedTitle()!=cover->titleText()&&QFontMetrics(cover->titleFont()).horizontalAdvance(cover->displayedTitle())<=cover->titleRect().width()&&matchScroll->viewport()->rect().contains(relativeRect(cover,matchScroll->viewport()))&&!relativeRect(cover,matchScroll->viewport()).intersects(relativeRect(personalHome,matchScroll->viewport()))&&idleLabelsFit()&&fit();save("dashboard-minimum-idle-long-name");setLocalPlayerDisplayName(compactAccountName);resize(840,520);flush();
    if(const auto cover=findChild<QWidget*>(QStringLiteral("kdaCampaignCover"))){const auto motion=static_cast<SatinBanner*>(cover)->motionChecks(outputDir);for(auto it=motion.begin();it!=motion.end();++it)checks[it.key()]=it.value();}
    // Local-only personal-home fixtures never enter the anonymous runtime
    // snapshot or the current-account bridge.
    const auto homeGeneration=playerHistory->report().value("personal_home").toObject().value("generation");
    personalHome->setProfile({});personalHome->setState(false,QStringLiteral("登录客户端后显示本人战绩"));flush();
    checks["personal_home_empty_account_has_unknown_metrics_and_no_rows"]=!personalHome->report().value("has_profile").toBool()&&personalHome->report().value("win_rate").isNull()&&personalHome->report().value("kda_mean").isNull()&&personalHome->report().value("rows").toInt()==0;
    QJsonArray homeMatches;const int homeKills[]{8,4,6,2,0,3},homeDeaths[]{2,0,3,4,1,3},homeAssists[]{12,6,8,6,0,6};
    for(int i=0;i<6;++i)homeMatches.append(QJsonObject{{"match_ref",QStringLiteral("synthetic-personal-%1").arg(i)},{"champion_id",QJsonArray{1,10,22,86,12,64}[i]},{"win",i==2?QJsonValue():QJsonValue(i==0||i==3||i==5)},{"duration_seconds",900+i*33},{"scores",QJsonObject{{"kills",homeKills[i]},{"deaths",i==2?QJsonValue():QJsonValue(homeDeaths[i])},{"assists",homeAssists[i]}}}});
    QJsonArray homeAxes;const QStringList homeAxisLabels{QStringLiteral("输出"),QStringLiteral("经济"),QStringLiteral("参团"),QStringLiteral("生存"),QStringLiteral("承伤")};const double homeAxisValues[]{1062,890,65,.35,885},homeAxisRanks[]{65,50,0,35,55};
    for(int i=0;i<5;++i)homeAxes.append(QJsonObject{{"label",homeAxisLabels[i]},{"value",homeAxisValues[i]},{"unit",i==2?QStringLiteral("%"):QStringLiteral("/分")},{"n",6},{"percentile",homeAxisRanks[i]},{"percentile_n",5}});
    const QJsonObject homeProfile{{"player",QJsonObject{{"display_name",QStringLiteral("合成本人#00000")},{"server","TJ100"}}},{"coverage",QJsonObject{{"eligible_matches",38},{"selected_matches",6}}},{"matches",homeMatches},{"five_axes",homeAxes}};
    personalHome->setProfile(homeProfile);personalHome->setState(false,{});setLocalPlayerDisplayName(QStringLiteral("合成本人#00000"));applySnapshot({});selectPage(0);flush();
    checks["personal_home_nickname_separates_tag_without_losing_account_identity"]=cover->titleText()==QStringLiteral("合成本人")&&cover->toolTip()==QStringLiteral("合成本人#00000")&&cover->accessibleName().contains(QStringLiteral("合成本人#00000"));
    const auto homeNumbers=personalHome->report();checks["personal_home_metrics_use_their_actual_recorded_denominators"]=homeNumbers.value("win_samples").toInt()==5&&std::abs(homeNumbers.value("win_rate").toDouble()-60)<.001&&homeNumbers.value("kda_samples").toInt()==5&&std::abs(homeNumbers.value("kda_mean").toDouble()-5)<.001&&homeNumbers.value("effective_matches").toInt()==6;
    checks["personal_home_is_reduced_and_report_is_anonymous"]=!homeNumbers.value("radar_visible").toBool()&&homeNumbers.value("result_strip_count").toInt()==0&&!QJsonDocument(report()).toJson().contains("合成本人#00000")&&!QJsonDocument(homeNumbers).toJson().contains("synthetic-personal-");
    const auto savedMatchRequest=personalHome->onMatchRequested;const auto savedOpenAll=personalHome->onOpenAll;QString requestedHomeMatch;int openAllRequests=0;
    personalHome->onMatchRequested=[&](const QString &ref){requestedHomeMatch=ref;};personalHome->onOpenAll=[&]{++openAllRequests;};
    personalHome->findChild<QPushButton*>(QStringLiteral("personalHomeMatch_3"))->click();personalHome->findChild<QPushButton*>(QStringLiteral("personalHomeOpenAll"))->click();
    checks["personal_home_recent_actions_keep_stable_match_refs"]=requestedHomeMatch==QStringLiteral("synthetic-personal-3")&&openAllRequests==1;
    personalHome->onMatchRequested=savedMatchRequest;personalHome->onOpenAll=savedOpenAll;
    const auto homeSkin=Kda::skin();for(const auto &id:{QStringLiteral("kda"),QStringLiteral("kda_red")}){
        Kda::setSkin(id);resize(840,520);flush();save(QStringLiteral("dashboard-homepage-rich-")+id);
        const int compactHomeHeight=personalHome->height(),compactRowCount=personalHome->report().value("rows").toInt();
        resize(1260,780);flush();
        checks[QStringLiteral("personal_home_expands_recent_rows_to_use_large_window_")+id]=idleLabelsFit()&&fit()&&personalHome->height()>compactHomeHeight+100&&personalHome->report().value("rows").toInt()>compactRowCount&&personalHome->report().value("recent_list_width").toInt()==personalHome->width()-28;
        save(QStringLiteral("dashboard-homepage-rich-large-")+id);
        resize(760,500);flush();checks[QStringLiteral("personal_home_minimum_fits_")+id]=idleLabelsFit()&&fit()&&titlebarFits()&&personalHome->report().value("rows").toInt()>=3&&!findChild<QWidget*>(QStringLiteral("homeReadoutPlane"))->isVisibleTo(this);save(QStringLiteral("dashboard-homepage-rich-minimum-")+id);
    }
    Kda::setSkin(homeSkin);resize(840,520);
    auto partialHome=homeProfile;QJsonArray unknownHomeMatches;for(const auto &v:homeMatches){auto m=v.toObject();m["win"]=QJsonValue();auto s=m.value("scores").toObject();s["deaths"]=QJsonValue();m["scores"]=s;unknownHomeMatches.append(m);}partialHome["matches"]=unknownHomeMatches;auto partialHomeAxes=homeAxes;auto noRank=partialHomeAxes[0].toObject();noRank["percentile"]=QJsonValue();partialHomeAxes[0]=noRank;partialHome["five_axes"]=partialHomeAxes;personalHome->setProfile(partialHome);flush();
    checks["personal_home_missing_results_and_kda_are_not_zero"]=personalHome->report().value("win_rate").isNull()&&personalHome->report().value("kda_mean").isNull();
    checks["personal_home_fixtures_do_not_start_account_queries"]=homeGeneration==playerHistory->report().value("personal_home").toObject().value("generation");personalHome->setProfile(homeProfile);
    auto scoreFixture=liveFixture;auto scoreMatch=scoreFixture.value("match_context").toObject();auto scoreRoster=scoreMatch.value("roster").toArray();auto scoreHistory=scoreFixture.value("anonymous_player_history").toObject();auto scorePlayers=scoreHistory.value("players").toArray();
    for(int i=0;i<scoreRoster.size()&&i<scorePlayers.size();++i){const auto token=QString::number(i+1,16).rightJustified(32,QChar(u'0'));auto row=scoreRoster[i].toObject();row["slot_token"]=token;row["cell"]=i;scoreRoster[i]=row;auto reference=scorePlayers[i].toObject();reference["slot_token"]=token;reference["cell"]=i;reference["resolved_server"]="TJ100";reference["status"]="known";reference["recent_results_score"]=QJsonObject{{"status",i<8?"scored":"unknown"},{"reason",i<8?"":"index_player_missing"},{"value",i<8?QJsonValue(44.2+i):QJsonValue()},{"epoch","synthetic-score-epoch"},{"model_version",rosterRecentScoreModel()},{"as_of_ms",1000},{"server","TJ100"},{"window_days",30},{"queue_id",2400},{"mode","KIWI"},{"observations",QJsonObject{{"matches",12},{"wins",5},{"losses",7},{"independent_matchups",10},{"effective_n",8.4}}},{"sample_support",QJsonObject{{"level","low"}}}};scorePlayers[i]=reference;}
    scoreMatch["roster"]=scoreRoster;scoreMatch["stage"]="loading";scoreFixture["match_context"]=scoreMatch;scoreFixture["live_phase"]="GameStart";scoreFixture["live_data_fresh"]=false;scoreHistory["as_of_ms"]=2000;scoreHistory["players"]=scorePlayers;scoreHistory["score_context"]=QJsonObject{{"status","ready"},{"schema_version",1},{"server","TJ100"},{"epoch","synthetic-score-epoch"},{"model_version",rosterRecentScoreModel()},{"score_as_of_ms",1000},{"published_at_ms",1000},{"requested_as_of_ms",2000},{"network_requests",0},{"cached_only",true},{"read_only",true},{"stale",true}};scoreFixture["anonymous_player_history"]=scoreHistory;selectPage(0);resize(840,520);applySnapshot(scoreFixture);flush();
    checks["loading_recent_scores_show_cached_value_without_live_kda"]=allies.first().marker->property("fullText").toString().contains(QStringLiteral("44.2"))&&allies.first().detail->property("fullText").toString().contains(QStringLiteral("本人12局"));checks["partial_recent_scores_do_not_make_team_means"]=!matchView.value("recent_results").toObject().value("aggregate_allowed").toBool()&&matchView.value("recent_results").toObject().value("known").toInt()==8;checks["recent_score_layout_fits_default"]=fit()&&seatColumnsFit();save("dashboard-roster-recent-loading");resize(760,500);flush();checks["recent_score_layout_fits_minimum"]=fit()&&seatColumnsFit()&&firstScreen();save("dashboard-roster-recent-minimum");
    resize(840,520);scoreMatch["stage"]="in_game";scoreFixture["match_context"]=scoreMatch;scoreFixture["live_phase"]="InProgress";scoreFixture["live_data_fresh"]=true;applySnapshot(scoreFixture);flush();checks["ingame_recent_score_does_not_replace_live_kda"]=allies.first().marker->property("fullText")==QStringLiteral("8 / 3 / 11")&&allies.first().detail->property("fullText").toString().contains(QStringLiteral("44.2"));save("dashboard-roster-recent-ingame");applySnapshot(liveFixture);flush();
    const auto playerChecks=playerHistory->selfCheck(outputDir);for(auto it=playerChecks.begin();it!=playerChecks.end();++it)checks[it.key()]=it.value();
    const auto originalSkin=Kda::skin();const bool originalDepth=Kda::depthEnabled();const int originalSettingsTab=settingsTabs->currentIndex();
    const auto currentSettingsStack=findChild<QStackedWidget*>(QStringLiteral("settingsSections"));
    const auto currentSettingsHeightContract=[&]{
        const auto page=currentSettingsStack->currentWidget();if(!page)return false;
        const int width=currentSettingsStack->contentsRect().width();
        const int wrappedHeight=page->hasHeightForWidth()?page->heightForWidth(width):-1;
        const int expectedHeight=std::max({0,page->minimumSizeHint().height(),wrappedHeight>=0?wrappedHeight:page->sizeHint().height()});
        return currentSettingsStack->minimumSizeHint()==QSize(0,page->minimumSizeHint().height())&&
            currentSettingsStack->sizeHint()==page->sizeHint()&&currentSettingsStack->hasHeightForWidth()==page->hasHeightForWidth()&&
            currentSettingsStack->minimumHeight()==expectedHeight&&currentSettingsStack->maximumHeight()==expectedHeight&&
            currentSettingsStack->height()==expectedHeight;
    };
    settingsTabs->setCurrentIndex(settingsGeneralTab);resize(760,500);flush();const int generalMinimumHeight=currentSettingsStack->minimumSizeHint().height();
    const bool generalHeightContract=currentSettingsHeightContract();
    settingsTabs->setCurrentIndex(settingsModelsTab);flush();const int modelMinimumHeight=currentSettingsStack->minimumSizeHint().height();
    checks["general_tab_does_not_inherit_model_page_minimum_height"]=generalHeightContract;
    selectPage(1);settingsTabs->setCurrentIndex(settingsSkinsTab);resize(840,520);flush();
    const auto skinCardsFit=[&]{bool okay=skinChoices.size()==2;for(auto choice:skinChoices)okay&=choice->isVisibleTo(this)&&choice->width()>=150&&choice->height()==110&&QFontMetrics(Kda::font(Kda::FontRole::Body,12,QFont::DemiBold)).horizontalAdvance(choice->accessibleName())<=choice->width()-35&&QFontMetrics(Kda::font(Kda::FontRole::Body,12)).horizontalAdvance(choice->toolTip())<=choice->width()-20;for(int i=0;i<skinChoices.size();++i)for(int j=i+1;j<skinChoices.size();++j)okay&=!skinChoices[i]->geometry().intersects(skinChoices[j]->geometry());return okay;};
    checks["skin_tab_has_two_readable_kda_previews"]=skinCardsFit();
    const qreal originalHudStrength=Kda::HudStyle::strength();const auto strengthGeometry=hudSkinStrength->geometry();
    Kda::HudStyle::setStrength(1.);checks["hud_skin_strength_menu_syncs_visible_slider"]=hudSkinStrength->value()==100&&hudSkinStrengthValue->text()==QStringLiteral("100%");
    hudSkinStrength->setValue(0);checks["hud_skin_strength_slider_updates_all_huds_without_layout_growth"]=Kda::HudStyle::strength()==0&&hudSkinStrength->geometry()==strengthGeometry;
    Kda::HudStyle::setStrength(originalHudStrength);
    for(auto choice:skinChoices){choice->click();flush();const auto id=choice->property("skinId").toString();save(QStringLiteral("dashboard-skin-")+id);
        settingsTabs->setCurrentIndex(settingsAboutTab);flush();save(QStringLiteral("dashboard-version-")+id);settingsTabs->setCurrentIndex(settingsSkinsTab);flush();
        Kda::postNotice({QStringLiteral("inspection.notice"),QStringLiteral("战绩已就绪"),QStringLiteral("已读取本机保存的战绩。"),Kda::NoticeKind::Success,12000});flush();
        const auto notice=Kda::noticeReport();setProperty(qPrintable(id+"NoticeDiagnostics"),QVariant::fromValue(notice));checks[id+QStringLiteral("_global_notice_follows_skin_and_stays_inside_window")]=notice.value("visible").toBool()&&notice.value("within_host").toBool()&&notice.value("skin")==id&&!notice.value("desktop_window").toBool();save(QStringLiteral("dashboard-global-notice-")+id);
    }Kda::dismissNotice(QStringLiteral("inspection.notice"));
    resize(760,500);flush();const int skinScrollPosition=settingsScroll->verticalScrollBar()->value();bool skinCardsReachable=skinCardsFit();for(auto choice:skinChoices){settingsScroll->ensureWidgetVisible(choice,0,0);flush();skinCardsReachable&=settingsScroll->viewport()->rect().contains(relativeRect(choice,settingsScroll->viewport()));}settingsScroll->verticalScrollBar()->setValue(skinScrollPosition);flush();checks["skin_tab_fits_minimum_window"]=fit()&&skinCardsReachable&&skinChoices[0]->geometry().top()==skinChoices[1]->geometry().top();checks["minimum_skin_controls_scroll_into_view_without_overflow"]=settingsContentReachable();const int skinMinimumHeight=currentSettingsStack->minimumSizeHint().height();checks["skin_tab_does_not_inherit_model_page_minimum_height"]=currentSettingsHeightContract();settingsScroll->verticalScrollBar()->setValue(0);flush();save("dashboard-skins-minimum");
    settingsTabs->setCurrentIndex(settingsAboutTab);flush();
    const auto aboutView=findChild<QWidget*>(QStringLiteral("versionAboutPage"));checks["about_restores_original_centered_reading_width"]=aboutView&&aboutView->width()==std::min(settingsReadingWidth,settingsPage->layout()->contentsRect().width())&&std::abs(relativeRect(aboutView,settingsPage).center().x()-settingsPage->layout()->contentsRect().center().x())<=1&&settingsColumnFits();
    {
        auto *pin = findChild<QAbstractButton*>(QStringLiteral("chromePin"));
        const auto pinned = [&] {
#ifdef Q_OS_WIN
            if (QGuiApplication::platformName() == QStringLiteral("windows"))
                return bool(GetWindowLongPtrW(reinterpret_cast<HWND>(winId()), GWL_EXSTYLE) & WS_EX_TOPMOST);
#endif
            return windowHandle() && windowHandle()->flags().testFlag(Qt::WindowStaysOnTopHint);
        };
        const bool wasPinned = pin->isChecked();
        const auto beforePin = geometry();
        pin->click();
        checks["chrome_pin_toggles_topmost_without_moving_window"] =
            pin->isChecked() != wasPinned && pinned() != wasPinned && geometry() == beforePin && isVisible();
        if (pin->isChecked()) save("dashboard-pinned");
        pin->click();
        checks["chrome_pin_can_restore_original_stacking"] = pin->isChecked() == wasPinned && pinned() == wasPinned;

        auto *effect = static_cast<InactiveDashboardEffect*>(graphicsEffect());
        const auto desaturation = [&] { return effect->report().value("desaturation").toDouble(); };
        const bool oldReducedMotion = Kda::reducedMotion();
        Kda::setReducedMotion(false);
        effect->setInactive(false,false);
        save("dashboard-focus-active");
        // Exercise the production effect directly. This does not establish OS focus behavior.
        effect->setInactive(true,true);
        waitMotion(80);
        const auto fading = desaturation();
        effect->setInactive(false,true);
        const auto reversalStart = desaturation();
        waitMotion(80);
        const auto reversing = desaturation();
        checks["focus_desaturation_eases_and_reverses_from_current_amount"] =
            fading > 0 && fading < InactiveDashboardEffect::inactiveAmount &&
            qFuzzyCompare(reversalStart + 1, fading + 1) && reversing < reversalStart;
        effect->setInactive(true,true);
        waitMotion(InactiveDashboardEffect::durationMs + 30);
        checks["focus_desaturation_settles_at_eighty_percent"] = desaturation() == InactiveDashboardEffect::inactiveAmount;
        save("dashboard-focus-inactive");
        effect->setInactive(false,true);
        waitMotion(80);
        const auto restoring = desaturation();
        waitMotion(InactiveDashboardEffect::durationMs + 30);
        checks["focus_saturation_eases_back_to_full_color"] =
            restoring > 0 && restoring < InactiveDashboardEffect::inactiveAmount && desaturation() == 0 && !effect->isEnabled();
        Kda::setReducedMotion(true);
        effect->setInactive(true,!Kda::reducedMotion());
        const bool instantFade = desaturation() == InactiveDashboardEffect::inactiveAmount&&!effect->report().value("animating").toBool();
        effect->setInactive(false,!Kda::reducedMotion());
        checks["focus_desaturation_respects_reduce_motion"] = instantFade && desaturation() == 0&&!effect->report().value("animating").toBool();
        Kda::setReducedMotion(oldReducedMotion);
        QEvent activation(QEvent::ActivationChange);
        QCoreApplication::sendEvent(this,&activation);
        checks["activation_event_targets_actual_window_active_state"] =
            effect->report().value("target").toDouble()==(isActiveWindow()?0:InactiveDashboardEffect::inactiveAmount);
        if (!outputDir.isEmpty()) {
            QFile trace(QDir(outputDir).filePath(QStringLiteral("focus-appearance.json")));
            if (trace.open(QIODevice::WriteOnly)) trace.write(QJsonDocument(QJsonObject{
                {"fade_midpoint",fading},{"reversal_start",reversalStart},{"reversal_midpoint",reversing},
                {"restore_midpoint",restoring}}).toJson());
        }
    }
    checks["minimum_about_content_scrolls_into_view_without_overflow"]=settingsContentReachable();const int aboutMinimumHeight=currentSettingsStack->minimumSizeHint().height();checks["about_tab_does_not_inherit_model_page_minimum_height"]=currentSettingsHeightContract();settingsScroll->verticalScrollBar()->setValue(0);flush();save("dashboard-version-minimum");
    setProperty("settingsResponsiveDiagnostics",QVariant::fromValue(QJsonObject{{"breakpoint_content_width",settingsTwoColumnWidth},{"maximum_content_width",settingsReadingWidth},{"general_minimum_height",generalMinimumHeight},{"model_minimum_height",modelMinimumHeight},{"skin_minimum_height",skinMinimumHeight},{"about_minimum_height",aboutMinimumHeight},{"minimum_window",QJsonArray{760,500}},{"current_page_only_size_hint",true}}));
    Kda::setSkin(originalSkin);for(auto choice:skinChoices)choice->setChecked(choice->property("skinId").toString()==originalSkin);Kda::setDepthEnabled(originalDepth);settingsTabs->setCurrentIndex(originalSettingsTab);flush();
    const auto restoreGeometry=geometry();auto maximize=findChild<QAbstractButton*>(QStringLiteral("chromeMaximize"));auto minimize=findChild<QAbstractButton*>(QStringLiteral("chromeMinimize"));auto closeControl=findChild<QAbstractButton*>(QStringLiteral("chromeClose"));
    // QWidget state changes precede Windows' native placement notifications.
    // Wait for both layers and a stable client rectangle, then inspect the
    // actual restored size and alpha; a fixed sleep cannot establish either.
    const auto waitForWindowState=[&](bool maximized){QElapsedTimer deadline,stable;deadline.start();QRect previous;while(deadline.elapsed()<900){flush();bool ready=isMaximized()==maximized&&!isMinimized()&&(!windowHandle()||windowHandle()->windowState()==(maximized?Qt::WindowMaximized:Qt::WindowNoState));
#ifdef Q_OS_WIN
        // Qt's frameless Windows backend simulates maximization with MoveWindow;
        // IsZoomed need not follow its logical state. Compare the actual client.
        const HWND native=reinterpret_cast<HWND>(internalWinId());RECT client{};ready&=native&&!IsIconic(native)&&GetClientRect(native,&client)&&std::abs((client.right-client.left)-qRound(width()*devicePixelRatioF()))<=2&&std::abs((client.bottom-client.top)-qRound(height()*devicePixelRatioF()))<=2;
#endif
        if(!ready||geometry()!=previous){previous=geometry();stable.start();}else if(stable.isValid()&&stable.elapsed()>=35){repaint();flush();return true;}QThread::msleep(3);}return false;};
    QJsonObject transitions{{"before_maximize",chrome?chrome->report():QJsonObject{}}};
    if(maximize)maximize->click();transitions["maximize_immediate"]=chrome?chrome->report():QJsonObject{};const bool maximizedSettled=waitForWindowState(true);transitions["maximize_settled"]=chrome?chrome->report():QJsonObject{};checks["chrome_maximize_button_changes_state"]=maximizedSettled&&isMaximized();checks["chrome_maximize_respects_work_area"]=screen()&&screen()->availableGeometry().adjusted(-2,-2,2,2).contains(frameGeometry());save("dashboard-maximized");
    if(maximize)maximize->click();transitions["restore_immediate"]=chrome?chrome->report():QJsonObject{};const bool restoredSettled=waitForWindowState(false);transitions["restore_settled"]=chrome?chrome->report():QJsonObject{};setProperty("chromeStateDiagnostics",QVariant::fromValue(transitions));checks["chrome_restore_button_restores_size"]=restoredSettled&&!isMaximized()&&size()==restoreGeometry.size();const auto restored=grab().toImage();checks["chrome_restore_recovers_transparent_corners"]=restoredSettled&&restored.hasAlphaChannel()&&restored.pixelColor(0,0).alpha()==0;save("dashboard-restored");
    if(minimize)minimize->click();flush();checks["chrome_minimize_button_changes_state"]=isMinimized();showNormal();flush();checks["chrome_minimized_window_can_reopen"]=isVisible()&&!isMinimized();
    int closeRequests=0;
    const bool trayClose=closesToTray();
    auto realQuit=std::exchange(onQuit,[&]{++closeRequests;});
    if(closeControl)closeControl->click();
    onQuit=std::move(realQuit);
    flush();
    checks["closing_main_window_applies_preference"]=closeControl&&isHidden()&&pages->count()==3&&closeRequests==(trayClose?0:1);
    show();flush();checks["closed_fixture_can_reopen"]=isVisible()&&matchNavigation->isVisibleTo(this);
    apiKey->setText(originalKey);model->setCurrentText(originalModel);modelSlot->setCurrentIndex(originalEditingModelSlot);settingsTabs->setCurrentIndex(originalSettingsTabAtStart);modelSaveStatus->setText(originalModelSaveStatus);saveStatus->setText(originalStatus);setLocalPlayerDisplayName(originalPlayerName);applySnapshot(original);setGeometry(originalGeometry);setWindowState(originalWindowState);selectPage(originalPage);inSelfCheck=false;updatePersonalHome();updateView();if(homePages)homePages->setCurrentIndex(originalHomePage);replaySawMatch=originalReplaySawMatch;flush();if(!wasVisible)hide();if(originalFocus)originalFocus->setFocus(Qt::OtherFocusReason);
}

void CoachDashboard::querySummoner(const QString &id){if(startupNoticeActive)return;selectPage(2);showNormal();raise();activateWindow();playerHistory->query(id);}
QJsonObject CoachDashboard::playerReport()const{return playerHistory->report();}
