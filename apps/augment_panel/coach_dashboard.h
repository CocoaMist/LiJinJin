#pragma once
#include <QMainWindow>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <QList>
#include <functional>
#include "dashboard_match_view.h"

class Catalog;
class HudAssets;
class DashboardChrome;
class DashboardTutorial;
class HistoryPane;
class CoachReplay;
class PersonalHomeView;
class WaitingPlayerView;
class StartupNoticePage;
class ChampSelectView;
class QSettings;
class QStackedWidget;
class QPushButton;
class QLabel;
class QCheckBox;
class QSlider;
class QLineEdit;
class QComboBox;
class QScrollArea;
class QCloseEvent;
class QResizeEvent;
class QPaintEvent;
class QTabBar;
namespace Kda {class AppearanceSave;}

// A view over the existing assistant. It owns no game client, AI request or
// calculation worker. The supplied snapshot must already be anonymous.
class CoachDashboard : public QMainWindow {
public:
    CoachDashboard(const Catalog &,QSettings &,QWidget *parent=nullptr,bool startupGated=false);
    void beginStartupNotice(std::function<void()> onAccepted,std::function<void()> onRejected);
    bool startupNoticePending() const;
    void setSnapshot(const QJsonObject &);
    void setMatchRefreshStatus(const QString &);
    // Private account binding for local history, never added to a public snapshot.
    void setLocalAccountScope(const QString &);
    // Current account name is local UI state, never part of the anonymous snapshot.
    void setLocalPlayerDisplayName(const QString &);
    void setLocalPlayerBackdrop(const QByteArray &);
    QJsonObject report() const;
    void selfCheck(const QString &outputDir);
    void querySummoner(const QString &);
    void setReplayWidget(CoachReplay *);
    void showReplay();
    static QString decisionArchiveDirectory(const QString &root,const QString &accountScope);
    void decisionArchiveChanged();
    QJsonObject playerReport() const;
    std::function<void()> onVisibilityToggle,onResetOverlay,onOpenHudEditor,onQuit,onOpenReplay,onOpenUiBrowser,onGrowthCaptureSettings,onCompleteMatchInputs,onPreferencesChanged;
    std::function<QString()> onRefreshMatchData;
    // A runtime can dispatch appearance-only changes without recalculating advice.
    // The no-argument callback remains the fallback and the explicit DS save hook.
    std::function<void(const QString &)> onPreferenceChanged;
protected:
    bool eventFilter(QObject *,QEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void paintEvent(QPaintEvent *) override;
private:
    struct Seat {QWidget *body=nullptr;QLabel *icon=nullptr,*name=nullptr,*detail=nullptr,*marker=nullptr;};
    const Catalog &catalog;
    QSettings &preferences;
    QJsonObject snapshot,matchView,checks,skinSwitchTiming;
    DashboardMatchViewCache matchProjection;
    QString localPlayerDisplayName,localAccountScope;
    bool shouldDeferHomeView() const;
    HudAssets *assets=nullptr;
    DashboardChrome *chrome=nullptr;
    HistoryPane *playerHistory=nullptr;
    PersonalHomeView *personalHome=nullptr;
    WaitingPlayerView *waitingPlayer=nullptr;
    QStackedWidget *pages=nullptr;
    QStackedWidget *homePages=nullptr;
    QWidget *replayPage=nullptr;
    CoachReplay *replayView=nullptr;
    QLabel *replayTitle=nullptr;
    QPushButton *replayBack=nullptr;
    QString archivedReplayMatch;
    bool replaySawMatch=false;
    QWidget *matchPage=nullptr,*settingsPage=nullptr,*matchBoard=nullptr,*idleState=nullptr;
    ChampSelectView *champSelectView=nullptr;
    QWidget *purchaseOverview=nullptr;
    QCheckBox *shopRecommendationsEnabled=nullptr;
    QLabel *shopRecommendationsStatus=nullptr;
    QWidget *navigationRail=nullptr,*skinTransition=nullptr;
    QScrollArea *matchScroll=nullptr,*settingsScroll=nullptr;
    QPushButton *matchNavigation=nullptr,*settingsNavigation=nullptr,*historyNavigation=nullptr,*visibilityAction=nullptr,*uiBrowserAction=nullptr,*growthCaptureAction=nullptr,*matchInputsAction=nullptr;
    QLabel *connectionLabel=nullptr,*matchTitle=nullptr,*matchSubtitle=nullptr,*scoreLabel=nullptr,*timeLabel=nullptr;
    QList<QLabel*> phases;
    QList<Seat> allies,enemies;
    QLabel *actionTitle=nullptr,*actionDetail=nullptr,*actionIcon=nullptr,*priceLabel=nullptr,*routeLabel=nullptr,*rosterSummary=nullptr,*sourceLabel=nullptr,*coverageLabel=nullptr;
    QLabel *archiveUpdated=nullptr,*matchRefreshStatus=nullptr;
    QCheckBox *overlayEnabled=nullptr,*cardHintsEnabled=nullptr,*rosterDsEnabled=nullptr,*autoMinimize=nullptr;
    QCheckBox *reduceMotion=nullptr;
    QCheckBox *parallaxEnabled=nullptr;
    QCheckBox *frostedGlassEnabled=nullptr;
    QCheckBox *overlayBlur=nullptr;
    QSlider *overlayBackdrop=nullptr;QLabel *overlayBackdropValue=nullptr;
    QTabBar *settingsTabs=nullptr;
    QList<QPushButton*> skinChoices;
    QString dashboardBaseStyle;
    Kda::AppearanceSave *appearanceSave=nullptr;
    QSlider *hudSkinStrength=nullptr;QLabel *hudSkinStrengthValue=nullptr;
    QSlider *opacity=nullptr;QLabel *opacityCaption=nullptr,*opacityValue=nullptr,*saveStatus=nullptr,*growthCaptureStatus=nullptr;
    QLineEdit *apiKey=nullptr;QComboBox *model=nullptr;
    QComboBox *modelSlot=nullptr,*closeWindowBehavior=nullptr;
    QList<QPushButton*> modelSlotCards;
    QList<QWidget*> modelSlotBodies;
    QWidget *modelSlotPreview=nullptr;
    QWidget *modelSlotRoster=nullptr;
    QPushButton *modelLockButton=nullptr,*modelSaveButton=nullptr;
    QLabel *modelLockStatus=nullptr;
    QWidget *modelConfigurationEditor=nullptr;
    QStackedWidget *modelConnectionPages=nullptr;
    QLineEdit *localModelBaseUrl=nullptr,*localModelName=nullptr,*localModelApiKey=nullptr;
    QLabel *modelCurrentName=nullptr,*modelCurrentProvider=nullptr,*modelCurrentState=nullptr,*modelDraftState=nullptr,*modelSaveStatus=nullptr,*modelUsageScope=nullptr;
    QString savedModelSlot=QStringLiteral("deepseek"),savedDsModel,savedDsApiKey,savedLocalBaseUrl,savedLocalModel,savedLocalApiKey;
    bool savedModelConfigured=false,modelControlsLoaded=false;
    QLabel *dsUsageCurrent=nullptr,*dsUsageState=nullptr,*dsUsageTokens=nullptr,*dsUsageCalls=nullptr,*dsUsageAverage=nullptr,*dsUsageSamples=nullptr,*dsUsagePersistence=nullptr;
    QJsonObject dsUsagePresentation;
    bool savingPreferences=false,inSelfCheck=false;
    bool homeViewPending=false;
    qint64 homeViewRenderCount=0;
    StartupNoticePage *startupNoticePage=nullptr;
    DashboardTutorial *tutorial=nullptr;
    bool firstTutorialPending=false;
    bool startupNoticeActive=false,startupWorkspaceWasVisible=true,startupNavigationWasVisible=true,startupRevisionTimerWasActive=false;
    std::function<void()> startupAccepted,startupRejected;
    bool closesToTray() const;
    void finishStartupNotice(bool accepted);
    void showTutorial();
    void showFirstTutorial();
    void selectPage(int);
    void showDecisionReplay(const QString &matchRef);
    void dismissDecisionReplay();
    void applySnapshot(const QJsonObject &);
    void updateView();
    void updateConnectionState();
    void updateDsUsage();
    void updatePersonalHome();
    void updateIdlePresentation();
    void loadControls(bool loadSecrets=false);
    void savePreference(const QString &,const QVariant &);
    void saveDsSettings();
    void activateModelSlot(const QString &);
    QString refreshSavedModelConfiguration();
    void loadModelConfiguration(bool loadSecrets);
    void updateModelConfiguration();
    QJsonObject modelConfigurationReport() const;
    void renderSeats(QList<Seat> &,const QJsonArray &,const QString &);
    void updateArtwork();
};
