#pragma once

#include <QWidget>
#include <QVector>
#include <QString>

// Treemap of one memory domain (RAM or swap): one rectangle per consumer,
// area proportional to its share of the total capacity; unused capacity
// stays blank. Data comes from the GUI's live /proc sampling (see
// MainWindow::refreshMemoryTrees); colors follow the palette so both the
// dark and light themes work.
class TreemapWidget : public QWidget {
    Q_OBJECT
public:
    struct Block {
        QString label;
        double value = 0.0; // MiB
        bool muted = false; // aggregated "<0.1%" bucket renders in a neutral color
        bool freeSpace = false; // unused capacity renders as a white block
    };

    explicit TreemapWidget(QWidget *parent=nullptr);
    void setTitle(const QString &title);
    void setEmptyText(const QString &text);
    void setData(const QVector<Block> &blocks, double totalCapacityMiB);
    QSize minimumSizeHint() const override { return {280,150}; }
    QSize sizeHint() const override { return {460,240}; }

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    QString title_;
    QString emptyText_;
    QVector<Block> blocks_;
    double total_ = 0.0;
    QVector<QRectF> rects_; // last painted layout, for hit testing
    int hover_ = -1;
};
