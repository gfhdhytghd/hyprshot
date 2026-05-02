#pragma once

#include <QLabel>
#include <QMargins>
#include <QPoint>
#include <QTimer>

class ResultThumbnail final : public QLabel {
    Q_OBJECT

  public:
    ResultThumbnail(const QPixmap& pixmap, QString path, int timeoutMs, QWidget* parent = nullptr);

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    bool openPath(const QString& path);

    QString m_path;
    QPoint  m_pressGlobal;
    QPoint  m_dragStart;
    QMargins m_dragMargins;
    bool    m_dragMoved = false;
    QTimer  m_closeTimer;
};
