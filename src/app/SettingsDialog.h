#pragma once
#include <QDialog>
#include <QHash>
#include <QVariant>
#include <QVector>
#include <functional>

class QLineEdit;
class QLabel;
class QComboBox;
class QPushButton;
class QGridLayout;
class QCheckBox;
class QTabWidget;

// Categorized settings, modeled on d4analyzer's startup window. First category
// "Directories": Game Path, TACT keys, D4 Data — each with a path, a live validity
// check, a Browse button and a Download button — plus a read-only Version row.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    void showExportTab();            // open the dialog focused on the Export tab

signals:
    // Emitted the moment a Wardrobe option is toggled (settings are written live),
    // so the open Wardrobe tab can update without waiting for OK. needsRebuild is
    // true when the change affects geometry/textures (e.g. nude base).
    void wardrobeLiveChanged(bool needsRebuild);
    // Emitted when any live-persisted option changes (Export/Models/Wardrobe/Performance),
    // so the host can re-sync tabs (e.g. the Models tab's mirrored Tex/Anim checkboxes).
    void settingsChanged();

private:
    void accept() override;          // persist to Config (paths/product/view), then close
    void reject() override;          // restore the open-time snapshot of live settings, then close
    void snapshotLiveSettings();     // capture live-persisted keys so Cancel can revert them
    void updateChecks();             // refresh the ✓/✗ indicators
    void updateVersion();            // read d4data/buildVersion.txt
    void downloadTactKeys();
    void downloadD4Data();
    void runUpdateCheck();           // notify-only probe: is newer d4data / TACT-keys data available?

    QTabWidget* m_tabs = nullptr;    // the category tab bar
    int         m_exportTabIndex = -1;
    QHash<QString, QVariant> m_snapshot;   // open-time values of the live keys (for Cancel revert)
    QVector<std::function<void()>> m_exportResetActions;   // set each Export widget back to its default

    QLineEdit* m_game    = nullptr;  QLabel* m_gameChk   = nullptr;
    QLineEdit* m_tact    = nullptr;  QLabel* m_tactChk   = nullptr;
    QLineEdit* m_d4data  = nullptr;  QLabel* m_d4dataChk = nullptr;
    QLabel*    m_version = nullptr;
    QComboBox* m_product = nullptr;
    QPushButton* m_tactDl   = nullptr;
    QPushButton* m_d4dataDl = nullptr;
    QPushButton* m_updCheckBtn = nullptr;   // "Check for updates" (does not download)
    QCheckBox*   m_updAuto     = nullptr;   // run the check at every startup
    QLabel*      m_updStatus   = nullptr;   // last check result
    QCheckBox*   m_rememberTab = nullptr;   // View: remember last selected tab
    QCheckBox*   m_rememberPanels = nullptr; // View: remember panel/column widths (splitters)
    QCheckBox*   m_includeLocale = nullptr; // Dev: also index locale text/speech/video packs
    QCheckBox*   m_mdlHover = nullptr;        // Models: on-hover preview pop-up
    QCheckBox*   m_mdlRememberLast = nullptr; // Models: remember last selected model
    // Wardrobe section toggles (its own panels + animations + ensembles player).
    QCheckBox*   m_w2NudeBase    = nullptr;
    QCheckBox*   m_w2AutoAnimate = nullptr;
    QCheckBox*   m_w2RememberPanels = nullptr; // remember the side-panel layout (strip toggles)
    QCheckBox*   m_w2SecEnsembles= nullptr; // ENSEMBLES (saved-looks panel)
    QCheckBox*   m_w2ShowLog     = nullptr; // DEBUG LOG (status text + Copy debug)
};
