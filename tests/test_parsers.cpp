// Unit tests for the /proc parsers shared by collector and GUI
// (LinuxUtils::parsePsiContent / parseProcStat / parseProcStatusSwap)
// and the treemap layout used by the Memory page.
#include "linuxutils.h"
#include "treemaplayout.h"

#include <QCoreApplication>
#include <QRectF>
#include <cmath>
#include <cstdio>

using namespace LinuxUtils;

static int failures = 0;
static void expect(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "test_parsers FAIL: %s\n", what); ++failures; }
}
static void expectClose(double v, double ref, const char *what) {
    expect(std::isfinite(v) && std::fabs(v - ref) < 0.005, what);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    // PSI: both lines, avg10 picked.
    const auto psi = parsePsiContent(
        "some avg10=1.23 avg300=0.00 avg600=0.00 avg20=0.00\n"
        "full avg10=4.56 avg300=0.00 avg600=0.00 avg20=0.00\n");
    expectClose(psi.some, 1.23, "psi some avg10");
    expectClose(psi.full, 4.56, "psi full avg10");

    // PSI: cpu resource has no "full" line.
    const auto cpu = parsePsiContent("some avg10=0.00 avg300=0.10 avg600=0.00 avg20=0.00\n");
    expectClose(cpu.some, 0.0, "cpu psi some");
    expect(!std::isfinite(cpu.full), "cpu psi full must be NaN");

    // PSI: empty / missing file.
    const auto empty = parsePsiContent(QString());
    expect(!std::isfinite(empty.some) && !std::isfinite(empty.full), "empty psi NaN");

    // /proc/<pid>/stat: comm containing spaces and a closing paren.
    // Fields after comm: state(3) ppid(4) pgrp(5) session(6) tty(7) tpgid(8)
    // flags(9) minflt(10) cminflt(11) majflt(12) cmajflt(13) utime(14)
    // stime(15) cutime(16) cstime(17) priority(18) nice(19) threads(20)
    // itrealvalue(21) starttime(22) vsize(23) rss(24)
    const QString stat =
        "1234 (chrome:Renderer) S 900 900 900 0 -1 4194560 12345 0 678 0 42 7 0 0 20 0 8 0 987654 1234567890 5678 18446744073709551615 12345 12345 140737488355328 0 0 0 0 0 0 0 0 0 17 4 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
    const auto p = parseProcStat(stat);
    expect(p.has_value(), "parseProcStat ok");
    if (p) {
        expect(p->pid == 1234, "proc pid");
        expect(p->name == "chrome:Renderer", "proc name keeps spaces/parens");
        expect(p->state == "S", "proc state");
        expect(p->cpuTicks == 49, "proc cpuTicks = utime(42)+stime(7)");
        expect(p->threads == 8, "proc threads");
        expect(p->rssPages == 5678, "proc rss pages");
    }

    // Garbage input must not parse.
    expect(!parseProcStat("not a stat file").has_value(), "garbage rejected");
    expect(!parseProcStat("999 (truncated").has_value(), "truncated rejected");

    // VmSwap from /proc/<pid>/status.
    const auto swap = parseProcStatusSwap(
        "Name:\tbash\nVmPeak:\t 10000 kB\nVmRSS:\t    4096 kB\nVmSwap:\t\t 2048 kB\n");
    expect(swap.has_value() && *swap == 2048, "vmswap value");
    expect(!parseProcStatusSwap("VmPeak:\t 1000 kB\nVmRSS:\t 512 kB\n").has_value(),
           "no vmswap line -> nullopt");
    const auto swapZero = parseProcStatusSwap("VmSwap:\t   0 kB\n");
    expect(swapZero.has_value() && *swapZero == 0, "vmswap zero");
    expect(!parseProcStatusSwap(QString()).has_value(), "empty status -> nullopt");

    // Treemap layout: two equal blocks that tile the panel exactly become two
    // 10x10 squares (the final strip absorbs no drift, areas stay exact).
    {
        const auto rects = squarifiedTreemap({100.0, 100.0}, QRectF(0, 0, 20, 10));
        expect(rects.size() == 2, "treemap rect count");
        const auto close = [](double a, double b) { return std::fabs(a - b) < 1e-3; };
        if (rects.size() == 2) {
            expect(close(rects[0].width(), 10) && close(rects[0].height(), 10), "treemap first square");
            expect(close(rects[1].width(), 10) && close(rects[1].height(), 10), "treemap second square");
        }
    }
    // Sum below the panel with an explicit free item: complete tiling, areas
    // match the weights exactly, and the free item takes the remainder.
    {
        const QVector<double> areas{640.0, 200.0, 160.0}; // sum 1000 == panel 20x50
        const auto rects = squarifiedTreemap(areas, QRectF(0, 0, 20, 50));
        double sum = 0.0;
        for (const auto &r : rects) sum += r.isValid() ? r.width() * r.height() : 0.0;
        expect(std::fabs(sum - 1000.0) < 1e-3, "treemap complete tiling");
        if (rects.size() == 3)
            expect(std::fabs(rects[2].width() * rects[2].height() - 160.0) < 1e-3, "treemap free remainder");
    }
    // Skewed weights must stay reasonably square, not overlap, and leave the
    // uncovered part of the rect blank.
    {
        const QVector<double> areas{640.0, 200.0, 90.0, 40.0, 20.0, 8.0};
        const auto rects = squarifiedTreemap(areas, QRectF(0, 0, 100, 50));
        double sum = 0.0;
        for (const auto &r : rects) {
            expect(r.isValid(), "treemap rect valid");
            sum += r.width() * r.height();
            const double aspect = std::max(r.width() / r.height(), r.height() / r.width());
            expect(aspect < 10.0, "treemap aspect reasonable");
        }
        expectClose(sum, 998.0, "treemap covers weights");
        for (qsizetype a = 0; a < rects.size(); ++a)
            for (qsizetype b = a + 1; b < rects.size(); ++b)
                expect(!rects[a].intersects(rects[b]), "treemap no overlap");
    }
    // Degenerate inputs must not crash or produce valid rects.
    {
        const auto rects = squarifiedTreemap({0.0, -5.0}, QRectF(0, 0, 10, 10));
        expect(rects.size() == 2 && !rects[0].isValid() && !rects[1].isValid(), "degenerate weights");
    }
    {
        const auto rects = squarifiedTreemap({1.0}, QRectF());
        expect(rects.size() == 1 && !rects[0].isValid(), "empty rect rejected");
    }

    if (failures > 0) return 1;
    std::puts("Parsers PASS");
    return 0;
}
