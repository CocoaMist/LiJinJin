#pragma once
#include <QWidget>
#include <QSettings>
#include <QLabel>
#include <QJsonObject>
#include <QVariantAnimation>
#include <functional>

class QVBoxLayout;
class OverlayWindow : public QWidget {
public:
    OverlayWindow(const QString &title,const QString &key,QSettings &,bool inspection);
    QWidget *body=nullptr;
    QLabel *source=nullptr;
    std::function<void()> onClose,onReset,onOpenDashboard;
    std::function<void(int)> onHotkey;
    std::function<void(bool)> onThrough;
    bool collapsed() const {return folded;}
    void setCollapsed(bool);
    void setContentHeight(int);
    void setHeading(const QString &);
    void setSafeLane(const QRect &,bool initial=false,bool bottomAligned=false);
    void setCustomLayout(const QRect &);
    void setThrough(bool);
    void setInteractive(bool);
    void setPresentedVisible(bool visible,bool animated=true);
    void setWindowOpacity(qreal opacity);
    qreal configuredOpacity() const {return baseOpacity;}
    void setHotkeyAvailable(bool value){hotkeyAvailable=value;}
    QJsonObject report() const;
protected:
    void closeEvent(QCloseEvent *) override;
    void moveEvent(QMoveEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *,QEvent *) override;
    bool nativeEvent(const QByteArray &,void *,qintptr *) override;
private:
    QSettings &settings;QString prefix,headingText;QRect lane;int expandedHeight=164;
    QLabel *heading=nullptr;
    bool folded=false,through=false,constraining=false,hotkeyAvailable=false,bottomAnchored=false;
    bool inspectionMode=false,interactive=false;
    QVariantAnimation visibilityMotion;
    qreal baseOpacity=.88,reveal=1.;
    bool visibilityRequested=false,applyingVisibility=false,rebuildingFlags=false;
    int visibilityFrames=0;
    void applyVisibilityOpacity();
    void finishVisibility();
    void menu(QWidget *);
    void constrain();
};
