#pragma once

#include <QJsonObject>
#include <QList>
#include <QSize>
#include <QString>
#include <QWidget>
#include <functional>

class QCloseEvent;
class QComboBox;
class QLabel;
class QPushButton;
class UiBrowserExplanation;

struct UiBrowserScene {
    QString id;
    QString title;
    QString description;
};

const QList<UiBrowserScene> &uiBrowserScenes();

// Controls a separate synthetic presentation instance. This window has no
// settings, game client, model worker or network ownership.
class UiBrowserWindow final : public QWidget {
public:
    explicit UiBrowserWindow(QWidget *parent=nullptr);
    bool selectScene(const QString &id);
    QString sceneId() const;
    QSize viewportSize() const;
    void setExplanation(const QString &text);
    QJsonObject report() const;

    std::function<void(QString)> onScene;
    std::function<void(QSize)> onViewport;
    std::function<void()> onReplay,onQuit;

protected:
    void closeEvent(QCloseEvent *) override;

private:
    QComboBox *scenes=nullptr,*viewports=nullptr;
    QLabel *sceneDescription=nullptr,*positionLabel=nullptr,*explanationPosition=nullptr;
    UiBrowserExplanation *explanation=nullptr;
    QPushButton *previous=nullptr,*next=nullptr,*replay=nullptr,*quit=nullptr;
    QPushButton *explanationPrevious=nullptr,*explanationNext=nullptr;
    QList<QWidget *> interactive;
    bool quitting=false;
    void refreshScene();
    void refreshExplanationControls();
    void step(int delta);
};
