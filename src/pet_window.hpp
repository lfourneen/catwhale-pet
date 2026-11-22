#pragma once

#include "config.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QQuickWindow>
#include <QTimer>

namespace catwhale {

class PetItem;

class PetWindow final : public QQuickWindow {
    Q_OBJECT
public:
    PetWindow();
    void present(const QImage &frame, const Clip &clip);
    void setPetSize(int height);
    void setShadow(bool enabled);
    void setAgentBadge(int count, const QString &state);
    bool dragInProgress() const { return dragging_; }

signals:
    void clicked();
    void dragStarted();
    void dragFinished();
    void menuRequested(QPoint position);
    void panelToggled();
    void agentDockRequested();
    void targetSizeChanged(QSize pixels);

protected:
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void updateMask(bool force);
    void finishDrag();
    void refreshSize();

    PetItem *shadowItem_;
    PetItem *item_;
    PetItem *badgeItem_;
    QRect badgeRect_;
    QImage frame_;
    Clip clip_;
    QPointF press_;
    QElapsedTimer maskUpdated_;
    QTimer endDrag_;
    int height_ = 340;
    bool armed_ = false;
    bool dragging_ = false;
    bool dragLeft_ = false;
};

bool hasVulkanDevice();
bool hasOpenGlDevice();

} // namespace catwhale
