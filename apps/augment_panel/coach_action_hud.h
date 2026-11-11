#pragma once
#include <QWidget>
#include <QJsonObject>
class HudAssets;
namespace Kda { class Transition; }

// A compact broadcast-style information strip. It renders the current local
// decision only; all data and automatic triggering stay in DualPanels.
class CoachActionHud : public QWidget {
public:
    explicit CoachActionHud(QWidget *parent=nullptr);
    void setAction(const QString &caption,const QString &title,const QString &status,bool buyable=false);
    void setItem(HudAssets *assets,int itemId);
    void setChampion(HudAssets *assets,int championId);
    bool purchaseTextFits(const QString &title,const QString &status) const;
    QString text() const;
    QJsonObject report() const;
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QString captionText,titleText,statusText;
    bool purchaseReady=false;
    HudAssets *assets=nullptr;
    int itemId=0;
    int championId=0;
    Kda::Transition *motion=nullptr;
};
