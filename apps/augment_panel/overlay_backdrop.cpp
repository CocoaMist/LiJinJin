#include "overlay_backdrop.h"
#include "kda_theme.h"
#include "native_desktop_blur.h"
#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QPointer>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QDir>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace Kda {
OverlayAppearanceEvents *overlayAppearanceEvents(){static QPointer<OverlayAppearanceEvents> events;if(!events)events=new OverlayAppearanceEvents(qApp);return events;}
bool overlayBlurEnabled(){return !qApp||!qApp->property("lixinOverlayBlurEnabled").isValid()||qApp->property("lixinOverlayBlurEnabled").toBool();}
qreal overlayBackdropOpacity(){const auto v=qApp?qApp->property("lixinOverlayBackdropOpacity"):QVariant();const auto n=v.isValid()?v.toDouble():.58;return std::isfinite(n)?std::clamp(n,.22,.88):.58;}
void setOverlayBlurEnabled(bool value){if(qApp&&overlayBlurEnabled()!=value){qApp->setProperty("lixinOverlayBlurEnabled",value);for(auto w:QApplication::allWidgets())w->update();emit overlayAppearanceEvents()->changed();}}
void setOverlayBackdropOpacity(qreal value){value=std::isfinite(value)?std::clamp(value,.22,.88):.58;if(qApp&&std::abs(overlayBackdropOpacity()-value)>.0001){qApp->setProperty("lixinOverlayBackdropOpacity",value);for(auto w:QApplication::allWidgets())w->update();emit overlayAppearanceEvents()->changed();}}
int overlaySurfaceAlpha(int original){return !glassEnabled()||highContrastEnabled()?original:qRound(std::clamp(original*overlayBackdropOpacity(),0.,255.));}
namespace {
QJsonObject values(){return {{"schema",1},{"blur",overlayBlurEnabled()},{"opacity",overlayBackdropOpacity()},{"glass",glassEnabled()}};}
class Appearance final:public QObject {
public:
    explicit Appearance():QObject(qApp){save.setSingleShot(true);save.setInterval(100);connect(overlayAppearanceEvents(),&OverlayAppearanceEvents::changed,this,[this]{if(!loading)save.start();});connect(&save,&QTimer::timeout,this,[this]{write();});connect(&watch,&QFileSystemWatcher::directoryChanged,this,[this]{read();});connect(qApp,&QCoreApplication::aboutToQuit,this,[this]{save.stop();write();});}
    void configure(QSettings &settings,bool writable){const auto path=QFileInfo(settings.fileName()).absolutePath()+QStringLiteral("/overlay-appearance.json");if(file==path&&store==&settings&&canWrite==writable)return;save.stop();lastWritten.clear();store=&settings;canWrite=writable;file=path;watch.removePaths(watch.directories());if(writable){QDir().mkpath(QFileInfo(file).absolutePath());watch.addPath(QFileInfo(file).absolutePath());}loading=true;setOverlayBlurEnabled(settings.value("appearance/overlayBlur",true).toBool());setOverlayBackdropOpacity(settings.value("appearance/overlayBackdropOpacity",.58).toDouble());loading=false;if(QFile::exists(file))read();else if(canWrite)write();}
    void read(){QFile source(file);if(!source.open(QIODevice::ReadOnly)||source.size()>4096)return;const auto bytes=source.readAll();if(bytes==lastWritten)return;const auto data=QJsonDocument::fromJson(bytes).object();if(data.value("schema").toInt()!=1||!data.value("blur").isBool()||!data.value("opacity").isDouble()||!data.value("glass").isBool())return;const double opacity=data.value("opacity").toDouble();if(!std::isfinite(opacity)||opacity<.22||opacity>.88)return;loading=true;setOverlayBlurEnabled(data.value("blur").toBool());setOverlayBackdropOpacity(opacity);setGlassEnabled(data.value("glass").toBool());if(canWrite&&store){store->setValue("appearance/overlayBlur",overlayBlurEnabled());store->setValue("appearance/overlayBackdropOpacity",overlayBackdropOpacity());store->setValue("appearance/frostedGlass",glassEnabled());}loading=false;}
    void write(){if(!canWrite||file.isEmpty())return;const auto bytes=QJsonDocument(values()).toJson(QJsonDocument::Compact);QFile old(file);const bool unchanged=old.open(QIODevice::ReadOnly)&&old.readAll()==bytes;old.close();if(unchanged){lastWritten=bytes;return;}QSaveFile output(file);if(output.open(QIODevice::WriteOnly)){output.write(bytes);if(output.commit())lastWritten=bytes;}}
private:QPointer<QSettings> store;QString file;QByteArray lastWritten;QFileSystemWatcher watch;QTimer save;bool loading=false,canWrite=false;
};
Appearance *appearance(){static QPointer<Appearance> value;if(!value)value=new Appearance;return value;}
class Backdrop final:public QObject {
public:
    Backdrop(QWidget *owner,qreal radius):QObject(owner),widget(owner),radius(radius){owner->installEventFilter(this);timer.setSingleShot(true);timer.setInterval(50);connect(&timer,&QTimer::timeout,this,[this]{apply();});connect(overlayAppearanceEvents(),&OverlayAppearanceEvents::changed,this,[this]{timer.start();});if(owner->isVisible())timer.start();}
protected:
    bool eventFilter(QObject *object,QEvent *event)override{if(object==widget){if(event->type()==QEvent::Show){timer.stop();apply();}else if(event->type()==QEvent::Resize||event->type()==QEvent::WinIdChange||event->type()==QEvent::ThemeChange)timer.start();}return false;}
private:
    void apply(){if(!widget||!widget->isVisible())return;bool accepted=false;
#ifdef Q_OS_WIN
        if(QGuiApplication::platformName()!=QStringLiteral("windows"))return;
        const auto id=widget->internalWinId();const auto size=widget->size();const auto scale=widget->devicePixelRatioF();const bool enabled=overlayBlurEnabled()&&glassEnabled()&&!highContrastEnabled();
        // Setting the native clip can send another resize notification. Apply
        // each geometry once so an idle HUD never starts a repaint loop.
        if(applied&&id==lastId&&size==lastSize&&scale==lastScale&&enabled==lastEnabled)return;
        applied=true;lastId=id;lastSize=size;lastScale=scale;lastEnabled=enabled;
        const auto result=DesktopBlur::apply(reinterpret_cast<HWND>(id),enabled,qRound(radius*scale));accepted=result.accepted;
#else
        return;
#endif
        if(widget->property("overlayBackdropAccepted").toBool()!=accepted){widget->setProperty("overlayBackdropAccepted",accepted);widget->update();}
    }
    QPointer<QWidget> widget;qreal radius;QTimer timer;bool applied=false,lastEnabled=false;WId lastId=0;QSize lastSize;qreal lastScale=0;
};
}
void configureOverlayAppearance(QSettings &settings,bool writable){appearance()->configure(settings,writable);}
void attachOverlayBackdrop(QWidget *widget,qreal radius){if(widget&&!widget->property("overlayBackdropAttached").toBool()){widget->setProperty("overlayBackdropAttached",true);new Backdrop(widget,radius);}}
QJsonObject overlayAppearanceReport(){auto result=values();int attached=0,accepted=0;for(auto widget:QApplication::topLevelWidgets()){attached+=widget->property("overlayBackdropAttached").toBool();accepted+=widget->property("overlayBackdropAccepted").toBool();}result["attached_windows"]=attached;result["native_api_accepted_windows"]=accepted;result["visual_blur_verified"]=false;result["text_blurred"]=false;return result;}
}
