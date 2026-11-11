#pragma once

#include <QJsonObject>
#include <QTextDocument>
#include <QUrl>
#include <QWidget>
#include <functional>

class QLabel;
class QPushButton;
class HudPagedTextCanvas;

// A document is laid out in real pages. Navigation changes the painted page,
// rather than hiding a scroll bar on a partially visible scrolling document.
class HudPagedText final : public QWidget {
public:
    explicit HudPagedText(QWidget *parent=nullptr);
    void setHtml(const QString &html);
    QString toPlainText() const;
    QTextDocument *document();
    const QTextDocument *document() const;
    QWidget *viewport() const;
    void setPage(int page);
    int pageIndex() const;
    int pageCount() const;
    QJsonObject report() const;

    std::function<void(QUrl)> onAnchor,onHighlighted;

private:
    friend class HudPagedTextCanvas;
    QTextDocument text;
    HudPagedTextCanvas *canvas=nullptr;
    QWidget *pager=nullptr;
    QPushButton *previous=nullptr,*next=nullptr;
    QLabel *position=nullptr;
    QString html;
    int page=0,pages=1;
    bool layingOut=false;

    void layoutDocument();
    void syncNavigation();
    QPointF documentPoint(const QPointF &point) const;
    QUrl anchorAt(const QPointF &point) const;
    QString pageText(int index) const;
};
