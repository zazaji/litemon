#pragma once

#include <QWidget>
#include <QVector>
#include <QString>

class ChartWidget : public QWidget {
    Q_OBJECT
public:
    struct Point { qint64 ts; double value; };
    struct Series { QString name; QVector<Point> points; };

    explicit ChartWidget(QWidget *parent=nullptr);
    void setTitle(const QString &title, const QString &unit = {});
    void setSeries(const QVector<Series> &series);
    void setFixedYRange(double min, double max);
    void clearFixedYRange();
    QSize minimumSizeHint() const override { return {360,170}; }
    QSize sizeHint() const override { return {520,230}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString title_;
    QString unit_;
    QVector<Series> series_;
    bool fixedRange_ = false;
    double fixedMin_ = 0, fixedMax_ = 100;
};
