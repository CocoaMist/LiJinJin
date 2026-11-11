#pragma once
#include <QColor>
#include <QFont>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QPixmap>
#include <QVariantAnimation>
#include <functional>

class QWidget;
class QPainter;
class QImage;
class QPainterPath;

namespace Kda {
enum class FontRole { Body, Label, Title, Numeric, Display };
// Logical pixels: reduce container overhead without reducing readable type.
namespace Density {
inline constexpr int controlHeight=26;
inline constexpr int switchHeight=28;
inline constexpr int sectionGap=8;
inline constexpr int pageInset=12;
}
// Call before QApplication; preserve Qt's default or explicitly selected backend.
void configureApplicationRendering();
QFont font(FontRole role,int pixels,QFont::Weight weight=QFont::Normal);
void configurePainter(QPainter &painter);
// Preserve fractional edge coverage when compositing bitmap materials.
// QPainter's path clip itself is binary even with antialiasing enabled.
void applyMaterialCoverage(QImage &layer,const QPainterPath &localShape);
QPixmap roundedPixmap(const QPixmap &source,const QSize &logicalSize,qreal radius,qreal dpr);
QColor color(const char *token);
QString skin();
QString normalizedSkin(const QString &id);
QColor skinColor(const QString &id,const char *token);
void setSkin(const QString &id);
// Complete owner stylesheet; hidden owners update immediately before display.
void bindThemeStyle(QWidget *owner,std::function<QString()> builder);
// Shared native QToolTip material, wrapping and screen placement; no text rewrite.
void installToolTipStyle();
QString themedStyleSheet(QString sheet);
bool depthEnabled();
void setDepthEnabled(bool value);
bool highContrastEnabled();
bool glassEnabled();
void setGlassEnabled(bool value);
qreal radius(const char *token);
bool reducedMotion();
void setReducedMotion(bool value);
QJsonObject themeReport();
QString overlayStyleSheet();

// Paint-only event transition: data and hit targets never wait for animation.
// Qt drives frames only while a visible owner has a finite transition running.
class Transition final : public QObject {
public:
    explicit Transition(QWidget *owner,int durationMs=220);
    void trigger();
    void finish();
    qreal progress() const;
    bool running() const;
    QJsonObject report() const;
protected:
    bool eventFilter(QObject *,QEvent *) override;
private:
    QPointer<QWidget> owner;
    QVariantAnimation animation;
    int starts=0,frames=0;
    qreal phase=1.;
};
}
