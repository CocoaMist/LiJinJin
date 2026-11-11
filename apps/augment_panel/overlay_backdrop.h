#pragma once
#include <QJsonObject>
#include <QObject>
#include <QtGlobal>
class QWidget;
class QSettings;
namespace Kda {
class OverlayAppearanceEvents final:public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    void changed();
};
OverlayAppearanceEvents *overlayAppearanceEvents();
bool overlayBlurEnabled();
void setOverlayBlurEnabled(bool);
qreal overlayBackdropOpacity();
void setOverlayBackdropOpacity(qreal);
int overlaySurfaceAlpha(int original=255);
void configureOverlayAppearance(QSettings &,bool writable=true);
void attachOverlayBackdrop(QWidget *,qreal radius=5.);
QJsonObject overlayAppearanceReport();
}
