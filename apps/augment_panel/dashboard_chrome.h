#pragma once

#include <QObject>
#include <QCursor>
#include <QJsonObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#endif

class QAbstractButton;
class QMainWindow;
class QWidget;

// Window behaviour only. The dashboard owns its shell and content painting.
// No game/client state, preferences, network calls or global shortcuts live here.
class DashboardChrome final : public QObject
#ifdef Q_OS_WIN
    , public QAbstractNativeEventFilter
#endif
{
public:
    static constexpr int TitleBarHeight = 44;
    // Includes the transparent shell inset, leaving a usable resize target
    // immediately inside an 8-10 px painted outer inset.
    static constexpr int ResizeMargin = 16;

    DashboardChrome(QMainWindow *window, QWidget *titleBar);
    ~DashboardChrome() override;

    QWidget *controls() const;
    bool setPinned(bool pinned);
    QJsonObject report() const;
    // Pure geometry/control checks; these do not simulate a native drag.
    QJsonObject geometryChecks() const;

protected:
    bool eventFilter(QObject *, QEvent *) override;
#ifdef Q_OS_WIN
    bool nativeEventFilter(const QByteArray &, void *, qintptr *) override;
#endif

private:
    QPointer<QMainWindow> window;
    QPointer<QWidget> titleBar, buttons, cursorWidget;
    QPointer<QAbstractButton> pinButton, minimizeButton, maximizeButton, closeButton;
    QCursor savedCursor;
    bool savedCursorWasExplicit = false;
    bool applicationFilterInstalled = false;
    bool pinned = false;
    bool fallbackMoving = false, fallbackResizing = false;
    QPoint pressGlobal;
    QRect pressGeometry;
    Qt::Edges resizeEdges;
    int moveRequests = 0, acceptedMoveRequests = 0;
    int resizeRequests = 0, acceptedResizeRequests = 0;
    int fallbackMoves = 0, fallbackResizes = 0;
    // Acceptance is an OS API result, never a claim that desktop pixels were
    // visually checked. The dashboard uses tint_alpha only for its outer shell.
    QJsonObject backdropState;
    QJsonObject shapeState;
    QString backdropUpdateReason;
    bool backdropUpdatePending = false;
    int backdropUpdateRequests = 0, backdropApplyAttempts = 0;

    Qt::Edges edgesAt(const QPoint &, bool maximized) const;
    bool belongsToWindow(const QWidget *) const;
    bool interactive(const QWidget *) const;
    bool resizeTarget(const QWidget *, Qt::Edges) const;
    bool titleTarget(const QWidget *, const QPoint &global) const;
    void updateButtons();
    bool applyTopmost(bool enabled);
    void toggleMaximized();
    void setResizeCursor(QWidget *, Qt::Edges);
    void resetCursor();
    void setInteractiveMoveSize(bool active);
    void stopFallback();
    void scheduleBackdropUpdate(const QString &reason);
    void applyBackdrop();
    void applyWindowShape();
    void publishBackdrop(QJsonObject state);
};
