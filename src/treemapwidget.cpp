#include "treemapwidget.h"
#include "treemaplayout.h"
#include "linuxutils.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>
#include <cmath>

using LinuxUtils::humanBytesMiB;

TreemapWidget::TreemapWidget(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true); // tooltips on hover need move events without buttons
}

void TreemapWidget::setTitle(const QString &title) { title_ = title; update(); }
void TreemapWidget::setEmptyText(const QString &text) { emptyText_ = text; update(); }

void TreemapWidget::setData(const QVector<Block> &blocks, double totalCapacityMiB) {
    blocks_ = blocks;
    total_ = totalCapacityMiB;
    hover_ = -1;
    update();
}

// Non-muted blocks share the highlight hue with stepped lightness so
// neighbors stay distinguishable; hue can be NaN for some palettes, where
// setHslF falls back to a gray.
static QColor blockColor(const QPalette &pal, int step) {
    const QColor base = pal.color(QPalette::Highlight);
    static const double lightness[] = {0.42, 0.52, 0.34, 0.58, 0.46, 0.38};
    QColor c;
    c.setHslF(base.hueF(), static_cast<float>(qMax(0.30, base.saturationF() * 0.8)),
              static_cast<float>(lightness[step % 6]));
    return c;
}

void TreemapWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Header: bold title on the left, used/total summary on the right.
    QFont bold = font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() + 1.5);
    const QFontMetricsF boldFm(bold);
    const qreal headerH = boldFm.height() + 6;

    bool haveCapacity = std::isfinite(total_) && total_ > 0.0;
    // Free space is capacity, not usage: excluded from the used summary.
    double used = 0.0;
    for (const auto &b : blocks_)
        if (!b.freeSpace && std::isfinite(b.value) && b.value > 0.0) used += b.value;
    if (haveCapacity) used = std::min(used, total_);

    QString summary;
    if (haveCapacity) {
        summary = QStringLiteral("%1 / %2 · %3% used")
                      .arg(humanBytesMiB(used), humanBytesMiB(total_))
                      .arg(used / total_ * 100.0, 0, 'f', 0);
    }
    if (!title_.isEmpty()) {
        p.setFont(bold);
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(QRectF(0, 0, width(), headerH), Qt::AlignVCenter | Qt::AlignLeft, title_);
    }
    if (!summary.isEmpty()) {
        QFont plain = font();
        plain.setPointSizeF(plain.pointSizeF() - 0.5);
        p.setFont(plain);
        const QFontMetricsF plainFm(plain);
        if (boldFm.horizontalAdvance(title_) + 12.0 + plainFm.horizontalAdvance(summary) <= width()) {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(QRectF(0, 0, width(), headerH), Qt::AlignVCenter | Qt::AlignRight, summary);
        }
    }

    const QRectF panel = QRectF(rect()).adjusted(0, headerH, 0, 0);
    p.setPen(Qt::NoPen);
    p.setBrush(palette().color(QPalette::AlternateBase));
    p.drawRoundedRect(panel, 8, 8);

    if (!haveCapacity || blocks_.isEmpty()) {
        if (!emptyText_.isEmpty()) {
            p.setFont(font());
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(panel, Qt::AlignCenter, emptyText_);
        }
        rects_.clear();
        return;
    }

    const QRectF area = panel.adjusted(8, 8, -8, -8);
    // RSS sums double-count shared pages and can exceed physical capacity;
    // renormalize proportionally so the blocks always tile exactly the panel.
    double blockSum = 0.0;
    for (const auto &b : blocks_)
        if (std::isfinite(b.value) && b.value > 0.0) blockSum += b.value;
    const double scale = blockSum > 0.0 && blockSum > total_ ? total_ / blockSum : 1.0;
    const double pxScale = area.width() * area.height() / total_; // MiB -> px²
    QVector<double> areas;
    areas.reserve(blocks_.size());
    for (const auto &b : blocks_) {
        const double v = std::isfinite(b.value) && b.value > 0.0 ? b.value * scale : -1.0;
        areas.push_back(v > 0.0 ? v * pxScale : -1.0);
    }
    rects_ = squarifiedTreemap(areas, area);

    p.save();
    p.setClipRect(panel); // nothing may paint outside the rounded panel

    const QColor gap = palette().color(QPalette::AlternateBase);
    QFontMetrics fm(font());
    const qreal pad = 5.0;
    int step = 0;
    for (qsizetype i = 0; i < blocks_.size(); ++i) {
        const QRectF r = rects_[i];
        if (!r.isValid() || r.width() < 1.0 || r.height() < 1.0) continue;
        const QColor fill = blocks_[i].freeSpace ? QColor(Qt::white)
                            : blocks_[i].muted   ? palette().color(QPalette::Mid)
                                                 : blockColor(palette(), step++);
        p.setFont(font());
        // Free space is white on a light panel: outline it so the block
        // boundary stays visible.
        p.setPen(QPen(blocks_[i].freeSpace ? palette().color(QPalette::Mid) : gap, 1));
        p.setBrush(fill);
        p.drawRect(r);
        if (i == hover_) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(palette().color(QPalette::WindowText), 2));
            p.drawRect(r.adjusted(1, 1, -1, -1));
        }
        // Labels only where they fit: full name + size, or just the name.
        if (r.width() > 2 * pad + 8 && r.height() > 2 * pad + fm.height()) {
            const bool twoLines = r.height() >= 2 * pad + 2 * fm.height() + 2;
            p.setFont(font());
            p.setPen(fill.lightnessF() > 0.55 ? QColor(Qt::black) : QColor(Qt::white));
            if (twoLines) {
                p.drawText(QRectF(r.left() + pad, r.top() + pad, r.width() - 2 * pad, fm.height()),
                           Qt::AlignLeft | Qt::AlignTop,
                           fm.elidedText(blocks_[i].label, Qt::ElideRight,
                                         static_cast<int>(r.width() - 2 * pad)));
                p.drawText(QRectF(r.left() + pad, r.top() + pad + fm.height(), r.width() - 2 * pad, fm.height()),
                           Qt::AlignLeft | Qt::AlignTop, humanBytesMiB(blocks_[i].value));
            } else {
                p.drawText(QRectF(r.left() + pad, r.top() + pad, r.width() - 2 * pad, fm.height()),
                           Qt::AlignLeft | Qt::AlignTop,
                           fm.elidedText(blocks_[i].label, Qt::ElideRight,
                                         static_cast<int>(r.width() - 2 * pad)));
            }
        }
    }
    p.restore();
}

void TreemapWidget::mouseMoveEvent(QMouseEvent *e) {
    int hit = -1;
    for (qsizetype i = 0; i < rects_.size(); ++i)
        if (rects_[i].contains(e->position())) { hit = static_cast<int>(i); break; }
    if (hit != hover_) { hover_ = hit; update(); }
    if (hit >= 0 && std::isfinite(total_) && total_ > 0.0) {
        const auto &b = blocks_[hit];
        QToolTip::showText(e->globalPosition().toPoint(),
            QStringLiteral("%1\n%2 · %3% of capacity")
                .arg(b.label, humanBytesMiB(b.value))
                .arg(b.value / total_ * 100.0, 0, 'f', 1),
            this);
    }
}

void TreemapWidget::leaveEvent(QEvent *) {
    if (hover_ != -1) { hover_ = -1; update(); }
}
