// =============================================================================
// PinnedParamsPanel.cpp
// =============================================================================
//
// Implementation of PinnedParamsPanel.
//
// REGISTRY LAYOUT:
//   HKCU\Software\Brad Simplified\Lucid Camera Acquisition Tool\pinnedParams
//   Value type: REG_SZ (string)
//   Format:     "NodeA,NodeB,NodeC,..."   (comma-separated GenICam node names)
//
//   QSettings with no arguments automatically targets this path because
//   QApplication::setOrganizationName("Brad Simplified") and
//   QApplication::setApplicationName("Lucid Camera Acquisition Tool") are
//   called in main.cpp.
//
// WIDGET BUILDING:
//   The widget-building logic here mirrors AdvancedParamsDialog::showValueWidget()
//   and MainWindow::buildDynamicParamWidgets(). When a node is not available on
//   the connected camera (or no camera is connected) we show "(not available)"
//   as a grayed-out label.
// =============================================================================

#include "PinnedParamsPanel.h"
#include "CameraManager.h"

// Qt
#include <QSettings>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QJsonObject>
#include <QEvent>           // QEvent::Wheel
#include <QWidget>          // QWidget::hasFocus
#include <QDialog>          // QDialog — base for reorder dialog
#include <QDialogButtonBox> // Standard Ok/Cancel buttons
#include <QListWidget>      // Drag-and-drop list for reorder dialog
#include <QPushButton>      // Delete Selected button in reorder dialog

// GenICam / Arena
#include "GenApi/GenApi.h"

// C++ standard library
#include <algorithm> // std::sort — used to sort pinned list alphabetically
#include <climits>   // INT_MIN, INT_MAX
#include <QSet>      // QSet — fast unordered membership test for the "paired seconds" set


// =============================================================================
// PermissiveDoubleSpinBox — QDoubleSpinBox that never blocks a keystroke for a
// parseable number, even when the typed value is outside [min, max].
//
// WHY THIS EXISTS:
//   Qt's QDoubleSpinBox::validate() returns QValidator::Invalid for values
//   outside [min, max], which causes the spinbox to reject the keystroke with a
//   beep.  The camera's GetMax() is a *dynamic* value — it shrinks when frame
//   rate is high, and expands when it drops.  If the panel was built while the
//   camera was at a high frame rate, the spinbox's stored maximum may be lower
//   than what the user now wants to type.  Arrows work because stepBy() skips
//   validate() and simply clamps; keyboard input does not get the same grace.
//
//   The fix: downgrade Invalid → Intermediate for any keystroke that produces a
//   parseable number.  Intermediate means "accepted but not final" — the spinbox
//   shows the value in its editing color and commits (clamping to [min, max])
//   when the user presses Enter or moves focus away.
// =============================================================================
class PermissiveDoubleSpinBox : public QDoubleSpinBox
{
public:
    explicit PermissiveDoubleSpinBox(QWidget* parent = nullptr)
        : QDoubleSpinBox(parent) {}

protected:
    QValidator::State validate(QString& input, int& pos) const override
    {
        QValidator::State state = QDoubleSpinBox::validate(input, pos);
        if (state != QValidator::Invalid)
            return state;

        // Base class rejected it.  Accept it as Intermediate if the
        // text (minus the suffix) is a parseable floating-point number —
        // the value will be clamped to [min, max] on commit.
        QString copy = input;
        if (!suffix().isEmpty() && copy.endsWith(suffix()))
            copy.chop(suffix().length());
        bool ok = false;
        copy.trimmed().toDouble(&ok);
        return ok ? QValidator::Intermediate : QValidator::Invalid;
    }
};


// =============================================================================
// WheelGuard — event filter that blocks scroll-wheel on unfocused widgets
// =============================================================================
//
// WHY THIS EXISTS:
//   QSpinBox and QComboBox consume wheel events even when they don't have
//   keyboard focus. Inside a QScrollArea this means the user trying to scroll
//   the list accidentally changes a spinner value instead. The fix: install
//   this filter on every interactive widget inside the scroll area. When a
//   wheel event arrives and the widget doesn't have focus, we ignore() it so
//   the event bubbles up to the scroll area and scrolls the list as expected.
//
// C++ CONCEPT — QObject event filter:
//   Any QObject can intercept events destined for *another* QObject by
//   installing itself as an event filter via installEventFilter(). Qt calls
//   eventFilter() before delivering the event to the target. Returning true
//   consumes the event (stops it here); returning false lets it continue.
//
// LIFETIME:
//   We pass the guarded widget as the parent, so Qt deletes the filter
//   automatically when the widget is destroyed — no manual cleanup needed.
namespace {
class WheelGuard : public QObject
{
public:
    explicit WheelGuard(QObject* parent) : QObject(parent) {}

protected:
    bool eventFilter(QObject* obj, QEvent* event) override
    {
        if (event->type() == QEvent::Wheel)
        {
            auto* w = qobject_cast<QWidget*>(obj);
            // Only let the wheel through if the widget already has keyboard focus.
            // If it doesn't, ignore() the event so it propagates to the scroll area.
            if (w && !w->hasFocus())
            {
                event->ignore();
                return true;  // consumed — do NOT pass to the widget
            }
        }
        return QObject::eventFilter(obj, event);
    }
};
} // anonymous namespace


// Helper: apply WheelGuard + StrongFocus to any interactive widget in the panel.
// StrongFocus means the widget only gets focus via click or Tab, not hover —
// so the guard triggers correctly until the user explicitly clicks the widget.
static void installWheelGuard(QWidget* w)
{
    w->setFocusPolicy(Qt::StrongFocus);
    w->installEventFilter(new WheelGuard(w));
}


// =============================================================================
// Default pinned list — written to registry on first run
// =============================================================================
//
// C++ CONCEPT — static member definition:
//   's_defaults' was declared 'static' in the header, but static members must
//   also be *defined* exactly once in a .cpp file. This is that definition.
//   The initializer list creates the QStringList at program start.
const QStringList PinnedParamsPanel::s_defaults = {
    "PixelFormat",
    "Width",
    "Height",
    "OffsetX",
    "OffsetY",
    "BinningSelector",
    "BinningHorizontal",
    "BinningVertical",
    "DecimationHorizontal",
    "DecimationVertical",
    "TriggerMode",
    "Gain",
    "GainAuto",
    "ExposureTime",
    "ExposureAuto"
};


// =============================================================================
// Static registry helpers
// =============================================================================

// loadPinnedList — read the comma-separated list from the registry.
// If the key doesn't exist yet, write the defaults first.
QStringList PinnedParamsPanel::loadPinnedList()
{
    QSettings settings;

    // Check if the key exists. QSettings::contains() queries the registry directly.
    if (!settings.contains("pinnedParams"))
    {
        // First run — seed with the default list so users have something useful immediately.
        settings.setValue("pinnedParams", s_defaults.join(","));
    }

    QString raw = settings.value("pinnedParams", "").toString();

    // Split on comma, filtering out empty strings that appear if commas are adjacent
    // (e.g., "A,,B" → ["A","B"] rather than ["A","","B"]).
    QStringList list = raw.split(',', Qt::SkipEmptyParts);
    return list;
}

// addPinnedNode — append nodeName to the registry list if not already present.
void PinnedParamsPanel::addPinnedNode(const QString& nodeName)
{
    QStringList list = loadPinnedList();
    if (!list.contains(nodeName))
    {
        list.append(nodeName);
        QSettings().setValue("pinnedParams", list.join(","));
    }
}

// removePinnedNode — remove nodeName from the registry list. No-op if absent.
void PinnedParamsPanel::removePinnedNode(const QString& nodeName)
{
    QStringList list = loadPinnedList();
    int removed = list.removeAll(nodeName);  // removeAll returns the count removed
    if (removed > 0)
        QSettings().setValue("pinnedParams", list.join(","));
}

// isPinnedNode — check whether nodeName is in the registry list.
bool PinnedParamsPanel::isPinnedNode(const QString& nodeName)
{
    return loadPinnedList().contains(nodeName);
}


// =============================================================================
// Constructor
// =============================================================================
PinnedParamsPanel::PinnedParamsPanel(CameraManager* mgr, QWidget* parent)
    : QWidget(parent)
    , m_mgr(mgr)
    , m_formLayout(nullptr)
{
    // -------------------------------------------------------------------------
    // Outer layout: scroll area on top, Apply button on bottom
    // -------------------------------------------------------------------------
    //
    // C++ CONCEPT — 'this' in layout constructor:
    //   Passing 'this' as the parent sets the layout directly on this widget.
    //   All child widgets added to the layout are automatically re-parented to
    //   'this', so Qt manages their lifetimes.
    QVBoxLayout* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(4);

    // -------------------------------------------------------------------------
    // Scroll area — holds all the parameter rows
    // -------------------------------------------------------------------------
    //
    // QScrollArea wraps a child widget and adds scroll bars when the child is
    // taller than the visible area. setWidgetResizable(true) tells it to resize
    // the child widget to fill the available width (important for form layout).
    QScrollArea* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);  // Cleaner look — no border around the scroll area

    // The contents widget lives inside the scroll area. We give it a QFormLayout
    // that we rebuild each time refreshFromCamera() is called.
    QWidget* scrollContents = new QWidget(scrollArea);
    m_formLayout = new QFormLayout(scrollContents);
    m_formLayout->setRowWrapPolicy(QFormLayout::DontWrapRows);
    m_formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_formLayout->setContentsMargins(4, 4, 4, 4);
    m_formLayout->setSpacing(4);

    scrollArea->setWidget(scrollContents);

    // -------------------------------------------------------------------------
    // Assemble — scroll area fills all available height; Apply button lives
    // in MainWindow's parameter button row alongside "Advanced..."
    // -------------------------------------------------------------------------
    outerLayout->addWidget(scrollArea, 1);   // stretch=1 → takes all available height
}


// =============================================================================
// refreshFromCamera — rebuild all rows from registry + camera state
// =============================================================================
void PinnedParamsPanel::refreshFromCamera()
{
    clearRows();
    buildRows();
}


// =============================================================================
// clearRows — delete all existing form rows and their widgets
// =============================================================================
void PinnedParamsPanel::clearRows()
{
    // QFormLayout doesn't have a "removeAllRows" helper, so we remove them
    // one by one from the end. Removing a row also deletes the widgets that
    // were added to it (because they were parented to the scroll contents widget).
    //
    // C++ CONCEPT — while loop with rowCount():
    //   We remove from the back to avoid index shifting as we delete.
    while (m_formLayout->rowCount() > 0)
        m_formLayout->removeRow(m_formLayout->rowCount() - 1);

    m_rows.clear();
}


// =============================================================================
// Pair-detection helpers (file-scope, only used by buildRows)
// =============================================================================

// Returns the "second" partner node name when nodeName is the "first" of an
// X/Y, Horizontal/Vertical, or Width/Height pair, AND the partner is present in
// pinnedList.  Returns "" if this node has no pairable partner in the list.
//
// "First" is always the X/Horizontal/Width variant so the ordering in the UI
// (left = X/H/W, right = Y/V/H) is consistent regardless of registry order.
static QString findPairPartner(const QString& nodeName, const QStringList& pinnedList)
{
    // Width <-> Height (special case — no suffix to strip)
    if (nodeName == "Width" && pinnedList.contains("Height"))
        return "Height";

    // *X <-> *Y  (e.g. OffsetX/OffsetY, ScaleFovX/ScaleFovY)
    if (nodeName.endsWith("X"))
    {
        QString partner = nodeName.chopped(1) + "Y";
        if (pinnedList.contains(partner)) return partner;
    }

    // *Horizontal <-> *Vertical  (e.g. BinningHorizontal/BinningVertical)
    if (nodeName.endsWith("Horizontal"))
    {
        QString partner = nodeName.chopped(10) + "Vertical";
        if (pinnedList.contains(partner)) return partner;
    }

    return {};
}

// Short axis labels placed before each widget in a paired row.
// Returns {leftLabel, rightLabel}, e.g. {" X:", " Y:"} or {" W:", " H:"}.
static std::pair<QString, QString> getPairSubLabels(const QString& firstName)
{
    if (firstName == "Width")               return {" W:", " H:"};
    if (firstName.endsWith("X"))            return {" X:", " Y:"};
    if (firstName.endsWith("Horizontal"))   return {" H:", " V:"};
    return {" 1:", " 2:"};  // fallback
}

// Form-row label for a paired row: strips the repeated suffix and appends the
// compact axis specifier, e.g. "Binning Horizontal" → "Binning H/V".
static QString getPairRowLabel(const QString& firstName, GenApi::INodeMap* pMap)
{
    auto displayName = [&](const QString& name) -> QString {
        if (!pMap) return name;
        try {
            GenApi::INode* n = pMap->GetNode(name.toStdString().c_str());
            if (n) return QString::fromLatin1(n->GetDisplayName().c_str());
        } catch (...) {}
        return name;
    };

    if (firstName == "Width")
        return "Width / Height";

    if (firstName.endsWith("X"))
    {
        // "Offset X" → strip trailing " X" → "Offset X/Y"
        QString d = displayName(firstName);
        if (d.endsWith(" X")) d.chop(2);
        return d + " X/Y";
    }

    if (firstName.endsWith("Horizontal"))
    {
        // "Binning Horizontal" → strip " Horizontal" → "Binning H/V"
        QString d = displayName(firstName);
        if (d.endsWith(" Horizontal")) d.chop(11);
        return d + " H/V";
    }

    return displayName(firstName);
}

// Build the appropriate value widget for a single node.
// Returns {widget, GenApi::EInterfaceType cast to int} or {nullptr, 0} when
// the node is unavailable, not readable, or an unsupported type (Command/String).
//
// The caller owns the returned widget and must parent it before adding to a layout.
static std::pair<QWidget*, int> buildValueWidget(
    const std::string& nodeName, CameraManager* mgr, GenApi::INodeMap* pMap)
{
    if (!pMap || !mgr) return {nullptr, 0};

    GenApi::INode* pNode = nullptr;
    try { pNode = pMap->GetNode(nodeName.c_str()); }
    catch (...) { return {nullptr, 0}; }

    if (!pNode || !GenApi::IsReadable(pNode))
        return {nullptr, 0};

    GenApi::EInterfaceType ifType = pNode->GetPrincipalInterfaceType();

    if (ifType == GenApi::intfIEnumeration)
    {
        QComboBox* combo = new QComboBox();
        for (const auto& opt : mgr->getEnumOptions(nodeName))
            combo->addItem(QString::fromStdString(opt));
        combo->setCurrentText(QString::fromStdString(mgr->getEnumValue(nodeName)));
        combo->setEnabled(GenApi::IsWritable(pNode));
        installWheelGuard(combo);
        return {combo, static_cast<int>(ifType)};
    }

    if (ifType == GenApi::intfIFloat)
    {
        GenApi::CFloatPtr pFloat(pNode);
        QDoubleSpinBox* spin = new PermissiveDoubleSpinBox();
        if (pFloat)
        {
            double lo = pFloat->GetMin(), hi = pFloat->GetMax();
            spin->setRange(lo, hi);
            spin->setDecimals(4);
            spin->setSingleStep((hi - lo) / 100.0);
            spin->setValue(pFloat->GetValue());
            QString unit = QString::fromLatin1(pFloat->GetUnit().c_str());
            if (!unit.isEmpty()) spin->setSuffix(" " + unit);
            spin->setEnabled(GenApi::IsWritable(pNode));
        }
        installWheelGuard(spin);
        return {spin, static_cast<int>(ifType)};
    }

    if (ifType == GenApi::intfIInteger)
    {
        GenApi::CIntegerPtr pInt(pNode);
        QSpinBox* spin = new QSpinBox();
        if (pInt)
        {
            int64_t rawMin = pInt->GetMin();
            int64_t rawMax = pInt->GetMax();

            // Width.GetMax()   == SensorWidth  - CurrentOffsetX
            // OffsetX.GetMax() == SensorWidth  - CurrentWidth
            // (same pattern for the Y axis)
            //
            // Both maxima are dynamic: they shrink when the other dimension is non-zero.
            // A spinbox built with the live GetMax() will silently clamp any value that
            // exceeds it — even a perfectly legal value that will become valid after the
            // companion node is written first.
            //
            // Fix: use the sensor dimension (WidthMax / HeightMax) as the spinbox ceiling
            // for all four ROI nodes.  The spinbox then accepts the full sensor range, and
            // applyPinnedParameters() enforces the correct three-step write order so the
            // camera never sees an out-of-range value.
            if (nodeName == "OffsetX" || nodeName == "Width")
            {
                GenApi::CIntegerPtr pSensor(pMap->GetNode("WidthMax"));
                if (pSensor && GenApi::IsReadable(pSensor))
                    rawMax = pSensor->GetValue();
            }
            else if (nodeName == "OffsetY" || nodeName == "Height")
            {
                GenApi::CIntegerPtr pSensor(pMap->GetNode("HeightMax"));
                if (pSensor && GenApi::IsReadable(pSensor))
                    rawMax = pSensor->GetValue();
            }

            int lo = static_cast<int>(std::max<int64_t>(rawMin, INT_MIN));
            int hi = static_cast<int>(std::min<int64_t>(rawMax, INT_MAX));
            spin->setRange(lo, hi);
            spin->setSingleStep(static_cast<int>(std::max<int64_t>(pInt->GetInc(), 1)));
            spin->setValue(static_cast<int>(pInt->GetValue()));
            spin->setEnabled(GenApi::IsWritable(pNode));
        }
        installWheelGuard(spin);
        return {spin, static_cast<int>(ifType)};
    }

    if (ifType == GenApi::intfIBoolean)
    {
        GenApi::CBooleanPtr pBool(pNode);
        QComboBox* combo = new QComboBox();
        combo->addItem("false");
        combo->addItem("true");
        if (pBool)
        {
            combo->setCurrentIndex(pBool->GetValue() ? 1 : 0);
            combo->setEnabled(GenApi::IsWritable(pNode));
        }
        installWheelGuard(combo);
        return {combo, static_cast<int>(ifType)};
    }

    return {nullptr, 0};  // Command / String / Register — not editable inline
}


// =============================================================================
// buildRows — create one form row per pinned parameter;
//             X/Y and H/V pairs share a single row
// =============================================================================
void PinnedParamsPanel::buildRows()
{
    QStringList pinned = loadPinnedList();

    if (pinned.isEmpty())
    {
        QLabel* hint = new QLabel(
            "No pinned parameters yet.\n"
            "Open Advanced Parameters and click ★ to pin a parameter here.");
        hint->setWordWrap(true);
        hint->setStyleSheet("color: #888; font-style: italic;");
        m_formLayout->addRow(hint);
        return;
    }

    GenApi::INodeMap* pMap = m_mgr ? m_mgr->getNodeMap() : nullptr;

    // Connect a widget's change signal to parameterChanged() so the Apply button
    // can turn blue when any value is edited.  The connection type depends on the
    // widget type, so we branch on nodeType (GenApi::EInterfaceType cast to int).
    //
    // C++ CONCEPT — generic lambda capturing 'this':
    //   The lambda captures 'this' (the PinnedParamsPanel instance) so it can call
    //   emit parameterChanged(). This is safe because the lambda's lifetime is tied
    //   to the widget, which is a child of this panel.
    // Connect widget change signals to parameterChanged() so the Apply button turns blue.
    // When live mode is active the parameter is already written immediately on each edit,
    // so there is no "pending" change — suppress the signal in that case to avoid a
    // misleading blue button.  If live-apply later fails, applyLiveSingleNode() will
    // emit parameterChanged() itself so the button turns blue as a fallback hint.
    auto connectDirty = [this](QWidget* w, int nodeType)
    {
        if (!w) return;
        auto ifType = static_cast<GenApi::EInterfaceType>(nodeType);
        if (ifType == GenApi::intfIEnumeration || ifType == GenApi::intfIBoolean)
        {
            auto* combo = qobject_cast<QComboBox*>(w);
            if (combo)
                connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this]() { if (!m_liveMode) emit parameterChanged(); });
        }
        else if (ifType == GenApi::intfIInteger)
        {
            auto* spin = qobject_cast<QSpinBox*>(w);
            if (spin)
                connect(spin, QOverload<int>::of(&QSpinBox::valueChanged),
                        this, [this]() { if (!m_liveMode) emit parameterChanged(); });
        }
        else if (ifType == GenApi::intfIFloat)
        {
            auto* spin = qobject_cast<QDoubleSpinBox*>(w);
            if (spin)
                connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        this, [this]() { if (!m_liveMode) emit parameterChanged(); });
        }
    };

    // For integer spinboxes, snap the typed value to the nearest valid camera increment
    // when the user commits (Enter or focus-loss).  The spinbox singleStep() was set to
    // GetInc() in buildValueWidget, so we reuse it as the snap step.
    // This connection is wired BEFORE connectLiveApply so the snapped value is what
    // gets written to the camera rather than the raw typed value.
    auto connectSnapToIncrement = [](QWidget* w, int nodeType)
    {
        if (!w) return;
        if (static_cast<GenApi::EInterfaceType>(nodeType) != GenApi::intfIInteger) return;
        auto* spin = qobject_cast<QSpinBox*>(w);
        if (!spin || spin->singleStep() <= 1) return;

        connect(spin, &QSpinBox::editingFinished, spin, [spin]()
        {
            const int step   = spin->singleStep();
            const int minVal = spin->minimum();
            const int val    = spin->value();
            const int offset = val - minVal;
            // Round to nearest multiple of step above minimum
            const int snapped = minVal + static_cast<int>(
                qRound(static_cast<double>(offset) / step)) * step;
            const int clamped = qBound(spin->minimum(), snapped, spin->maximum());
            if (clamped != val)
                spin->setValue(clamped);  // valueChanged fires here, not editingFinished
        });
    };

    // Connect each widget so that when live mode is active the parameter is written
    // to the camera immediately on user input, without requiring "Apply."
    //
    // For spinboxes we use editingFinished (fires on Enter or focus-loss after
    // typing) rather than valueChanged so arrow-key auto-repeat doesn't flood the
    // camera with writes.  For combos, currentIndexChanged is appropriate because
    // each selection is a discrete, deliberate choice.
    //
    // The lambda checks m_liveMode at call time — if preview is not running the
    // connection exists but is a no-op, which is cheaper than adding/removing it.
    auto connectLiveApply = [this](QWidget* w, int nodeType, const std::string& nodeName)
    {
        if (!w) return;
        auto ifType = static_cast<GenApi::EInterfaceType>(nodeType);
        if (ifType == GenApi::intfIEnumeration || ifType == GenApi::intfIBoolean)
        {
            auto* combo = qobject_cast<QComboBox*>(w);
            if (combo)
                connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this, nodeName, combo]() {
                            if (m_liveMode) applyLiveSingleNode(nodeName, combo);
                        });
        }
        else if (ifType == GenApi::intfIInteger)
        {
            auto* spin = qobject_cast<QSpinBox*>(w);
            if (spin)
                connect(spin, &QSpinBox::editingFinished,
                        this, [this, nodeName, spin]() {
                            if (m_liveMode) applyLiveSingleNode(nodeName, spin);
                        });
        }
        else if (ifType == GenApi::intfIFloat)
        {
            auto* dspin = qobject_cast<QDoubleSpinBox*>(w);
            if (dspin)
                connect(dspin, &QDoubleSpinBox::editingFinished,
                        this, [this, nodeName, dspin]() {
                            if (m_liveMode) applyLiveSingleNode(nodeName, dspin);
                        });
        }
    };

    // Pre-scan: collect every node that is the "second" of a pair (Y / Vertical / Height).
    // These are skipped in the main loop — they get rendered inside the first node's row.
    //
    // C++ CONCEPT — QSet:
    //   QSet<T> is an unordered hash-set. contains() is O(1) average, which matters
    //   here because we call it on every iteration of the main loop below.
    QSet<QString> pairedSeconds;
    for (const QString& name : pinned)
    {
        QString partner = findPairPartner(name, pinned);
        if (!partner.isEmpty())
            pairedSeconds.insert(partner);  // partner is always the "second" (Y/V/H)
    }

    for (const QString& nodeNameQ : pinned)
    {
        // Already rendered as the right-hand widget of a paired row — skip.
        if (pairedSeconds.contains(nodeNameQ))
            continue;

        const std::string nodeName = nodeNameQ.toStdString();
        const QString partner = findPairPartner(nodeNameQ, pinned);

        // ==================================================================
        // PAIRED ROW — two spinboxes side by side  (e.g. Width / Height)
        // ==================================================================
        if (!partner.isEmpty())
        {
            const std::string partnerName = partner.toStdString();

            auto [w1, t1] = buildValueWidget(nodeName,    m_mgr, pMap);
            auto [w2, t2] = buildValueWidget(partnerName, m_mgr, pMap);

            // Container widget: [sub-label] [spinbox] [sub-label] [spinbox]
            QWidget*     container = new QWidget();
            QHBoxLayout* hl        = new QHBoxLayout(container);
            hl->setContentsMargins(0, 0, 0, 0);
            hl->setSpacing(3);

            auto [sub1, sub2] = getPairSubLabels(nodeNameQ);

            // Small dim label before each spinbox: "X:", "Y:", "W:", "H:", etc.
            auto makeSubLabel = [container](const QString& text) -> QLabel* {
                QLabel* lbl = new QLabel(text, container);
                lbl->setStyleSheet("color: #aaa; font-size: 10px;");
                return lbl;
            };

            hl->addWidget(makeSubLabel(sub1));
            if (w1) { w1->setParent(container); hl->addWidget(w1, 1); }
            else    { auto* na = new QLabel("(n/a)", container);
                      na->setStyleSheet("color:#aaa;font-style:italic;");
                      hl->addWidget(na, 1); }

            hl->addWidget(makeSubLabel(sub2));
            if (w2) { w2->setParent(container); hl->addWidget(w2, 1); }
            else    { auto* na = new QLabel("(n/a)", container);
                      na->setStyleSheet("color:#aaa;font-style:italic;");
                      hl->addWidget(na, 1); }

            m_formLayout->addRow(getPairRowLabel(nodeNameQ, pMap) + ":", container);

            // Register both as individual PinnedRows so applyPinnedParameters()
            // can still write each widget's value to the camera independently.
            m_rows.push_back({nodeName,    w1 ? t1 : 0, w1});
            m_rows.push_back({partnerName, w2 ? t2 : 0, w2});
            connectDirty(w1, t1);
            connectDirty(w2, t2);
            connectSnapToIncrement(w1, t1);
            connectSnapToIncrement(w2, t2);
            connectLiveApply(w1, t1, nodeName);
            connectLiveApply(w2, t2, partnerName);
            continue;
        }

        // ==================================================================
        // SINGLE ROW — existing one-param-per-row behaviour
        // ==================================================================

        GenApi::INode* pNode = nullptr;
        if (pMap)
        {
            try { pNode = pMap->GetNode(nodeName.c_str()); }
            catch (...) {}
        }

        QString label = pNode
            ? QString::fromLatin1(pNode->GetDisplayName().c_str())
            : nodeNameQ;

        if (!pNode || !GenApi::IsReadable(pNode))
        {
            QLabel* notAvail = new QLabel("(not available)");
            notAvail->setEnabled(false);
            notAvail->setStyleSheet("color: #aaa; font-style: italic;");
            m_formLayout->addRow(label + ":", notAvail);
            m_rows.push_back({nodeName, 0, nullptr});
            continue;
        }

        auto [widget, nodeType] = buildValueWidget(nodeName, m_mgr, pMap);

        if (!widget)
        {
            QLabel* unsupported = new QLabel("(view-only in Advanced dialog)");
            unsupported->setStyleSheet("color: #aaa; font-style: italic;");
            m_formLayout->addRow(label + ":", unsupported);
            m_rows.push_back({nodeName, 0, nullptr});
            continue;
        }

        m_formLayout->addRow(label + ":", widget);
        m_rows.push_back({nodeName, nodeType, widget});
        connectDirty(widget, nodeType);
        connectSnapToIncrement(widget, nodeType);
        connectLiveApply(widget, nodeType, nodeName);
    }
}


// =============================================================================
// currentValuesJson — snapshot current widget values as a JSON object
// =============================================================================
//
// Called by MainWindow before starting acquisition so the parameter state can
// be embedded in metadata.json alongside the saved frames.
QJsonObject PinnedParamsPanel::currentValuesJson() const
{
    QJsonObject obj;

    for (const PinnedRow& row : m_rows)
    {
        if (!row.valueWidget || row.nodeType == 0)
            continue;

        QString key = QString::fromStdString(row.nodeName);
        GenApi::EInterfaceType ifType = static_cast<GenApi::EInterfaceType>(row.nodeType);

        if (ifType == GenApi::intfIEnumeration || ifType == GenApi::intfIBoolean)
        {
            QComboBox* combo = qobject_cast<QComboBox*>(row.valueWidget);
            if (combo)
                obj[key] = combo->currentText();
        }
        else if (ifType == GenApi::intfIFloat)
        {
            QDoubleSpinBox* spin = qobject_cast<QDoubleSpinBox*>(row.valueWidget);
            if (spin)
                obj[key] = spin->value();
        }
        else if (ifType == GenApi::intfIInteger)
        {
            QSpinBox* spin = qobject_cast<QSpinBox*>(row.valueWidget);
            if (spin)
                obj[key] = spin->value();
        }
    }

    return obj;
}


// =============================================================================
// SLOT: applyPinnedParameters — write all widget values to the camera
// =============================================================================
//
// Three outcomes per row:
//   1. Node is disabled by another parameter (e.g. ExposureTime when ExposureAuto
//      is on) — access mode is NA (Not Available). We emit a statusMessage so the
//      main window logs it, but show NO error dialog.  This is expected behavior,
//      not a problem the user needs to act on.
//   2. Genuine write failure (bad value, hardware error, etc.) — added to the
//      errors list and shown in a warning dialog at the end.
//   3. Success — silent.
void PinnedParamsPanel::applyPinnedParameters()
{
    if (!m_mgr || !m_mgr->isConnected())
        return;

    QStringList errors;

    // We need the raw node map to check access mode before attempting a write.
    GenApi::INodeMap* pMap = m_mgr->getNodeMap();

    // =========================================================================
    // ROI write-order problem:
    //
    // The camera enforces: OffsetX + Width <= SensorWidth (and same for Y).
    // This means the live node maximum depends on both values simultaneously:
    //   OffsetX.GetMax() == SensorWidth - CurrentWidth
    //   Width.GetMax()   == SensorWidth - CurrentOffsetX
    //
    // Consequence: the order we write the four ROI nodes matters:
    //
    //   Going from FULL FRAME → ROI  (e.g. OffsetX 0→300, Width max→2000):
    //     Must write Width first (reduce size while offset=0 is still valid),
    //     then OffsetX (GetMax is now SensorWidth-2000, so 300 is in range).
    //
    //   Going from ROI → FULL FRAME  (e.g. OffsetX 300→0, Width 2000→max):
    //     Writing Width=max first FAILS because GetMax = SensorWidth-300 < max.
    //     Must zero offsets first, then write the new larger size.
    //
    // There is no fixed two-step order that handles both directions.
    // The always-safe three-step sequence is:
    //   1. Zero OffsetX/OffsetY  (always valid regardless of current size)
    //   2. Write Width/Height    (now valid because offsets are 0)
    //   3. Write final OffsetX/OffsetY  (valid because size is already the target)
    //
    // We only apply this special ordering when Width or Height is actually being
    // changed.  If the pinned list doesn't include those nodes we fall through to
    // the normal loop and don't touch the offsets.
    // =========================================================================

    // Collect the ROI node names we need to handle specially.
    const std::array<std::string, 4> kRoiNodes = {"OffsetX", "OffsetY", "Width", "Height"};

    // Check whether Width or Height is in the pending write list.  If neither is
    // present we don't need the special ordering.
    bool hasWidthOrHeight = false;
    for (const PinnedRow& row : m_rows)
    {
        if (row.nodeName == "Width" || row.nodeName == "Height")
        {
            hasWidthOrHeight = true;
            break;
        }
    }

    // Helper lambda: attempt to write one row and append any error message.
    // Returns true if the write succeeded (or the node was silently skipped).
    auto writeRow = [&](const PinnedRow& row) -> bool
    {
        if (!row.valueWidget || row.nodeType == 0)
            return true;  // Nothing to write — not an error

        // Pre-flight: check access mode
        if (pMap)
        {
            try
            {
                GenApi::INode* pNode = pMap->GetNode(row.nodeName.c_str());
                if (!pNode || !GenApi::IsAvailable(pNode))
                {
                    // Most common case: ExposureAuto/GainAuto disabled the paired param.
                    // Log a short note so the user knows it was skipped intentionally.
                    emit statusMessage(
                        QString("Skipped '%1': currently disabled (controlled by another parameter)")
                            .arg(QString::fromStdString(row.nodeName)));
                    return true;  // Expected — not an error
                }
                if (!GenApi::IsWritable(pNode))
                {
                    emit statusMessage(
                        QString("Skipped '%1': read-only")
                            .arg(QString::fromStdString(row.nodeName)));
                    return true;  // Expected — not an error
                }
            }
            catch (...) {}  // If GetNode throws, fall through and let the setter report it
        }

        // Attempt the write
        std::string errMsg;
        bool ok = false;

        // C++ CONCEPT — casting enum stored as int:
        //   We stored the GenApi::EInterfaceType as a plain 'int' to avoid including
        //   GenApi headers in the .h file. Here we cast it back to use in the switch.
        GenApi::EInterfaceType ifType = static_cast<GenApi::EInterfaceType>(row.nodeType);

        if (ifType == GenApi::intfIEnumeration)
        {
            QComboBox* combo = qobject_cast<QComboBox*>(row.valueWidget);
            if (combo)
                ok = m_mgr->setNodeEnumValue(row.nodeName, combo->currentText().toStdString(), errMsg);
        }
        else if (ifType == GenApi::intfIFloat)
        {
            QDoubleSpinBox* spin = qobject_cast<QDoubleSpinBox*>(row.valueWidget);
            if (spin)
                ok = m_mgr->setNodeDoubleValue(row.nodeName, spin->value(), errMsg);
        }
        else if (ifType == GenApi::intfIInteger)
        {
            QSpinBox* spin = qobject_cast<QSpinBox*>(row.valueWidget);
            if (spin)
                ok = m_mgr->setNodeInt64Value(row.nodeName, static_cast<int64_t>(spin->value()), errMsg);
        }
        else if (ifType == GenApi::intfIBoolean)
        {
            QComboBox* combo = qobject_cast<QComboBox*>(row.valueWidget);
            if (combo)
                ok = m_mgr->setNodeBoolValue(row.nodeName, combo->currentIndex() == 1, errMsg);
        }

        if (!ok && !errMsg.empty())
        {
            errors.append(QString::fromStdString(row.nodeName) + ": " + QString::fromStdString(errMsg));
            return false;
        }
        return true;
    };

    if (hasWidthOrHeight)
    {
        // ---- Step 1: zero OffsetX and OffsetY so that any new Width/Height is
        //              always in range regardless of the previous ROI state ----
        std::string dummy;
        m_mgr->setNodeInt64Value("OffsetX", 0, dummy);
        m_mgr->setNodeInt64Value("OffsetY", 0, dummy);

        // ---- Step 2: write Width and Height ----
        for (const PinnedRow& row : m_rows)
        {
            if (row.nodeName == "Width" || row.nodeName == "Height")
                writeRow(row);
        }

        // ---- Step 3: write the final OffsetX and OffsetY ----
        for (const PinnedRow& row : m_rows)
        {
            if (row.nodeName == "OffsetX" || row.nodeName == "OffsetY")
                writeRow(row);
        }

        // ---- All other rows (excluding the four ROI nodes) ----
        for (const PinnedRow& row : m_rows)
        {
            bool isRoiNode = false;
            for (const auto& name : kRoiNodes)
            {
                if (row.nodeName == name) { isRoiNode = true; break; }
            }
            if (!isRoiNode)
                writeRow(row);
        }
    }
    else
    {
        // No Width/Height in the list — normal order, no ROI sequencing needed.
        for (const PinnedRow& row : m_rows)
            writeRow(row);
    }

    if (!errors.isEmpty())
    {
        QMessageBox::warning(nullptr, "Apply — Some Parameters Failed",
            "The following parameters could not be written:\n\n" + errors.join("\n"));
    }

    // Refresh widgets with values the camera actually accepted
    // (camera may clamp or round what we sent)
    refreshFromCamera();
}


// =============================================================================
// setIntNodeValue — programmatically set an integer spinbox row
// =============================================================================
//
// Scans m_rows for a row whose nodeName matches the given name, then sets
// its QSpinBox value.  Called by MainWindow after an ROI is drawn so the four
// ROI spinboxes reflect the drawn rectangle without needing to rebuild the panel.
void PinnedParamsPanel::setIntNodeValue(const QString& nodeName, int value)
{
    for (const PinnedRow& row : m_rows)
    {
        if (QString::fromStdString(row.nodeName) == nodeName)
        {
            QSpinBox* spin = qobject_cast<QSpinBox*>(row.valueWidget);
            if (spin)
                spin->setValue(value);
            return;
        }
    }
}


// =============================================================================
// setIntNodeMaximum — temporarily widen an integer spinbox's upper bound
// =============================================================================
void PinnedParamsPanel::setIntNodeMaximum(const QString& nodeName, int max)
{
    for (const PinnedRow& row : m_rows)
    {
        if (QString::fromStdString(row.nodeName) == nodeName)
        {
            QSpinBox* spin = qobject_cast<QSpinBox*>(row.valueWidget);
            if (spin)
                spin->setMaximum(max);
            return;
        }
    }
}


// =============================================================================
// showReorderDialog — let the user drag rows to reorder the pinned list
// =============================================================================
//
// Opens a modal dialog containing a QListWidget with InternalMove drag-and-drop.
// Each row displays the human-readable display name from the camera's GenICam node
// map; the raw node name is stored in Qt::UserRole so we can recover the order.
// On OK the new ordering is persisted to the registry and the panel refreshes.
void PinnedParamsPanel::showReorderDialog(QWidget* parent)
{
    QStringList pinned = loadPinnedList();

    if (pinned.isEmpty())
    {
        QMessageBox::information(parent, "Reorder Parameters",
            "No pinned parameters to reorder.\n"
            "Use ★ in the Advanced dialog to pin parameters first.");
        return;
    }

    QDialog dlg(parent);
    dlg.setWindowTitle("Reorder Pinned Parameters");
    dlg.setMinimumSize(320, 400);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);

    QLabel* hint = new QLabel("Drag rows to reorder, then click OK to save.", &dlg);
    hint->setStyleSheet("color: #888; font-style: italic;");
    layout->addWidget(hint);

    // QListWidget with InternalMove allows the user to drag rows into any order.
    // Qt handles the visual indicator and the underlying item movement automatically.
    QListWidget* list = new QListWidget(&dlg);
    list->setDragDropMode(QAbstractItemView::InternalMove);
    list->setDefaultDropAction(Qt::MoveAction);
    list->setSelectionMode(QAbstractItemView::SingleSelection);

    // Try to look up display names from the live camera node map.
    // Falls back to the raw node name if the camera is disconnected or the node
    // isn't found (which is fine — the raw name is still readable).
    GenApi::INodeMap* pMap = m_mgr ? m_mgr->getNodeMap() : nullptr;

    for (const QString& nodeName : pinned)
    {
        QString displayName = nodeName;  // Default to node name if no map is available
        if (pMap)
        {
            try
            {
                GenApi::INode* pNode = pMap->GetNode(nodeName.toStdString().c_str());
                if (pNode)
                    displayName = QString::fromLatin1(pNode->GetDisplayName().c_str());
            }
            catch (...) {}
        }

        QListWidgetItem* item = new QListWidgetItem(displayName, list);
        // Store the actual node name in UserRole — this is what we persist,
        // not the display name, because the display name can change between cameras.
        item->setData(Qt::UserRole, nodeName);
        list->addItem(item);
    }

    layout->addWidget(list, 1);  // stretch=1 so the list takes most of the dialog height

    // Delete button — removes the currently selected item from the list.
    // This unpins the parameter immediately (visible on OK).
    QPushButton* deleteButton = new QPushButton("Delete Selected", &dlg);
    deleteButton->setToolTip("Remove the selected parameter from the pinned list");
    deleteButton->setStyleSheet(
        "QPushButton { color: #c62828; }"
        "QPushButton:disabled { color: #aaa; }");
    deleteButton->setEnabled(false);  // Disabled until an item is selected

    // Enable/disable the button as the selection changes.
    connect(list, &QListWidget::itemSelectionChanged, deleteButton, [list, deleteButton]()
    {
        deleteButton->setEnabled(list->currentRow() >= 0);
    });

    // Delete the selected row when clicked.
    connect(deleteButton, &QPushButton::clicked, list, [list]()
    {
        int row = list->currentRow();
        if (row >= 0)
            delete list->takeItem(row);
    });

    layout->addWidget(deleteButton);

    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);

    if (dlg.exec() == QDialog::Accepted)
    {
        // Rebuild the ordered list from whatever order the user left the items in
        QStringList newOrder;
        for (int i = 0; i < list->count(); ++i)
            newOrder.append(list->item(i)->data(Qt::UserRole).toString());

        QSettings().setValue("pinnedParams", newOrder.join(","));
        refreshFromCamera();
    }
}


// =============================================================================
// setLiveMode — enable or disable live-apply mode
// =============================================================================
//
// When enabled, any widget change (editingFinished for spinboxes,
// currentIndexChanged for combos) immediately writes that single parameter
// to the camera.  The live-apply connections already exist in each widget
// (wired in buildRows); this flag gates whether they do anything.
void PinnedParamsPanel::setLiveMode(bool enabled)
{
    m_liveMode = enabled;
}


// =============================================================================
// applyLiveSingleNode — write one parameter to the camera immediately
// =============================================================================
//
// Called by the lambda connections wired in buildRows() when m_liveMode is true.
// Silently skips nodes that are not writable during streaming (e.g. PixelFormat).
// Does NOT call refreshFromCamera() so the panel doesn't flicker on every keystroke.
void PinnedParamsPanel::applyLiveSingleNode(const std::string& nodeName, QWidget* w)
{
    if (!m_mgr || !m_mgr->isConnected() || !w)
        return;

    // Check whether the node is currently writable before attempting a write.
    // During streaming many nodes are locked to RO or NA — skip them silently
    // rather than popping an error dialog that interrupts the user's workflow.
    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (pMap)
    {
        try
        {
            GenApi::INode* pNode = pMap->GetNode(nodeName.c_str());
            if (!pNode || !GenApi::IsWritable(pNode))
            {
                // Node is locked during streaming (e.g. PixelFormat, Width).
                // Log it quietly and turn the Apply button blue so the user knows
                // there's a pending change that will need to be applied after stopping.
                emit statusMessage(
                    QString("Live-apply skipped '%1': not writable during streaming — click Apply after stopping")
                        .arg(QString::fromStdString(nodeName)));
                emit parameterChanged();
                return;
            }
        }
        catch (...) { return; }
    }

    // Find this node's type in m_rows so we know which widget cast to use.
    // (We can't trust w's dynamic type alone — bool and enum both use QComboBox.)
    int nodeType = 0;
    for (const PinnedRow& row : m_rows)
    {
        if (row.nodeName == nodeName)
        {
            nodeType = row.nodeType;
            break;
        }
    }

    if (nodeType == 0)
        return;  // Node is unavailable — no type was stored

    std::string errMsg;
    bool ok = false;
    GenApi::EInterfaceType ifType = static_cast<GenApi::EInterfaceType>(nodeType);

    if (ifType == GenApi::intfIEnumeration)
    {
        QComboBox* combo = qobject_cast<QComboBox*>(w);
        if (combo)
            ok = m_mgr->setNodeEnumValue(nodeName, combo->currentText().toStdString(), errMsg);
    }
    else if (ifType == GenApi::intfIFloat)
    {
        QDoubleSpinBox* dspin = qobject_cast<QDoubleSpinBox*>(w);
        if (dspin)
            ok = m_mgr->setNodeDoubleValue(nodeName, dspin->value(), errMsg);
    }
    else if (ifType == GenApi::intfIInteger)
    {
        QSpinBox* spin = qobject_cast<QSpinBox*>(w);
        if (spin)
            ok = m_mgr->setNodeInt64Value(nodeName, static_cast<int64_t>(spin->value()), errMsg);
    }
    else if (ifType == GenApi::intfIBoolean)
    {
        QComboBox* combo = qobject_cast<QComboBox*>(w);
        if (combo)
            ok = m_mgr->setNodeBoolValue(nodeName, combo->currentIndex() == 1, errMsg);
    }

    if (ok)
    {
        emit statusMessage(
            QString("Live-applied '%1'").arg(QString::fromStdString(nodeName)));
        emit liveNodeApplied(QString::fromStdString(nodeName));
        // Refresh all other widget values from the camera so side-effects are visible
        // (e.g. changing binning adjusts the valid Width/Height the camera reports).
        // updateWidgetValuesFromCamera() updates values in-place — no rebuild/flicker.
        updateWidgetValuesFromCamera();
    }
    else if (!errMsg.empty())
    {
        // Write failed — the spinbox shows a value the camera didn't accept.
        // Turn the Apply button blue so the user knows there's a pending mismatch.
        emit statusMessage(
            QString("Live-apply failed for '%1': %2")
                .arg(QString::fromStdString(nodeName))
                .arg(QString::fromStdString(errMsg)));
        emit parameterChanged();
    }
}


// =============================================================================
// updateWidgetValuesFromCamera — read camera values into existing widgets in-place
// =============================================================================
//
// Called after applyLiveSingleNode() succeeds.  Writing one parameter can cause
// the camera to adjust others (e.g. increasing BinningHorizontal shrinks the
// maximum Width, which may also change the current Width).  This reads each
// pinned node's current value back from the camera and pushes it into the widget
// without destroying and recreating the widget — so there is no rebuild flicker
// and the user's focus/edit state is preserved.
//
// Signals are blocked on each widget while its value is updated so the dirty-flag
// and live-apply connections don't fire spuriously during the refresh pass.
void PinnedParamsPanel::updateWidgetValuesFromCamera()
{
    if (!m_mgr || !m_mgr->isConnected()) return;

    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap) return;

    for (const PinnedRow& row : m_rows)
    {
        if (!row.valueWidget || row.nodeType == 0) continue;

        GenApi::EInterfaceType ifType = static_cast<GenApi::EInterfaceType>(row.nodeType);

        try
        {
            GenApi::INode* pNode = pMap->GetNode(row.nodeName.c_str());
            if (!pNode || !GenApi::IsReadable(pNode)) continue;

            // Reflect writability changes (e.g. GainAuto Off → Gain becomes writable).
            row.valueWidget->setEnabled(GenApi::IsWritable(pNode));

            if (ifType == GenApi::intfIEnumeration)
            {
                QComboBox* combo = qobject_cast<QComboBox*>(row.valueWidget);
                if (!combo) continue;
                GenApi::CEnumerationPtr pEnum(pNode);
                if (!pEnum) continue;
                QString current = QString::fromLatin1(
                    pEnum->GetCurrentEntry()->GetSymbolic().c_str());
                QSignalBlocker blocker(combo);
                combo->setCurrentText(current);
            }
            else if (ifType == GenApi::intfIFloat)
            {
                QDoubleSpinBox* spin = qobject_cast<QDoubleSpinBox*>(row.valueWidget);
                if (!spin) continue;
                GenApi::CFloatPtr pFloat(pNode);
                if (!pFloat) continue;
                QSignalBlocker blocker(spin);
                spin->setValue(pFloat->GetValue());
            }
            else if (ifType == GenApi::intfIInteger)
            {
                QSpinBox* spin = qobject_cast<QSpinBox*>(row.valueWidget);
                if (!spin) continue;
                GenApi::CIntegerPtr pInt(pNode);
                if (!pInt) continue;
                QSignalBlocker blocker(spin);
                spin->setValue(static_cast<int>(pInt->GetValue()));
            }
            else if (ifType == GenApi::intfIBoolean)
            {
                QComboBox* combo = qobject_cast<QComboBox*>(row.valueWidget);
                if (!combo) continue;
                GenApi::CBooleanPtr pBool(pNode);
                if (!pBool) continue;
                QSignalBlocker blocker(combo);
                combo->setCurrentIndex(pBool->GetValue() ? 1 : 0);
            }
        }
        catch (...) {}  // If any node read fails, leave that widget as-is
    }
}
