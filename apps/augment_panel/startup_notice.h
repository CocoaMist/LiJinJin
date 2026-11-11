#pragma once
#include <QDialog>
#include <functional>

class QPushButton;
class QShowEvent;
class QKeyEvent;
class QResizeEvent;
class QBoxLayout;

// The live reading step is a child page of the existing dashboard window.
// It only reports a deliberate decision; its owner controls page changes/exit.
class StartupNoticePage final : public QWidget {
public:
    explicit StartupNoticePage(QWidget *parent=nullptr);
    std::function<void()> onAccepted,onRejected;
    void focusExit();
    void reject();
protected:
    void showEvent(QShowEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    QPushButton *exitButton=nullptr;
    QPushButton *continueButton=nullptr;
    QBoxLayout *footerLayout=nullptr;
    bool decided=false;
    void decide(bool accepted);
};

// Compatibility wrapper for explicit standalone previews only.
class StartupNoticeDialog final : public QDialog {
public:
    explicit StartupNoticeDialog(QWidget *parent=nullptr);
protected:
    void showEvent(QShowEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    StartupNoticePage *page=nullptr;
    void fitAvailableScreen();
};

bool showStartupNotice(QWidget *parent=nullptr);
