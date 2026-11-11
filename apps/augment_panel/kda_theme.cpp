#include "kda_theme.h"
#include "overlay_backdrop.h"
#include "kda_tooltip_style.h"
#include "kda_theme_binding.h"
#include <QApplication>
#include <QCache>
#include <QDynamicPropertyChangeEvent>
#include <QEasingCurve>
#include <QFontDatabase>
#include <QFile>
#include <QJsonDocument>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QDataStream>
#include <QPixmapCache>
#include <QRegion>
#include <QResource>
#include <QWidget>
#include <QRegularExpression>
#include <QJsonArray>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <optional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static void initializeKdaResources(){static const bool initialized=[] {Q_INIT_RESOURCE(kda_assets);return true;}();Q_UNUSED(initialized);}
namespace {
bool fontsReady(){
    static const bool ready=[] {
        initializeKdaResources();
        const auto chinese=QFontDatabase::addApplicationFont(QStringLiteral(":/lixin/kda/fonts/NotoSansSC-Variable.ttf"));
        const auto latin=QFontDatabase::addApplicationFont(QStringLiteral(":/lixin/kda/fonts/Inter-Variable.ttf"));
        return chinese>=0&&latin>=0;
    }();
    return ready;
}
const QJsonObject &themeData(){
    static const auto data=[] {initializeKdaResources();QFile file(QStringLiteral(":/lixin/kda/theme.json"));if(!file.open(QIODevice::ReadOnly))return QJsonObject{};return QJsonDocument::fromJson(file.readAll()).object();}();return data;
}
bool systemMotionReduced(){
#ifdef Q_OS_WIN
    BOOL enabled=TRUE;
    if(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0))return !enabled;
#endif
    return false;
}

std::optional<QRegion> coverageEdges(const QImage &coverage){
    constexpr int tileSize=16;
    const auto ratio=coverage.devicePixelRatio();
    const qint64 maxDirtyPixels=qint64(coverage.width())*coverage.height()/2;
    qint64 dirtyPixels=0;
    QRegion edges;
    for(int top=0;top<coverage.height();top+=tileSize){
        const int bottom=std::min(top+tileSize,coverage.height());
        for(int left=0;left<coverage.width();left+=tileSize){
            const int right=std::min(left+tileSize,coverage.width());
            bool affected=false;
            for(int y=top;y<bottom&&!affected;++y){
                const auto *row=reinterpret_cast<const QRgb*>(coverage.constScanLine(y));
                affected=std::any_of(row+left,row+right,[](QRgb pixel){return qAlpha(pixel)!=255;});
            }
            if(!affected)continue;
            dirtyPixels+=qint64(right-left)*(bottom-top);
            if(dirtyPixels>maxDirtyPixels)return std::nullopt;
            // Expand by a physical pixel before rounding outward into the
            // painter's logical coordinates, including at fractional DPR.
            const int x=qFloor((left-1)/ratio),y=qFloor((top-1)/ratio);
            edges|=QRect(x,y,qCeil((right+1)/ratio)-x,qCeil((bottom+1)/ratio)-y);
        }
    }
    return edges;
}

struct MaterialCoverageMask {
    QPixmap pixmap;
    std::optional<QRegion> edges;

    void apply(QImage &layer)const{
        if(edges&&edges->isEmpty())return;
        QPainter painter(&layer);
        if(edges)painter.setClipRegion(*edges);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.drawPixmap(QPointF(),pixmap);
    }
};
}
namespace Kda {
void configureApplicationRendering(){
    // The Windows Qt 6.8 native path can retain LCD fringes despite the
    // NoSubpixelAntialias request. The bundled FreeType path honours grayscale
    // coverage on both opaque and translucent surfaces. Preserve explicit
    // caller choices (including offscreen diagnostics).
#ifdef Q_OS_WIN
    if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM","windows:fontengine=freetype");
#endif
}
QFont font(FontRole role,int pixels,QFont::Weight weight){
    fontsReady();QFont result;
    result.setFamilies({QStringLiteral("Inter"),QStringLiteral("Noto Sans SC"),QStringLiteral("Segoe UI")});
    // Both bundled families provide real variable weights. Respect each role's
    // requested weight instead of turning every Chinese label into bold text.
    result.setPixelSize(pixels);result.setWeight(weight);
    // Align vertical stems without forcing horizontal shapes onto the pixel
    // grid. Grayscale coverage stays consistent over changing UI materials.
    result.setHintingPreference(pixels>=18?QFont::PreferNoHinting:QFont::PreferVerticalHinting);
    result.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias|QFont::NoSubpixelAntialias));
    if(role==FontRole::Label)result.setLetterSpacing(QFont::AbsoluteSpacing,.1);
    if(role==FontRole::Display)result.setLetterSpacing(QFont::AbsoluteSpacing,.5);
    return result;
}
void configurePainter(QPainter &painter){
    painter.setRenderHints(QPainter::Antialiasing|QPainter::TextAntialiasing|QPainter::SmoothPixmapTransform);
}
void applyMaterialCoverage(QImage &layer,const QPainterPath &localShape){
    if(layer.isNull()||localShape.isEmpty())return;
    const qreal ratio=layer.devicePixelRatio();
    QByteArray key;
    QDataStream stream(&key,QIODevice::WriteOnly);
    stream<<layer.size()<<ratio<<localShape;
    // Material paint runs on the GUI thread. Give its masks a fixed budget
    // independent of the global pixmap cache used by other artwork.
    static QCache<QByteArray,MaterialCoverageMask> cache(8*1024*1024);
    if(const auto *cached=cache.object(key)){
        cached->apply(layer);
        return;
    }
    QImage coverage(layer.size(),QImage::Format_ARGB32_Premultiplied);
    coverage.setDevicePixelRatio(ratio);
    coverage.fill(Qt::transparent);
    {QPainter painter(&coverage);configurePainter(painter);painter.fillPath(localShape,Qt::white);}
    const auto bytes=coverage.sizeInBytes();
    const bool cacheable=bytes<=cache.maxCost();
    auto edges=cacheable?coverageEdges(coverage):std::nullopt;
    MaterialCoverageMask mask{QPixmap::fromImage(std::move(coverage)),std::move(edges)};
    mask.apply(layer);
    if(cacheable)cache.insert(key,new MaterialCoverageMask(std::move(mask)),bytes);
}
QPixmap roundedPixmap(const QPixmap &source,const QSize &logicalSize,qreal radius,qreal dpr){
    if(source.isNull()||logicalSize.isEmpty())return {};
    if(!std::isfinite(dpr)||dpr<=0)dpr=1.;
    radius=std::clamp(radius,qreal(0),qreal(std::min(logicalSize.width(),logicalSize.height()))/2.);
    const auto key=QStringLiteral("lixin-kda-rounded:%1:%2:%3:%4:%5")
        .arg(source.cacheKey()).arg(logicalSize.width()).arg(logicalSize.height())
        .arg(radius,0,'g',12).arg(dpr,0,'g',12);
    QPixmap cached;if(QPixmapCache::find(key,&cached))return cached;
    const QSize physical(std::max(1,qRound(logicalSize.width()*dpr)),std::max(1,qRound(logicalSize.height()*dpr)));
    // Resize the texture once. Only the small corner masks are supersampled:
    // this improves subpixel coverage without softening the avatar's interior.
    const auto scaled=source.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied)
        .scaled(physical,Qt::KeepAspectRatioByExpanding,Qt::SmoothTransformation);
    auto image=scaled.copy((scaled.width()-physical.width())/2,(scaled.height()-physical.height())/2,physical.width(),physical.height());
    image.setDevicePixelRatio(1.);
    QImage mask(physical,QImage::Format_ARGB32_Premultiplied);mask.fill(Qt::white);
    const qreal physicalRadius=std::min(radius*dpr,qreal(std::min(physical.width(),physical.height()))/2.);
    if(physicalRadius>0){
        constexpr int samples=16;
        const int side=qCeil(physicalRadius);
        QImage corner(QSize(side,side)*samples,QImage::Format_ARGB32_Premultiplied);corner.fill(Qt::transparent);
        {QPainter painter(&corner);configurePainter(painter);painter.setPen(Qt::NoPen);painter.setBrush(Qt::white);
            painter.drawRoundedRect(QRectF(0,0,physical.width()*samples,physical.height()*samples),physicalRadius*samples,physicalRadius*samples);}
        corner=corner.scaled(side,side,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
        for(int y=0;y<side;++y)for(int x=0;x<side;++x){
            const auto coverage=corner.pixel(x,y);
            mask.setPixel(x,y,coverage);mask.setPixel(physical.width()-1-x,y,coverage);
            mask.setPixel(x,physical.height()-1-y,coverage);mask.setPixel(physical.width()-1-x,physical.height()-1-y,coverage);
        }
    }
    {QPainter painter(&image);painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);painter.drawImage(0,0,mask);}
    image.setDevicePixelRatio(dpr);cached=QPixmap::fromImage(image);QPixmapCache::insert(key,cached);return cached;
}
QString normalizedSkin(const QString &id){return id==QStringLiteral("kda_red")?id:QStringLiteral("kda");}
QString skin(){return normalizedSkin(qApp?qApp->property("lixinSkin").toString():QString());}
QColor skinColor(const QString &id,const char *token){
    const auto selected=normalizedSkin(id);
    if(selected==QStringLiteral("kda_red")){
        struct RedToken{const char *token;const char *hex;};
        static constexpr RedToken red[]={{"bg","#0d0b0f"},{"surface","#171419"},{"raised","#241c23"},{"border","#513640"},{"text","#f9eded"},{"muted","#bfa9ae"},{"silver","#f3ddd8"},{"ice","#ff6474"},{"lilac","#dc3d66"}};
        for(const auto &entry:red)if(std::strcmp(token,entry.token)==0)return QColor(QString::fromLatin1(entry.hex));
    }
    const QColor configured(themeData().value("palette").toObject().value(QString::fromLatin1(token)).toString());
    if(configured.isValid())return configured;
    struct Entry{const char *name;const char *hex;};
    static constexpr Entry entries[]={{"bg","#0b0b10"},{"surface","#15141c"},{"raised","#24222d"},{"border","#3c3748"},{"text","#f0edf5"},{"muted","#a09aaa"},{"silver","#ddd8e8"},{"ice","#b5e1ed"},{"lilac","#c9b0ed"},{"ready","#71bfa8"},{"danger","#d79aad"}};
    for(const auto &entry:entries)if(std::strcmp(token,entry.name)==0)return QColor(QString::fromLatin1(entry.hex));
    return QColor(QStringLiteral("#f0edf5"));
}
QColor color(const char *token){return skinColor(skin(),token);}
bool depthEnabled(){return !qApp||!qApp->property("lixinDepthEnabled").isValid()||qApp->property("lixinDepthEnabled").toBool();}
void setDepthEnabled(bool value){
    if(!qApp)return;
    const auto current=qApp->property("lixinDepthEnabled");
    if(current.isValid()&&current.toBool()==value)return;
    qApp->setProperty("lixinDepthEnabled",value);
    for(auto widget:QApplication::allWidgets())widget->update();
}
bool highContrastEnabled(){
#ifdef Q_OS_WIN
    static const bool enabled=[] {HIGHCONTRASTW state{};state.cbSize=sizeof(state);return SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(state),&state,0)&&bool(state.dwFlags&HCF_HIGHCONTRASTON);}();
    return enabled;
#else
    return false;
#endif
}
bool glassEnabled(){return !qApp||!qApp->property("lixinGlassEnabled").isValid()||qApp->property("lixinGlassEnabled").toBool();}
void setGlassEnabled(bool value){if(qApp&&glassEnabled()!=value){qApp->setProperty("lixinGlassEnabled",value);if(auto registry=Detail::themeStyleRegistry())registry->refresh();for(auto widget:QApplication::allWidgets())widget->update();emit overlayAppearanceEvents()->changed();}}
QString themedStyleSheet(QString sheet){
    if(skin()==QStringLiteral("kda"))return sheet;
    // Adapt the existing owner stylesheet, keeping specificity and geometry.
    static const QRegularExpression hex(QStringLiteral("#[0-9a-fA-F]{6}"));
    QString result;qsizetype last=0;auto matches=hex.globalMatch(sheet);
    while(matches.hasNext()){
        const auto match=matches.next();const auto value=match.captured().toLower();const char *token=nullptr;
        if(QStringList{"#09090b","#0b0b10","#101218","#101017"}.contains(value))token="bg";
        else if(QStringList{"#18181b","#191c24","#15141c","#15151c","#191920"}.contains(value))token="surface";
        else if(QStringList{"#222225","#24262e","#26222d","#25252e","#2b2934","#23232c"}.contains(value))token="raised";
        else if(QStringList{"#373740","#39333f","#3d424f","#3a414f","#30343e"}.contains(value))token="border";
        else if(QStringList{"#aaabb7","#9298a5","#a09aaa","#9da3af","#a09aa9"}.contains(value))token="muted";
        // This blue belongs to team labels, rather than the skin accent.
        else if(value==QStringLiteral("#acc3d3")&&skin()==QStringLiteral("kda_red"))token=nullptr;
        else if(QStringList{"#bedee9","#accdd9","#acc3d3"}.contains(value))token="ice";
        else if(QStringList{"#dce0eb","#d0d8e4","#e4deed"}.contains(value))token="silver";
        else if(QStringList{"#edeef4","#e7e9ef","#eceef3","#f0eff6"}.contains(value))token="text";
        result+=sheet.mid(last,match.capturedStart()-last);result+=token?color(token).name():match.captured();last=match.capturedEnd();
    }
    return result+sheet.mid(last);
}
void setSkin(const QString &id){
    if(!qApp)return;installToolTipStyle();const auto next=normalizedSkin(id);if(qApp->property("lixinSkin").toString()==next)return;
    auto registry=Detail::themeStyleRegistry();qApp->setProperty("lixinSkin",next);registry->refresh();
    for(auto widget:QApplication::allWidgets())widget->update();
}
void bindThemeStyle(QWidget *owner,std::function<QString()> builder){if(auto registry=Detail::themeStyleRegistry())registry->bind(owner,std::move(builder));}
void installToolTipStyle(){Detail::installToolTipStyle();}
qreal radius(const char *token){
    const auto configured=themeData().value("shape").toObject().value(QString::fromLatin1(token));
    if(configured.isDouble()&&std::isfinite(configured.toDouble())&&configured.toDouble()>=0&&configured.toDouble()<=32)return configured.toDouble();
    struct Entry{const char *name;qreal value;};
    static constexpr Entry entries[]={{"window",8},{"workspace",4},{"gap",4},{"section",4},{"control",4},{"navigation_group",4},{"navigation",2},{"artwork",3},{"hud",4},{"card_outer",4},{"card_inner",1},{"tag",2},{"small",1}};
    for(const auto &entry:entries)if(std::strcmp(token,entry.name)==0)return entry.value;
    return 4;
}
bool reducedMotion(){
    return qEnvironmentVariableIntValue("LIXIN_REDUCED_MOTION")==1||systemMotionReduced()||(qApp&&qApp->property("lixinReducedMotion").toBool());
}
void setReducedMotion(bool value){if(qApp)qApp->setProperty("lixinReducedMotion",value);}
QJsonObject themeReport(){
    const auto bindingReport=Detail::themeStyleInstance()?Detail::themeStyleInstance()->report():QJsonObject{{"installed",false}};
    return {{"overlay_appearance",overlayAppearanceReport()},{"glass_material",QJsonObject{{"enabled",glassEnabled()},{"text_blurred",false},{"titlebar_blurred",false},{"cached_bitmap_materials",true}}},{"theme_style_binding",bindingReport},{"skin",skin()},{"available_skins",QJsonArray{"kda","kda_red"}},{"depth_enabled",depthEnabled()},{"id",skin()=="kda_red"?"kda_firestarter_lacquer":"kda_all_out_editorial"},{"version",12},{"composition_layers",QJsonArray{"backdrop","midground","foreground"}},{"interactive_layer","foreground"},{"material",skin()=="kda_red"?"firestarter_chrome_veil_embers":"more_platinum_foil"},{"material_asset",skin()=="kda_red"?":/lixin/kda/textures/firestarter-key-art.png":":/lixin/kda/textures/more-foil.png"},{"bundled_font","Inter + Noto Sans SC"},{"fonts_loaded",fontsReady()},{"body_font",font(FontRole::Body,13).family()},{"chinese_font","Noto Sans SC"},{"latin_font","Inter"},{"body_weight",int(font(FontRole::Body,13).weight())},{"numeric_font",font(FontRole::Numeric,24).family()},{"font_license","SIL OFL 1.1"},{"text_antialiasing","grayscale_requested_vertical_hinting"},{"font_hinting","vertical_small_text_unhinted_display"},{"hud_text_antialiasing","grayscale_supersampled_2x_vertical_hinting"},{"font_backend_policy","windows_freetype_grayscale_default_or_explicit_configuration"},{"platform_configuration",QString::fromLocal8Bit(qgetenv("QT_QPA_PLATFORM"))},{"artwork_antialiasing","physical_resolution_alpha_mask"},{"reduced_motion",reducedMotion()},{"shape",themeData().value("shape")},{"motion_looping",!reducedMotion()},{"ambient_particles",QStringLiteral("visible_hud_and_active_dashboard")},{"background_asset",skin()==QStringLiteral("kda_red")?":/lixin/kda/textures/home-fire-mirror-v2.png":":/lixin/kda/textures/home-platinum-stage-v1.jpg"},{"midground_asset",skin()==QStringLiteral("kda_red")?":/lixin/kda/textures/home-fire-ribbon-v2.png":":/lixin/kda/textures/home-silver-ribbon-v2.png"},{"firestarter_flames",skin()==QStringLiteral("kda_red")},{"firestarter_emission",skin()==QStringLiteral("kda_red")?"red_orange_flame_warm_core_rising_embers":"not_active"},{"tooltips",QJsonObject{{"installed",qApp&&qApp->property("kdaToolTipStyleInstalled").toBool()},{"native",true},{"material",skin()=="kda_red"?"fire_lacquer_warm_rim":"silver_smoke_crystal_rim"},{"font_pixels",13},{"preferred_width",360},{"radius",7},{"word_wrap",true},{"source_text_preserved",true},{"mouse_transparent",true},{"looping_timer",false}}},{"data_updates_wait_for_animation",false}};
}
QString overlayStyleSheet(){
    installToolTipStyle();return themedStyleSheet(QStringLiteral(R"(
      QWidget{font-family:'Inter','Noto Sans SC','Segoe UI';font-size:13px;font-weight:400;color:#edeef4;}
      QFrame#shell{background:transparent;border:0;border-radius:%1px;}
      QWidget#titlebar{background:transparent;border:0;}
      QLabel#brand{font-family:'Inter','Noto Sans SC';font-style:normal;font-size:16px;font-weight:500;color:#ececf4;background:transparent;}
      QWidget#panelbody{background:transparent;border:0;}
      QLabel{background:transparent;border:0;}
      QLabel#muted,QLabel#source{color:#aaabb7;font-size:12px;}
      QPushButton{background:#25252e;border:1px solid #373740;border-radius:%2px;color:#dce0eb;padding:2px 4px;}
      QPushButton:hover{background:#30303a;border-color:#898394;}
      QPushButton:pressed{background:#18181f;}
      QWidget#titlebar QPushButton{background:transparent;border:0;font-size:15px;padding:1px;}
      QWidget#titlebar QPushButton:hover{background:#30303a;}
      QTabWidget::pane{border:0;background:transparent;}
      QTabBar::tab{background:transparent;color:#aaabb7;padding:4px 6px;border:0;border-bottom:2px solid transparent;}
      QTabBar::tab:selected{color:#f0eff6;background:#2b2934;border-bottom-color:#bedee9;}
      QFrame#card{background:#15151c;border:1px solid #373740;border-radius:%3px;}
      QWidget#hudFooter{background:transparent;border:0;}
      QPushButton#detailsToggle{background:transparent;border:0;border-radius:0;color:#aaabb7;font-size:12px;padding:0;}
      QPushButton#detailsToggle:hover{color:#edeef4;}
      QLineEdit,QComboBox{background:#101017;border:1px solid #373740;border-radius:%2px;padding:3px 5px;selection-background-color:#514a63;}
      QLineEdit:focus,QComboBox:focus{border-color:#bedee9;}
      QComboBox::drop-down{border:0;width:22px;background:transparent;}QComboBox::down-arrow{image:none;width:10px;height:7px;}
      QComboBox QAbstractItemView{background:#191920;color:#edeef4;selection-background-color:#514a63;}
      QScrollArea,QScrollArea>QWidget>QWidget{background:transparent;border:0;}
      QScrollBar:vertical{width:5px;background:transparent;margin:3px 0;}
      QScrollBar::handle:vertical{background:#5f5c6a;min-height:20px;border-radius:%4px;}
      QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}
      QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}
      QDialog{background:#14141b;color:#edeef4;}
      QTreeWidget,QTextBrowser,QPlainTextEdit{background:#111118;color:#e7e6ef;border:1px solid #373740;selection-background-color:#35313f;}
      QTreeWidget::item{padding:4px 3px;border-bottom:1px solid #2d2c35;}
      QHeaderView::section{background:#23232c;color:#d2d0e0;border:0;border-bottom:1px solid #373740;padding:5px;}
      QMenu{background:#191920;color:#ececf4;border:1px solid #484550;padding:5px;}
      QMenu::item:selected{background:#35313f;}
    )").arg(radius("hud")).arg(radius("tag")).arg(radius("control")).arg(radius("small")))
    // Popup and reading states use semantic roles directly. Keeping these out
    // of the legacy literal-color adapter also covers future palette changes.
    +QStringLiteral("QDialog{background:%1;color:%2;}"
        "QTreeWidget,QTextBrowser,QPlainTextEdit{background:%3;color:%2;border-color:%4;selection-background-color:%5;}"
        "QTreeWidget::item{border-bottom-color:%4;}QHeaderView::section{background:%5;color:%6;border-bottom-color:%4;}"
        "QMenu{background:%1;color:%2;border-color:%4;}QMenu::item:selected{background:%5;color:%2;}"
        "QPushButton:hover{background:%5;border-color:%7;}QPushButton:pressed{background:%3;}"
        "QLineEdit,QComboBox{selection-background-color:%5;}QComboBox QAbstractItemView{background:%1;color:%2;selection-background-color:%5;}")
        .arg(color("surface").name(),color("text").name(),color("bg").name(),color("border").name(),color("raised").name(),color("silver").name(),color("muted").name())
    +Detail::toolTipSheet();
}
Transition::Transition(QWidget *widget,int durationMs):QObject(widget),owner(widget){
    animation.setDuration(std::clamp(durationMs,80,800));animation.setStartValue(0.);animation.setEndValue(1.);animation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&animation,&QVariantAnimation::valueChanged,this,[this](const QVariant &value){phase=value.toReal();++frames;if(owner&&owner->isVisible())owner->update();});
    connect(&animation,&QVariantAnimation::finished,this,[this]{phase=1.;if(owner)owner->update();});
    if(owner)owner->installEventFilter(this);if(qApp)qApp->installEventFilter(this);
}
void Transition::trigger(){
    if(!owner||!owner->isVisible()||reducedMotion()){finish();return;}
    if(running())return;
    ++starts;phase=0.;animation.start();owner->update();
}
void Transition::finish(){animation.stop();phase=1.;if(owner)owner->update();}
qreal Transition::progress()const{return phase;}
bool Transition::running()const{return animation.state()==QAbstractAnimation::Running;}
QJsonObject Transition::report()const{return {{"running",running()},{"duration_ms",animation.duration()},{"easing","OutCubic"},{"progress",phase},{"starts",starts},{"frames",frames},{"reduced_motion",reducedMotion()},{"loops",1},{"geometry_animated",false},{"data_animated",false}};}
bool Transition::eventFilter(QObject *object,QEvent *event){
    if(object==owner&&(event->type()==QEvent::Hide||event->type()==QEvent::Close))finish();
    if(object==qApp&&event->type()==QEvent::DynamicPropertyChange){const auto change=static_cast<QDynamicPropertyChangeEvent*>(event);if(change->propertyName()=="lixinReducedMotion"&&reducedMotion())finish();}
    return QObject::eventFilter(object,event);
}
}
