#pragma once

#include <QPoint>
#include <QPointF>
#include <QTimer>
#include <QWidget>

class QLabel;
class QPixmap;
class QWidget;
class SwipeBackdrop;

class ResultThumbnail final : public QWidget {
    Q_OBJECT

  public:
    ResultThumbnail(const QPixmap& pixmap, QString path, QString restoreClipboardPath, QString deleteRoot, int timeoutMs, QWidget* parent = nullptr);

  protected:
    void closeEvent(QCloseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    enum class SwipeAction { Close, Delete };

    bool openPath(const QString& path);
    void toggleMenu();
    void applyLayerSize();
    void startFileDrag();
    void updateSwipeVisual();
    void resetSwipe();
    void animateSwipeOut(SwipeAction action);
    void deleteAndClose();
    void restoreClipboard() const;

    QString m_path;
    QString m_restoreClipboardPath;
    QString m_deleteRoot;
    QWidget* m_card = nullptr;
    SwipeBackdrop* m_swipeBackdrop = nullptr;
    QLabel* m_imageLabel = nullptr;
    QWidget* m_menuPanel = nullptr;
    QPoint  m_pressGlobal;
    QPoint  m_imageOrigin;
    QPointF m_swipeOffset;
    bool    m_dragMoved = false;
    bool    m_draggingFile = false;
    bool    m_swipeCompleting = false;
    QTimer  m_closeTimer;
    QTimer  m_swipeResetTimer;
};
