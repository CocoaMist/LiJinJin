#include "growth_capture_settings.h"
#include "kda_controls.h"
#include "kda_glass_surface.h"
#include "kda_theme.h"
#include "preference_switch.h"
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QGuiApplication>
#include <QScreen>
#include <QMouseEvent>
#include <QPainter>
#include <cmath>

namespace {
class HudRegionPicker:public QDialog {
public:
    QRect selection;QPoint start;QString instruction;
    explicit HudRegionPicker(const QRect &viewport,const QString &text):QDialog(nullptr),instruction(text){
        setWindowFlags(Qt::Tool|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint);setAttribute(Qt::WA_TranslucentBackground);setCursor(Qt::CrossCursor);setGeometry(viewport);
    }
    void paintEvent(QPaintEvent *)override{QPainter p(this);Kda::configurePainter(p);p.fillRect(rect(),QColor(0,0,0,90));p.setPen(Qt::white);p.setFont(Kda::font(Kda::FontRole::Body,18));p.drawText(rect().adjusted(30,30,-30,-30),Qt::AlignTop|Qt::AlignHCenter,instruction+QStringLiteral("\n拖动框选 · Esc 取消"));p.setPen(QPen(Kda::color("ice"),2));p.drawRect(selection);}
    void mousePressEvent(QMouseEvent *e)override{if(e->button()==Qt::LeftButton){start=e->position().toPoint();selection=QRect(start,QSize());update();}}
    void mouseMoveEvent(QMouseEvent *e)override{if(e->buttons()&Qt::LeftButton){selection=QRect(start,e->position().toPoint()).normalized().intersected(rect());update();}}
    void mouseReleaseEvent(QMouseEvent *e)override{if(e->button()==Qt::LeftButton&&selection.width()>=8&&selection.height()>=8)accept();}
};
bool hasReason(const QJsonObject &status,const QString &reason){
    for(const auto &value:status.value("unknowns").toArray())if(value.toString()==reason||value.toObject().value("code").toString()==reason)return true;
    return status.value("reason").toString()==reason;
}
bool sourceExpired(const QJsonObject &status){
    const auto state=status.value("status").toString();
    return status.value("stale").toBool()||status.value("expired").toBool()
        ||state==QStringLiteral("stale")||state==QStringLiteral("expired")||state==QStringLiteral("data_expired")
        ||hasReason(status,QStringLiteral("context_expired"));
}
bool sourceConnected(const QJsonObject &status){
    const auto state=status.value("status").toString();
    if(state==QStringLiteral("unavailable")||state==QStringLiteral("error"))return false;
    return status.value("connected").toBool()||status.value("structured_source_connected").toBool()
        ||state==QStringLiteral("connected")||state==QStringLiteral("ready")||state==QStringLiteral("observed");
}
bool partialGrowthAvailable(const QJsonObject &status){
    const auto coverage=status.value("input_coverage").toObject().isEmpty()?status.value("coverage").toObject():status.value("input_coverage").toObject();
    const auto glory=coverage.value("glory_stacks").toObject();
    return sourceConnected(status)&&glory.value("available").toBool()&&glory.value("source_kind")==QStringLiteral("event_derived_counter");
}
bool layersMissing(const QJsonObject &status){
    const auto state=status.value("status").toString();
    if(state==QStringLiteral("unavailable")||state==QStringLiteral("error"))return false;
    return status.value("layers_missing").toBool()||state==QStringLiteral("layers_unavailable")||state==QStringLiteral("missing_stacks")
        ||hasReason(status,QStringLiteral("structured_growth_counters_unavailable"))||hasReason(status,QStringLiteral("growth_counters_not_provided"))
        ||hasReason(status,QStringLiteral("equipment_growth_counters_not_exposed"))||hasReason(status,QStringLiteral("augment_task_progress_not_exposed"));
}
QString sourceExplanation(const QJsonObject &status){
    if(sourceExpired(status))return QStringLiteral("上次记录已过期，等待当前对局的新数据。");
    const auto visible=status.value("visible_reader").toObject();if(visible.value("enabled").toBool())return QStringLiteral("悬停自己的装备或海克斯图标后，连续两次核对名称和数值。遮挡、失焦或过期时保持未知；可先收起助手浮层再悬停。");
    if(partialGrowthAvailable(status))return QStringLiteral("已有部分成长记录；未提供项保持未知。");
    if(layersMissing(status))return QStringLiteral("已有来源未提供成长层数或任务进度，这些数据继续保持未知。");
    if(sourceConnected(status))return QStringLiteral("结构化来源已连接；只有明确提供的输入参与计算。");
    return QStringLiteral("等待可核实的结构化来源；当前没有可用的成长层数记录。");
}
bool manualSource(const QJsonObject &value){
    const auto source=value.value("source_kind").toString(value.value("source").toString());
    return value.value("manual").toBool()||source==QStringLiteral("manual")||source==QStringLiteral("manual_input")
        ||source==QStringLiteral("manual_record")||source==QStringLiteral("manual_confirmation")||source==QStringLiteral("imported_record")||source==QStringLiteral("offline_assumption");
}
QString coverageText(const QJsonObject &status,const QStringList &keys){
    if(sourceExpired(status))return QStringLiteral("数据过期");
    const auto coverage=status.value("input_coverage").toObject().isEmpty()?status.value("coverage").toObject():status.value("input_coverage").toObject();
    for(const auto &key:keys){
        const auto value=coverage.value(key);const auto row=value.toObject();const auto state=row.value("status").toString();
        const bool provided=value==QJsonValue(true)||row.value("available").toBool()||row.value("provided").toBool()
            ||state==QStringLiteral("provided")||state==QStringLiteral("observed")||state==QStringLiteral("available");
        if(!provided)continue;
        if(manualSource(row)||manualSource(status))return QStringLiteral("手动记录 · 非自动实测");
        if(row.value("source_kind")==QStringLiteral("event_derived_counter"))return QStringLiteral("事件推导 · 有条件");
        return QStringLiteral("已提供");
    }
    return QStringLiteral("未提供");
}
QLabel *copy(const QString &text,QWidget *parent=nullptr){
    auto label=new QLabel(text,parent);label->setTextFormat(Qt::PlainText);label->setWordWrap(true);label->setMinimumWidth(0);label->setFont(Kda::font(Kda::FontRole::Body,13));return label;
}
}

QString growthCaptureStatusText(const QJsonObject &status){
    const auto visible=status.value("visible_reader").toObject();
    if(visible.value("settings").toObject().value("enabled").toBool()&&!visible.value("enabled").toBool())return QStringLiteral("等待定位图标栏");
    if(sourceExpired(status))return QStringLiteral("数据过期");
    if(partialGrowthAvailable(status))return QStringLiteral("部分成长可用");
    if(visible.value("enabled").toBool()){
        const int observed=visible.value("selected_inputs").toObject().value("observed_count").toInt();
        if(observed>0)return QStringLiteral("强化已读 %1 个").arg(observed);
        return visible.value("process_running").toBool()?QStringLiteral("悬停自身图标读取"):QStringLiteral("自动读取未启动");
    }
    if(layersMissing(status))return QStringLiteral("未提供层数");
    if(sourceConnected(status))return QStringLiteral("已连接");
    return QStringLiteral("等候结构化来源");
}

QJsonObject editGrowthCaptureSettings(QWidget *parent,const QJsonObject &status,const QRect &gameViewport,qreal gameViewportScale){
    QDialog dialog(parent);dialog.setWindowTitle(QStringLiteral("自动读取设置"));dialog.setObjectName(QStringLiteral("growthDataSourceDialog"));dialog.setMinimumSize(420,360);dialog.resize(580,480);
    Kda::bindThemeStyle(&dialog,[]{return Kda::overlayStyleSheet()+QStringLiteral("QDialog#growthDataSourceDialog{background:%1;color:%2;} QLabel{color:%3;} QWidget#growthSourceCoverage{background:transparent;border:0;border-radius:%4px;} QScrollArea{background:transparent;border:0;} QScrollArea>QWidget>QWidget{background:transparent;}").arg(Kda::color("bg").name(),Kda::color("text").name(),Kda::color("silver").name()).arg(Kda::radius("section"));});
    auto layout=new QVBoxLayout(&dialog);layout->setContentsMargins(16,14,16,14);layout->setSpacing(10);
    auto title=copy(QStringLiteral("自动读取设置"));title->setFont(Kda::font(Kda::FontRole::Title,19,QFont::DemiBold));layout->addWidget(title);
    auto scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);auto body=new QWidget;auto content=new QVBoxLayout(body);content->setContentsMargins(0,0,4,0);content->setSpacing(10);
    auto state=copy(growthCaptureStatusText(status));state->setObjectName(QStringLiteral("growthDataSourceState"));state->setFont(Kda::font(Kda::FontRole::Body,15,QFont::DemiBold));content->addWidget(state);content->addWidget(copy(sourceExplanation(status)));
    auto visibleSettings=status.value("visible_reader").toObject().value("settings").toObject();auto originalVisibleSettings=visibleSettings;originalVisibleSettings["enabled"]=visibleSettings.value("enabled").toBool();
    auto policy=copy(QStringLiteral("结构化数据优先。可选读取自己图标的可见提示；画面与识别原文不保存，也不会移动游戏鼠标。"));policy->setObjectName(QStringLiteral("growthDataSourcePolicy"));content->addWidget(policy);
    auto visibleToggle=new PreferenceSwitch(QStringLiteral("自动读取自己的海克斯与成长提示"));visibleToggle->setObjectName(QStringLiteral("visibleMatchInputsEnabled"));visibleToggle->setChecked(visibleSettings.value("enabled").toBool());content->addWidget(visibleToggle);
    auto calibration=copy(gameViewport.isEmpty()?QStringLiteral("尚未定位时，勾选开关不会开始识别。进入对局后，分别框选自己的装备栏和海克斯栏；正常悬停图标即可读取可见提示。"):QStringLiteral("分别定位装备栏和海克斯栏。定位后，正常悬停图标即可读取可见提示；分辨率或 HUD 大小变化后需重新定位。"));content->addWidget(calibration);
    auto pickerActions=new QHBoxLayout;auto itemPicker=new Kda::Button(QStringLiteral("定位装备栏"));auto augmentPicker=new Kda::Button(QStringLiteral("定位海克斯栏"));pickerActions->addWidget(itemPicker);pickerActions->addWidget(augmentPicker);content->addLayout(pickerActions);itemPicker->setEnabled(!gameViewport.isEmpty());augmentPicker->setEnabled(!gameViewport.isEmpty());
    const auto choose=[&](const QString &key,const QString &instruction){
        dialog.hide();HudRegionPicker picker(gameViewport,instruction);const bool chosen=picker.exec()==QDialog::Accepted;dialog.show();
        if(!chosen)return;auto screen=QGuiApplication::screenAt(gameViewport.center());const double scale=std::isfinite(gameViewportScale)&&gameViewportScale>0?gameViewportScale:(screen?screen->devicePixelRatio():1.);
        auto binding=visibleSettings.value("capture_binding").toObject();const int width=qRound(gameViewport.width()*scale),height=qRound(gameViewport.height()*scale);
        if(binding.value("client_width").toInt()!=width||binding.value("client_height").toInt()!=height)binding={};
        const auto r=picker.selection;binding["kind"]="self_hud_calibration";binding["client_width"]=width;binding["client_height"]=height;
        binding[key]=QJsonArray{qRound(r.x()*scale),qRound(r.y()*scale),qRound(r.width()*scale),qRound(r.height()*scale)};visibleSettings["capture_binding"]=binding;visibleToggle->setChecked(true);calibration->setText(QStringLiteral("定位已记录。只框选自己的图标，勿包含商店、队友或记分板；游戏 HUD 缩放后请重新定位。"));
    };
    QObject::connect(itemPicker,&QPushButton::clicked,&dialog,[&]{choose(QStringLiteral("self_inventory_rect"),QStringLiteral("框选自己的装备图标栏"));});
    QObject::connect(augmentPicker,&QPushButton::clicked,&dialog,[&]{choose(QStringLiteral("self_augment_rect"),QStringLiteral("框选自己已持有的海克斯图标栏"));});
    auto panel=new Kda::GlassSurface(nullptr,Kda::GlassSurfaceRole::Reading);panel->setObjectName(QStringLiteral("growthSourceCoverage"));auto grid=new QGridLayout(panel);grid->setContentsMargins(12,10,12,10);grid->setHorizontalSpacing(14);grid->setVerticalSpacing(9);int row=0;
    const auto add=[&](const QString &name,const QStringList &keys){grid->addWidget(copy(name),row,0);auto value=copy(coverageText(status,keys));value->setObjectName(QStringLiteral("growthCoverage_")+keys.first());value->setAlignment(Qt::AlignRight|Qt::AlignVCenter);grid->addWidget(value,row++,1);};
    add(QStringLiteral("自身当前生命值"),{QStringLiteral("current_health"),QStringLiteral("currentHealth")});
    add(QStringLiteral("自身最大生命值"),{QStringLiteral("max_health"),QStringLiteral("maxHealth")});
    add(QStringLiteral("当前装备"),{QStringLiteral("owned_items"),QStringLiteral("inventory")});
    add(QStringLiteral("已选海克斯"),{QStringLiteral("selected_augments"),QStringLiteral("augments")});
    add(QStringLiteral("心钢成长记录"),{QStringLiteral("heartsteel_stacks"),QStringLiteral("heartsteel_permanent_health"),QStringLiteral("item:3084:permanent_hp")});
    add(QStringLiteral("荣耀层数"),{QStringLiteral("glory_stacks")});
    add(QStringLiteral("海克斯任务进度"),{QStringLiteral("augment_quest_progress"),QStringLiteral("quest_progress")});
    grid->setColumnStretch(0,1);grid->setColumnStretch(1,1);content->addWidget(panel);
    content->addWidget(copy(QStringLiteral("未提供的数据保持未知，不从总生命值反推心钢层数。手动记录会单独标明，不能视为自动实测；事件推导依赖完整记录。")));content->addStretch(1);scroll->setWidget(body);layout->addWidget(scroll,1);
    auto actions=new QHBoxLayout;actions->addStretch(1);auto close=new Kda::Button(QStringLiteral("完成"));close->setObjectName(QStringLiteral("closeGrowthDataSources"));close->setDefault(true);actions->addWidget(close);layout->addLayout(actions);QObject::connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);
    if(dialog.exec()!=QDialog::Accepted)return {};visibleSettings["enabled"]=visibleToggle->isChecked();return visibleSettings==originalVisibleSettings?QJsonObject{}:visibleSettings;
}
