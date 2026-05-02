#pragma once

#include <QLabel>
#include <QPoint>
#include <QTimer>

class ResultThumbnail final : public QLabel {
    Q_OBJECT

  public:
    ResultThumbnail(const QPixmap& pixmap, QString path, int timeoutMs, QWidget* parent = nullptr);

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    QString m_path;
    QPoint  m_dragStart;
    QTimer  m_closeTimer;
};
