#pragma once
#include <QJsonObject>
#include <QVariantAnimation>
#include <QTimer>
#include <QWidget>
#include <QPixmap>
#include <functional>
#include <utility>
namespace Kda {class SparkleField;}

// Display only. The caller owns death/focus/freshness eligibility and tip evidence.
class DeathTipsWindow final : public QWidget {
public:
    explicit DeathTipsWindow();
    void present(const QRect &viewport,const QJsonObject &data,double opacity=1.0);
    void dismiss();
    QJsonObject report() const;
    void setIconResolver(std::function<QPixmap(const QString &,int)> resolver){iconResolver=std::move(resolver);update();}
protected:
    void paintEvent(QPaintEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *,QEvent *) override;
private:
    void begin(const QString &phase);
    bool motionEnabled() const;
    void stopDecorations();
    QJsonObject content;
    QVariantAnimation animation;
    QTimer decorationTimer;
    Kda::SparkleField *sparkles=nullptr;
    QString phase=QStringLiteral("hidden");
    qreal progress=1.;
    int transitions=0,frames=0,decorationStarts=0;
    std::function<QPixmap(const QString &,int)> iconResolver;
};
