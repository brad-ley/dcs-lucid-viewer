// =============================================================================
// FocusDiagnosticDialog.h
// =============================================================================
#pragma once

#include <QDialog>
#include <QVector>
#include <QWidget>
#include <QImage>
#include <QPointF>

class QComboBox;
class QPushButton;
class QLabel;
class QSpinBox;
class QWheelEvent;
class QMouseEvent;

// =============================================================================
// FocusPlotWidget — zoomable/pannable time-series chart with outlier rejection
// =============================================================================
class FocusPlotWidget : public QWidget
{
    Q_OBJECT
public:
    explicit FocusPlotWidget(QWidget* parent = nullptr);

    // Add a new focus metric value.  Applies outlier rejection before plotting.
    void addValue(double v);

    // Clear all stored data and reset view.
    void reset();

    // Set how many points the std-tracking window holds and what the default
    // view width is.  Call this when the user changes the "Window (s)" spinbox.
    void setWindowPoints(int n);

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    // Maximum plot history kept in memory (regardless of window size).
    static constexpr int kMaxHistory = 10000;
    // Pixels reserved on the left for Y-axis labels.
    static constexpr int kLeftMargin = 50;

    // Returns the drawable plot area (inside margins).
    QRect plotArea() const;

    // Accepted (non-outlier) values for display, bounded to kMaxHistory.
    QVector<double> m_plotValues;

    // ALL incoming values (including outliers), bounded to m_windowPoints.
    // Used only for running-std computation so a real step change is detected fast.
    QVector<double> m_stdWindow;

    int    m_windowPoints = 300;   // std window size + default view width
    double m_viewStart    = 0.0;   // leftmost visible index (fractional)
    double m_viewWidth    = 300.0; // number of data indices in current view
    bool   m_autoScroll   = true;  // when true, view follows new data at the right

    bool    m_dragging           = false;
    QPointF m_dragStartPos;
    double  m_dragStartViewStart = 0.0;
};


// =============================================================================
// FocusDiagnosticDialog
// =============================================================================
class FocusDiagnosticDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FocusDiagnosticDialog(QWidget* parent = nullptr);
    ~FocusDiagnosticDialog() override = default;

signals:
    // Emitted when the user clicks "Redraw ROI" — PreviewDialog re-enters draw mode.
    void redrawRoiRequested();

    // Emitted from closeEvent so PreviewDialog can clear the focus overlay.
    void dialogClosed();

public slots:
    // Called by PreviewDialog each display frame with the (possibly ROI-cropped) raw image.
    void addFrame(const QImage& image);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onResetClicked();
    void onWindowPointsChanged();

private:
    enum class Metric { Variance, Brenner, Laplacian, Tenengrad, Entropy };

    static double computeMetric    (Metric m, const QImage& img);
    static double computeVariance  (const uchar* p, int w, int h, int stride);
    static double computeBrenner   (const uchar* p, int w, int h, int stride);
    static double computeLaplacian (const uchar* p, int w, int h, int stride);
    static double computeTenengrad (const uchar* p, int w, int h, int stride);
    static double computeEntropy   (const uchar* p, int w, int h, int stride);

    QComboBox*       m_metricCombo;
    QPushButton*     m_resetButton;
    QPushButton*     m_redrawRoiButton;
    QSpinBox*        m_windowSpinBox;
    FocusPlotWidget* m_plot;
    QLabel*          m_valueLabel;
};
