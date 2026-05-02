#pragma once

#include <QMargins>
#include <QPoint>
#include <QTimer>
#include <QWidget>

class QLabel;
class QPixmap;
class QWidget;

class ResultThumbnail final : public QWidget {
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
    void toggleMenu();
    void applyLayerSize();

    QString m_path;
    QLabel* m_imageLabel = nullptr;
    QWidget* m_menuPanel = nullptr;
    QPoint  m_pressGlobal;
    QPoint  m_dragStart;
    QMargins m_dragMargins;
    bool    m_dragMoved = false;
    QTimer  m_closeTimer;
};
