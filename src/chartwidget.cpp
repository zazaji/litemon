#include "chartwidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QDateTime>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>

namespace {

constexpr int kMarginLeft = 14;
constexpr int kMarginRight = 14;
constexpr int kMarginTop = 6;
constexpr int kTitleRowHeight = 22;
constexpr int kOverflowRowHeight = 20;
constexpr int kGapLegendPlot = 6;
constexpr int kYLabelWidth = 52;
constexpr int kPlotRight = 16;
constexpr int kXLabelHeight = 20;
constexpr int kPlotBottom = 6;

QList<QColor> seriesColors(const QPalette &pal) {
    return {
        pal.color(QPalette::Highlight),
        QColor(219, 93, 93),
        QColor(70, 170, 120),
        QColor(166, 109, 212),
        QColor(232, 150, 58),
        QColor(66, 148, 195),
    };
}

QString rangeLabel(qint64 spanSec) {
    if (spanSec > 172800) { return QStringLiteral("MM-dd"); }
    return QStringLiteral("HH:mm");
}

struct LegendItem { QString name; int width; int seriesIndex; };
struct LegendRow { QList<LegendItem> items; int xOffset; };

// All layout numbers (header rows, plot rect, axis scales) computed in one
// place so painting and mouse hit-testing can never drift apart.
struct Layout {
    bool hasData = false;
    QString titleElided;
    int headerAvailForTitle = 0;
    int unitWidth = 0;
    QList<LegendRow> legendRows;
    int hiddenSeries = 0;
    int moreWidth = 0;
    int headerHeight = kTitleRowHeight;
    QRectF plot;
    qint64 minTs = 0, maxTs = 1;
    double minY = 0, maxY = 1;
    int decimals = 0;
    QString timeFmt = QStringLiteral("HH:mm");
};

Layout computeLayout(const QVector<ChartWidget::Series> &series, const QString &title,
                     const QString &unit, bool fixedRange, double fixedMin, double fixedMax,
                     const QFont &baseFontIn, int w, int h) {
    Layout lay;

    const QFont baseFont = baseFontIn;
    const QFontMetrics fm(baseFont);
    const int contentWidth = qMax(0, w - kMarginLeft - kMarginRight);

    // Title font metrics (geometry only; the painter re-derives the same font).
    QFont titleFont = baseFont;
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.5);
    const QFontMetrics titleFm(titleFont);

    const int unitWidth = unit.isEmpty() ? 0 : (titleFm.horizontalAdvance(unit) + 18);
    const int gapAfterTitle = 14; // space between title text and first legend item
    const int headerAvailForTitle = qMax(0, contentWidth - unitWidth - (unitWidth > 0 ? 8 : 0));
    const QString titleElided = titleFm.elidedText(title, Qt::ElideRight, qMax(10, headerAvailForTitle));
    const int titleDrawnWidth = titleFm.horizontalAdvance(titleElided) + gapAfterTitle;

    // Wrap legend items into rows. Row 0 shares space with the title (leaving
    // room for the unit badge on the right); overflow rows span the full width.
    const int seriesCount = static_cast<int>(series.size());
    int legendDrawn = 0;
    if (seriesCount > 0) {
        const int firstRowAvail = contentWidth - titleDrawnWidth - unitWidth - (unitWidth > 0 ? 8 : 0);
        const bool shareFirstRow = (firstRowAvail >= 60);
        QList<LegendItem> currentRow;
        int rowAvail = shareFirstRow ? firstRowAvail : contentWidth;
        int rowUsed = 0;
        const int firstXOffset = shareFirstRow ? titleDrawnWidth : 0;
        int si = 0;
        for (; si < seriesCount; ++si) {
            const auto &s = series.at(si);
            const int itemW = 8 + 4 + fm.horizontalAdvance(s.name) + 14;
            if (!currentRow.isEmpty() && rowUsed + itemW > rowAvail) {
                lay.legendRows.append({currentRow, lay.legendRows.isEmpty() ? firstXOffset : 0});
                currentRow.clear();
                rowAvail = contentWidth;
                rowUsed = 0;
                if (lay.legendRows.size() >= 3) { break; }
            }
            currentRow.append({s.name, itemW, si});
            rowUsed += itemW;
        }
        if (!currentRow.isEmpty() && lay.legendRows.size() < 3) {
            lay.legendRows.append({currentRow, lay.legendRows.isEmpty() ? firstXOffset : 0});
        }
        legendDrawn = si;
    }
    lay.hiddenSeries = seriesCount - legendDrawn;
    const int extraRows = static_cast<int>(lay.legendRows.size()) - 1;
    lay.headerHeight = kTitleRowHeight + qMax(0, extraRows) * kOverflowRowHeight;

    lay.titleElided = titleElided;
    lay.headerAvailForTitle = headerAvailForTitle;
    lay.unitWidth = unitWidth;

    const double plotTop = static_cast<double>(kMarginTop + lay.headerHeight + kGapLegendPlot);
    const double plotLeft = static_cast<double>(kMarginLeft + kYLabelWidth);
    const double plotRight = static_cast<double>(w - kPlotRight);
    const double plotBottom = static_cast<double>(h - kPlotBottom - kXLabelHeight);
    lay.plot = QRectF(plotLeft, plotTop, qMax(0.0, plotRight - plotLeft), qMax(0.0, plotBottom - plotTop));

    qint64 minTs = LLONG_MAX, maxTs = LLONG_MIN;
    double minY = std::numeric_limits<double>::infinity(), maxY = -std::numeric_limits<double>::infinity();
    for (const auto &s : series) for (const auto &pt : s.points) if (std::isfinite(pt.value)) {
        minTs = std::min(minTs, pt.ts);
        maxTs = std::max(maxTs, pt.ts);
        minY = std::min(minY, pt.value);
        maxY = std::max(maxY, pt.value);
    }
    if (minTs == LLONG_MAX) { return lay; } // no data; plot rect is still valid
    if (maxTs <= minTs) { maxTs = minTs + 1; }
    if (fixedRange) {
        minY = fixedMin;
        maxY = fixedMax;
    } else {
        if (!std::isfinite(minY) || !std::isfinite(maxY)) { minY = 0; maxY = 1; }
        if (std::abs(maxY - minY) < 1e-9) { maxY = minY + 1; }
        const double pad = (maxY - minY) * 0.1;
        minY = std::max(0.0, minY - pad);
        maxY += pad;
    }
    lay.hasData = true;
    lay.minTs = minTs;
    lay.maxTs = maxTs;
    lay.minY = minY;
    lay.maxY = maxY;
    lay.decimals = (maxY - minY) < 10.0 ? 1 : 0;
    lay.timeFmt = rangeLabel(maxTs - minTs);
    return lay;
}

// Nearest sampled timestamp (across all series) to a widget-space x position.
qint64 nearestTimestamp(const QVector<ChartWidget::Series> &series, const Layout &lay, double widgetX) {
    qint64 best = lay.minTs;
    double bestDist = std::numeric_limits<double>::infinity();
    for (const auto &s : series) {
        for (const auto &pt : s.points) {
            if (!std::isfinite(pt.value)) { continue; }
            const double px = lay.plot.left() + static_cast<double>(pt.ts - lay.minTs) * lay.plot.width()
                / static_cast<double>(lay.maxTs - lay.minTs);
            const double d = std::abs(px - widgetX);
            if (d < bestDist) { bestDist = d; best = pt.ts; }
        }
    }
    return best;
}

// Vertical crosshair line at ts, a marker per series, and a value box with
// the timestamp (x) and each series' value (y) at that moment.
void drawCrosshair(QPainter &p, const Layout &lay, const QVector<ChartWidget::Series> &series,
                   const QString &unit, const QPalette &pal, const QList<QColor> &colors,
                   qint64 ts) {
    if (ts < lay.minTs || ts > lay.maxTs) { return; }
    const double x = lay.plot.left() + static_cast<double>(ts - lay.minTs) * lay.plot.width()
        / static_cast<double>(lay.maxTs - lay.minTs);
    QPen linePen(pal.color(QPalette::PlaceholderText));
    linePen.setWidthF(1.2);
    p.setPen(linePen);
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(x, lay.plot.top()), QPointF(x, lay.plot.bottom()));

    struct TipRow { QColor color; QString text; };
    QList<TipRow> tipRows;
    const bool simpleUnit = !unit.isEmpty() && !unit.contains('/');
    int si = 0;
    for (const auto &s : series) {
        for (const auto &pt : s.points) {
            if (pt.ts != ts || !std::isfinite(pt.value)) { continue; }
            QString text = s.name + QStringLiteral(": ") + QString::number(pt.value, 'f', lay.decimals);
            if (simpleUnit) { text += QStringLiteral(" ") + unit; }
            tipRows.append({colors.at(si % static_cast<int>(colors.size())), text});
            const double y = lay.plot.bottom() - (pt.value - lay.minY) * lay.plot.height()
                / (lay.maxY - lay.minY);
            p.setPen(QPen(pal.color(QPalette::Base), 1.2));
            p.setBrush(colors.at(si % static_cast<int>(colors.size())));
            p.drawEllipse(QPointF(x, y), 3.5, 3.5);
            break;
        }
        ++si;
    }
    if (tipRows.isEmpty()) { return; }

    const QFontMetrics fm(p.font());
    const QString header = QDateTime::fromSecsSinceEpoch(ts).toString(QStringLiteral("HH:mm:ss"));
    qreal textW = fm.horizontalAdvance(header);
    for (const auto &r : tipRows) { textW = qMax(textW, static_cast<qreal>(fm.horizontalAdvance(r.text)) + 12.0); }
    const qreal padX = 8.0, padY = 6.0;
    const qreal lineH = static_cast<qreal>(fm.height()) + 4.0;
    const qreal boxW = textW + 2.0 * padX;
    const qreal boxH = static_cast<qreal>(tipRows.size() + 1) * lineH + 2.0 * padY;
    qreal bx = x + 10.0;
    if (bx + boxW > lay.plot.right()) { bx = x - 10.0 - boxW; }
    bx = qBound(lay.plot.left(), bx, qMax(lay.plot.left(), lay.plot.right() - boxW));
    const qreal by = lay.plot.top() + 6.0;

    QColor bg = pal.color(QPalette::Base);
    bg.setAlpha(235);
    p.setPen(QPen(pal.color(QPalette::Mid), 1.0));
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(bx, by, boxW, boxH), 4.0, 4.0);

    p.setPen(pal.color(QPalette::PlaceholderText));
    p.drawText(QRectF(bx + padX, by + padY, textW, lineH), Qt::AlignLeft | Qt::AlignVCenter, header);
    qreal ty = by + padY + lineH;
    for (const auto &r : tipRows) {
        p.setPen(Qt::NoPen);
        p.setBrush(r.color);
        p.drawEllipse(QRectF(bx + padX, ty + (lineH - 7.0) / 2.0, 7.0, 7.0));
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRectF(bx + padX + 11.0, ty, textW - 11.0, lineH), Qt::AlignLeft | Qt::AlignVCenter, r.text);
        ty += lineH;
    }
}

} // namespace

ChartWidget::ChartWidget(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(150);
    setAutoFillBackground(false);
    setMouseTracking(true); // hover crosshair
    setFocusPolicy(Qt::ClickFocus); // Escape clears the pinned crosshair
}

void ChartWidget::setTitle(const QString &title, const QString &unit) { title_=title; unit_=unit; update(); }
void ChartWidget::setSeries(const QVector<Series> &series) { series_=series; update(); }
void ChartWidget::setFixedYRange(double min, double max) { fixedRange_=true; fixedMin_=min; fixedMax_=max; update(); }
void ChartWidget::clearFixedYRange() { fixedRange_=false; update(); }

void ChartWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    const QFont baseFont = font();
    const QFontMetrics fm(baseFont);

    p.fillRect(rect(), pal.color(QPalette::Base));

    const Layout lay = computeLayout(series_, title_, unit_, fixedRange_, fixedMin_, fixedMax_, baseFont, width(), height());
    const QList<QColor> colors = seriesColors(pal);

    // Title on the left of the first row.
    QFont titleFont = baseFont;
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.5);
    p.setFont(titleFont);
    p.setPen(pal.color(QPalette::Text));
    p.drawText(QRectF(static_cast<double>(kMarginLeft), static_cast<double>(kMarginTop),
                      static_cast<double>(lay.headerAvailForTitle), static_cast<double>(kTitleRowHeight)),
               Qt::AlignLeft | Qt::AlignVCenter, lay.titleElided);

    // Unit badge on the far right of the first row.
    if (lay.unitWidth > 0) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.setFont(baseFont);
        p.drawText(QRectF(static_cast<double>(width() - kMarginRight - lay.unitWidth), static_cast<double>(kMarginTop),
                          static_cast<double>(lay.unitWidth), static_cast<double>(kTitleRowHeight)),
                   Qt::AlignRight | Qt::AlignVCenter, unit_);
    }

    // Legend items: row 0 inline after the title, the rest from the left.
    p.setFont(baseFont);
    const QString moreText = (lay.hiddenSeries > 0)
        ? tr("+%n more", nullptr, lay.hiddenSeries) : QString();
    const int moreWidth = moreText.isEmpty() ? 0 : (fm.horizontalAdvance(moreText) + 10);
    const int rowCount = static_cast<int>(lay.legendRows.size());
    for (int lineIdx = 0; lineIdx < rowCount; ++lineIdx) {
        const LegendRow &legendRow = lay.legendRows.at(lineIdx);
        const bool isFirst = (lineIdx == 0);
        const bool isLast = (lineIdx == rowCount - 1);
        const int lineHeight = isFirst ? kTitleRowHeight : kOverflowRowHeight;
        const double lineTop = static_cast<double>(kMarginTop)
            + static_cast<double>(isFirst ? 0 : (kTitleRowHeight + (lineIdx - 1) * kOverflowRowHeight));
        double lx = static_cast<double>(kMarginLeft + legendRow.xOffset);
        double rightEdge = static_cast<double>(width() - kMarginRight);
        if (isFirst && lay.unitWidth > 0) { rightEdge -= static_cast<double>(lay.unitWidth + 8); }
        if (isLast && moreWidth > 0) { rightEdge -= static_cast<double>(moreWidth); }
        for (const LegendItem &item : legendRow.items) {
            const double rowAvail = rightEdge - (lx + 12.0);
            if (rowAvail < 10.0) { break; }
            const QString nameElided = fm.elidedText(item.name, Qt::ElideRight, qMax(10, static_cast<int>(rowAvail)));
            p.setPen(Qt::NoPen);
            p.setBrush(colors.at(item.seriesIndex % static_cast<int>(colors.size())));
            const double dotY = lineTop + (static_cast<double>(lineHeight) - 8.0) / 2.0;
            p.drawEllipse(QRectF(lx, dotY, 8.0, 8.0));
            p.setPen(pal.color(QPalette::Text));
            p.drawText(QRectF(lx + 12.0, lineTop, rowAvail, static_cast<double>(lineHeight)),
                       Qt::AlignLeft | Qt::AlignVCenter, nameElided);
            lx += static_cast<double>(item.width);
            if (lx > rightEdge) { break; }
        }
        if (isLast && !moreText.isEmpty()) {
            p.setPen(pal.color(QPalette::PlaceholderText));
            p.drawText(QRectF(rightEdge, lineTop, static_cast<double>(moreWidth), static_cast<double>(lineHeight)),
                       Qt::AlignRight | Qt::AlignVCenter, moreText);
        }
    }
    p.setFont(baseFont);

    const QRectF plot = lay.plot;
    if (plot.right() <= plot.left() + 10.0 || plot.bottom() <= plot.top() + 10.0) { return; }

    // Plot panel background with subtle border.
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::AlternateBase));
    p.drawRoundedRect(plot.adjusted(-8, -8, 8, 8), 8.0, 8.0);
    QPen framePen(pal.color(QPalette::Mid));
    framePen.setWidthF(1.0);
    p.setPen(framePen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(plot.adjusted(-8, -8, 8, 8), 8.0, 8.0);

    if (!lay.hasData) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(plot, Qt::AlignCenter, tr("No historical data yet"));
        return;
    }

    const double minY = lay.minY, maxY = lay.maxY;
    QPen grid(pal.color(QPalette::Mid));
    grid.setWidthF(1.0);
    grid.setStyle(Qt::DotLine);
    for (int i = 0; i <= 4; ++i) {
        const double y = plot.top() + plot.height() * static_cast<double>(i) / 4.0;
        p.setPen(grid);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        const double val = maxY - (maxY - minY) * static_cast<double>(i) / 4.0;
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(QRectF(static_cast<double>(kMarginLeft), y - 10.0, static_cast<double>(kYLabelWidth - 8), 20.0),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number(val, 'f', lay.decimals));
    }
    for (int i = 0; i <= 4; ++i) {
        const double x = plot.left() + plot.width() * static_cast<double>(i) / 4.0;
        p.setPen(grid);
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        const qint64 ts = lay.minTs + (lay.maxTs - lay.minTs) * i / 4;
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(QRectF(x - 50.0, plot.bottom() + 10.0, 100.0, 18.0), Qt::AlignCenter,
                   QDateTime::fromSecsSinceEpoch(ts).toString(lay.timeFmt));
    }

    int si = 0;
    for (const auto &s : series_) {
        QPainterPath path;
        bool started = false;
        for (const auto &pt : s.points) {
            if (!std::isfinite(pt.value)) { continue; }
            const double x = plot.left() + static_cast<double>(pt.ts - lay.minTs) * plot.width()
                / static_cast<double>(lay.maxTs - lay.minTs);
            const double y = plot.bottom() - (pt.value - minY) * plot.height() / (maxY - minY);
            if (!started) { path.moveTo(x, y); started = true; } else { path.lineTo(x, y); }
        }
        QPen pen(colors.at(si % static_cast<int>(colors.size())));
        pen.setWidthF(2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        ++si;
    }

    // Crosshair: a pinned one wins over the transient hover position.
    if (pinned_) {
        drawCrosshair(p, lay, series_, unit_, pal, colors, pinnedTs_);
    } else if (hoverValid_ && hoverX_ >= plot.left() && hoverX_ <= plot.right()) {
        const qint64 ts = nearestTimestamp(series_, lay, hoverX_);
        drawCrosshair(p, lay, series_, unit_, pal, colors, ts);
    }
}

void ChartWidget::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) { return; }
    const Layout lay = computeLayout(series_, title_, unit_, fixedRange_, fixedMin_, fixedMax_, font(), width(), height());
    if (!lay.hasData || !lay.plot.contains(e->position())) {
        if (pinned_) { pinned_ = false; update(); }
        return;
    }
    const qint64 ts = nearestTimestamp(series_, lay, e->position().x());
    if (pinned_ && ts == pinnedTs_) {
        pinned_ = false; // same point again clears the pin
    } else {
        pinned_ = true;
        pinnedTs_ = ts;
    }
    update();
}

void ChartWidget::mouseMoveEvent(QMouseEvent *e) {
    hoverX_ = e->position().x();
    hoverValid_ = true;
    update();
}

void ChartWidget::leaveEvent(QEvent *) {
    hoverValid_ = false;
    update();
}

void ChartWidget::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape && pinned_) {
        pinned_ = false;
        update();
        return;
    }
    QWidget::keyPressEvent(e);
}
