#pragma once
#include <QJsonObject>
#include <QString>
#include <QRect>

class QWidget;

// Input coverage and optional passive self-tooltip reader settings. Unchanged
// settings return empty; the owner starts readers after an explicit change.
QJsonObject editGrowthCaptureSettings(QWidget *parent,const QJsonObject &status,const QRect &gameViewport={},qreal gameViewportScale=0);
QString growthCaptureStatusText(const QJsonObject &status);
