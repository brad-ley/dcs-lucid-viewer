// =============================================================================
// AdvancedParamsDialog.cpp
// =============================================================================
//
// Implementation of the Advanced Parameters Dialog.
//
// KEY PATTERNS USED:
//   - GenICam category tree traversal (CCategoryPtr, NodeList_t)
//   - Dynamic widget creation based on node type at runtime
//   - qobject_cast<> for runtime type-checking of Qt widgets
//   - GenApi::EInterfaceType enum for determining node type
// =============================================================================

#include "AdvancedParamsDialog.h"
#include "CameraManager.h"
#include "PinnedParamsPanel.h"  // for addPinnedNode/removePinnedNode/isPinnedNode

// Qt includes
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QSplitter>
#include <QMessageBox>
#include <QScrollArea>
#include <QSizePolicy>
#include <QCompleter>
#include <QAbstractProxyModel>   // qobject_cast target for completer->completionModel()
#include <QStandardItemModel>
#include <QTimer>      // QTimer::singleShot — used to reset m_searchNavigating
#include <QSettings>   // QSettings — persists last-selected node name across dialog instances
#include <QMenu>       // QMenu — popup menus for Save Config / Load Config buttons
#include <QFileDialog> // QFileDialog — open/save dialogs for .json config files
#include <QInputDialog>// QInputDialog — password prompt for DCS settings save
#include <QJsonDocument>  // JSON serialization for config file save/load
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>      // Timestamp embedded in saved .json metadata
#include <QFile>          // QFile — used to read/write .json config files
#include <QStandardPaths>  // QStandardPaths::DocumentsLocation — default save folder fallback
#include <QProgressDialog> // Modal progress dialog shown during threaded load operations
#include <climits>         // INT_MIN, INT_MAX
#include <algorithm>       // std::sort — alphabetical ordering of categories and features
#include <functional>      // std::function — progress callback type for loadSettingsFromJson
#include <thread>          // std::thread — off-main-thread load so the UI stays responsive

// GenICam / Arena SDK headers — the full parameter node system
#include "GenApi/GenApi.h"

// Arena SDK — needed for SetNodeValue / GetNodeValue used in config save/load helpers
#include "ArenaApi.h"




// =============================================================================
// Constructor
// =============================================================================
AdvancedParamsDialog::AdvancedParamsDialog(CameraManager* mgr, QWidget* parent)
    : QDialog(parent)
    , m_mgr(mgr)
    , m_visibilityCombo(nullptr)
    , m_currentValueWidget(nullptr)
    , m_currentNodeType(0)
    , m_favoriteButton(nullptr)
    , m_searchModel(new QStandardItemModel(this))
    , m_featureCompleter(new QCompleter(m_searchModel, this))
{
    setWindowTitle("Advanced Camera Parameters");
    setMinimumSize(560, 540);
    setSizeGripEnabled(true);  // Allow the user to resize the dialog

    // =========================================================================
    // Layout structure:
    //
    //   [Refresh]
    //   Show:     [All Levels | Front Panel | Beginner | Expert | Guru]
    //   Category: [dropdown]
    //   Feature:  [dropdown]
    //   ─────────────────────────────
    //   [Info label: description + range]
    //   Value: [dynamic widget]
    //   [Apply] [Pin] [Close]
    // =========================================================================

    QVBoxLayout* outerLayout = new QVBoxLayout(this);
    outerLayout->setSpacing(8);
    outerLayout->setContentsMargins(12, 12, 12, 12);

    // --- Top button row: Load Config | Save Config | Refresh Node Tree ---
    //
    // Three buttons that each expand equally to fill the full dialog width.
    // QSizePolicy::Expanding + a stretch factor of 1 on each button achieves this
    // without any fixed widths — the buttons stay balanced when the dialog is resized.
    QHBoxLayout* topButtonRow = new QHBoxLayout();
    topButtonRow->setSpacing(6);

    m_loadConfigButton = new QPushButton("Load Config", this);
    m_loadConfigButton->setToolTip(
        "Load camera settings from a UserSet stored on the camera or from a .json file.\n"
        "After loading, you will be prompted to enable the LineStatusAll chunk if it is off.");
    m_loadConfigButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_saveConfigButton = new QPushButton("Save Config", this);
    m_saveConfigButton->setToolTip(
        "Save current camera settings.\n"
        "  Save Custom — writes to UserSet2 on camera + prompts for a .json file\n"
        "  Save DCS    — writes to UserSet1 (password protected; facility default)");
    m_saveConfigButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_refreshButton = new QPushButton("Refresh Node Tree", this);
    m_refreshButton->setToolTip(
        "Re-scan the camera's parameter tree.\n"
        "Use this if you think parameter availability has changed.");
    m_refreshButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    topButtonRow->addWidget(m_loadConfigButton, 1);
    topButtonRow->addWidget(m_saveConfigButton, 1);
    topButtonRow->addWidget(m_refreshButton,    1);

    outerLayout->addLayout(topButtonRow);

    // --- Navigation form: visibility filter + category + feature ---
    QFormLayout* navForm = new QFormLayout();
    navForm->setRowWrapPolicy(QFormLayout::DontWrapRows);
    navForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // ---- Visibility filter ----
    // GenICam assigns every node a visibility level that indicates how advanced
    // the setting is. Filtering by level makes the list much more manageable:
    //   Beginner  — basics every user needs (exposure, gain, pixel format)
    //   Expert    — Beginner + moderately advanced settings
    //   Guru      — Beginner + Expert + deep camera internals
    //   Front Panel — shows only parameters pinned to the main window panel
    //   All Levels  — every readable node regardless of level
    m_visibilityCombo = new QComboBox(this);
    // NOTE: "Front Panel" is not a GenICam visibility level — it is a local filter
    // that matches against the app's pinned-params registry list.  GenICam has no
    // built-in "Favorites" concept in the standard node map.
    m_visibilityCombo->addItem("All Levels");    // index 0
    m_visibilityCombo->addItem("Front Panel");   // index 1 — pinned to main window panel
    m_visibilityCombo->addItem("Beginner");      // index 2 → GenApi::Beginner (0)
    m_visibilityCombo->addItem("Expert");        // index 3 → GenApi::Expert   (1) and below
    m_visibilityCombo->addItem("Guru");          // index 4 → GenApi::Guru     (2) and below
    m_visibilityCombo->setCurrentIndex(0);
    m_visibilityCombo->setToolTip(
        "Filter the Feature list by parameter complexity.\n"
        "  Beginner    — common settings safe for everyday use\n"
        "  Expert      — adds less common, more technical settings\n"
        "  Guru        — adds low-level camera internals\n"
        "  Front Panel — shows only parameters pinned to the main window\n"
        "  All Levels  — shows every readable parameter");
    navForm->addRow("Show:", m_visibilityCombo);

    // ---- Category dropdown ----
    m_categoryCombo = new QComboBox(this);
    m_categoryCombo->setToolTip(
        "Top-level parameter categories from the camera's GenICam node tree.\n"
        "Examples: Acquisition Control, Image Format Control, Analog Control.");
    navForm->addRow("Category:", m_categoryCombo);

    // ---- Feature dropdown ----
    //
    // setEditable(true) adds a QLineEdit inside the combo so the user can type to search.
    // setInsertPolicy(NoInsert) prevents typed text from being added as new combo items.
    // We attach a custom completer (m_featureCompleter) that searches ALL categories —
    // not just the items currently visible in the combo's own list.
    m_featureCombo = new QComboBox(this);
    m_featureCombo->setEditable(true);
    m_featureCombo->setInsertPolicy(QComboBox::NoInsert);
    m_featureCombo->setToolTip(
        "Type to search any parameter across all categories.\n"
        "Selecting a result will switch to that parameter's category automatically.\n"
        "Or use the Category dropdown above to browse normally.");

    // Configure the cross-category completer:
    //   Qt::MatchContains   — "gain" matches "AutoGain", "GainAuto", "Gain", etc.
    //   CaseInsensitive     — case doesn't matter
    //   PopupCompletion     — show a dropdown popup as the user types
    m_featureCompleter->setCompletionColumn(0);
    m_featureCompleter->setCompletionRole(Qt::DisplayRole);
    m_featureCompleter->setFilterMode(Qt::MatchContains);
    m_featureCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_featureCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_featureCombo->setCompleter(m_featureCompleter);

    // ---- Pin button beside the feature combo ----
    // Wrap the combo and the pin button in a container so QFormLayout
    // sees a single widget for the "Feature:" row.
    QWidget*     featureRow  = new QWidget(this);
    QHBoxLayout* featureHBox = new QHBoxLayout(featureRow);
    featureHBox->setContentsMargins(0, 0, 0, 0);
    featureHBox->setSpacing(4);
    featureHBox->addWidget(m_featureCombo, 1);   // combo stretches to fill the row

    // ---- Pin button ----
    // Created here so it sits visually next to the combo before the form is added.
    // The icon and text are set by updateFavoriteButton(); here we just establish
    // the fixed width (wide enough for "Unpin" + icon) so the layout never shifts.
    m_favoriteButton = new QPushButton("☆", this);  // ☆ empty star — updated by updateFavoriteButton()
    m_favoriteButton->setEnabled(false);
    m_favoriteButton->setFixedSize(32, 32);
    m_favoriteButton->setToolTip(
        "Pin this parameter to the main window panel.\n"
        "Pinned parameters are saved to the registry and persist across sessions.");
    m_favoriteButton->setAutoDefault(false);
    featureHBox->addWidget(m_favoriteButton, 0); // button does not stretch

    navForm->addRow("Feature:", featureRow);

    outerLayout->addLayout(navForm);

    // --- Separator ---
    QFrame* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    outerLayout->addWidget(line);

    // --- Info label ---
    // Shows the node's description text and numeric range (if applicable).
    // Wraps text so long descriptions don't explode the dialog width.
    m_infoLabel = new QLabel(
        "Select a feature above to view its description and current value.", this);
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setStyleSheet("color: #666; font-style: italic;");
    m_infoLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_infoLabel->setMinimumHeight(48);
    outerLayout->addWidget(m_infoLabel);

    // --- Value area ---
    // A labeled group box that holds the dynamic value widget.
    // The widget inside is replaced each time the user picks a different feature.
    QGroupBox* valueGroup = new QGroupBox("Value", this);
    m_valueLayout = new QVBoxLayout(valueGroup);
    m_valueLayout->setContentsMargins(8, 8, 8, 8);
    outerLayout->addWidget(valueGroup);

    // ---- Disable the QDialog default-button mechanism ----
    //
    // In any QDialog, QPushButton has autoDefault=true by default.  Qt would then
    // simulate a click on the "default" button whenever the user presses Enter —
    // even while focused inside a spinbox.  Setting autoDefault=false prevents that.
    // m_favoriteButton->setAutoDefault(false) is already set where it is created above.
    m_refreshButton->setAutoDefault(false);

    // Disable the QDialog default-button mechanism for all top-row buttons.
    // autoDefault=true would cause Qt to "click" whichever button has focus when
    // the user presses Enter, which is surprising inside a parameter browser.
    m_saveConfigButton->setAutoDefault(false);
    m_loadConfigButton->setAutoDefault(false);

    // Apply and Close buttons have been intentionally removed:
    //   - Apply: all widget types auto-apply on change/Enter/focus-loss, so a manual
    //     Apply button is redundant.  Command nodes use their own inline Execute button.
    //   - Close: the window's X button is sufficient; a duplicate button adds clutter.

    // --- Wire up signals ---
    connect(m_refreshButton,    &QPushButton::clicked,
            this, &AdvancedParamsDialog::onRefreshClicked);
    connect(m_saveConfigButton, &QPushButton::clicked,
            this, &AdvancedParamsDialog::onSaveConfigClicked);
    connect(m_loadConfigButton, &QPushButton::clicked,
            this, &AdvancedParamsDialog::onLoadConfigClicked);
    connect(m_visibilityCombo,  &QComboBox::currentIndexChanged,
            this, &AdvancedParamsDialog::onVisibilityFilterChanged);
    connect(m_categoryCombo,    &QComboBox::currentIndexChanged,
            this, &AdvancedParamsDialog::onCategoryChanged);

    // Use activated(int) instead of currentIndexChanged for the feature combo.
    // activated fires only when the user explicitly picks an item (click or Enter),
    // not while they are typing in the editable line edit.  This prevents the value
    // widget from rebuilding on every keystroke during a search.
    connect(m_featureCombo, QOverload<int>::of(&QComboBox::activated),
            this, &AdvancedParamsDialog::onFeatureChanged);

    // Cross-category search: when the completer's popup delivers a selection,
    // flip the category and navigate to the matching feature.
    connect(m_featureCompleter, QOverload<const QString&>::of(&QCompleter::activated),
            this, &AdvancedParamsDialog::onSearchCompleterActivated);
    connect(m_favoriteButton,   &QPushButton::clicked,
            this, &AdvancedParamsDialog::onFavoriteClicked);

    // --- Initial population ---
    // Cache the saved node name NOW, before populateCategories() → onFeatureChanged(0)
    // overwrites QSettings with the first feature of the first category.
    m_pendingRestoreNode = QSettings().value("AdvancedParamsDialog/lastNodeName").toString();
    populateCategories();

    // Reopen to the parameter the user was looking at last time (if any).
    restoreLastSelection();
}


// =============================================================================
// restoreLastSelection — navigate to the last-opened parameter
// =============================================================================
//
// The dialog is created fresh from the stack on every "Advanced..." click, so
// the combos always start at index 0.  We save the selected node name to
// QSettings in onFeatureChanged and onSearchCompleterActivated, then call this
// once after populateCategories() so the dialog reopens in the same place.
//
// The restore logic mirrors onSearchCompleterActivated: switch to "All Levels"
// so the node is guaranteed to be visible regardless of any saved filter state.
void AdvancedParamsDialog::restoreLastSelection()
{
    QString savedNode = m_pendingRestoreNode;
    if (savedNode.isEmpty()) return;

    // The node must appear in the search index — if the camera no longer exposes
    // it (different model, firmware update) just silently stay at index 0.
    QString catNodeName = m_nodeToCategory.value(savedNode);
    if (catNodeName.isEmpty()) return;

    // Make every category visible so the target is always findable.
    if (m_visibilityCombo->currentIndex() != 0)
    {
        m_visibilityCombo->blockSignals(true);
        m_visibilityCombo->setCurrentIndex(0);
        m_visibilityCombo->blockSignals(false);

        m_categoryCombo->blockSignals(true);
        m_categoryCombo->clear();
        for (const auto& pair : m_allCategories)
            m_categoryCombo->addItem(pair.first, pair.second);
        m_categoryCombo->blockSignals(false);
    }

    // Switch category combo, repopulate features.
    int catIdx = m_categoryCombo->findData(catNodeName);
    if (catIdx < 0) return;

    m_categoryCombo->blockSignals(true);
    m_categoryCombo->setCurrentIndex(catIdx);
    m_categoryCombo->blockSignals(false);

    // m_searchNavigating suppresses the spurious onFeatureChanged(0) that
    // populateFeatures fires — we select the real feature explicitly below.
    m_searchNavigating = true;
    populateFeatures(catNodeName);
    m_searchNavigating = false;

    int featIdx = m_featureCombo->findData(savedNode);
    if (featIdx < 0) return;

    m_featureCombo->blockSignals(true);
    m_featureCombo->setCurrentIndex(featIdx);
    m_featureCombo->blockSignals(false);

    showValueWidget(savedNode.toStdString());
    updateFavoriteButton();
}


// =============================================================================
// populateCategories — walk Root category and fill the category combo
// =============================================================================
void AdvancedParamsDialog::populateCategories()
{
    // Block signals so that adding items doesn't trigger onCategoryChanged
    // before we finish populating (which would try to populate features before
    // the category list is ready).
    m_categoryCombo->blockSignals(true);
    m_categoryCombo->clear();
    m_featureCombo->clear();
    clearValueWidget();
    m_categoryCombo->blockSignals(false);

    // Also reset the full unfiltered category list; it gets rebuilt below.
    m_allCategories.clear();

    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap)
    {
        m_infoLabel->setText("No camera connected.");
        return;
    }

    // The GenICam standard mandates a "Root" category node at the top of the tree.
    // Every camera parameter is reachable by walking the tree from Root.
    GenApi::CNodePtr pRootNode = pMap->GetNode("Root");
    GenApi::CCategoryPtr pRoot(pRootNode);
    if (!pRoot)
    {
        m_infoLabel->setText("Could not find Root category node — unusual camera.");
        return;
    }

    // GetFeatures() fills a FeatureList_t (a std::vector<IValue*>) with the
    // direct children of this category node.
    // Note: this SDK version uses FeatureList_t (not NodeList_t) for GetFeatures().
    // IValue inherits from INode, so IValue* implicitly converts to INode*.
    GenApi::FeatureList_t rootChildren;
    pRoot->GetFeatures(rootChildren);

    // First pass: collect all categories into m_allCategories
    for (GenApi::IValue* pChild : rootChildren)
    {
        // C++ CONCEPT — GenICam typed pointer cast:
        //   CCategoryPtr can be constructed from any INode*. Internally it does
        //   a dynamic_cast. If the node is actually a category, the pointer is valid.
        //   If not (e.g., it's a leaf node), the pointer is null/empty.
        GenApi::CCategoryPtr pCat(pChild);
        if (!pCat || !GenApi::IsReadable(pCat))
            continue;  // Skip non-category and unreadable children

        // Store the internal GenICam node name as "user data" on the combo item.
        // GetDisplayName() and GetName() live on INode*, not on ICategory/IValue.
        // GetNode() is inherited from IValue and returns the underlying INode*,
        // which provides the human-readable label and internal ID we need.
        GenApi::INode* pCatNode = pCat->GetNode();
        QString displayName = QString::fromLatin1(pCatNode->GetDisplayName().c_str());
        QString nodeName    = QString::fromLatin1(pCatNode->GetName().c_str());

        m_allCategories.append({displayName, nodeName});
    }

    // Sort alphabetically by display name before populating the combo
    std::sort(m_allCategories.begin(), m_allCategories.end(),
              [](const QPair<QString,QString>& a, const QPair<QString,QString>& b)
              { return a.first.compare(b.first, Qt::CaseInsensitive) < 0; });

    m_categoryCombo->blockSignals(true);
    for (const auto& pair : m_allCategories)
        m_categoryCombo->addItem(pair.first, pair.second);
    m_categoryCombo->blockSignals(false);

    // Trigger feature population for the first category
    if (m_categoryCombo->count() > 0)
        onCategoryChanged(0);

    // Build the cross-category search index now that all categories are known.
    // This must run after the category combo is fully populated.
    buildSearchIndex();
}


// =============================================================================
// populateFeatures — fill feature combo with leaf nodes in the given category
// =============================================================================
//
// We recurse through the category's subtree and collect all NON-category nodes
// (leaf nodes). This flattens sub-categories (e.g., "Sequencer Control" under
// "Acquisition Control") into a single list — simpler than a tree widget.
//
// C++ CONCEPT — local helper lambda:
//   We define a recursive lambda inside this function. Lambdas that call
//   themselves must capture themselves by reference using std::function.
void AdvancedParamsDialog::populateFeatures(const QString& categoryNodeName)
{
    m_featureCombo->blockSignals(true);
    m_featureCombo->clear();
    m_featureCombo->blockSignals(false);
    clearValueWidget();

    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap) return;

    GenApi::CNodePtr pCatNode = pMap->GetNode(categoryNodeName.toStdString().c_str());
    GenApi::CCategoryPtr pCat(pCatNode);
    if (!pCat) return;

    // Collect all leaf nodes recursively into this vector.
    // Each entry is (displayName, nodeName).
    QVector<QPair<QString, QString>> leaves;

    // C++ CONCEPT — std::function for recursive lambda:
    //   A regular lambda can't refer to itself. std::function<void(...)> lets us
    //   store the lambda and capture it by reference so the lambda can call itself.
    std::function<void(GenApi::CCategoryPtr)> collectLeaves =
        [&](GenApi::CCategoryPtr pParent)
    {
        GenApi::FeatureList_t children;
        pParent->GetFeatures(children);

        for (GenApi::IValue* pNode : children)
        {
            GenApi::CCategoryPtr pSubCat(pNode);
            if (pSubCat && GenApi::IsReadable(pSubCat))
            {
                // Recurse into sub-categories
                collectLeaves(pSubCat);
            }
            else if (GenApi::IsAvailable(pNode))
            {
                // Leaf node — IValue* and INode* are separate parallel interfaces in this SDK.
                // static_cast between them is illegal. GetNode() returns the underlying INode*
                // which provides GetDisplayName(), GetName(), and GetVisibility().
                //
                // NOTE: We use IsAvailable() (not IsReadable()) so that Command nodes are
                // included. Command nodes are write-only — IsReadable() returns false for them
                // even though they are perfectly valid, accessible nodes. IsAvailable() returns
                // true for any node whose AccessMode is RO, WO, or RW (i.e. not NA/NI).
                GenApi::INode* pINode = pNode->GetNode();
                QString display = QString::fromLatin1(pINode->GetDisplayName().c_str());
                QString name    = QString::fromLatin1(pINode->GetName().c_str());

                // ---- Visibility / Front Panel filter ----
                //
                // m_visibilityCombo index mapping:
                //   0 = All Levels  — no filtering
                //   1 = Front Panel — only nodes currently pinned to the main window
                //   2 = Beginner    — GenApi::Beginner (0) only
                //   3 = Expert      — Beginner + Expert (≤ 1)
                //   4 = Guru        — Beginner + Expert + Guru (≤ 2)
                //
                // GenApi::EVisibility values: Beginner=0, Expert=1, Guru=2, Invisible=3
                // For levels 2-4 we show all nodes AT OR BELOW the chosen level, which
                // matches the GenICam convention ("Expert viewer sees Beginner + Expert nodes").
                int filterIdx = m_visibilityCombo->currentIndex();

                if (filterIdx == 1)
                {
                    // Front Panel — only show nodes pinned to the main window panel
                    if (!PinnedParamsPanel::isPinnedNode(name))
                        continue;
                }
                else if (filterIdx == 2)
                {
                    // Beginner — skip anything above Beginner
                    if (pINode->GetVisibility() > GenApi::Beginner)
                        continue;
                }
                else if (filterIdx == 3)
                {
                    // Expert — show Beginner and Expert; skip Guru and Invisible
                    if (pINode->GetVisibility() > GenApi::Expert)
                        continue;
                }
                else if (filterIdx == 4)
                {
                    // Guru — show everything up to Guru; skip Invisible
                    if (pINode->GetVisibility() > GenApi::Guru)
                        continue;
                }
                // filterIdx == 0 (All Levels): no filtering, always include

                leaves.append({display, name});
            }
        }
    };

    collectLeaves(pCat);

    // Sort alphabetically by display name before populating the combo
    std::sort(leaves.begin(), leaves.end(),
              [](const QPair<QString,QString>& a, const QPair<QString,QString>& b)
              { return a.first.compare(b.first, Qt::CaseInsensitive) < 0; });

    m_featureCombo->blockSignals(true);
    for (const auto& pair : leaves)
    {
        m_featureCombo->addItem(pair.first, pair.second);  // Display, NodeName as data
    }
    m_featureCombo->blockSignals(false);

    // Show the first feature's value widget
    if (m_featureCombo->count() > 0)
        onFeatureChanged(0);
}


// =============================================================================
// buildSearchIndex — collect every readable leaf node from every category
// =============================================================================
//
// This runs once after populateCategories() (and again after Refresh).
// It walks all categories — ignoring the current visibility filter — so that
// the user can search for any parameter regardless of the Show setting.
//
// Data stored per node:
//   QStandardItem::text()       → human-readable display name (shown in popup)
//   QStandardItem::data(UserRole) → internal GenICam node name (used for lookup)
//   m_nodeToCategory[nodeName]  → which category node name owns this node
void AdvancedParamsDialog::buildSearchIndex()
{
    m_searchModel->clear();
    m_nodeToCategory.clear();
    m_displayToNode.clear();

    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap) return;

    // Iterate m_allCategories (the unfiltered list) so the search index always
    // covers every parameter regardless of what the visibility filter is set to.
    for (const auto& catPair : m_allCategories)
    {
        const QString& catDisplayName = catPair.first;   // e.g. "Acquisition Control"
        const QString& catNodeName    = catPair.second;  // e.g. "AcquisitionControl"

        GenApi::CNodePtr pCatNode = pMap->GetNode(catNodeName.toStdString().c_str());
        GenApi::CCategoryPtr pCat(pCatNode);
        if (!pCat) continue;

        // Recursive lambda — collect all leaf nodes under pParent into the model.
        // We use std::function so the lambda can refer to itself.
        std::function<void(GenApi::CCategoryPtr)> collectAll =
            [&](GenApi::CCategoryPtr pParent)
        {
            GenApi::FeatureList_t children;
            pParent->GetFeatures(children);

            for (GenApi::IValue* pChild : children)
            {
                GenApi::CCategoryPtr pSubCat(pChild);
                if (pSubCat && GenApi::IsReadable(pSubCat))
                {
                    collectAll(pSubCat);   // recurse into sub-categories
                }
                else if (GenApi::IsAvailable(pChild))
                {
                    // Use IsAvailable() instead of IsReadable() so that Command nodes
                    // (which are write-only and fail IsReadable()) appear in search results.
                    GenApi::INode* pINode = pChild->GetNode();
                    QString display  = QString::fromLatin1(pINode->GetDisplayName().c_str());
                    QString nodeName = QString::fromLatin1(pINode->GetName().c_str());

                    // First category that exposes this node wins; skip duplicates.
                    if (!m_nodeToCategory.contains(nodeName))
                    {
                        m_nodeToCategory[nodeName] = catNodeName;

                        // Display "Category → Feature" so the user can see which
                        // category owns each result without switching first.
                        // fromUtf8() ensures the → arrow (U+2192) is decoded correctly
                        // rather than being misread as Latin-1 garbage bytes.
                        QString itemText = catDisplayName
                                         + QString::fromUtf8(" \xe2\x86\x92 ")
                                         + display;
                        QStandardItem* item = new QStandardItem(itemText);
                        item->setData(nodeName, Qt::UserRole);
                        m_searchModel->appendRow(item);

                        // Also store the display text → node name mapping so
                        // onSearchCompleterActivated can look up the node name by
                        // reading the item's DisplayRole — which is always routed
                        // correctly through any number of proxy layers.
                        m_displayToNode[itemText] = nodeName;
                    }
                }
            }
        };

        collectAll(pCat);
    }
}


// =============================================================================
// hasVisibleFeatures — check if any leaf node in a category passes a filter
// =============================================================================
//
// Walks the category subtree exactly like collectLeaves() in populateFeatures().
// Returns true as soon as one qualifying node is found (early exit).
// Used by onVisibilityFilterChanged() to decide whether to include a category.
bool AdvancedParamsDialog::hasVisibleFeatures(const QString& catNodeName, int filterIdx) const
{
    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap) return false;

    GenApi::CNodePtr pCatNode = pMap->GetNode(catNodeName.toStdString().c_str());
    GenApi::CCategoryPtr pCat(pCatNode);
    if (!pCat) return false;

    bool found = false;

    // Recursive lambda — mirrors the visibility logic in populateFeatures().
    std::function<void(GenApi::CCategoryPtr)> check = [&](GenApi::CCategoryPtr pParent)
    {
        if (found) return;  // Early exit once we know the category qualifies

        GenApi::FeatureList_t children;
        pParent->GetFeatures(children);

        for (GenApi::IValue* pNode : children)
        {
            if (found) break;

            GenApi::CCategoryPtr pSubCat(pNode);
            if (pSubCat && GenApi::IsReadable(pSubCat))
            {
                check(pSubCat);  // Recurse
            }
            else if (GenApi::IsReadable(pNode))
            {
                GenApi::INode* pINode = pNode->GetNode();
                QString name = QString::fromLatin1(pINode->GetName().c_str());

                if (filterIdx == 1)
                {
                    if (!PinnedParamsPanel::isPinnedNode(name)) continue;
                }
                else if (filterIdx == 2)
                {
                    if (pINode->GetVisibility() > GenApi::Beginner) continue;
                }
                else if (filterIdx == 3)
                {
                    if (pINode->GetVisibility() > GenApi::Expert) continue;
                }
                else if (filterIdx == 4)
                {
                    if (pINode->GetVisibility() > GenApi::Guru) continue;
                }

                found = true;  // This node passes the filter
            }
        }
    };

    check(pCat);
    return found;
}


// =============================================================================
// SLOT: onSearchCompleterActivated — jump to the category that owns the result
// =============================================================================
//
// Called when the user selects an entry from the completer popup.
// We use QCompleter::activated(const QString&) — Qt calls pathFromIndex() and
// delivers the item's display text directly, with no proxy-model indirection.
// That text is the exact key we stored in m_displayToNode in buildSearchIndex().
//
// ---- The spurious-signal problem ----
//
// After this slot returns, Qt fires a spurious QComboBox::activated(int) on the
// feature combo.  We block that by setting m_searchNavigating = true BEFORE
// returning, and resetting it via QTimer::singleShot(0) so the reset happens on
// the NEXT event-loop iteration (after the spurious signal fires).
//
// All navigation is done SYNCHRONOUSLY here — only the flag reset is deferred.
void AdvancedParamsDialog::onSearchCompleterActivated(const QString& completionText)
{
    // Set guard BEFORE any early returns so the spurious activated(int) is always blocked.
    m_searchNavigating = true;

    // completionText is the full "Category → Feature" display text from the popup.
    // Look it up directly in m_displayToNode — no proxy indirection needed.
    QString nodeName = m_displayToNode.value(completionText);

    if (nodeName.isEmpty())
    {
        // Defer the reset so the spurious activated(int) is still blocked after we return.
        QTimer::singleShot(0, this, [this]{ m_searchNavigating = false; });
        return;
    }

    QString catNodeName = m_nodeToCategory.value(nodeName);
    if (catNodeName.isEmpty())
    {
        QTimer::singleShot(0, this, [this]{ m_searchNavigating = false; });
        return;
    }

    // Restore "All Levels" visibility so the target category and feature are always
    // findable in the combos, regardless of what filter the user had active.
    if (m_visibilityCombo->currentIndex() != 0)
    {
        m_visibilityCombo->blockSignals(true);
        m_visibilityCombo->setCurrentIndex(0);
        m_visibilityCombo->blockSignals(false);

        // Refill the category combo with the full unfiltered list.
        m_categoryCombo->blockSignals(true);
        m_categoryCombo->clear();
        for (const auto& pair : m_allCategories)
            m_categoryCombo->addItem(pair.first, pair.second);
        m_categoryCombo->blockSignals(false);
    }

    // Switch the category combo to the one that owns this node.
    int catIdx = m_categoryCombo->findData(catNodeName);
    if (catIdx < 0)
    {
        QTimer::singleShot(0, this, [this]{ m_searchNavigating = false; });
        return;
    }

    m_categoryCombo->blockSignals(true);
    m_categoryCombo->setCurrentIndex(catIdx);
    m_categoryCombo->blockSignals(false);

    // Rebuild the feature list for the new category.
    // populateFeatures() ends by calling onFeatureChanged(0), but m_searchNavigating
    // is still true so that call is suppressed — we manually pick the right feature below.
    populateFeatures(catNodeName);

    // Select the target feature.
    int featIdx = m_featureCombo->findData(nodeName);
    if (featIdx >= 0)
    {
        m_featureCombo->blockSignals(true);
        m_featureCombo->setCurrentIndex(featIdx);
        m_featureCombo->blockSignals(false);
    }

    // Show the value widget for the selected feature now (synchronously).
    if (featIdx >= 0)
    {
        QString nodeNameQ = m_featureCombo->itemData(featIdx).toString();
        showValueWidget(nodeNameQ.toStdString());
        updateFavoriteButton();
        QSettings().setValue("AdvancedParamsDialog/lastNodeName", nodeNameQ);
    }

    // Defer two clean-up steps until the next event-loop iteration so they run
    // AFTER the completer's own post-activation processing (which writes the
    // "Category → Feature" search text back into the line edit and may emit
    // a spurious activated(int) on the feature combo):
    //   1. Overwrite the search text with just the plain feature display name.
    //   2. Reset the navigation guard so normal user interaction resumes.
    QString featureName = (featIdx >= 0) ? m_featureCombo->itemText(featIdx) : QString();
    QTimer::singleShot(0, this, [this, featureName, featIdx]
    {
        if (featIdx >= 0 && m_featureCombo->lineEdit())
            m_featureCombo->lineEdit()->setText(featureName);
        m_searchNavigating = false;
    });
}


// =============================================================================
// showValueWidget — create a type-appropriate widget for the given node
// =============================================================================
//
// The widget is different for each node type:
//   Enumeration → QComboBox (options read from camera)
//   Float       → QDoubleSpinBox (range read from camera)
//   Integer     → QSpinBox (range read from camera)
//   Boolean     → QComboBox with "false"/"true"
//   String      → QLineEdit (read-only if not writable)
//   Command     → QPushButton "Execute" (sends the command immediately on Apply)
//   Other       → QLabel "(unsupported type)"
void AdvancedParamsDialog::showValueWidget(const std::string& nodeName)
{
    clearValueWidget();
    m_currentNodeName = nodeName;
    m_currentNodeType = 0;
    m_infoLabel->setText("");

    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap) return;

    GenApi::CNodePtr pNode = pMap->GetNode(nodeName.c_str());
    // IsAvailable() returns true for RO, WO, and RW nodes — including Command nodes
    // which are write-only and therefore return false from IsReadable().
    // Using IsReadable() here was silently blocking Command nodes from showing their
    // Execute button and displaying "Node not available." instead.
    if (!pNode || !GenApi::IsAvailable(pNode))
    {
        m_infoLabel->setText("Node not available.");
        return;
    }

    // Force a fresh read from the camera. GenICam caches node values and only
    // auto-invalidates when a declared dependency is written — so a node like
    // TriggerSelector can show a stale value if TriggerSource was changed externally
    // or via a different panel. InvalidateNode() clears the cache so the very next
    // GetValue()/GetCurrentEntry() call re-polls the camera.
    pNode->InvalidateNode();

    // --- Build the info label ---
    // Show description and (for numeric nodes) range.
    // GetDescription() returns a verbose string from the camera's GenICam XML.
    QString description = QString::fromLatin1(pNode->GetDescription().c_str()).trimmed();
    QString infoText    = description.isEmpty() ? "(no description)" : description;

    bool isWritable = GenApi::IsWritable(pNode);
    if (!isWritable)
        infoText += "\n[Read-only]";

    // --- Create the widget based on node type ---
    GenApi::EInterfaceType ifType = pNode->GetPrincipalInterfaceType();
    m_currentNodeType = static_cast<int>(ifType);
    QWidget* widget = nullptr;

    if (ifType == GenApi::intfIEnumeration)
    {
        GenApi::CEnumerationPtr pEnum(pNode);
        QComboBox* combo = new QComboBox(this);

        GenApi::NodeList_t entries;
        pEnum->GetEntries(entries);
        for (GenApi::INode* pEntry : entries)
        {
            GenApi::CEnumEntryPtr pEnumEntry(pEntry);
            if (pEnumEntry && GenApi::IsReadable(pEnumEntry))
                combo->addItem(QString::fromLatin1(pEnumEntry->GetSymbolic().c_str()));
        }

        if (GenApi::IsReadable(pEnum))
            combo->setCurrentText(
                QString::fromLatin1(pEnum->GetCurrentEntry()->GetSymbolic().c_str()));

        combo->setEnabled(isWritable);

        // Auto-apply when the user picks a different enum value.
        // QComboBox::activated fires only on explicit user interaction (click or
        // keyboard selection), NOT when setCurrentText/setCurrentIndex is called
        // programmatically in showValueWidget().  This prevents spurious camera
        // writes every time the user navigates to a different feature.
        // The deferred QTimer::singleShot(0) is used here too so the combo's own
        // selection-commit logic finishes before onApplyClicked reads combo->currentText().
        if (isWritable)
            connect(combo, QOverload<int>::of(&QComboBox::activated), this, [this]{
                QTimer::singleShot(0, this, &AdvancedParamsDialog::onApplyClicked);
            });

        widget = combo;
    }
    else if (ifType == GenApi::intfIFloat)
    {
        GenApi::CFloatPtr pFloat(pNode);
        QDoubleSpinBox* spin = new QDoubleSpinBox(this);

        double minVal = pFloat->GetMin();
        double maxVal = pFloat->GetMax();
        spin->setRange(minVal, maxVal);
        spin->setDecimals(4);

        // Use the camera's declared fixed increment as the arrow-button step when
        // available.  Without this, the default step of (range/100) can be smaller
        // than the camera's quantization — e.g., 0.48 for a 0-48 dB range — so
        // the camera rounds the incremented value back to the previous value and
        // the up/down arrows appear to do nothing.
        // For parameters with no declared increment (continuous float), fall back
        // to 1% of the range, which is a reasonable interactive step size.
        double step = (maxVal - minVal) / 100.0;
        if (pFloat->GetIncMode() == GenApi::fixedIncrement)
            step = pFloat->GetInc();
        spin->setSingleStep(step);
        spin->setValue(pFloat->GetValue());
        spin->setEnabled(isWritable);

        // Append unit to the suffix if the node exposes one
        QString unit = QString::fromLatin1(pFloat->GetUnit().c_str());
        if (!unit.isEmpty())
            spin->setSuffix(" " + unit);

        infoText += QString("\nRange: %1 – %2")
                        .arg(minVal, 0, 'g', 6)
                        .arg(maxVal, 0, 'g', 6);
        if (!unit.isEmpty()) infoText += " " + unit;

        // setKeyboardTracking(false) changes when valueChanged fires:
        //   DEFAULT (tracking=true):  fires on EVERY keystroke while the user types
        //                             — sends "4" before the user finishes typing "40"
        //   tracking=false:           fires only when the value is COMMITTED:
        //                               • Enter or Return key
        //                               • Widget loses focus
        //                               • Up/down arrow button clicked
        //
        // This gives us exactly the right behaviour: arrow clicks apply immediately,
        // but typing a multi-digit number waits until the user confirms with Enter or
        // moves focus away.  We can drop editingFinished entirely — it is now redundant.
        //
        // The connect is placed AFTER setValue() so the programmatic initial load
        // does NOT trigger an auto-apply when the widget is first created.
        //
        // QTimer::singleShot(0) defers the call one event-loop iteration so
        // onApplyClicked → clearValueWidget → delete spin cannot free the spinbox
        // while we are still inside its own valueChanged signal handler.
        spin->setKeyboardTracking(false);
        if (isWritable)
            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this]{
                QTimer::singleShot(0, this, &AdvancedParamsDialog::onApplyClicked);
            });

        widget = spin;
    }
    else if (ifType == GenApi::intfIInteger)
    {
        GenApi::CIntegerPtr pInt(pNode);
        QSpinBox* spin = new QSpinBox(this);

        int64_t rawMin = pInt->GetMin();
        int64_t rawMax = pInt->GetMax();
        int64_t inc    = pInt->GetInc();

        // QSpinBox uses 32-bit int internally — clamp large GenICam ranges
        int minVal = static_cast<int>(std::max<int64_t>(rawMin, static_cast<int64_t>(INT_MIN)));
        int maxVal = static_cast<int>(std::min<int64_t>(rawMax, static_cast<int64_t>(INT_MAX)));
        spin->setRange(minVal, maxVal);
        spin->setSingleStep(static_cast<int>(std::max<int64_t>(inc, 1)));
        spin->setValue(static_cast<int>(pInt->GetValue()));
        spin->setEnabled(isWritable);

        infoText += QString("\nRange: %1 – %2  (step: %3)")
                        .arg(rawMin).arg(rawMax).arg(inc);

        // Same strategy as the float spinbox above — see comment there for full explanation.
        // keyboardTracking=false: valueChanged fires on arrow clicks, Enter, and focus-loss,
        // but NOT on each individual keystroke while the user is mid-number.
        spin->setKeyboardTracking(false);
        if (isWritable)
            connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]{
                QTimer::singleShot(0, this, &AdvancedParamsDialog::onApplyClicked);
            });

        widget = spin;
    }
    else if (ifType == GenApi::intfIBoolean)
    {
        GenApi::CBooleanPtr pBool(pNode);
        QComboBox* combo = new QComboBox(this);
        combo->addItem("false");
        combo->addItem("true");
        if (GenApi::IsReadable(pBool))
            combo->setCurrentIndex(pBool->GetValue() ? 1 : 0);
        combo->setEnabled(isWritable);

        // Auto-apply on user selection — same reasoning as the enum combo above.
        if (isWritable)
            connect(combo, QOverload<int>::of(&QComboBox::activated), this, [this]{
                QTimer::singleShot(0, this, &AdvancedParamsDialog::onApplyClicked);
            });

        widget = combo;
    }
    else if (ifType == GenApi::intfIString)
    {
        GenApi::CStringPtr pStr(pNode);
        QLineEdit* edit = new QLineEdit(this);
        if (GenApi::IsReadable(pStr))
            edit->setText(QString::fromLatin1(pStr->GetValue().c_str()));
        edit->setReadOnly(!isWritable);

        // Auto-apply when the user presses Enter in the text field.
        // returnPressed fires only on Enter — no focus-loss variant like editingFinished,
        // so the user must explicitly confirm before the value is sent.
        // Deferral is still needed to avoid deleting the QLineEdit inside its own signal.
        if (isWritable)
            connect(edit, &QLineEdit::returnPressed, this, [this]{
                QTimer::singleShot(0, this, &AdvancedParamsDialog::onApplyClicked);
            });

        widget = edit;
    }
    else if (ifType == GenApi::intfICommand)
    {
        // Command nodes have no readable value — they are one-shot actions.
        // We show a short explanation label on the left and a compact Execute
        // button on the right so the user has a single, obvious target to click.
        // The main Apply button is left disabled for command nodes (see below)
        // so there is no ambiguity about which button triggers the action.
        QWidget* container = new QWidget(this);
        QHBoxLayout* hbox  = new QHBoxLayout(container);
        hbox->setContentsMargins(0, 0, 0, 0);
        hbox->setSpacing(8);

        QLabel* hint = new QLabel("One-shot command — press Execute to run it on the camera.", container);
        hint->setWordWrap(true);
        hint->setStyleSheet("color: #c97000;");

        QPushButton* execBtn = new QPushButton("Execute", container);
        execBtn->setStyleSheet(
            "QPushButton { background-color: #E65100; color: white; "
            "font-weight: bold; padding: 6px 16px; min-width: 72px; }");
        execBtn->setToolTip("Send this command to the camera");
        execBtn->setAutoDefault(false);  // prevent Enter from firing it accidentally

        // Defer with singleShot so the button's clicked signal fully unwinds before
        // onApplyClicked → showValueWidget → clearValueWidget deletes this container.
        connect(execBtn, &QPushButton::clicked, this, [this]{
            QTimer::singleShot(0, this, &AdvancedParamsDialog::onApplyClicked);
        });

        hbox->addWidget(hint, /*stretch=*/1);
        hbox->addWidget(execBtn, /*stretch=*/0);

        widget = container;
    }
    else
    {
        QLabel* unsupported = new QLabel("(value type not editable in this dialog)", this);
        unsupported->setStyleSheet("color: gray; font-style: italic;");
        widget = unsupported;
    }

    m_infoLabel->setText(infoText);
    m_currentValueWidget = widget;
    m_valueLayout->addWidget(widget);
}


// =============================================================================
// clearValueWidget — remove and delete the current value widget
// =============================================================================
void AdvancedParamsDialog::clearValueWidget()
{
    if (m_currentValueWidget)
    {
        m_valueLayout->removeWidget(m_currentValueWidget);
        delete m_currentValueWidget;
        m_currentValueWidget = nullptr;
    }
    m_currentNodeName.clear();
    m_currentNodeType = 0;
}


// =============================================================================
// Destructor
// =============================================================================
//
// Detach the completer before Qt's LIFO child-destruction runs.
// m_featureCompleter is created in the member-initializer list, so it is added
// to this dialog's child list BEFORE m_featureCombo (created in the constructor
// body).  Qt's LIFO teardown therefore deletes m_featureCombo first, which
// destroys the internal QLineEdit that the completer still holds a pointer to.
// Calling setCompleter(nullptr) here transfers ownership cleanly before either
// object is destroyed.
AdvancedParamsDialog::~AdvancedParamsDialog()
{
    if (m_featureCombo)
        m_featureCombo->setCompleter(nullptr);
}


// =============================================================================
// SLOT: onCategoryChanged
// =============================================================================
void AdvancedParamsDialog::onCategoryChanged(int index)
{
    // During search navigation we drive populateFeatures() directly, so suppress
    // any signal-triggered call that could override the category we just set.
    if (m_searchNavigating) return;
    if (index < 0) return;
    QString catNodeName = m_categoryCombo->itemData(index).toString();
    populateFeatures(catNodeName);
}


// =============================================================================
// SLOT: onFeatureChanged
// =============================================================================
void AdvancedParamsDialog::onFeatureChanged(int index)
{
    // Block the spurious activated(0) signal Qt fires after the completer popup
    // closes.  onSearchCompleterActivated sets this flag and QTimer::singleShot(0)
    // clears it on the next event-loop iteration — so only the completer-triggered
    // call is suppressed; normal user clicks are never affected.
    if (m_searchNavigating) return;

    if (index < 0) return;
    QString nodeNameQ = m_featureCombo->itemData(index).toString();
    showValueWidget(nodeNameQ.toStdString());

    // Update the pin button to reflect the new feature's pinned status
    updateFavoriteButton();

    // Persist the selection so the next time this dialog opens it can restore it.
    QSettings().setValue("AdvancedParamsDialog/lastNodeName", nodeNameQ);
}


// =============================================================================
// SLOT: onApplyClicked — write the widget value to the camera
// =============================================================================
void AdvancedParamsDialog::onApplyClicked()
{
    if (m_currentNodeName.empty() || !m_currentValueWidget) return;

    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap) return;

    // -------------------------------------------------------------------------
    // Guard: warn before disabling chunk parameters the acquisition system uses.
    //
    // The acquisition worker enables ChunkModeActive and three specific chunks
    // (Gain, ExposureTime, LineStatusAll) at stream start.  Turning them off
    // means per-frame gain, exposure, and GPIO line state will not be recorded
    // in frame_data.csv for any subsequent acquisition.
    // -------------------------------------------------------------------------
    if (m_currentNodeName == "ChunkModeActive" || m_currentNodeName == "ChunkEnable")
    {
        // Both nodes are GenICam booleans, presented as a combo: index 0 = false, 1 = true.
        QComboBox* boolCombo = qobject_cast<QComboBox*>(m_currentValueWidget);
        const bool wouldDisable = boolCombo && (boolCombo->currentIndex() == 0);

        if (wouldDisable)
        {
            QString detail;
            if (m_currentNodeName == "ChunkModeActive")
            {
                detail = "Disabling ChunkModeActive will stop all per-frame metadata "
                         "(gain, exposure time, and GPIO line states) from being "
                         "embedded in images. The acquisition system uses these to "
                         "populate frame_data.csv — that data will not be recorded.";
            }
            else  // ChunkEnable — only warn for the three chunks we actually use
            {
                const std::string sel = m_mgr->getEnumValue("ChunkSelector");
                if (sel == "Gain" || sel == "ExposureTime" || sel == "LineStatusAll")
                {
                    detail = QString(
                        "Disabling the \"%1\" chunk will prevent that value from being "
                        "recorded in frame_data.csv for each acquired frame.\n\n"
                        "The acquisition system depends on the Gain, ExposureTime, and "
                        "LineStatusAll chunks to populate frame_data.csv.")
                            .arg(QString::fromStdString(sel));
                }
            }

            if (!detail.isEmpty())
            {
                const int ret = QMessageBox::warning(
                    this,
                    "Chunk Data Warning",
                    detail + "\n\nProceed anyway?",
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                if (ret == QMessageBox::No)
                    return;
            }
        }
    }

    std::string errorMsg;
    bool ok = false;

    GenApi::EInterfaceType ifType = static_cast<GenApi::EInterfaceType>(m_currentNodeType);

    if (ifType == GenApi::intfIEnumeration)
    {
        QComboBox* combo = qobject_cast<QComboBox*>(m_currentValueWidget);
        if (combo)
        {
            std::string val = combo->currentText().toStdString();
            ok = m_mgr->setNodeEnumValue(m_currentNodeName, val, errorMsg);
        }
    }
    else if (ifType == GenApi::intfIFloat)
    {
        QDoubleSpinBox* spin = qobject_cast<QDoubleSpinBox*>(m_currentValueWidget);
        if (spin)
            ok = m_mgr->setNodeDoubleValue(m_currentNodeName, spin->value(), errorMsg);
    }
    else if (ifType == GenApi::intfIInteger)
    {
        QSpinBox* spin = qobject_cast<QSpinBox*>(m_currentValueWidget);
        if (spin)
            ok = m_mgr->setNodeInt64Value(m_currentNodeName,
                                          static_cast<int64_t>(spin->value()), errorMsg);
    }
    else if (ifType == GenApi::intfIBoolean)
    {
        QComboBox* combo = qobject_cast<QComboBox*>(m_currentValueWidget);
        if (combo)
            ok = m_mgr->setNodeBoolValue(m_currentNodeName, combo->currentIndex() == 1, errorMsg);
    }
    else if (ifType == GenApi::intfIString)
    {
        QLineEdit* edit = qobject_cast<QLineEdit*>(m_currentValueWidget);
        if (edit)
        {
            // There is no setNodeStringValue helper (uncommon use case).
            // Write directly via GenApi.
            try
            {
                GenApi::CStringPtr pStr = pMap->GetNode(m_currentNodeName.c_str());
                if (pStr && GenApi::IsWritable(pStr))
                {
                    pStr->SetValue(edit->text().toStdString().c_str());
                    ok = true;
                }
                else
                {
                    errorMsg = "Node is not writable.";
                }
            }
            catch (const GenICam::GenericException& e) { errorMsg = e.what(); }
        }
    }
    else if (ifType == GenApi::intfICommand)
    {
        try
        {
            GenApi::CCommandPtr pCmd = pMap->GetNode(m_currentNodeName.c_str());
            if (pCmd && GenApi::IsWritable(pCmd))
            {
                pCmd->Execute();
                ok = true;
            }
            else
            {
                errorMsg = "Command node is not executable.";
            }
        }
        catch (const GenICam::GenericException& e) { errorMsg = e.what(); }
    }

    if (!ok)
    {
        QMessageBox::warning(this, "Apply Failed",
            "Could not write to camera:\n" + QString::fromStdString(errorMsg));
        return;
    }

    // Re-read the node to show the value the camera actually accepted.
    // (Cameras often round or clamp values to the nearest valid step — so
    // you might send 25.3 dB and the camera silently accepts 25.0 dB instead.)
    //
    // IMPORTANT: copy m_currentNodeName before calling showValueWidget().
    // showValueWidget() takes a const std::string& and its first action is to call
    // clearValueWidget(), which calls m_currentNodeName.clear().  Passing m_currentNodeName
    // directly would make the reference point to an empty string before GetNode() is called,
    // causing "Node not available."  The local copy escapes that lifetime issue.
    std::string nodeNameCopy = m_currentNodeName;
    showValueWidget(nodeNameCopy);

    // Append a readback line to the info label so the user can confirm the
    // camera accepted the new value.  showValueWidget() has already populated
    // m_infoLabel with the description and range; we just add one line at the end.
    //
    // For Command nodes there is no value to read back — just show "Executed".
    // For unsupported/read-only nodes we never reach this point (ok would be false).
    GenApi::EInterfaceType ifType2 = static_cast<GenApi::EInterfaceType>(m_currentNodeType);
    QString readback;

    if (ifType2 == GenApi::intfICommand)
    {
        readback = "Command executed.";
    }
    else if (m_currentValueWidget)
    {
        // Read the camera-accepted value back out of the widget that showValueWidget
        // just rebuilt and populated from the live camera state.
        if (auto* spin = qobject_cast<QDoubleSpinBox*>(m_currentValueWidget))
            readback = QString("Camera accepted: %1").arg(spin->value(), 0, 'g', 6);
        else if (auto* spin = qobject_cast<QSpinBox*>(m_currentValueWidget))
            readback = QString("Camera accepted: %1").arg(spin->value());
        else if (auto* combo = qobject_cast<QComboBox*>(m_currentValueWidget))
            readback = QString("Camera accepted: %1").arg(combo->currentText());
        else if (auto* edit = qobject_cast<QLineEdit*>(m_currentValueWidget))
            readback = QString("Camera accepted: \"%1\"").arg(edit->text());
    }

    if (!readback.isEmpty())
        m_infoLabel->setText(m_infoLabel->text() + "\n\n✓ " + readback);
}


// =============================================================================
// SLOT: onRefreshClicked — re-scan the full node tree
// =============================================================================
void AdvancedParamsDialog::onRefreshClicked()
{
    // Remember what was selected so we can try to restore it after the refresh.
    QString prevCategory = m_categoryCombo->currentData().toString();
    QString prevFeature  = m_featureCombo->currentData().toString();

    // populateCategories() always rebuilds the full (unfiltered) list.
    // If a visibility filter is active we apply it afterwards.
    populateCategories();

    int filterIdx = m_visibilityCombo->currentIndex();
    if (filterIdx != 0)
    {
        // Re-apply the filter: this rebuilds m_categoryCombo with only the
        // categories that have visible features, and populates the first one.
        onVisibilityFilterChanged(filterIdx);
    }

    // Restore previous category and feature if they are still visible.
    int catIdx = m_categoryCombo->findData(prevCategory);
    if (catIdx >= 0)
    {
        m_categoryCombo->setCurrentIndex(catIdx);

        // populateFeatures() ends by calling onFeatureChanged(0), so the value widget
        // is showing the FIRST feature at this point — not the one the user had selected.
        populateFeatures(prevCategory);

        int featIdx = m_featureCombo->findData(prevFeature);
        if (featIdx >= 0)
        {
            m_featureCombo->setCurrentIndex(featIdx);
            // Override the index-0 widget populateFeatures just showed with the
            // correct feature.  onFeatureChanged calls showValueWidget + updateFavoriteButton.
            onFeatureChanged(featIdx);
        }
        // If featIdx < 0, the previously selected feature is gone after refresh;
        // populateFeatures already showed index 0 which is the best fallback.
    }
}


// =============================================================================
// SLOT: onVisibilityFilterChanged — rebuild both categories and features
// =============================================================================
//
// When the Show filter changes we must:
//   1. Rebuild m_categoryCombo — hide categories that have zero visible features
//      under the new filter (otherwise the user sees empty category pages).
//   2. Re-populate the feature list for whatever category ends up selected.
//
// m_allCategories holds the full unfiltered list so we can always restore
// categories when the filter widens (e.g., switching back to "All Levels").
void AdvancedParamsDialog::onVisibilityFilterChanged(int filterIdx)
{
    // Remember the current selection so we can restore it if still visible.
    QString prevCatNodeName = m_categoryCombo->currentData().toString();

    // --- Rebuild the category combo ---
    m_categoryCombo->blockSignals(true);
    m_categoryCombo->clear();

    if (filterIdx == 0)
    {
        // "All Levels" — restore every category
        for (const auto& pair : m_allCategories)
            m_categoryCombo->addItem(pair.first, pair.second);
    }
    else
    {
        // Filtered mode — only add categories that contain at least one visible feature
        for (const auto& pair : m_allCategories)
        {
            if (hasVisibleFeatures(pair.second, filterIdx))
                m_categoryCombo->addItem(pair.first, pair.second);
        }
    }

    m_categoryCombo->blockSignals(false);

    // Restore previous category selection if it is still visible under the new filter;
    // otherwise fall back to the first available category.
    int catIdx = m_categoryCombo->findData(prevCatNodeName);
    if (catIdx >= 0)
        m_categoryCombo->setCurrentIndex(catIdx);
    else if (m_categoryCombo->count() > 0)
        m_categoryCombo->setCurrentIndex(0);

    // Repopulate the feature list for the now-selected category.
    QString catNodeName = m_categoryCombo->currentData().toString();
    if (!catNodeName.isEmpty())
        populateFeatures(catNodeName);
    else
        clearValueWidget();
}


// =============================================================================
// SLOT: onFavoriteClicked — toggle pinned status of current feature
// =============================================================================
void AdvancedParamsDialog::onFavoriteClicked()
{
    if (m_currentNodeName.empty())
        return;

    QString nodeName = QString::fromStdString(m_currentNodeName);

    if (PinnedParamsPanel::isPinnedNode(nodeName))
    {
        // Currently pinned → unpin it
        PinnedParamsPanel::removePinnedNode(nodeName);
    }
    else
    {
        // Not pinned → pin it
        PinnedParamsPanel::addPinnedNode(nodeName);
    }

    // Update the button appearance to match new state
    updateFavoriteButton();

    // Tell MainWindow's PinnedParamsPanel to rebuild its rows
    emit pinnedParamsChanged();
}


// =============================================================================
// updateFavoriteButton — set pin button icon/label based on current pinned status
// =============================================================================
void AdvancedParamsDialog::updateFavoriteButton()
{
    if (!m_favoriteButton)
        return;

    if (m_currentNodeName.empty())
    {
        // No feature selected — disable and show the "not pinned" state
        m_favoriteButton->setText("☆");
        m_favoriteButton->setEnabled(false);
        return;
    }

    m_favoriteButton->setEnabled(true);

    bool pinned = PinnedParamsPanel::isPinnedNode(QString::fromStdString(m_currentNodeName));
    if (pinned)
    {
        // Parameter is pinned — offer to unpin it
        m_favoriteButton->setText("★");
        m_favoriteButton->setToolTip(
            "This parameter is pinned to the main window panel.\n"
            "Click to unpin it.");
    }
    else
    {
        // Parameter is not pinned — offer to pin it
        m_favoriteButton->setText("☆");
        m_favoriteButton->setToolTip(
            "Pin this parameter to the main window panel.\n"
            "Pinned parameters are saved to the registry and persist across sessions.");
    }
}


// =============================================================================
// SLOT: onSaveConfigClicked — Save Config popup menu
// =============================================================================
//
// Pops up a menu with two choices:
//   "Save Custom Settings" — saves to UserSet2 on the camera (non-destructive
//       to the facility default), then opens a file dialog to also write a .json.
//   "Save DCS Settings"    — saves to UserSet1 (facility default), password-
//       protected so casual users can't accidentally overwrite it.
//
// WHY TWO USERSETS?
//   UserSet1 = the facility-wide DCS (Data Collection System) baseline.
//              Locked behind a password so only admins change it.
//   UserSet2 = per-user / per-experiment custom settings.
//              Anyone can overwrite it freely.
//   Default  = factory defaults; not writable by this app.
void AdvancedParamsDialog::onSaveConfigClicked()
{
    // Build the popup menu.  exec() blocks until the user clicks or dismisses it.
    QMenu menu(this);
    QAction* saveCustom = menu.addAction("Save Custom Settings");
    QAction* saveDCS    = menu.addAction("Save DCS Settings");

    // Show the menu directly below the button that was clicked.
    // mapToGlobal() converts the button's local (0, height) coordinate — which is
    // its bottom-left corner in button-local space — into screen coordinates so the
    // menu appears in the right place regardless of where the dialog is on screen.
    QAction* selected = menu.exec(
        m_saveConfigButton->mapToGlobal(QPoint(0, m_saveConfigButton->height())));

    if (!selected)
        return;  // User dismissed the menu without picking anything

    // ------------------------------------------------------------------
    // Save Custom Settings: UserSet2 + .json file
    // ------------------------------------------------------------------
    if (selected == saveCustom)
    {
        // Step 1: prompt for a .json file path first — if cancelled, do nothing at all.
        QString defaultDir = QSettings().value("outputPath",
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                + "/LucidCaptures").toString();

        QString filePath = QFileDialog::getSaveFileName(
            this,
            "Save Custom Settings to JSON",
            defaultDir + "/camera_settings_custom.json",
            "JSON Files (*.json);;All Files (*)");

        if (filePath.isEmpty())
            return;  // user cancelled — do not save to UserSet2

        // Step 2: save to UserSet2 on the camera
        QString saveErr;
        if (!saveToUserSet("UserSet2", saveErr))
        {
            QMessageBox::warning(this, "Save Failed",
                "Could not save to UserSet2:\n" + saveErr);
            return;
        }

        // Step 3: write .json
        QString jsonErr;
        if (!saveSettingsToJson(filePath, jsonErr))
            QMessageBox::warning(this, "JSON Save Failed", jsonErr);
        else
            QMessageBox::information(this, "Settings Saved",
                "Custom settings saved to UserSet2 on the camera\nand to:\n" + filePath);
    }

    // ------------------------------------------------------------------
    // Save DCS Settings: UserSet1, password protected
    // ------------------------------------------------------------------
    else if (selected == saveDCS)
    {
        // QInputDialog::getText() with QLineEdit::Password mode shows asterisks
        // instead of the typed characters, keeping the password off the screen.
        bool   ok  = false;
        QString pwd = QInputDialog::getText(
            this,
            "DCS Settings — Password Required",
            "Enter the DCS administrator password:",
            QLineEdit::Password,
            QString(),
            &ok);

        if (!ok)
            return;  // User pressed Cancel

        // C++ NOTE: QString::compare() with Qt::CaseSensitive is the safe comparison
        // for passwords — "Admin" != "admin".  Using == would also work here since
        // QString::operator== is case-sensitive, but compare() makes the intent explicit.
        if (pwd.compare("admin", Qt::CaseSensitive) != 0)
        {
            QMessageBox::critical(this, "Access Denied",
                "Incorrect password.\n"
                "The DCS settings were NOT saved.");
            return;
        }

        QString saveErr;
        if (!saveToUserSet("UserSet1", saveErr))
            QMessageBox::warning(this, "Save Failed",
                "Could not save to UserSet1:\n" + saveErr);
        else
            QMessageBox::information(this, "DCS Settings Saved",
                "Facility default (DCS) settings saved to UserSet1 on the camera.");
    }
}


// =============================================================================
// SLOT: onLoadConfigClicked — Load Config popup menu
// =============================================================================
//
// Pops up a menu with four choices:
//   "Load Default"       — camera's GenICam factory defaults
//   "Load Custom"        — UserSet2 (per-user saved settings)
//   "Load DCS"           — UserSet1 (facility baseline)
//   "Load from File..."  — pick a .json written by Save Custom Settings
//
// After any successful load the dialog is refreshed so widgets show the new
// values, and checkAndPromptChunkLineStatus() runs to make sure the user
// doesn't lose trigger-time GPIO logging because the loaded profile had chunks off.
void AdvancedParamsDialog::onLoadConfigClicked()
{
    QMenu menu(this);
    QAction* loadDefault = menu.addAction("Load Default");
    QAction* loadCustom  = menu.addAction("Load Custom");
    QAction* loadDCS     = menu.addAction("Load DCS");
    menu.addSeparator();
    QAction* loadFile    = menu.addAction("Load from File...");

    QAction* selected = menu.exec(
        m_loadConfigButton->mapToGlobal(QPoint(0, m_loadConfigButton->height())));

    if (!selected)
        return;

    // -------------------------------------------------------------------------
    // runUserSetLoad — helper that runs loadFromUserSet() on a worker thread
    // while showing a modal indeterminate progress dialog on the main thread.
    //
    // WHY THREADED:
    //   UserSetLoad tells the camera to copy a flash slot into its live registers.
    //   That round-trip takes 1–5 seconds.  Without threading, Qt cannot repaint
    //   the progress dialog because the main thread is blocked in the GenICam call.
    //
    // HOW IT WORKS:
    //   1. Show a QProgressDialog with range (0, 0) — that makes the bar animate
    //      as an infinite busy indicator instead of a fixed-range fill.
    //   2. Launch a std::thread that calls loadFromUserSet().
    //   3. When the thread finishes it calls QProgressDialog::accept() via
    //      QMetaObject::invokeMethod (the only safe way to call a Qt slot from a
    //      non-GUI thread — it posts an event to the GUI thread's event queue).
    //   4. dlg.exec() blocks the *calling* code but keeps the Qt event loop
    //      spinning, so the animated bar paints correctly.
    //   5. After exec() returns we join() the thread — it has already exited by
    //      then, so join() returns instantly.
    // -------------------------------------------------------------------------
    auto runUserSetLoad = [this](const QString& label,
                                 const QString& userSetName,
                                 bool& outSuccess,
                                 QString& outError)
    {
        QProgressDialog dlg(label, QString(), 0, 0, this);
        dlg.setWindowTitle("Loading Settings");
        dlg.setWindowModality(Qt::WindowModal);
        dlg.setMinimumDuration(0);   // show immediately, no delay
        dlg.setCancelButton(nullptr); // no cancel — can't interrupt a camera command

        std::thread t([&]()
        {
            try
            {
                outSuccess = loadFromUserSet(userSetName, outError);
            }
            catch (...)
            {
                outSuccess = false;
                outError   = "Unexpected error during UserSet load.";
            }
            // Post accept() to the GUI thread so dlg.exec() returns.
            QMetaObject::invokeMethod(&dlg, "accept", Qt::QueuedConnection);
        });

        dlg.exec();   // keeps event loop alive; returns when accept() is posted
        t.join();     // thread has already exited by now — instant return
    };

    bool    success  = false;
    QString errorMsg;

    if (selected == loadDefault)
    {
        runUserSetLoad("Loading factory defaults from camera...", "Default", success, errorMsg);
        if (success)
            QMessageBox::information(this, "Settings Loaded",
                "Factory default settings loaded from the camera.");
    }
    else if (selected == loadCustom)
    {
        runUserSetLoad("Loading custom settings (UserSet2) from camera...", "UserSet2", success, errorMsg);
        if (success)
            QMessageBox::information(this, "Settings Loaded",
                "Custom settings (UserSet2) loaded from the camera.");
    }
    else if (selected == loadDCS)
    {
        runUserSetLoad("Loading DCS settings (UserSet1) from camera...", "UserSet1", success, errorMsg);
        if (success)
            QMessageBox::information(this, "Settings Loaded",
                "DCS facility settings (UserSet1) loaded from the camera.");
    }
    else if (selected == loadFile)
    {
        QString defaultDir = QSettings().value("outputPath",
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                + "/LucidCaptures").toString();

        QString filePath = QFileDialog::getOpenFileName(
            this,
            "Load Camera Settings from JSON File",
            defaultDir,
            "JSON Files (*.json);;All Files (*)");

        if (filePath.isEmpty())
            return;  // Cancelled

        // For JSON load we know exactly how many nodes there are, so we can show
        // a real percentage bar instead of an indeterminate one.
        // range (0, 0) initially; the progress callback sets the maximum on the
        // first call once the node count is known inside loadSettingsFromJson.
        QProgressDialog dlg("Applying settings from file...", QString(), 0, 0, this);
        dlg.setWindowTitle("Loading Settings");
        dlg.setWindowModality(Qt::WindowModal);
        dlg.setMinimumDuration(0);
        dlg.setCancelButton(nullptr);

        std::thread t([&]()
        {
            try
            {
                // Progress callback — called from the worker thread after each node.
                // QMetaObject::invokeMethod posts the calls to the GUI thread's event
                // queue so they are processed safely during dlg.exec()'s event loop.
                auto onProgress = [&](int current, int total)
                {
                    // Set maximum once so the bar switches from indeterminate to
                    // determinate as soon as the node count is known.
                    if (dlg.maximum() == 0)
                        QMetaObject::invokeMethod(&dlg, "setMaximum",
                                                  Qt::QueuedConnection,
                                                  Q_ARG(int, total));

                    QMetaObject::invokeMethod(&dlg, "setValue",
                                             Qt::QueuedConnection,
                                             Q_ARG(int, current));
                };

                success = loadSettingsFromJson(filePath, errorMsg, onProgress);
            }
            catch (...)
            {
                success  = false;
                errorMsg = "Unexpected error while loading JSON settings file.";
            }
            QMetaObject::invokeMethod(&dlg, "accept", Qt::QueuedConnection);
        });

        dlg.exec();
        t.join();

        if (success)
        {
            QMessageBox::information(this, "Settings Loaded",
                "Settings loaded from:\n" + filePath);
        }
        else if (!errorMsg.isEmpty())
        {
            // Partial success — some nodes applied, some failed.  Show the summary
            // but still fall through to refresh + chunk check below.
            QMessageBox::warning(this, "Load Partial / Error", errorMsg);
        }

        // Treat file loads as "attempted" so refresh + chunk check always run below.
        success = true;
    }

    if (!success)
    {
        // Clean failure path (UserSet load failed outright, or file load totally failed)
        if (!errorMsg.isEmpty())
            QMessageBox::warning(this, "Load Failed", errorMsg);
        return;
    }

    // Refresh the entire node tree so all widgets show the newly loaded values.
    // onRefreshClicked() preserves the current category/feature selection.
    onRefreshClicked();

    // Check if the LineStatusAll chunk is enabled after the load.
    // Loaded settings might have had chunks disabled; prompt to re-enable.
    checkAndPromptChunkLineStatus();
}


// =============================================================================
// saveToUserSet — execute UserSetSave for the given user set name
// =============================================================================
//
// GenICam UserSet mechanism:
//   1. Write the desired set name to UserSetSelector (enum node).
//   2. Execute the UserSetSave command node.
// The camera then copies its current live settings into the selected slot in
// non-volatile flash.  On the next power cycle, the camera boots with whatever
// UserSet is selected in UserSetDefault.
//
// IMPORTANT: UserSetSave only exists on cameras whose firmware implements it.
// If the node is missing or not writable, errorMsg will say so and we return false.
bool AdvancedParamsDialog::saveToUserSet(const QString& userSetName, QString& errorMsg)
{
    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap)
    {
        errorMsg = "No camera is connected.";
        return false;
    }

    try
    {
        // Step 1: select which UserSet to write into
        Arena::SetNodeValue<GenICam::gcstring>(
            pMap, "UserSetSelector", userSetName.toStdString().c_str());

        // Step 2: get the UserSetSave command node and execute it
        GenApi::CCommandPtr pSave(pMap->GetNode("UserSetSave"));
        if (!pSave || !GenApi::IsWritable(pSave))
        {
            errorMsg = "UserSetSave command is not available on this camera.\n"
                       "The camera firmware may not support persistent UserSet storage.";
            return false;
        }

        pSave->Execute();
        return true;
    }
    catch (const GenICam::GenericException& e)
    {
        errorMsg = QString::fromLatin1(e.what());
        return false;
    }
}


// =============================================================================
// loadFromUserSet — execute UserSetLoad for the given user set name
// =============================================================================
//
// Same selector mechanism as saveToUserSet, but calls UserSetLoad instead.
// After this returns, all live camera parameters reflect the stored values.
bool AdvancedParamsDialog::loadFromUserSet(const QString& userSetName, QString& errorMsg)
{
    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap)
    {
        errorMsg = "No camera is connected.";
        return false;
    }

    try
    {
        Arena::SetNodeValue<GenICam::gcstring>(
            pMap, "UserSetSelector", userSetName.toStdString().c_str());

        GenApi::CCommandPtr pLoad(pMap->GetNode("UserSetLoad"));
        if (!pLoad || !GenApi::IsWritable(pLoad))
        {
            errorMsg = "UserSetLoad command is not available on this camera.";
            return false;
        }

        pLoad->Execute();
        return true;
    }
    catch (const GenICam::GenericException& e)
    {
        errorMsg = QString::fromLatin1(e.what());
        return false;
    }
}


// =============================================================================
// collectAllNodeValues — build a flat JSON object of all readable leaf nodes
// =============================================================================
//
// Walks every category in m_allCategories (the full unfiltered list) and reads
// the current value of each non-Command readable leaf node into a QJsonObject.
// Keys are GenICam node names (ASCII); values use native JSON types:
//   float  → QJsonValue::Double
//   int    → QJsonValue::Double  (JSON has no distinct integer type)
//   enum   → QJsonValue::String  (symbolic name, e.g. "Mono8")
//   bool   → QJsonValue::Bool
//   string → QJsonValue::String
// Command nodes are skipped — they have no storable value.
QJsonObject AdvancedParamsDialog::collectAllNodeValues()
{
    QJsonObject result;

    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap)
        return result;

    // Walk every top-level category (same list that populates m_categoryCombo)
    for (const auto& catPair : m_allCategories)
    {
        GenApi::CNodePtr   pCatNode(pMap->GetNode(catPair.second.toStdString().c_str()));
        GenApi::CCategoryPtr pCat(pCatNode);
        if (!pCat)
            continue;

        // Recursive lambda — collects leaf values under pParent into 'result'.
        std::function<void(GenApi::CCategoryPtr)> collect =
            [&](GenApi::CCategoryPtr pParent)
        {
            GenApi::FeatureList_t children;
            pParent->GetFeatures(children);

            for (GenApi::IValue* pNode : children)
            {
                GenApi::CCategoryPtr pSubCat(pNode);
                if (pSubCat && GenApi::IsReadable(pSubCat))
                {
                    collect(pSubCat);  // recurse
                    continue;
                }

                // Only process readable leaf nodes; skip everything else
                if (!GenApi::IsReadable(pNode))
                    continue;

                GenApi::INode* pINode = pNode->GetNode();
                QString name = QString::fromLatin1(pINode->GetName().c_str());

                // Skip Command nodes — there is no "value" to serialize for them
                if (pINode->GetPrincipalInterfaceType() == GenApi::intfICommand)
                    continue;

                // Skip duplicates — first category that exposes a node wins
                if (result.contains(name))
                    continue;

                try
                {
                    switch (pINode->GetPrincipalInterfaceType())
                    {
                        case GenApi::intfIFloat:
                        {
                            GenApi::CFloatPtr p(pINode);
                            if (p) result[name] = p->GetValue();
                            break;
                        }
                        case GenApi::intfIInteger:
                        {
                            GenApi::CIntegerPtr p(pINode);
                            // QJsonValue doesn't have a distinct int64 type;
                            // store as double — sufficient for any camera integer param.
                            if (p) result[name] = (double)p->GetValue();
                            break;
                        }
                        case GenApi::intfIEnumeration:
                        {
                            GenApi::CEnumerationPtr p(pINode);
                            if (p) result[name] = QString::fromLatin1(p->ToString().c_str());
                            break;
                        }
                        case GenApi::intfIBoolean:
                        {
                            GenApi::CBooleanPtr p(pINode);
                            if (p) result[name] = (bool)p->GetValue();
                            break;
                        }
                        case GenApi::intfIString:
                        {
                            GenApi::CStringPtr p(pINode);
                            if (p) result[name] = QString::fromLatin1(p->GetValue().c_str());
                            break;
                        }
                        default:
                            break;  // Ignore Port and other exotic types
                    }
                }
                catch (...) {}  // Skip any node that throws (e.g., temporarily unavailable)
            }
        };

        collect(pCat);
    }

    return result;
}


// =============================================================================
// saveSettingsToJson — serialize all readable camera nodes to a .json file
// =============================================================================
//
// File format:
// {
//   "format_version": 1,
//   "saved_at": "2026-06-06T14:30:00Z",
//   "nodes": {
//     "ExposureTime": 5000.0,
//     "Gain": 6.5,
//     "PixelFormat": "Mono16",
//     ...
//   }
// }
bool AdvancedParamsDialog::saveSettingsToJson(const QString& filePath, QString& errorMsg)
{
    QJsonObject root;
    root["format_version"] = 1;
    root["saved_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root["nodes"]    = collectAllNodeValues();

    QJsonDocument doc(root);

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        errorMsg = "Could not open file for writing:\n" + filePath
                 + "\n" + file.errorString();
        return false;
    }

    // QJsonDocument::Indented adds newlines and indentation for human readability.
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}


// =============================================================================
// loadSettingsFromJson — parse a .json settings file and apply to camera
// =============================================================================
//
// Applies every node in the "nodes" object.  Nodes that are not writable on
// the current camera are silently skipped (read-only or not present).
// Nodes that throw during writing are collected and reported in errorMsg.
//
// Returns true only if ALL nodes applied without error.
// On partial success it still returns false but writes a count summary.
bool AdvancedParamsDialog::loadSettingsFromJson(const QString& filePath, QString& errorMsg,
                                                std::function<void(int, int)> onProgress)
{
    // --- Parse the file ---
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        errorMsg = "Could not open file:\n" + filePath + "\n" + file.errorString();
        return false;
    }

    QJsonParseError parseErr;
    QJsonDocument   doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    file.close();

    if (doc.isNull())
    {
        errorMsg = "Could not parse JSON file:\n" + parseErr.errorString();
        return false;
    }

    QJsonObject root  = doc.object();
    QJsonObject nodes = root["nodes"].toObject();
    if (nodes.isEmpty())
    {
        errorMsg = "The file does not contain a \"nodes\" section or it is empty.";
        return false;
    }

    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap)
    {
        errorMsg = "No camera is connected.";
        return false;
    }

    // --- Apply each node ---
    //
    // We apply nodes in the order they appear in the JSON.  That order mirrors
    // the order they were collected (category traversal), which generally puts
    // "gate" nodes (AcquisitionMode, ExposureAuto, GainAuto) before the nodes
    // they gate — so ExposureAuto=Off tends to arrive before ExposureTime.
    // This isn't guaranteed; nodes that fail due to ordering can be retried by
    // clicking Load again after a first pass.
    QStringList failedNodes;
    int applied   = 0;
    int skipped   = 0;  // Nodes not writable on this camera (not counted as errors)
    int processed = 0;
    const int total = nodes.size();

    for (auto it = nodes.begin(); it != nodes.end(); ++it)
    {
        const std::string name = it.key().toStdString();
        const QJsonValue  val  = it.value();

        try
        {
            GenApi::CNodePtr pNode(pMap->GetNode(name.c_str()));
            if (!pNode || !GenApi::IsWritable(pNode))
            {
                skipped++;
                continue;
            }

            switch (pNode->GetPrincipalInterfaceType())
            {
                case GenApi::intfIFloat:
                {
                    GenApi::CFloatPtr p(pNode);
                    if (p && val.isDouble()) { p->SetValue(val.toDouble()); applied++; }
                    break;
                }
                case GenApi::intfIInteger:
                {
                    GenApi::CIntegerPtr p(pNode);
                    if (p && val.isDouble()) { p->SetValue((int64_t)val.toDouble()); applied++; }
                    break;
                }
                case GenApi::intfIEnumeration:
                {
                    GenApi::CEnumerationPtr p(pNode);
                    if (p && val.isString())
                    {
                        p->FromString(val.toString().toStdString().c_str());
                        applied++;
                    }
                    break;
                }
                case GenApi::intfIBoolean:
                {
                    GenApi::CBooleanPtr p(pNode);
                    if (p && val.isBool()) { p->SetValue(val.toBool()); applied++; }
                    break;
                }
                case GenApi::intfIString:
                {
                    GenApi::CStringPtr p(pNode);
                    if (p && val.isString())
                    {
                        p->SetValue(val.toString().toStdString().c_str());
                        applied++;
                    }
                    break;
                }
                default:
                    skipped++;
                    break;
            }
        }
        catch (...)
        {
            // Node threw on write — e.g., value out of range, or a dependency
            // (like ExposureAuto) is still in Auto mode.  Collect and report.
            failedNodes.append(it.key());
        }

        // Report progress after every node so the caller can update a progress bar.
        // Called from the worker thread — the caller must use Qt::QueuedConnection
        // (via QMetaObject::invokeMethod) to marshal the update to the GUI thread.
        ++processed;
        if (onProgress) onProgress(processed, total);
    }

    if (!failedNodes.isEmpty())
    {
        errorMsg = QString("Applied %1 setting(s); skipped %2 (read-only or not present).\n\n"
                           "The following %3 node(s) could not be written — they may depend "
                           "on other nodes (e.g. ExposureAuto must be Off before ExposureTime "
                           "can be set):\n  %4\n\n"
                           "You can try loading again after the first pass has set gate nodes.")
            .arg(applied)
            .arg(skipped)
            .arg(failedNodes.size())
            .arg(failedNodes.join(", "));
        return false;
    }

    return true;
}


// =============================================================================
// checkAndPromptChunkLineStatus — prompt to enable GPIO chunk if it is off
// =============================================================================
//
// WHY THIS EXISTS:
//   If the user loads a settings file or UserSet that was saved without chunk
//   data enabled, the AcquisitionWorker's LineStatusAll column in frame_data.csv
//   will be all zeros — silently, with no error message.
//
//   This function checks after every config load whether:
//     (a) ChunkModeActive is true, AND
//     (b) the LineStatusAll chunk is enabled
//   If either is false, it shows a dialog explaining the consequence and offers
//   to enable the chunk right now.
void AdvancedParamsDialog::checkAndPromptChunkLineStatus()
{
    GenApi::INodeMap* pMap = m_mgr->getNodeMap();
    if (!pMap)
        return;

    // --- Check ChunkModeActive ---
    bool chunkModeActive = false;
    try
    {
        chunkModeActive = Arena::GetNodeValue<bool>(pMap, "ChunkModeActive");
    }
    catch (...)
    {
        // Camera doesn't support chunk mode at all — nothing to do.
        return;
    }

    // --- Check LineStatusAll chunk enable ---
    bool lineStatusEnabled = false;
    if (chunkModeActive)
    {
        try
        {
            Arena::SetNodeValue<GenICam::gcstring>(pMap, "ChunkSelector", "LineStatusAll");
            lineStatusEnabled = Arena::GetNodeValue<bool>(pMap, "ChunkEnable");
        }
        catch (...) {}
    }

    if (lineStatusEnabled)
        return;  // Everything is already on — nothing to prompt

    // --- Prompt the user ---
    //
    // The message explains WHAT the chunk is and WHY it matters rather than
    // just asking "enable it?" — that way the user can make an informed decision.
    int ret = QMessageBox::question(
        this,
        "GPIO Chunk Data Not Enabled",
        "The loaded settings do not have the LineStatusAll chunk enabled.\n\n"
        "This chunk embeds the camera's GPIO line state (digital input/output values) "
        "directly into each frame's metadata, so frame_data.csv records exactly which "
        "digital lines were active at the moment each frame was captured.\n\n"
        "Without it, the 'line_status_all' column in frame_data.csv will be absent or "
        "all zeros — even if an external trigger or signal is connected.\n\n"
        "Would you like to enable the LineStatusAll chunk now?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);   // Default button = Yes so a quick Enter accepts

    if (ret != QMessageBox::Yes)
        return;

    // --- Enable it ---
    try
    {
        Arena::SetNodeValue<bool>(pMap, "ChunkModeActive", true);
        Arena::SetNodeValue<GenICam::gcstring>(pMap, "ChunkSelector", "LineStatusAll");
        Arena::SetNodeValue<bool>(pMap, "ChunkEnable", true);

        QMessageBox::information(this, "Chunk Enabled",
            "LineStatusAll chunk enabled successfully.\n"
            "GPIO state will be recorded per-frame in frame_data.csv.");
    }
    catch (const GenICam::GenericException& e)
    {
        QMessageBox::warning(this, "Could Not Enable Chunk",
            QString("Failed to enable LineStatusAll chunk:\n%1")
                .arg(QString::fromLatin1(e.what())));
    }
}
