#pragma once

// Squarified treemap layout (Bruls, Huizing & van Wijk): packs the given
// areas into `rect` so each output rectangle's area matches its weight and
// shapes stay as square as possible. Weights are interpreted directly as
// rectangle areas — the caller scales them (e.g. value/capacity * rect
// area) — so whatever they do not cover stays blank. Zero/negative or
// non-finite weights yield an invalid rect. Header-only so unit tests can
// exercise the geometry without linking the GUI.
#include <QRectF>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <limits>

inline QVector<QRectF> squarifiedTreemap(const QVector<double> &areas, const QRectF &rect) {
    QVector<QRectF> out(areas.size());
    struct Item { qsizetype idx; double area; };
    QVector<Item> items;
    items.reserve(areas.size());
    for (qsizetype i = 0; i < areas.size(); ++i)
        if (areas[i] > 0.0 && std::isfinite(areas[i])) items.push_back({i, areas[i]});
    if (items.isEmpty() || rect.width() <= 0.0 || rect.height() <= 0.0) return out;

    // When the weights tile the rect exactly (e.g. an explicit "free" item
    // takes the remainder), the final strip stretches across the whole
    // remaining rect to absorb normalization drift; otherwise the classic
    // contract holds — exact areas, uncovered rect stays blank.
    qreal weightSum = 0.0;
    for (const auto &it : items) weightSum += it.area;
    const qreal rectArea = rect.width() * rect.height();
    const bool stretchFinal = std::fabs(weightSum - rectArea) <= 1e-6 * qMax<qreal>(1.0, rectArea);

    qreal x = rect.x(), y = rect.y(), w = rect.width(), h = rect.height();
    qsizetype i = 0;
    while (i < items.size() && w > 0.0 && h > 0.0) {
        // Grow one strip along the shorter side while the worst aspect
        // ratio keeps improving; stop at the first item that makes it worse.
        const bool vertical = w >= h;
        const qreal side = vertical ? h : w;
        qreal stripArea = 0.0, worst = std::numeric_limits<double>::infinity();
        qsizetype end = i;
        for (qsizetype j = i; j < items.size(); ++j) {
            const qreal next = stripArea + items[j].area;
            const qreal thickness = next / side;
            qreal ratio = 0.0;
            for (qsizetype k = i; k <= j; ++k) {
                const qreal len = items[k].area / thickness;
                ratio = std::max(ratio, std::max(len / thickness, thickness / len));
            }
            if (j > i && ratio > worst) break;
            worst = ratio;
            stripArea = next;
            end = j;
        }
        const qreal thickness = stripArea / side;
        if (stretchFinal && end == items.size() - 1) {
            // Final strip: stretch it across the entire remaining rect so the
            // layout is always a complete rectangle — intermediate strips keep
            // exact areas, the last one absorbs normalization drift.
            qreal off = 0.0;
            for (qsizetype k = i; k <= end; ++k) {
                const qreal frac = items[k].area / stripArea;
                out[items[k].idx] = vertical ? QRectF(x + off, y, w * frac, h)
                                             : QRectF(x, y + off, w, h * frac);
                off += vertical ? w * frac : h * frac;
            }
        } else {
            qreal offset = 0.0;
            for (qsizetype k = i; k <= end; ++k) {
                const qreal len = items[k].area / thickness;
                out[items[k].idx] = vertical ? QRectF(x, y + offset, thickness, len)
                                             : QRectF(x + offset, y, len, thickness);
                offset += len;
            }
        }
        if (vertical) { x += thickness; w -= thickness; }
        else          { y += thickness; h -= thickness; }
        i = end + 1;
    }
    return out;
}
