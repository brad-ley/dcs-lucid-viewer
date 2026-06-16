// =============================================================================
// AdvancedParamsDialog.h
// =============================================================================
//
// WHAT THIS FILE DOES:
//   Declares AdvancedParamsDialog — a modal dialog that lets the user browse
//   and edit ANY GenICam parameter exposed by the connected camera.
//
// WHY THIS EXISTS:
//   The main window only shows the user's pinned parameters (stored in the registry).
//   But cameras expose hundreds of nodes covering everything from IP configuration
//   to lens distortion coefficients.
//   This dialog gives full access without cluttering the main UI.
//
// HOW IT WORKS:
//   GenICam organizes parameters into a tree rooted at a "Root" category node.
//   Direct children of Root are category nodes (e.g., "Acquisition Control",
//   "Image Format Control", "Analog Control"). Under each category are the
//   actual parameter nodes (leaf nodes).
//
//   This dialog shows:
//     - A "Category" dropdown: the top-level categories from Root's children.
//     - A "Feature" dropdown: all readable leaf nodes in the selected category
//       (including nodes nested in sub-categories, flattened for simplicity).
//     - A dynamic value widget: type-appropriate for the selected feature
//       (QComboBox for enum, QDoubleSpinBox for float, QSpinBox for integer,
//        QCheckBox-style combo for bool, QLineEdit for string, "Execute" button
//        for command nodes).
//     - An info label: shows the node description and min/max range.
//     - Apply: writes the current widget value to the camera.
//
// C++ CONCEPT — forward declarations:
//   We forward-declare CameraManager rather than including the full header.
//   "class CameraManager;" tells the compiler "this class exists" without
//   pulling in all of CameraManager.h's dependencies (ArenaApi.h etc.).
//   This is only possible because we use CameraManager only as a pointer here.
//   We DO need the full include in the .cpp where we actually call methods.
// =============================================================================

#pragma once

#include <QDialog>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QString>
#include <QVector>
#include <QPair>
#include <QMap>
#include <QJsonObject>  // Used by saveSettingsToJson / loadSettingsFromJson helpers
#include <functional>   // std::function — progress callback for loadSettingsFromJson
#include <string>

// Forward declarations — avoid pulling in heavy headers here
class CameraManager;
class QCompleter;
class QStandardItemModel;
class QModelIndex;


// =============================================================================
// AdvancedParamsDialog
// =============================================================================
class AdvancedParamsDialog : public QDialog
{
    Q_OBJECT

public:
    // mgr    — pointer to the connected CameraManager (not owned; caller still owns it)
    // parent — Qt parent widget (centers the dialog over the main window)
    explicit AdvancedParamsDialog(CameraManager* mgr, QWidget* parent = nullptr);

    // Destructor: detaches the completer from the feature combo before Qt's LIFO
    // child-destruction runs.  Without this, m_featureCombo (created after the
    // completer in the constructor body) is deleted first, leaving the completer
    // holding a stale pointer to the combo's internal QLineEdit — a crash on close.
    ~AdvancedParamsDialog() override;

signals:
    // Emitted after the user toggles the star (★/☆) button, adding or removing
    // the current parameter from the main window's pinned panel.
    // MainWindow connects this to PinnedParamsPanel::refreshFromCamera() so the
    // panel updates live without needing to close the dialog.
    void pinnedParamsChanged();

private slots:
    // Called when the user picks a different category in the top dropdown.
    // Rebuilds the feature list for that category.
    void onCategoryChanged(int index);

    // Called when the user picks a different feature in the second dropdown.
    // Rebuilds the value widget and info label for that feature.
    void onFeatureChanged(int index);

    // Writes the current widget value back to the camera.
    void onApplyClicked();

    // Re-scans the camera's node tree and refreshes all dropdowns.
    // Useful if a parameter change made other parameters appear/disappear.
    void onRefreshClicked();

    // Toggles the pinned status of the currently selected feature.
    // Calls PinnedParamsPanel::addPinnedNode() or removePinnedNode(),
    // then emits pinnedParamsChanged() and updates the star button appearance.
    void onFavoriteClicked();

    // Called when the visibility filter combo changes.
    // Re-populates the feature list applying the new filter.
    void onVisibilityFilterChanged(int index);

    // Called when the user selects an item from the cross-category search completer.
    // Flips the category combo to the one that owns the selected node, then selects it.
    // Uses the QString overload so Qt delivers the item text directly — no proxy indirection.
    void onSearchCompleterActivated(const QString& completionText);

    // Opens a popup menu with two save options:
    //   "Save Custom Settings" — writes to UserSet2 on camera, then prompts for .json file path
    //   "Save DCS Settings"    — writes to UserSet1 after password check (password: "admin")
    void onSaveConfigClicked();

    // Opens a popup menu with four load options:
    //   "Load Default"        — executes UserSetLoad with UserSetSelector = Default
    //   "Load Custom"         — executes UserSetLoad with UserSetSelector = UserSet2
    //   "Load DCS"            — executes UserSetLoad with UserSetSelector = UserSet1
    //   "Load from File..."   — opens a .json file and applies each node value found in it
    // After any successful load, prompts the user to enable the LineStatusAll chunk if it is off.
    void onLoadConfigClicked();

private:
    // ----- Setup helpers -----

    // Walk the camera's node tree and populate m_categoryCombo.
    void populateCategories();

    // Fill m_featureCombo with all readable leaf nodes under the given
    // category node, filtered by the current visibility level selection.
    void populateFeatures(const QString& categoryNodeName);

    // Build the cross-category search index (m_searchModel + m_nodeToCategory).
    // Called after populateCategories() and after Refresh so search always reflects
    // the current camera state.  Collects ALL nodes from ALL categories — visibility
    // filtering does not apply so the user can search for any parameter by name.
    void buildSearchIndex();

    // Returns true if at least one readable leaf node in catNodeName passes
    // the given visibility filter index (same mapping as m_visibilityCombo).
    // Used by onVisibilityFilterChanged to skip empty categories.
    bool hasVisibleFeatures(const QString& catNodeName, int filterIdx) const;

    // Create and display a type-appropriate widget for the feature whose
    // GenICam node name is nodeName.  Clears any previously shown widget.
    void showValueWidget(const std::string& nodeName);

    // Remove and delete the current value widget (if any) from m_valueLayout.
    void clearValueWidget();

    // Update m_favoriteButton appearance (★ filled vs ☆ empty) based on
    // whether the currently selected feature is in the pinned list.
    void updateFavoriteButton();

    // Read "AdvancedParamsDialog/lastNodeName" from QSettings and navigate to
    // that feature.  Called at the end of the constructor so the dialog reopens
    // to the previously-selected parameter across separate invocations.
    void restoreLastSelection();

    // ----- Config save / load helpers -----

    // Set UserSetSelector to userSetName (e.g. "UserSet1", "UserSet2", "Default")
    // and execute UserSetSave.  Returns true on success; writes error text on failure.
    bool saveToUserSet(const QString& userSetName, QString& errorMsg);

    // Set UserSetSelector to userSetName and execute UserSetLoad.
    // Returns true on success; writes error text on failure.
    bool loadFromUserSet(const QString& userSetName, QString& errorMsg);

    // Walk every category and read all readable, non-Command leaf nodes into a
    // flat QJsonObject (nodeName → value).  Used by saveSettingsToJson().
    QJsonObject collectAllNodeValues();

    // Serialize all readable camera node values to a JSON file at filePath.
    // The file includes metadata (timestamp, format_version) and a "nodes" object.
    bool saveSettingsToJson(const QString& filePath, QString& errorMsg);

    // Parse a JSON settings file written by saveSettingsToJson() and apply each
    // node value that is currently writable on the camera.
    // Returns true if all nodes applied without error; on partial success or failure
    // it still applies as many settings as possible and writes a summary to errorMsg.
    // onProgress(current, total) is called after each node so the caller can drive a
    // progress bar; omit or pass {} to skip progress reporting.
    bool loadSettingsFromJson(const QString& filePath, QString& errorMsg,
                              std::function<void(int current, int total)> onProgress = {});

    // Check whether ChunkModeActive is enabled and the LineStatusAll chunk is on.
    // If not, shows a dialog explaining why it matters and offers to enable it.
    // Call this after any config load so the user doesn't lose trigger-time logging
    // because a saved config happened to have chunks off.
    void checkAndPromptChunkLineStatus();

    // ----- Member variables -----

    CameraManager* m_mgr;  // Connected camera manager; NOT owned by this dialog

    // ---- Navigation controls ----
    QComboBox*   m_visibilityCombo;   // Filter: "All Levels | Favorites | Beginner | Expert | Guru"
    QComboBox*   m_categoryCombo;     // "Category:" dropdown (top-level GenICam categories)
    QComboBox*   m_featureCombo;      // "Feature:" dropdown (leaf nodes in selected category)

    // ---- Info and value area ----
    QLabel*      m_infoLabel;          // Displays description and min/max range of the node
    QWidget*     m_currentValueWidget; // Dynamic widget — nullptr when none shown
    QVBoxLayout* m_valueLayout;        // Layout that holds m_currentValueWidget

    // ---- Buttons ----
    QPushButton* m_refreshButton;     // Re-scans node tree
    QPushButton* m_saveConfigButton;  // Save Config popup menu (UserSet or .json)
    QPushButton* m_loadConfigButton;  // Load Config popup menu (UserSet or .json)
    QPushButton* m_favoriteButton;    // ★/☆ toggles pinned status on main window

    // ---- Cross-category search ----
    //
    // m_searchModel holds one row per readable node across ALL categories.
    // Each item's DisplayRole = "Category → Feature" text shown in the popup.
    // UserRole = internal GenICam node name (originally intended for lookup, but
    // unreliable across Qt's internal proxy layers — use m_displayToNode instead).
    //
    // m_featureCompleter is attached to m_featureCombo (which is set editable).
    // It uses Qt::MatchContains so typing "gain" matches "Gain", "AutoGain", etc.
    //
    // m_nodeToCategory maps GenICam node name → the category node name that owns it.
    // m_displayToNode  maps the item's display text → GenICam node name.
    //   This is the reliable lookup path in onSearchCompleterActivated: reading the
    //   DisplayRole from the activated QModelIndex always works regardless of how many
    //   internal QSortFilterProxyModel layers Qt has stacked inside QCompleter, whereas
    //   reading UserRole through mapToSource() can silently land on an intermediate
    //   proxy index and return an empty QVariant.
    QStandardItemModel*    m_searchModel;
    QCompleter*            m_featureCompleter;
    QMap<QString, QString> m_nodeToCategory;  // nodeName       → categoryNodeName
    QMap<QString, QString> m_displayToNode;   // display text   → nodeName

    // All top-level categories discovered by populateCategories() — never filtered.
    // Used by buildSearchIndex() so the search index always covers the full tree,
    // and by onVisibilityFilterChanged() to rebuild m_categoryCombo with a filter.
    // Each entry is {displayName, nodeName} matching the combo item format.
    QVector<QPair<QString,QString>> m_allCategories;

    // GenICam node name of the feature currently displayed in m_currentValueWidget.
    // Stored as std::string because GenICam names are ASCII and we pass them
    // directly to GetNode() without an intermediate conversion.
    std::string m_currentNodeName;

    // Cached last-selected node name, read from QSettings before populateCategories()
    // is called.  populateCategories() → onFeatureChanged(0) overwrites QSettings with
    // the first feature, so we must capture the real saved value ahead of that.
    QString m_pendingRestoreNode;

    // Integer type code for m_currentNodeName's node, so onApplyClicked() knows
    // how to read the widget and which CameraManager setter to call.
    // We use int instead of GenApi::EInterfaceType in the header to avoid
    // including GenApi headers here (and in every file that includes this header).
    int m_currentNodeType;  // Cast to GenApi::EInterfaceType when used in .cpp

    // Guard flag: set to true at the end of onSearchCompleterActivated to block
    // the spurious QComboBox::activated(int) signal that Qt fires after the
    // completer popup closes.  Without this, onFeatureChanged gets called a second
    // time with index 0, undoing the correct navigation.  A QTimer::singleShot(0)
    // resets it on the very next event loop iteration so normal user interaction
    // is never affected.
    bool m_searchNavigating = false;
};
