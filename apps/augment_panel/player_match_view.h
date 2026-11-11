#pragma once
#include <QWidget>
#include <QJsonObject>
#include <functional>
class Catalog;
class HudAssets;
class QListWidget;
class QStackedWidget;
class QLabel;
class QScrollArea;
class QPushButton;
class PlayerMatchView : public QWidget {
public:
    explicit PlayerMatchView(const Catalog &,QWidget *parent=nullptr);
    void setProfile(const QJsonObject &);
    void clear(const QString &message = {});
    void setArchiveHeading(QWidget *);
    QJsonObject report()const;
    void openMatch(int);
    void closeMatch();
    QJsonObject navigationState() const;
    void restoreNavigationState(const QJsonObject &);
    bool detailsOpen() const { return selected >= 0; }
    void setArchivePage(int);
    void refreshDecisionReplay();
    std::function<void(bool)> onDetailChanged;
    std::function<void(const QJsonObject &)> onPlayerRequested;
    std::function<bool(const QString &)> decisionReplayAvailable;
    std::function<void(const QString &)> onDecisionReplay;
protected:
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *,QEvent *) override;
private:
    void refreshArchivePage();
    void refreshDetailPage();
    QString currentMatchRef() const;
    const Catalog &catalog;
    HudAssets *assets;
    QJsonObject profile;
    QStackedWidget *pages;
    QListWidget *list;
    QWidget *summary,*scoreboard,*archiveSidebar,*archiveListPanel;
    QLabel *detailTitle,*emptyMessage;
    QScrollArea *detailScroll;
    QWidget *archivePager;
    QPushButton *previousPage,*nextPage;
    QLabel *pageNumber;
    QPushButton *detailPrevious,*detailNext;
    QPushButton *decisionReplay;
    QLabel *detailPageNumber;
    int archivePageIndex=0,archivePageSize=3;
    int selected=-1;
};
