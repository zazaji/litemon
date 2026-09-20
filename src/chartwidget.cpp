#include "chartwidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QDateTime>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>

namespace {

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

} // namespace

ChartWidget::ChartWidget(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(150);
    setAutoFillBackground(false);
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

    // ---- Header: title + inline legend + unit badge, all sharing the first row.
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

    const int contentWidth = qMax(0, width() - kMarginLeft - kMarginRight);
    const QList<QColor> colors = seriesColors(pal);

    // Title font setup.
    QFont titleFont = baseFont;
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.5);
    const QFontMetrics titleFm(titleFont);

    // Measure title and unit badge widths.
    const int unitWidth = unit_.isEmpty() ? 0 : (titleFm.horizontalAdvance(unit_) + 18);
    const int gapAfterTitle = 14; // space between title text and first legend item
    const int headerAvailForTitle = qMax(0, contentWidth - unitWidth - (unitWidth > 0 ? 8 : 0));
    const QString titleElided = titleFm.elidedText(title_, Qt::ElideRight, qMax(10, headerAvailForTitle));
    const int titleDrawnWidth = titleFm.horizontalAdvance(titleElided) + gapAfterTitle;

    // Wrap legend items into rows. Row 0 shares space with the title (leaving
    // room for the unit badge on the right); overflow rows span the full width.
    struct LegendItem { QString name; int width; int seriesIndex; };
    struct LegendRow { QList<LegendItem> items; int xOffset; };
    QList<LegendRow> legendRows;
    const int seriesCount = static_cast<int>(series_.size());
    int legendDrawn = 0;
    if (seriesCount > 0) {
        const int firstRowAvail = contentWidth - titleDrawnWidth - unitWidth - (unitWidth > 0 ? 8 : 0);
        const bool shareFirstRow = (firstRowAvail >= 60); // otherwise the legend starts on its own row
        QList<LegendItem> currentRow;
        int rowAvail = shareFirstRow ? firstRowAvail : contentWidth;
        int rowUsed = 0;
        const int firstXOffset = shareFirstRow ? titleDrawnWidth : 0;
        int si = 0;
        for (; si < seriesCount; ++si) {
            const auto &s = series_.at(si);
            const int itemW = 8 + 4 + fm.horizontalAdvance(s.name) + 14;
            if (!currentRow.isEmpty() && rowUsed + itemW > rowAvail) {
                legendRows.append({currentRow, legendRows.isEmpty() ? firstXOffset : 0});
                currentRow.clear();
                rowAvail = contentWidth;
                rowUsed = 0;
                if (legendRows.size() >= 3) { break; } // cap rows; extras still draw, see "+N" below
            }
            // If even the first item on a fresh row doesn't fit, still add it (elided later).
            currentRow.append({s.name, itemW, si});
            rowUsed += itemW;
        }
        if (!currentRow.isEmpty() && legendRows.size() < 3) {
            legendRows.append({currentRow, legendRows.isEmpty() ? firstXOffset : 0});
        }
        legendDrawn = si;
    }
    const int hiddenSeries = seriesCount - legendDrawn;
    const int extraRows = static_cast<int>(legendRows.size()) - 1;
    const int headerHeight = kTitleRowHeight + qMax(0, extraRows) * kOverflowRowHeight;

    // Draw title on the left of the first row.
    p.setFont(titleFont);
    p.setPen(pal.color(QPalette::Text));
    p.drawText(QRectF(static_cast<double>(kMarginLeft), static_cast<double>(kMarginTop),
                      static_cast<double>(headerAvailForTitle), static_cast<double>(kTitleRowHeight)),
               Qt::AlignLeft | Qt::AlignVCenter, titleElided);

    // Draw unit badge on the far right of the first row.
    if (!unit_.isEmpty()) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.setFont(baseFont);
        p.drawText(QRectF(static_cast<double>(width() - kMarginRight - unitWidth), static_cast<double>(kMarginTop),
                          static_cast<double>(unitWidth), static_cast<double>(kTitleRowHeight)),
                   Qt::AlignRight | Qt::AlignVCenter, unit_);
    }

    // Draw legend items: row 0 inline after the title, the rest from the left.
    // Each item carries its own series index, so wrapped rows show the
    // correct names and colors.
    p.setFont(baseFont);
    const QString moreText = (hiddenSeries > 0)
        ? tr("+%n more", nullptr, hiddenSeries) : QString();
    const int moreWidth = moreText.isEmpty() ? 0 : (fm.horizontalAdvance(moreText) + 10);
    const int rowCount = static_cast<int>(legendRows.size());
    for (int lineIdx = 0; lineIdx < rowCount; ++lineIdx) {
        const LegendRow &legendRow = legendRows.at(lineIdx);
        const bool isFirst = (lineIdx == 0);
        const bool isLast = (lineIdx == rowCount - 1);
        const int lineHeight = isFirst ? kTitleRowHeight : kOverflowRowHeight;
        const double lineTop = static_cast<double>(kMarginTop)
            + static_cast<double>(isFirst ? 0 : (kTitleRowHeight + (lineIdx - 1) * kOverflowRowHeight));
        double lx = static_cast<double>(kMarginLeft + legendRow.xOffset);
        // Right boundary: unit badge on row 0, "+N more" hint on the last row.
        double rightEdge = static_cast<double>(width() - kMarginRight);
        if (isFirst && unitWidth > 0) { rightEdge -= static_cast<double>(unitWidth + 8); }
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
            if (lx > rightEdge) { break; } // never draw into the unit / "+N" zone
        }
        if (isLast && !moreText.isEmpty()) {
            p.setPen(pal.color(QPalette::PlaceholderText));
            p.drawText(QRectF(rightEdge, lineTop, static_cast<double>(moreWidth), static_cast<double>(lineHeight)),
                       Qt::AlignRight | Qt::AlignVCenter, moreText);
        }
    }
    p.setFont(baseFont);

    const double plotTop = static_cast<double>(kMarginTop + headerHeight + kGapLegendPlot);
    const double plotLeft = static_cast<double>(kMarginLeft + kYLabelWidth);
    const double plotRight = static_cast<double>(width() - kPlotRight);
    const double plotBottom = static_cast<double>(height() - kPlotBottom - kXLabelHeight);
    if (plotRight <= plotLeft + 10.0 || plotBottom <= plotTop + 10.0) { return; }
    const QRectF plot(plotLeft, plotTop, plotRight - plotLeft, plotBottom - plotTop);

    // Plot panel background with subtle border.
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::AlternateBase));
    p.drawRoundedRect(plot.adjusted(-8, -8, 8, 8), 8.0, 8.0);
    QPen framePen(pal.color(QPalette::Mid));
    framePen.setWidthF(1.0);
    p.setPen(framePen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(plot.adjusted(-8, -8, 8, 8), 8.0, 8.0);

    qint64 minTs=LLONG_MAX,maxTs=LLONG_MIN;
    double minY=std::numeric_limits<double>::infinity(),maxY=-std::numeric_limits<double>::infinity();
    for(const auto&s:series_) for(const auto&pt:s.points) if(std::isfinite(pt.value)){minTs=std::min(minTs,pt.ts);maxTs=std::max(maxTs,pt.ts);minY=std::min(minY,pt.value);maxY=std::max(maxY,pt.value);}
    if(minTs==LLONG_MAX){
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(plot,Qt::AlignCenter, tr("No historical data yet"));
        return;
    }
    if(maxTs<=minTs) maxTs=minTs+1;
    if(fixedRange_){minY=fixedMin_;maxY=fixedMax_;} else { if(!std::isfinite(minY)||!std::isfinite(maxY)){minY=0;maxY=1;} if(std::abs(maxY-minY)<1e-9){maxY=minY+1;} double pad=(maxY-minY)*0.1; minY=std::max(0.0,minY-pad);maxY+=pad; }

    QPen grid(pal.color(QPalette::Mid));
    grid.setWidthF(1.0);
    grid.setStyle(Qt::DotLine);
    const int decimals = (maxY - minY) < 10.0 ? 1 : 0;
    for(int i=0;i<=4;++i){
        const double y=plot.top()+plot.height()*static_cast<double>(i)/4.0;
        p.setPen(grid);
        p.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y));
        const double val=maxY-(maxY-minY)*static_cast<double>(i)/4.0;
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(QRectF(static_cast<double>(kMarginLeft), y-10.0, static_cast<double>(kYLabelWidth - 8), 20.0),
                   Qt::AlignRight|Qt::AlignVCenter, QString::number(val,'f',decimals));
    }
    const QString timeFmt = rangeLabel(maxTs - minTs);
    for(int i=0;i<=4;++i){
        const double x=plot.left()+plot.width()*static_cast<double>(i)/4.0;
        p.setPen(grid);
        p.drawLine(QPointF(x,plot.top()),QPointF(x,plot.bottom()));
        const qint64 ts=minTs+(maxTs-minTs)*i/4;
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(QRectF(x-50.0,plot.bottom()+10.0,100.0,18.0),Qt::AlignCenter,
                   QDateTime::fromSecsSinceEpoch(ts).toString(timeFmt));
    }

    int si=0;
    for(const auto&s:series_){
        QPainterPath path;
        bool started=false;
        for(const auto&pt:s.points){
            if(!std::isfinite(pt.value))continue;
            const double x=plot.left()+static_cast<double>(pt.ts-minTs)*plot.width()/static_cast<double>(maxTs-minTs);
            const double y=plot.bottom()-(pt.value-minY)*plot.height()/(maxY-minY);
            if(!started){path.moveTo(x,y);started=true;}else path.lineTo(x,y);
        }
        QPen pen(colors.at(si%colors.size()));
        pen.setWidthF(2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        ++si;
    }
}
