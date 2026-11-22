#include "pet_window.hpp"

#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QQuickItem>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <QVulkanFunctions>
#include <QVulkanInstance>

#include <cmath>
#include <memory>
#include <vector>

namespace catwhale {
namespace {

class FrameNode final : public QSGSimpleTextureNode {
public:
    std::unique_ptr<QSGTexture> image;
    qint64 imageKey = 0;
};

QRectF clipRect(const QSize &size, const Clip &clip)
{
    const qreal w = size.width();
    const qreal h = size.height();
    const qreal foot = h - qMax<qreal>(4, h * 0.024);
    const QSizeF target(w * clip.scale * clip.scaleX, h * clip.scale * clip.scaleY);
    return {
        (w - target.width()) / 2 + w * clip.offsetX / 100,
        foot - target.height() * (1450.0 / 1500.0) + h * clip.offsetY / 100,
        target.width(), target.height()
    };
}

QImage shadowImage()
{
    QImage image(128, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    QRadialGradient gradient(QPointF(64, 64), 64);
    gradient.setColorAt(0, QColor(0, 0, 0, 75));
    gradient.setColorAt(1, Qt::transparent);
    painter.scale(1, 24.0 / 128);
    painter.fillRect(QRectF(0, 0, 128, 128), gradient);
    return image;
}

QRegion clickablePixels(const QImage &image, const QRectF &rect, const QSize &logicalSize)
{
    if (image.isNull() || logicalSize.isEmpty())
        return QRegion(QRect(-2, -2, 1, 1));
    QImage scaled(logicalSize, QImage::Format_ARGB32_Premultiplied);
    scaled.fill(Qt::transparent);
    {
        QPainter painter(&scaled);
        painter.drawImage(rect, image);
    }
    QRegion region;
    constexpr int cell = 4;
    constexpr int threshold = 84;
    for (int y = 0; y < scaled.height(); y += cell) {
        const auto *row = reinterpret_cast<const QRgb *>(
            scaled.constScanLine(qMin(y + cell / 2, scaled.height() - 1)));
        int start = -1;
        for (int x = 0; x < scaled.width(); x += cell) {
            const bool opaque = qAlpha(row[qMin(x + cell / 2, scaled.width() - 1)]) >= threshold;
            if (opaque && start < 0)
                start = x;
            if (!opaque && start >= 0) {
                region += QRect(start, y, x - start, qMin(cell, scaled.height() - y));
                start = -1;
            }
        }
        if (start >= 0)
            region += QRect(start, y, scaled.width() - start, qMin(cell, scaled.height() - y));
    }
    // An empty Qt mask restores the default full input area.
    return region.isEmpty() ? QRegion(QRect(-2, -2, 1, 1)) : region;
}

} // namespace

class PetItem final : public QQuickItem {
public:
    explicit PetItem(QQuickItem *parent) : QQuickItem(parent)
    {
        setFlag(ItemHasContents, true);
    }

    void setImage(QImage image)
    {
        image_ = std::move(image);
        update();
    }

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override
    {
        auto node = std::unique_ptr<FrameNode>(static_cast<FrameNode *>(old));
        if (image_.isNull())
            return nullptr;
        if (!node)
            node = std::make_unique<FrameNode>();
        if (node->imageKey != image_.cacheKey()) {
            std::unique_ptr<QSGTexture> texture(window()->createTextureFromImage(image_,
                QQuickWindow::TextureHasAlphaChannel));
            if (!texture)
                return nullptr;
            texture->setFiltering(QSGTexture::Linear);
            node->setTexture(texture.get());
            node->image = std::move(texture);
            node->imageKey = image_.cacheKey();
        }
        node->setRect(boundingRect());
        return node.release();
    }

private:
    QImage image_;
};

PetWindow::PetWindow()
    : shadowItem_(new PetItem(contentItem())), item_(new PetItem(contentItem())),
      badgeItem_(new PetItem(contentItem()))
{
    setTitle(QStringLiteral("Catwhale Pet"));
    setFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setColor(Qt::transparent);
    shadowItem_->setImage(shadowImage());
    endDrag_.setSingleShot(true);
    connect(&endDrag_, &QTimer::timeout, this, &PetWindow::finishDrag);
    connect(this, &QWindow::screenChanged, this, &PetWindow::refreshSize);
    setPetSize(height_);
    updateMask(true);
}

void PetWindow::setPetSize(int height)
{
    height_ = std::clamp(height, 200, 620);
    resize(qRound(height_ * 0.6), height_);
    refreshSize();
}

void PetWindow::refreshSize()
{
    const qreal shadowWidth = width() * 0.44;
    shadowItem_->setSize(QSizeF(shadowWidth, shadowWidth * 24 / 128));
    shadowItem_->setPosition(QPointF(width() * 0.25, height() * 0.94));
    const auto target = (QSizeF(size()) * devicePixelRatio()).toSize();
    emit targetSizeChanged(target);
    present(frame_, clip_);
    updateMask(true);
}

void PetWindow::setShadow(bool enabled)
{
    shadowItem_->setVisible(enabled);
}

void PetWindow::setAgentBadge(int count, const QString &state)
{
    if (count <= 0) {
        badgeRect_ = {};
        badgeItem_->setVisible(false);
        updateMask(true);
        return;
    }
    const QString text = tr("Agents: %1").arg(count);
    const QFont font = QGuiApplication::font();
    const int width = QFontMetrics(font).horizontalAdvance(text) + 32;
    badgeRect_ = QRect(4, 5, width, 24);
    QImage badge(badgeRect_.size(), QImage::Format_ARGB32_Premultiplied);
    badge.fill(Qt::transparent);
    QPainter painter(&badge);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor("#796c55"));
    painter.setBrush(QColor(29, 27, 35, 230));
    painter.drawRoundedRect(badge.rect().adjusted(1, 1, -1, -1), 6, 6);
    painter.setPen(Qt::NoPen);
    QColor color("#aac6eb");
    if (state == "error" || state == "waiting_user")
        color = QColor("#ffc38f");
    else if (state == "working" || state == "speaking")
        color = QColor("#a5e0b9");
    painter.setBrush(color);
    painter.drawEllipse(QPointF(12, 12), 3, 3);
    painter.setFont(font);
    painter.setPen(QColor("#fff5dd"));
    painter.drawText(QRect(22, 0, width - 25, 24), Qt::AlignVCenter | Qt::AlignLeft, text);
    painter.end();
    badgeItem_->setPosition(badgeRect_.topLeft());
    badgeItem_->setSize(badgeRect_.size());
    badgeItem_->setImage(std::move(badge));
    badgeItem_->setVisible(true);
    updateMask(true);
}

void PetWindow::present(const QImage &frame, const Clip &clip)
{
    if (frame.isNull())
        return;
    frame_ = frame;
    clip_ = clip;
    const auto rect = clipRect(size(), clip_);
    item_->setPosition(rect.topLeft());
    item_->setSize(rect.size());
    item_->setImage(frame_);
    updateMask(false);
}

void PetWindow::updateMask(bool force)
{
    if (dragging_ || (!force && maskUpdated_.isValid() && maskUpdated_.elapsed() < 100))
        return;
    maskUpdated_.restart();
    const auto region = clickablePixels(frame_, clipRect(size(), clip_), size()) | badgeRect_;
    if (region != mask())
        setMask(region);
}

bool PetWindow::event(QEvent *event)
{
    if (event->type() == QEvent::Leave && dragging_)
        dragLeft_ = true;
    if (event->type() == QEvent::Enter && dragging_ && dragLeft_)
        finishDrag();
    if (event->type() == QEvent::DevicePixelRatioChange)
        refreshSize();
    if (event->type() == QEvent::Hide) {
        armed_ = false;
        finishDrag();
    }
    return QQuickWindow::event(event);
}

void PetWindow::mousePressEvent(QMouseEvent *event)
{
    if (dragging_)
        finishDrag();
    if (event->button() == Qt::LeftButton && badgeRect_.contains(event->position().toPoint())) {
        armed_ = false;
        emit agentDockRequested();
        event->accept();
    } else if (event->button() == Qt::LeftButton) {
        armed_ = mask().contains(event->position().toPoint());
        press_ = event->position();
        event->accept();
    } else if (event->button() == Qt::RightButton) {
        emit menuRequested(event->globalPosition().toPoint());
        event->accept();
    } else {
        QQuickWindow::mousePressEvent(event);
    }
}

void PetWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (dragging_ && !(event->buttons() & Qt::LeftButton)) {
        finishDrag();
        return;
    }
    if (!armed_ || !(event->buttons() & Qt::LeftButton)
        || (event->position() - press_).manhattanLength() < QGuiApplication::styleHints()->startDragDistance())
        return;
    armed_ = false;
    if (!startSystemMove())
        return;
    dragging_ = true;
    dragLeft_ = false;
    // A compositor grab can consume the release. Re-entry or a timeout clears the pose.
    endDrag_.start(3000);
    emit dragStarted();
}

void PetWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (dragging_)
        finishDrag();
    else if (armed_)
        emit clicked();
    armed_ = false;
    event->accept();
}

void PetWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat())
        emit clicked();
    else if (event->key() == Qt::Key_Menu)
        emit panelToggled();
    else
        QQuickWindow::keyPressEvent(event);
}

void PetWindow::finishDrag()
{
    if (!dragging_)
        return;
    dragging_ = false;
    endDrag_.stop();
    updateMask(true);
    emit dragFinished();
}

bool hasVulkanDevice()
{
#if QT_CONFIG(vulkan)
    QVulkanInstance instance;
    instance.setFlags(QVulkanInstance::NoDebugOutputRedirect);
    if (!instance.create())
        return false;

    auto *functions = instance.functions();
    uint32_t count = 0;
    if (functions->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, nullptr) != VK_SUCCESS || !count)
        return false;
    std::vector<VkPhysicalDevice> devices(count);
    if (functions->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, devices.data()) != VK_SUCCESS)
        return false;
    for (const auto device : devices) {
        VkPhysicalDeviceProperties properties{};
        functions->vkGetPhysicalDeviceProperties(device, &properties);
        const auto type = properties.deviceType;
        if (type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
            || type == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU)
            return true;
    }
#endif
    return false;
}

bool hasOpenGlDevice()
{
#if QT_CONFIG(opengl)
    QOpenGLContext context;
    if (!context.create())
        return false;
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface))
        return false;
    const auto *raw = context.functions()->glGetString(GL_RENDERER);
    const auto renderer = QByteArray(raw ? reinterpret_cast<const char *>(raw) : "").toLower();
    context.doneCurrent();
    return !renderer.isEmpty() && !renderer.contains("llvmpipe") && !renderer.contains("softpipe")
        && !renderer.contains("swiftshader");
#else
    return false;
#endif
}

} // namespace catwhale
