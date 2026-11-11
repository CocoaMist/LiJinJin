#include "dashboard_chrome.h"
#include "kda_theme.h"
#include "kda_motion.h"
#include "dashboard_window_frame.h"

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QChildEvent>
#include <QComboBox>
#include <QEvent>
#include <QDynamicPropertyChangeEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QOperatingSystemVersion>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QRegion>
#include <QScreen>
#include <QSettings>
#include <QTextEdit>
#include <QTimer>
#include <QLibrary>
#include <QWindow>
#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
class ChromeButton final : public QAbstractButton {
public:
    enum class Symbol { Pin, Minimize, Maximize, Restore, Close };
    explicit ChromeButton(Symbol value, QWidget *parent)
        : QAbstractButton(parent), symbol(value),hoverMotion(this),pressMotion(this,140) {
        setFixedSize(36, 32);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setAttribute(Qt::WA_NoSystemBackground);
        connect(this,&QAbstractButton::pressed,this,[this]{pressMotion.retarget(1);});
        connect(this,&QAbstractButton::released,this,[this]{pressMotion.retarget(0);});
    }
    void setSymbol(Symbol value) { symbol = value; update(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF face = QRectF(rect()).adjusted(1, 1, -1, -1);
        if (isChecked() || hoverMotion.value()>0 || pressMotion.value()>0) {
            painter.setPen(Qt::NoPen);
            auto hoverFace = isChecked() ? Kda::color("accent") :
                symbol == Symbol::Close ? QColor(184, 81, 116) : QColor(225, 231, 244);
            hoverFace.setAlpha(qRound(hoverMotion.value()*(symbol == Symbol::Close ? 65 : 22)+pressMotion.value()*18)+(isChecked()?36:0));
            painter.setBrush(hoverFace);
            painter.drawRoundedRect(face, Kda::radius("control"), Kda::radius("control"));
        }
        painter.setPen(QPen(isEnabled() ? QColor(210, 214, 224)
                                       : QColor(105, 109, 119), 1.25,
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        const QPointF middle = face.center()+QPointF(0,pressMotion.value()*.8);
        switch (symbol) {
        case Symbol::Pin: {
            painter.translate(middle);
            painter.rotate(32);
            QPainterPath head;
            head.moveTo(-3.5,-5);
            head.lineTo(3.5,-5);
            head.lineTo(2.3,-3.6);
            head.lineTo(2.3,-.5);
            head.lineTo(4,1.6);
            head.lineTo(-4,1.6);
            head.lineTo(-2.3,-.5);
            head.lineTo(-2.3,-3.6);
            head.closeSubpath();
            if (isChecked()) painter.setBrush(painter.pen().color());
            painter.drawPath(head);
            painter.drawLine(QPointF(0,1.6),QPointF(0,6));
            break;
        }
        case Symbol::Minimize:
            painter.drawLine(middle + QPointF(-4.5, 1.5), middle + QPointF(4.5, 1.5));
            break;
        case Symbol::Maximize:
            painter.drawRoundedRect(QRectF(middle.x() - 4.5, middle.y() - 4.5, 9, 9), 1, 1);
            break;
        case Symbol::Restore:
            painter.drawLine(middle + QPointF(-2, -4.5), middle + QPointF(4.5, -4.5));
            painter.drawLine(middle + QPointF(4.5, -4.5), middle + QPointF(4.5, 2));
            painter.drawRoundedRect(QRectF(middle.x() - 4.5, middle.y() - 2, 7, 7), 1, 1);
            break;
        case Symbol::Close:
            painter.drawLine(middle + QPointF(-4, -4), middle + QPointF(4, 4));
            painter.drawLine(middle + QPointF(4, -4), middle + QPointF(-4, 4));
            break;
        }
    }
    void enterEvent(QEnterEvent *event) override {
        QAbstractButton::enterEvent(event); hoverMotion.retarget(1);
    }
    void leaveEvent(QEvent *event) override {
        QAbstractButton::leaveEvent(event); hoverMotion.retarget(0);
    }

private:
    Symbol symbol;
    Kda::MotionValue hoverMotion,pressMotion;
};

QJsonArray rectArray(const QRect &rect) {
    return {rect.x(), rect.y(), rect.width(), rect.height()};
}

Qt::CursorShape edgeCursor(Qt::Edges edges) {
    const bool horizontal = edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge);
    const bool vertical = edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge);
    if (horizontal && vertical) {
        const bool descending = (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::TopEdge)) ||
                                (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::BottomEdge));
        return descending ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
    }
    return horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor;
}

#ifdef Q_OS_WIN
// SetWindowCompositionAttribute has no public SDK declaration. Keep this small
// optional compatibility path local, resolve it at runtime, and retain an
// opaque shell whenever the OS declines it. Never alter Qt's WS_EX_LAYERED.
struct AccentPolicy {
    int state = 0;
    int flags = 0;
    DWORD gradientColor = 0;
    int animationId = 0;
};
struct AccentAttributeData {
    int attribute = 19; // WCA_ACCENT_POLICY
    void *data = nullptr;
    SIZE_T size = 0;
};
using SetCompositionAttribute = BOOL(WINAPI *)(HWND, AccentAttributeData *);
using SetDwmAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
using IsDwmCompositionEnabled = HRESULT(WINAPI *)(BOOL *);
constexpr DWORD SystemBackdropAttribute = 38; // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int NoSystemBackdrop = 1;          // DWMSBT_NONE
constexpr int DesktopAcrylicBackdrop = 3;    // DWMSBT_TRANSIENTWINDOW
#endif
}

DashboardChrome::DashboardChrome(QMainWindow *owner, QWidget *bar)
    : QObject(owner), window(owner), titleBar(bar) {
    setObjectName(QStringLiteral("dashboardChrome"));
    if (!window || !titleBar) return;

    // Construct before the first show so changing the flags never hides a live
    // window or recreates the user's current native window unexpectedly.
    window->setWindowFlags(window->windowFlags() | Qt::FramelessWindowHint);
    window->setAttribute(Qt::WA_TranslucentBackground);
    window->setAutoFillBackground(false);
    window->setMouseTracking(true);
    titleBar->setMouseTracking(true);
    publishBackdrop({{"api_accepted", false}, {"backend", "opaque"},
        {"tint_alpha", 255}, {"fallback_reason", "awaiting_native_window"}});

    buttons = new QWidget(titleBar);
    buttons->setObjectName(QStringLiteral("dashboardChromeControls"));
    buttons->setProperty("dashboardDragBlock", true);
    buttons->setAccessibleName(QStringLiteral("窗口控制"));
    buttons->setFixedSize(150, 34);
    buttons->setStyleSheet(QStringLiteral("QWidget#dashboardChromeControls { background: transparent; border: 0; }"));
    auto *layout = new QHBoxLayout(buttons);
    layout->setContentsMargins(0, 1, 0, 1);
    layout->setSpacing(2);

    auto make = [&](ChromeButton::Symbol symbol, const char *name, const QString &accessible) {
        auto *button = new ChromeButton(symbol, buttons);
        button->setObjectName(QString::fromLatin1(name));
        button->setAccessibleName(accessible);
        button->setToolTip(accessible);
        button->setAccessibleDescription(accessible);
        layout->addWidget(button);
        return button;
    };
    pinButton = make(ChromeButton::Symbol::Pin, "chromePin", QStringLiteral("保持置顶"));
    pinButton->setCheckable(true);
    minimizeButton = make(ChromeButton::Symbol::Minimize, "chromeMinimize", QStringLiteral("最小化窗口"));
    maximizeButton = make(ChromeButton::Symbol::Maximize, "chromeMaximize", QStringLiteral("最大化窗口"));
    closeButton = make(ChromeButton::Symbol::Close, "chromeClose", QStringLiteral("关闭窗口"));
    connect(minimizeButton, &QAbstractButton::clicked, this, [this] {
        if (window) window->showMinimized();
    });
    connect(maximizeButton, &QAbstractButton::clicked, this, [this] { toggleMaximized(); });
    connect(closeButton, &QAbstractButton::clicked, this, [this] {
        // Route the title-bar button through the same close event as Alt+F4.
        if (window) window->close();
    });

    for (auto *child : window->findChildren<QWidget *>()) child->setMouseTracking(true);
    if (qApp) {
        qApp->installEventFilter(this);
        applicationFilterInstalled = true;
#ifdef Q_OS_WIN
        qApp->installNativeEventFilter(this);
#endif
    }
    updateButtons();
    scheduleBackdropUpdate(QStringLiteral("construction"));
}

DashboardChrome::~DashboardChrome() {
    stopFallback();
    resetCursor();
    if (qApp && applicationFilterInstalled) {
        qApp->removeEventFilter(this);
#ifdef Q_OS_WIN
        qApp->removeNativeEventFilter(this);
#endif
    }
}

QWidget *DashboardChrome::controls() const { return buttons.data(); }

bool DashboardChrome::setPinned(bool enabled) {
    const bool accepted = applyTopmost(enabled);
    if (accepted) pinned = enabled;
    pinButton->setChecked(pinned);
    const auto action = pinned ? QStringLiteral("取消置顶") : QStringLiteral("保持置顶");
    pinButton->setToolTip(action);
    pinButton->setAccessibleDescription(action);
    return accepted;
}

bool DashboardChrome::applyTopmost(bool enabled) {
    auto *handle = window ? window->windowHandle() : nullptr;
    if (!handle) return bool(window);
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const auto hwnd = reinterpret_cast<HWND>(window->internalWinId());
        if (!hwnd) return true;
        if (bool(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == enabled) return true;
        return SetWindowPos(hwnd, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
    }
#endif
    handle->setFlag(Qt::WindowStaysOnTopHint, enabled);
    return handle->flags().testFlag(Qt::WindowStaysOnTopHint) == enabled;
}

bool DashboardChrome::belongsToWindow(const QWidget *widget) const {
    return window && widget && (widget == window || window->isAncestorOf(widget)) &&
           widget->window() == window;
}

Qt::Edges DashboardChrome::edgesAt(const QPoint &point, bool maximized) const {
    if (!window || maximized || !window->rect().contains(point)) return {};
    Qt::Edges edges;
    if (point.x() < ResizeMargin) edges |= Qt::LeftEdge;
    else if (point.x() >= window->width() - ResizeMargin) edges |= Qt::RightEdge;
    if (point.y() < ResizeMargin) edges |= Qt::TopEdge;
    else if (point.y() >= window->height() - ResizeMargin) edges |= Qt::BottomEdge;
    return edges;
}

bool DashboardChrome::interactive(const QWidget *widget) const {
    for (auto *candidate = widget; candidate && candidate != window; candidate = candidate->parentWidget()) {
        if (candidate->property("dashboardDragBlock").toBool() ||
            qobject_cast<const QAbstractButton *>(candidate) ||
            qobject_cast<const QLineEdit *>(candidate) ||
            qobject_cast<const QComboBox *>(candidate) ||
            qobject_cast<const QAbstractSlider *>(candidate) ||
            qobject_cast<const QAbstractSpinBox *>(candidate) ||
            qobject_cast<const QTextEdit *>(candidate) ||
            qobject_cast<const QPlainTextEdit *>(candidate) ||
            qobject_cast<const QMenuBar *>(candidate)) return true;
        if (candidate != titleBar && candidate->focusPolicy() != Qt::NoFocus) return true;
    }
    return false;
}

bool DashboardChrome::resizeTarget(const QWidget *widget, Qt::Edges edges) const {
    // Scroll bars, buttons and editors remain usable even when a compact
    // layout places them inside the window's generous resize target band.
    return bool(edges) && !interactive(widget);
}

bool DashboardChrome::titleTarget(const QWidget *widget, const QPoint &global) const {
    if (!titleBar || !belongsToWindow(widget) || interactive(widget)) return false;
    return titleBar->rect().contains(titleBar->mapFromGlobal(global));
}

void DashboardChrome::updateButtons() {
    if (!window || !maximizeButton) return;
    auto *button = static_cast<ChromeButton *>(maximizeButton.data());
    const bool maximized = window->isMaximized();
    button->setSymbol(maximized ? ChromeButton::Symbol::Restore : ChromeButton::Symbol::Maximize);
    const auto label = maximized ? QStringLiteral("还原窗口") : QStringLiteral("最大化窗口");
    button->setAccessibleName(label);
    button->setAccessibleDescription(label);
    button->setToolTip(label);
    window->update();
}

void DashboardChrome::toggleMaximized() {
    if (!window) return;
    stopFallback();
    resetCursor();
    if (window->isMaximized()) window->showNormal();
    else window->showMaximized();
    updateButtons();
}

void DashboardChrome::setResizeCursor(QWidget *widget, Qt::Edges edges) {
    if (!widget || !edges) { resetCursor(); return; }
    if (cursorWidget != widget) {
        resetCursor();
        cursorWidget = widget;
        savedCursor = widget->cursor();
        savedCursorWasExplicit = widget->testAttribute(Qt::WA_SetCursor);
    }
    widget->setCursor(edgeCursor(edges));
}

void DashboardChrome::resetCursor() {
    if (cursorWidget) {
        if (savedCursorWasExplicit) cursorWidget->setCursor(savedCursor);
        else cursorWidget->unsetCursor();
    }
    cursorWidget.clear();
}

void DashboardChrome::setInteractiveMoveSize(bool active) {
    if (window && window->property("dashboardInteractiveMoveSize").toBool() != active)
        window->setProperty("dashboardInteractiveMoveSize", active);
}

void DashboardChrome::stopFallback() {
    const bool wasMovingOrResizing = fallbackMoving || fallbackResizing;
    fallbackMoving = fallbackResizing = false;
    resizeEdges = {};
    if (window && QWidget::mouseGrabber() == window) window->releaseMouse();
    if (wasMovingOrResizing) setInteractiveMoveSize(false);
}

void DashboardChrome::publishBackdrop(QJsonObject state) {
    state.insert("requested_material", QStringLiteral("desktop_blur"));
    state.insert("scope", QStringLiteral("rounded_main_window_only"));
    state.insert("visual_blur_verified", false);
    state.insert("layered_style_modified", false);
    state.insert("update_requests", backdropUpdateRequests);
    state.insert("apply_attempts", backdropApplyAttempts);
    state.insert("last_update_reason", backdropUpdateReason);
    backdropState = state;
    applyWindowShape();
    if (!window) return;
    window->setProperty("dashboardBackdropAccepted", state.value("api_accepted").toBool());
    window->setProperty("dashboardBackdropBackend", state.value("backend").toString());
    window->setProperty("dashboardBackdropTintAlpha", state.value("tint_alpha").toInt(255));
    window->setProperty("dashboardBackdropFallback", state.value("fallback_reason").toString());
    window->setProperty("dashboardBackdropVisualVerified", false);
    window->update();
}

void DashboardChrome::scheduleBackdropUpdate(const QString &reason) {
    ++backdropUpdateRequests;
    backdropUpdateReason = reason;
    if (backdropUpdatePending || !window) return;
    backdropUpdatePending = true;
    // HWND creation, system setting changes and maximization can arrive while
    // Qt is updating its native surface. Wait until that work finishes and
    // coalesce notifications instead of changing composition during painting.
    QTimer::singleShot(0, this, [this] {
        backdropUpdatePending = false;
        applyBackdrop();
    });
}

void DashboardChrome::applyBackdrop() {
    applyWindowShape();
    QJsonObject state{{"api_accepted", false}, {"backend", "opaque"},
        {"tint_alpha", 255}, {"fallback_reason", "platform_unsupported"}};
    if (!window) return;
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        state["fallback_reason"] = "non_native_qt_platform";
        publishBackdrop(state);
        return;
    }
    // Reading the cached ID avoids CreateWindow re-entry and leaves offscreen
    // checks free to run without creating or showing a native window.
    const HWND native = reinterpret_cast<HWND>(window->internalWinId());
    if (!native || !IsWindow(native)) {
        state["fallback_reason"] = "awaiting_native_window";
        publishBackdrop(state);
        return;
    }
    ++backdropApplyAttempts;
    const auto version = QOperatingSystemVersion::current();
    const int build = version.microVersion();
    const bool layered = bool(GetWindowLongPtrW(native, GWL_EXSTYLE) & WS_EX_LAYERED);
    state["windows_major"] = version.majorVersion();
    state["windows_build"] = build;
    state["native_layered"] = layered;
    state["native_window_present"] = true;

    QLibrary dwm(QStringLiteral("dwmapi"));
    const auto setDwm = reinterpret_cast<SetDwmAttribute>(dwm.resolve("DwmSetWindowAttribute"));
    const auto isComposed = reinterpret_cast<IsDwmCompositionEnabled>(dwm.resolve("DwmIsCompositionEnabled"));
    QLibrary user32(QStringLiteral("user32"));
    const auto setAccent = reinterpret_cast<SetCompositionAttribute>(user32.resolve("SetWindowCompositionAttribute"));
    state["dwm_attribute_api_available"] = bool(setDwm);
    state["accent_api_available"] = bool(setAccent);

    BOOL composed = FALSE;
    const bool compositionEnabled = isComposed && SUCCEEDED(isComposed(&composed)) && composed;
    HIGHCONTRASTW highContrast{};
    highContrast.cbSize = sizeof(highContrast);
    const bool contrastEnabled = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) &&
        bool(highContrast.dwFlags & HCF_HIGHCONTRASTON);
    QSettings personalization(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), QSettings::NativeFormat);
    const bool transparencyEnabled = personalization.value(QStringLiteral("EnableTransparency"), 1).toInt() != 0;
    const bool remote = GetSystemMetrics(SM_REMOTESESSION) != 0;
    state["composition_enabled"] = compositionEnabled;
    state["high_contrast"] = contrastEnabled;
    state["transparency_enabled"] = transparencyEnabled;
    state["remote_session"] = remote;

    auto accent = [&](int mode, DWORD tint) {
        if (!setAccent) return false;
        AccentPolicy policy;
        policy.state = mode;
        policy.gradientColor = tint; // AABBGGRR, not a Qt ARGB value.
        AccentAttributeData data;
        data.data = &policy;
        data.size = sizeof(policy);
        return bool(setAccent(native, &data));
    };
    auto clearEffects = [&] {
        accent(0, 0);
        if (setDwm && build >= 22621) setDwm(native, SystemBackdropAttribute, &NoSystemBackdrop, sizeof(NoSystemBackdrop));
    };
    QString unavailable;
    if (qEnvironmentVariableIntValue("LIXIN_DISABLE_NATIVE_BACKDROP") == 1) unavailable = QStringLiteral("disabled_by_environment");
    else if (!Kda::glassEnabled()) unavailable = QStringLiteral("glass_disabled_by_user");
    else if (!compositionEnabled) unavailable = QStringLiteral("composition_unavailable");
    else if (contrastEnabled) unavailable = QStringLiteral("high_contrast_enabled");
    else if (!transparencyEnabled) unavailable = QStringLiteral("system_transparency_disabled");
    else if (remote) unavailable = QStringLiteral("remote_session");
    if (!unavailable.isEmpty()) {
        clearEffects();
        state["fallback_reason"] = unavailable;
        publishBackdrop(state);
        return;
    }

    // Acrylic paints its own opaque rectangle on a layered Qt surface. The
    // blur-only accent samples the desktop while leaving Qt's alpha and tint
    // in charge of the rounded frame; never remove WS_EX_LAYERED for this.
    if (layered) {
        clearEffects();
        const bool accepted = version.majorVersion() >= 10 && accent(3, 0x00000000);
        state["api_accepted"] = accepted;
        state["accent_blur_accepted"] = accepted;
        state["backend"] = accepted ? "accent_blur_layered" : "opaque";
        state["tint_alpha"] = accepted ? 214 : 255;
        state["fallback_reason"] = accepted ? "" : "layered_blur_unavailable";
        state["uses_optional_undocumented_api"] = accepted;
        state["preserves_fractional_alpha"] = true;
        publishBackdrop(state);
        return;
    }
    // Normally composed HWNDs can use the documented Windows 11 backdrop.
    if (!layered && setDwm && version.majorVersion() >= 10 && build >= 22621) {
        const HRESULT outcome = setDwm(native, SystemBackdropAttribute, &DesktopAcrylicBackdrop, sizeof(DesktopAcrylicBackdrop));
        state["dwm_hresult"] = qint64(outcome);
        if (SUCCEEDED(outcome)) {
            state["api_accepted"] = true;
            state["backend"] = "dwm_desktop_acrylic";
            state["tint_alpha"] = 96;
            state["fallback_reason"] = "";
            publishBackdrop(state);
            return;
        }
    }
    if (setDwm && build >= 22621) setDwm(native, SystemBackdropAttribute, &NoSystemBackdrop, sizeof(NoSystemBackdrop));
    bool acrylicAccepted = false;
    if (version.majorVersion() >= 10 && build >= 17134)
        acrylicAccepted = accent(4, 0x990b0909); // ACCENT_ENABLE_ACRYLICBLURBEHIND
    state["accent_acrylic_accepted"] = acrylicAccepted;
    if (acrylicAccepted) {
        state["api_accepted"] = true;
        state["backend"] = "accent_acrylic";
        state["tint_alpha"] = 96;
        state["fallback_reason"] = "";
        state["uses_optional_undocumented_api"] = true;
        publishBackdrop(state);
        return;
    }
    const bool blurAccepted = version.majorVersion() >= 10 && accent(3, 0x00000000); // ACCENT_ENABLE_BLURBEHIND
    state["accent_blur_accepted"] = blurAccepted;
    if (blurAccepted) {
        state["api_accepted"] = true;
        state["backend"] = "accent_blur";
        state["tint_alpha"] = 185;
        state["fallback_reason"] = "acrylic_unavailable_using_blur";
        state["uses_optional_undocumented_api"] = true;
    } else {
        clearEffects();
        state["fallback_reason"] = setAccent ? "native_backdrop_rejected" : "native_backdrop_api_unavailable";
    }
#endif
    publishBackdrop(state);
}

void DashboardChrome::applyWindowShape() {
    if (!window) return;
    shapeState = {{"backend", "per_pixel_alpha"}, {"native_clip_applied", false},
        {"corner_supersample", 4}, {"preserves_fractional_alpha", true}, {"matches_window_state", true}};
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != QStringLiteral("windows")) return;
    const auto id = window->internalWinId();
    const HWND native = reinterpret_cast<HWND>(id);
    RECT bounds{};
    if (!native || !IsWindow(native) || !GetWindowRect(native, &bounds)) return;
    const QSize size(bounds.right - bounds.left, bounds.bottom - bounds.top);
    const int radius = window->isMaximized() || window->isFullScreen() ? 0
        : qRound(Kda::radius("window") * window->devicePixelRatioF());
    const bool blurred = backdropState.value("api_accepted").toBool() && backdropState.value("native_layered").toBool();
    if (blurred) {
        // Accent blur is rectangular unless the HWND has a region. Derive its
        // conservative coverage from the very same supersampled corner paint:
        // every fractional-alpha pixel stays inside the native clip. The clip
        // only removes fully transparent pixels, including after restore/resize.
        QRegion coverage(QRect(QPoint(),size));
        const qreal dpr=window->devicePixelRatioF();
        const int span=qCeil(radius)+3;
        if(radius>0 && size.width()>=span*2 && size.height()>=span*2){
            QImage mask(QSize(span*2,span*2),QImage::Format_ARGB32_Premultiplied);mask.fill(Qt::transparent);mask.setDevicePixelRatio(dpr);
            {QPainter p(&mask);DashboardFrame::paint(p,QSize(qRound(mask.width()/dpr),qRound(mask.height()/dpr)),dpr,Kda::radius("window"),Qt::white,Qt::white);}
            for(int y=0;y<span;++y){int transparent=0;while(transparent<span&&mask.pixelColor(transparent,y).alpha()==0)++transparent;
                if(!transparent)continue;
                for(const int row:{y,size.height()-1-y}){
                    coverage-=QRect(0,row,transparent,1);
                    coverage-=QRect(size.width()-transparent,row,transparent,1);
                }
            }
        }
        HRGN clip=CreateRectRgn(0,0,0,0);
        if(clip){for(const auto &run:coverage){HRGN part=CreateRectRgn(run.left(),run.top(),run.right()+1,run.bottom()+1);if(part){CombineRgn(clip,clip,part,RGN_OR);DeleteObject(part);}}
            if(!SetWindowRgn(native,clip,TRUE))DeleteObject(clip);}
    } else {
        SetWindowRgn(native,nullptr,TRUE);
    }
    HRGN actual = CreateRectRgn(0, 0, 0, 0);
    if (!actual) return;
    const int type = GetWindowRgn(native, actual);
    const bool clipped = type != ERROR;
    if (actual) DeleteObject(actual);
    shapeState = {{"backend", blurred?"per_pixel_alpha_with_blur_clip":"per_pixel_alpha"}, {"native_clip_applied", clipped},
        {"radius_physical", radius}, {"size_physical", QJsonArray{size.width(), size.height()}},
        {"corner_supersample", 4}, {"clip_excludes_only_zero_alpha", blurred&&clipped},
        {"preserves_fractional_alpha", !clipped||blurred},
        {"matches_window_state", clipped==blurred}};
#endif
}

bool DashboardChrome::eventFilter(QObject *object, QEvent *event) {
    if(object==qApp&&event->type()==QEvent::DynamicPropertyChange&&
        static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()==QByteArrayLiteral("lixinGlassEnabled"))
        scheduleBackdropUpdate(QStringLiteral("glass_preference_changed"));
    auto *widget = qobject_cast<QWidget *>(object);
    if (!belongsToWindow(widget)) {
        if (event->type() == QEvent::MouseMove && !fallbackMoving && !fallbackResizing) resetCursor();
        return QObject::eventFilter(object, event);
    }

    if (event->type() == QEvent::Polish) widget->setMouseTracking(true);
    if (widget == window && event->type() == QEvent::Resize) applyWindowShape();
    if (widget == window && (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Show)) {
        stopFallback(); resetCursor(); updateButtons();
    }
    if (widget == window && pinned && (event->type() == QEvent::Show || event->type() == QEvent::WinIdChange))
        QTimer::singleShot(0, this, [this] { if (pinned) setPinned(true); });
    if (widget == window && (event->type() == QEvent::Show || event->type() == QEvent::WinIdChange ||
        event->type() == QEvent::WindowStateChange || event->type() == QEvent::WindowActivate ||
        event->type() == QEvent::ThemeChange || event->type() == QEvent::PaletteChange))
        scheduleBackdropUpdate(QStringLiteral("qt_event_%1").arg(int(event->type())));
    if (widget == window && (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate ||
                             event->type() == QEvent::WinIdChange)) {
        stopFallback(); resetCursor();
        setInteractiveMoveSize(false);
    }
    if (event->type() == QEvent::Leave && widget == cursorWidget && !fallbackMoving && !fallbackResizing) resetCursor();

    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick &&
        event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonRelease)
        return QObject::eventFilter(object, event);

    auto *mouse = static_cast<QMouseEvent *>(event);
    const QPoint global = mouse->globalPosition().toPoint();
    const QPoint local = window->mapFromGlobal(global);
    const Qt::Edges edges = edgesAt(local, window->isMaximized() || window->isFullScreen());

    if (event->type() == QEvent::MouseButtonDblClick && mouse->button() == Qt::LeftButton &&
        !edges && titleTarget(widget, global)) {
        toggleMaximized(); mouse->accept(); return true;
    }
    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
        if (resizeTarget(widget, edges)) {
            ++resizeRequests;
            const bool accepted = window->windowHandle() && window->windowHandle()->startSystemResize(edges);
            if (accepted) ++acceptedResizeRequests;
            else {
                fallbackResizing = true; ++fallbackResizes;
                pressGlobal = global; pressGeometry = window->geometry(); resizeEdges = edges;
                setInteractiveMoveSize(true);
                window->grabMouse();
            }
            mouse->accept(); return true;
        }
        if (titleTarget(widget, global)) {
            ++moveRequests;
            const bool accepted = window->windowHandle() && window->windowHandle()->startSystemMove();
            if (accepted) ++acceptedMoveRequests;
            else {
                // Qt's native operation is preferred; this path also keeps the
                // window movable on platforms without startSystemMove support.
                if (window->isMaximized()) {
                    const qreal fraction = std::clamp(qreal(local.x()) / std::max(1, window->width()), 0., 1.);
                    window->showNormal();
                    window->move(global.x() - qRound(window->width() * fraction), global.y() - std::min(local.y(), 36));
                    updateButtons();
                }
                fallbackMoving = true; ++fallbackMoves;
                pressGlobal = global; pressGeometry = window->geometry();
                setInteractiveMoveSize(true);
                window->grabMouse();
            }
            mouse->accept(); return true;
        }
    }
    if (event->type() == QEvent::MouseMove) {
        if ((fallbackMoving || fallbackResizing) && !(mouse->buttons() & Qt::LeftButton)) stopFallback();
        if (fallbackMoving) {
            window->move(pressGeometry.topLeft() + global - pressGlobal);
            mouse->accept(); return true;
        }
        if (fallbackResizing) {
            const QPoint delta = global - pressGlobal;
            QRect next = pressGeometry;
            const int minimumWidth = std::max(1, window->minimumWidth());
            const int minimumHeight = std::max(1, window->minimumHeight());
            const int maximumWidth = std::max(minimumWidth, window->maximumWidth());
            const int maximumHeight = std::max(minimumHeight, window->maximumHeight());
            if (resizeEdges.testFlag(Qt::LeftEdge)) {
                const int width = std::clamp(pressGeometry.width() - delta.x(), minimumWidth, maximumWidth);
                next.setLeft(pressGeometry.right() - width + 1);
            } else if (resizeEdges.testFlag(Qt::RightEdge)) {
                next.setWidth(std::clamp(pressGeometry.width() + delta.x(), minimumWidth, maximumWidth));
            }
            if (resizeEdges.testFlag(Qt::TopEdge)) {
                const int height = std::clamp(pressGeometry.height() - delta.y(), minimumHeight, maximumHeight);
                next.setTop(pressGeometry.bottom() - height + 1);
            } else if (resizeEdges.testFlag(Qt::BottomEdge)) {
                next.setHeight(std::clamp(pressGeometry.height() + delta.y(), minimumHeight, maximumHeight));
            }
            window->setGeometry(next);
            mouse->accept(); return true;
        }
        if (resizeTarget(widget, edges) && !mouse->buttons()) setResizeCursor(widget, edges);
        else resetCursor();
    }
    if (event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton &&
        (fallbackMoving || fallbackResizing)) {
        stopFallback(); resetCursor(); mouse->accept(); return true;
    }
    return QObject::eventFilter(object, event);
}

#ifdef Q_OS_WIN
bool DashboardChrome::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
    if (!window || !message) return false;
    auto *native = static_cast<MSG *>(message);
    // Read the cached QWidget handle only. Asking QWindow::winId() while
    // CreateWindow is dispatching this message can re-enter native creation.
    const WId cachedId = window->internalWinId();
    if (!cachedId || native->hwnd != reinterpret_cast<HWND>(cachedId)) return false;
    if (native->message == WM_ENTERSIZEMOVE) setInteractiveMoveSize(true);
    if (native->message == WM_EXITSIZEMOVE || native->message == WM_CANCELMODE)
        setInteractiveMoveSize(false);
    if (native->message == 0x031E /* WM_DWMCOMPOSITIONCHANGED */ ||
        native->message == WM_THEMECHANGED || native->message == WM_SETTINGCHANGE ||
        native->message == WM_DPICHANGED)
        scheduleBackdropUpdate(QStringLiteral("native_message_%1").arg(native->message));
    if (native->message != WM_GETMINMAXINFO || !result) return false;
    const HMONITOR monitor = MonitorFromWindow(native->hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
    auto *limits = reinterpret_cast<MINMAXINFO *>(native->lParam);
    if (!limits) return false;
    // Native monitor/work rectangles use physical pixels, including negative
    // coordinates on secondary monitors. Keep the taskbar outside maximization.
    limits->ptMaxPosition.x = info.rcWork.left - info.rcMonitor.left;
    limits->ptMaxPosition.y = info.rcWork.top - info.rcMonitor.top;
    limits->ptMaxSize.x = info.rcWork.right - info.rcWork.left;
    limits->ptMaxSize.y = info.rcWork.bottom - info.rcWork.top;
    const qreal ratio = window->devicePixelRatioF();
    limits->ptMinTrackSize.x = std::max(limits->ptMinTrackSize.x, LONG(std::ceil(window->minimumWidth() * ratio)));
    limits->ptMinTrackSize.y = std::max(limits->ptMinTrackSize.y, LONG(std::ceil(window->minimumHeight() * ratio)));
    *result = 0;
    return true;
}
#endif

QJsonObject DashboardChrome::geometryChecks() const {
    if (!window || !titleBar || !buttons) return {{"window_chrome_present", false}};
    const int middleX = window->width() / 2, middleY = window->height() / 2;
    const int right = window->width() - 2, bottom = window->height() - 2;
    auto equals = [&](QPoint point, Qt::Edges expected) { return edgesAt(point, false) == expected; };
    bool slidersKeepInteraction = true;
    for (auto *slider : window->findChildren<QAbstractSlider *>())
        slidersKeepInteraction &= !resizeTarget(slider, Qt::RightEdge);
    return {
        {"window_chrome_present", true},
        {"chrome_frameless", window->windowFlags().testFlag(Qt::FramelessWindowHint)},
        {"chrome_translucent", window->testAttribute(Qt::WA_TranslucentBackground)},
        {"chrome_without_window_mask", window->mask().isEmpty()},
        {"chrome_resize_left", equals({1, middleY}, Qt::LeftEdge)},
        {"chrome_resize_right", equals({right, middleY}, Qt::RightEdge)},
        {"chrome_resize_top", equals({middleX, 1}, Qt::TopEdge)},
        {"chrome_resize_bottom", equals({middleX, bottom}, Qt::BottomEdge)},
        {"chrome_resize_top_left", equals({1, 1}, Qt::TopEdge | Qt::LeftEdge)},
        {"chrome_resize_top_right", equals({right, 1}, Qt::TopEdge | Qt::RightEdge)},
        {"chrome_resize_bottom_left", equals({1, bottom}, Qt::BottomEdge | Qt::LeftEdge)},
        {"chrome_resize_bottom_right", equals({right, bottom}, Qt::BottomEdge | Qt::RightEdge)},
        {"chrome_center_not_resize", !edgesAt({middleX, middleY}, false)},
        {"chrome_maximized_not_resize", !edgesAt({1, 1}, true)},
        {"chrome_outside_not_resize", !edgesAt({-1, middleY}, false)},
        {"chrome_controls_not_drag_target", interactive(buttons)},
        {"chrome_titlebar_height", titleBar->height() == TitleBarHeight},
        {"chrome_controls_inside_titlebar", titleBar->rect().contains(QRect(buttons->mapTo(titleBar, QPoint()), buttons->size()))},
        {"chrome_edge_buttons_keep_interaction", !resizeTarget(pinButton, Qt::TopEdge) && !resizeTarget(minimizeButton, Qt::TopEdge) &&
            !resizeTarget(maximizeButton, Qt::RightEdge) && !resizeTarget(closeButton, Qt::TopEdge | Qt::RightEdge)},
        {"chrome_edge_sliders_keep_interaction", slidersKeepInteraction},
        {"chrome_controls_accessible", minimizeButton && maximizeButton && closeButton &&
            !minimizeButton->accessibleName().isEmpty() && !maximizeButton->accessibleName().isEmpty() &&
            !closeButton->accessibleName().isEmpty()},
        {"chrome_controls_preserve_tab_order", minimizeButton && maximizeButton && closeButton &&
            minimizeButton->focusPolicy() == Qt::NoFocus && maximizeButton->focusPolicy() == Qt::NoFocus &&
            closeButton->focusPolicy() == Qt::NoFocus},
        {"chrome_application_filter_installed", applicationFilterInstalled}
    };
}

QJsonObject DashboardChrome::report() const {
    QJsonObject result{
        {"present", bool(window && titleBar && buttons)},
        {"resize_margin", ResizeMargin},
        {"control_count", 4},
        {"pinned", pinned},
        {"titlebar_height", titleBar ? titleBar->height() : 0},
        {"control_height", buttons ? buttons->height() : 0},
        {"system_move_requests", moveRequests},
        {"system_move_accepted_requests", acceptedMoveRequests},
        {"system_resize_requests", resizeRequests},
        {"system_resize_accepted_requests", acceptedResizeRequests},
        {"manual_move_fallbacks", fallbackMoves},
        {"manual_resize_fallbacks", fallbackResizes},
        {"backdrop", backdropState},
        {"shape", shapeState},
        {"native_drag_verified", false},
        {"native_resize_verified", false},
        {"geometry_checks", geometryChecks()}
    };
    if (!window) return result;
    result.insert("frameless", window->windowFlags().testFlag(Qt::FramelessWindowHint));
    result.insert("translucent", window->testAttribute(Qt::WA_TranslucentBackground));
    result.insert("maximized", window->isMaximized());
    result.insert("minimized", window->isMinimized());
    if (window->windowHandle()) {
        result.insert("qt_window_state", int(window->windowHandle()->windowState()));
        result.insert("qt_window_visibility", int(window->windowHandle()->visibility()));
    }
    result.insert("minimum_size", QJsonArray{window->minimumWidth(), window->minimumHeight()});
    result.insert("geometry", rectArray(window->geometry()));
    result.insert("frame_geometry", rectArray(window->frameGeometry()));
    result.insert("normal_geometry", rectArray(window->normalGeometry()));
    result.insert("device_pixel_ratio", window->devicePixelRatioF());
    if (window->screen()) result.insert("available_geometry", rectArray(window->screen()->availableGeometry()));
#ifdef Q_OS_WIN
    const HWND native = reinterpret_cast<HWND>(window->internalWinId());
    result.insert("native_maximized", native && IsZoomed(native));
    result.insert("native_minimized", native && IsIconic(native));
    RECT client{};
    if (native && GetClientRect(native, &client))
        result.insert("native_client_size", QJsonArray{int(client.right - client.left), int(client.bottom - client.top)});
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (native && GetWindowPlacement(native, &placement)) {
        result.insert("native_show_command", int(placement.showCmd));
        const auto &normal = placement.rcNormalPosition;
        result.insert("native_normal_geometry", QJsonArray{int(normal.left), int(normal.top), int(normal.right - normal.left), int(normal.bottom - normal.top)});
    }
#endif
    result.insert("titlebar_geometry", titleBar ? rectArray(QRect(titleBar->mapTo(window.data(), QPoint()), titleBar->size())) : QJsonArray{});
    result.insert("taskbar_work_area_respected", QJsonValue(QJsonValue::Null));
    if (window->isMaximized() && window->screen()) {
        const auto area = window->screen()->availableGeometry();
        result.insert("work_area", rectArray(area));
        result.insert("taskbar_work_area_respected", area.adjusted(-2, -2, 2, 2).contains(window->frameGeometry()));
    }
    return result;
}
