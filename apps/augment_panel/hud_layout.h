#pragma once
#include <QJsonObject>
#include <QMap>
#include <QRect>
#include <QStringList>
class QSettings;

// Only explicitly edited elements override the automatic, phase-aware layout.
class HudLayout {
public:
    static QStringList keys();
    static QString title(const QString &key);
    static QSize minimumSize(const QString &key);
    static HudLayout load(QSettings &);
    bool save(QSettings &) const;
    bool contains(const QString &key) const {return overrides.contains(key);}
    bool empty() const {return overrides.isEmpty();}
    bool operator==(const HudLayout &) const = default;
    QRect resolve(const QString &key,const QRect &viewport,const QRect &fallback) const;
    void set(const QString &key,const QRect &bounds,const QRect &viewport);
    void reset(const QString &key) {overrides.remove(key);}
    QJsonObject json() const;
private:
    QMap<QString,QRectF> overrides;
};
