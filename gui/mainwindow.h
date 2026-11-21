
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>

class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;
class QTextEdit;
class QProgressBar;
class QProcess;
class QCheckBox;
class QToolButton;
class TcpTransferWorker;

class MainWindow : public QMainWindow
{
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    QLineEdit *folderPath;
    QLineEdit *baselinePath;
    QLineEdit *manifestPath;
    QLineEdit *payloadPath;
    QLineEdit *destinationPath;
    QLineEdit *scanOutputName;
    QLineEdit *chunksOutputName;
    QLineEdit *payloadOutputName;
    QLineEdit *scanInputPath;
    QLineEdit *sendHost;
    QLineEdit *sendPort;
    QLineEdit *sendPairingCode;
    QLineEdit *receivePort;
    QLineEdit *receivePairingCode;
    QComboBox *strategyBox;
    QComboBox *operationBox;
    QCheckBox *useBaselineBox;
    QPushButton *browseButton;
    QPushButton *browseBaselineButton;
    QPushButton *browseManifestButton;
    QPushButton *browsePayloadButton;
    QPushButton *browseDestinationButton;
    QPushButton *browseScanInputButton;
    QPushButton *startButton;
    QLabel *statusLabel;
    QLabel *baselineLabel;
    QLabel *manifestLabel;
    QLabel *payloadLabel;
    QLabel *destinationLabel;
    QLabel *scanOutputLabel;
    QLabel *chunksOutputLabel;
    QLabel *payloadOutputLabel;
    QLabel *outputDirectoryLabel;
    QLabel *receiveAddressLabel;
    QLabel *originalSizeLabel;
    QLabel *payloadSizeLabel;
    QLabel *sizeSavedLabel;
    QTextEdit *outputArea;
    QProgressBar *progressBar;
    QWidget *scanChunkWidget;
    QWidget *payloadOutputWidget;
    QWidget *compressionWidget;
    QWidget *comparisonWidget;
    QWidget *reconstructionWidget;
    QWidget *destinationWidget;
    QWidget *networkSendWidget;
    QWidget *networkReceiveWidget;
    QWidget *folderWidget;
    QWidget *sidebarWidget;
    QWidget *sourceCard;
    QWidget *outputFilesCard;
    QWidget *strategyCard;
    QToolButton *settingsButton;

    QProcess *backendProcess;
    TcpTransferWorker *networkWorker;
    QString outputDirectory;

    void browseFolder();
    void browseBaseline();
    void browseManifest();
    void browsePayload();
    void browseDestination();
    void browseScanInput();
    void updateOperationFields();
    void showSettingsDialog();
    void appendConsoleOutput(const QString &tag, const QString &text);
    void updateCompressionComparison();
    void startTransfer();
    void startNetworkTransfer(bool receiving);
    void setOperationControlsEnabled(bool enabled);
};

#endif
