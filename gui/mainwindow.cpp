
#include "mainwindow.h"
#include "tcp_transfer.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QTextEdit>
#include <QProgressBar>
#include <QFileDialog>
#include <QProcess>
#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QCheckBox>
#include <QScrollArea>
#include <QFrame>
#include <QToolButton>
#include <QButtonGroup>
#include <QStyle>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QDialog>
#include <QDialogButtonBox>
#include <QRegularExpression>
#include <QFont>
#include <QColor>
#include <QNetworkInterface>
#include <QNetworkAddressEntry>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QLocale>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

enum class LineIcon {
    Scan,
    Compress,
    Reconstruct,
    Folder,
    OpenFolder,
    File,
    Settings,
    Play,
    Menu
};

QIcon lineIcon(LineIcon icon, const QColor &color = QColor("#94A3B8"))
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    QPainterPath path;
    switch (icon) {
    case LineIcon::Scan:
        path.moveTo(11, 18);
        path.lineTo(33, 18);
        path.lineTo(33, 41);
        path.lineTo(11, 41);
        path.closeSubpath();
        painter.drawPath(path);
        painter.drawLine(16, 25, 28, 25);
        painter.drawLine(16, 31, 26, 31);
        painter.drawEllipse(QRectF(33, 31, 18, 18));
        painter.drawLine(46, 45, 54, 53);
        break;
    case LineIcon::Compress:
        painter.drawRoundedRect(QRectF(12, 13, 40, 38), 5, 5);
        painter.drawLine(21, 22, 43, 22);
        painter.drawLine(21, 29, 43, 29);
        painter.drawLine(32, 34, 32, 45);
        painter.drawLine(26, 40, 32, 46);
        painter.drawLine(38, 40, 32, 46);
        break;
    case LineIcon::Reconstruct:
        path.moveTo(47, 25);
        path.cubicTo(42, 15, 27, 13, 19, 21);
        path.cubicTo(11, 29, 13, 41, 20, 46);
        painter.drawPath(path);
        painter.drawLine(42, 16, 48, 25);
        painter.drawLine(37, 26, 48, 25);
        path = QPainterPath();
        path.moveTo(18, 39);
        path.cubicTo(23, 49, 38, 51, 46, 43);
        path.cubicTo(54, 35, 52, 23, 45, 18);
        painter.drawPath(path);
        painter.drawLine(23, 48, 17, 39);
        painter.drawLine(28, 38, 17, 39);
        break;
    case LineIcon::Folder:
        path.moveTo(10, 19);
        path.lineTo(27, 19);
        path.lineTo(33, 25);
        path.lineTo(52, 25);
        path.lineTo(52, 46);
        path.lineTo(10, 46);
        path.closeSubpath();
        painter.drawPath(path);
        painter.drawLine(11, 25, 11, 19);
        break;
    case LineIcon::OpenFolder:
        path.moveTo(9, 22);
        path.lineTo(25, 22);
        path.lineTo(31, 28);
        path.lineTo(52, 28);
        path.lineTo(47, 46);
        path.lineTo(12, 46);
        path.closeSubpath();
        painter.drawPath(path);
        painter.drawLine(9, 22, 9, 18);
        painter.drawLine(9, 18, 26, 18);
        painter.drawLine(26, 18, 31, 23);
        break;
    case LineIcon::File:
        path.moveTo(18, 10);
        path.lineTo(37, 10);
        path.lineTo(48, 21);
        path.lineTo(48, 53);
        path.lineTo(18, 53);
        path.closeSubpath();
        painter.drawPath(path);
        painter.drawLine(37, 10, 37, 21);
        painter.drawLine(37, 21, 48, 21);
        painter.drawLine(24, 33, 42, 33);
        painter.drawLine(24, 40, 42, 40);
        break;
    case LineIcon::Settings:
        painter.drawEllipse(QRectF(23, 23, 18, 18));
        for (int angle = 0; angle < 360; angle += 45) {
            painter.save();
            painter.translate(32, 32);
            painter.rotate(angle);
            painter.drawLine(0, -22, 0, -27);
            painter.restore();
        }
        painter.drawEllipse(QRectF(10, 10, 44, 44));
        break;
    case LineIcon::Play:
        path.moveTo(23, 15);
        path.lineTo(48, 32);
        path.lineTo(23, 49);
        path.closeSubpath();
        painter.drawPath(path);
        break;
    case LineIcon::Menu:
        painter.drawLine(17, 21, 47, 21);
        painter.drawLine(17, 32, 47, 32);
        painter.drawLine(17, 43, 47, 43);
        break;
    }

    painter.end();
    return QIcon(pixmap);
}

QFrame *createCard(const QString &title, QVBoxLayout *&layout,
                   QWidget *parent = nullptr)
{
    QFrame *card = new QFrame(parent);
    card->setObjectName("card");
    layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 18);
    layout->setSpacing(12);

    if (!title.trimmed().isEmpty()) {
        QLabel *heading = new QLabel(title, card);
        heading->setObjectName("cardTitle");
        layout->addWidget(heading);
    }
    return card;
}

QToolButton *createNavButton(QWidget *parent, LineIcon icon,
                             const QString &tooltip)
{
    QToolButton *button = new QToolButton(parent);
    button->setObjectName("navButton");
    button->setIcon(lineIcon(icon));
    button->setIconSize(QSize(21, 21));
    button->setFixedSize(44, 44);
    button->setToolTip(tooltip);
    button->setCheckable(true);
    button->setAutoRaise(true);
    return button;
}

void addFieldIcon(QLineEdit *field, LineIcon icon)
{
    field->addAction(lineIcon(icon), QLineEdit::LeadingPosition);
}

QString formatByteSize(quint64 bytes)
{
    if (bytes < 1024) {
        return QString::number(bytes) + " B";
    }
    if (bytes < 1024ULL * 1024ULL) {
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 2);
    }
    if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
    }
    return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    networkWorker = nullptr;
    setWindowTitle("RetroTransfer - Local File Transfer System");
    resize(1280, 820);
    setMinimumSize(1040, 700);

    outputDirectory = QDir(QCoreApplication::applicationDirPath())
                          .absoluteFilePath("..");

    QScrollArea *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setFrameShape(QFrame::NoFrame);

    QWidget *dashboard = new QWidget();
    dashboard->setObjectName("dashboard");
    scrollArea->setWidget(dashboard);
    setCentralWidget(scrollArea);

    QHBoxLayout *dashboardLayout = new QHBoxLayout(dashboard);
    dashboardLayout->setContentsMargins(0, 0, 0, 0);
    dashboardLayout->setSpacing(0);

    sidebarWidget = new QFrame();
    sidebarWidget->setObjectName("sidebar");
    sidebarWidget->setFixedWidth(68);
    QVBoxLayout *sidebarLayout = new QVBoxLayout(sidebarWidget);
    sidebarLayout->setContentsMargins(12, 18, 12, 18);
    sidebarLayout->setSpacing(14);

    QLabel *brandMark = new QLabel();
    brandMark->setObjectName("brandMark");
    brandMark->setAlignment(Qt::AlignCenter);
    brandMark->setPixmap(lineIcon(LineIcon::Menu, QColor("#4ADE80")).pixmap(24, 24));
    brandMark->setFixedSize(42, 42);
    sidebarLayout->addWidget(brandMark, 0, Qt::AlignHCenter);
    sidebarLayout->addSpacing(14);

    QToolButton *scanNav = createNavButton(
        sidebarWidget, LineIcon::Scan, "Scan and chunk");
    QToolButton *compressNav = createNavButton(
        sidebarWidget, LineIcon::Compress, "Compress");
    QToolButton *reconstructNav = createNavButton(
        sidebarWidget, LineIcon::Reconstruct, "Reconstruct");
    QButtonGroup *navigationGroup = new QButtonGroup(this);
    navigationGroup->setExclusive(true);
    navigationGroup->addButton(scanNav, 0);
    navigationGroup->addButton(compressNav, 1);
    navigationGroup->addButton(reconstructNav, 2);
    sidebarLayout->addWidget(scanNav);
    sidebarLayout->addWidget(compressNav);
    sidebarLayout->addWidget(reconstructNav);
    sidebarLayout->addStretch();
    dashboardLayout->addWidget(sidebarWidget);

    QWidget *mainPanel = new QWidget();
    mainPanel->setObjectName("mainPanel");
    QVBoxLayout *mainPanelLayout = new QVBoxLayout(mainPanel);
    mainPanelLayout->setContentsMargins(26, 22, 26, 22);
    mainPanelLayout->setSpacing(18);
    dashboardLayout->addWidget(mainPanel, 1);

    QWidget *header = new QWidget();
    QHBoxLayout *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    QVBoxLayout *headingLayout = new QVBoxLayout();
    headingLayout->setSpacing(3);
    QLabel *title = new QLabel("RetroTransfer");
    title->setObjectName("pageTitle");
    QLabel *subtitle = new QLabel("Project packaging and reconstruction");
    subtitle->setObjectName("muted");
    headingLayout->addWidget(title);
    headingLayout->addWidget(subtitle);
    headerLayout->addLayout(headingLayout);
    headerLayout->addStretch();

    settingsButton = new QToolButton();
    settingsButton->setObjectName("settingsButton");
    settingsButton->setText("Settings");
    settingsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    settingsButton->setIcon(lineIcon(LineIcon::Settings));
    settingsButton->setIconSize(QSize(18, 18));
    settingsButton->setCursor(Qt::PointingHandCursor);
    headerLayout->addWidget(settingsButton);
    mainPanelLayout->addWidget(header);

    QWidget *columns = new QWidget();
    QGridLayout *columnsLayout = new QGridLayout(columns);
    columnsLayout->setContentsMargins(0, 0, 0, 0);
    columnsLayout->setSpacing(18);
    mainPanelLayout->addWidget(columns, 1);

    QWidget *leftColumn = new QWidget();
    leftColumn->setMinimumWidth(410);
    leftColumn->setMaximumWidth(510);
    QVBoxLayout *leftLayout = new QVBoxLayout(leftColumn);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(14);
    columnsLayout->addWidget(leftColumn, 0, 0);

    QWidget *rightColumn = new QWidget();
    QVBoxLayout *rightLayout = new QVBoxLayout(rightColumn);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(14);
    columnsLayout->addWidget(rightColumn, 0, 1);
    columnsLayout->setColumnStretch(0, 5);
    columnsLayout->setColumnStretch(1, 6);

    QVBoxLayout *operationCardLayout = nullptr;
    QFrame *operationCard = createCard("Operation", operationCardLayout);
    operationBox = new QComboBox();
    operationBox->addItem(lineIcon(LineIcon::Scan), "Scan and Chunk");
    operationBox->addItem(lineIcon(LineIcon::Compress), "Compress");
    operationBox->addItem(lineIcon(LineIcon::Reconstruct), "Reconstruct");
    operationBox->addItem("Send to Receiver");
    operationBox->addItem("Receive from Sender");
    operationCardLayout->addWidget(operationBox);
    leftLayout->addWidget(operationCard);

    QVBoxLayout *sourceLayout = nullptr;
    sourceCard = createCard("Project and inputs", sourceLayout);

    folderWidget = new QWidget();
    QVBoxLayout *folderLayout = new QVBoxLayout(folderWidget);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    QLabel *folderLabel = new QLabel("Project folder");
    folderLabel->setObjectName("fieldLabel");
    folderPath = new QLineEdit();
    folderPath->setPlaceholderText("Select the project to process");
    folderPath->setReadOnly(true);
    addFieldIcon(folderPath, LineIcon::Folder);
    browseButton = new QPushButton("Browse");
    browseButton->setIcon(lineIcon(LineIcon::OpenFolder));
    QHBoxLayout *folderRow = new QHBoxLayout();
    folderRow->setSpacing(8);
    folderRow->addWidget(folderPath, 1);
    folderRow->addWidget(browseButton);
    folderLayout->addWidget(folderLabel);
    folderLayout->addLayout(folderRow);
    sourceLayout->addWidget(folderWidget);

    compressionWidget = new QWidget();
    QVBoxLayout *compressionLayout = new QVBoxLayout(compressionWidget);
    compressionLayout->setContentsMargins(0, 8, 0, 0);
    useBaselineBox = new QCheckBox("Compare against an older project");
    compressionLayout->addWidget(useBaselineBox);
    baselineLabel = new QLabel("Older project folder");
    baselineLabel->setObjectName("fieldLabel");
    baselinePath = new QLineEdit();
    baselinePath->setPlaceholderText("Select the baseline project");
    baselinePath->setReadOnly(true);
    addFieldIcon(baselinePath, LineIcon::Folder);
    browseBaselineButton = new QPushButton("Browse");
    browseBaselineButton->setIcon(lineIcon(LineIcon::OpenFolder));
    QHBoxLayout *baselineRow = new QHBoxLayout();
    baselineRow->setSpacing(8);
    baselineRow->addWidget(baselinePath, 1);
    baselineRow->addWidget(browseBaselineButton);
    compressionLayout->addWidget(baselineLabel);
    compressionLayout->addLayout(baselineRow);
    sourceLayout->addWidget(compressionWidget);

    reconstructionWidget = new QWidget();
    QVBoxLayout *reconstructionLayout = new QVBoxLayout(reconstructionWidget);
    reconstructionLayout->setContentsMargins(0, 8, 0, 0);
    manifestLabel = new QLabel("Chunk manifest");
    manifestLabel->setObjectName("fieldLabel");
    manifestPath = new QLineEdit();
    manifestPath->setPlaceholderText("Select a chunks JSON manifest");
    manifestPath->setReadOnly(true);
    addFieldIcon(manifestPath, LineIcon::File);
    browseManifestButton = new QPushButton("Browse");
    browseManifestButton->setIcon(lineIcon(LineIcon::File));
    QHBoxLayout *manifestRow = new QHBoxLayout();
    manifestRow->setSpacing(8);
    manifestRow->addWidget(manifestPath, 1);
    manifestRow->addWidget(browseManifestButton);
    reconstructionLayout->addWidget(manifestLabel);
    reconstructionLayout->addLayout(manifestRow);

    payloadLabel = new QLabel("Payload (optional)");
    payloadLabel->setObjectName("fieldLabel");
    payloadPath = new QLineEdit();
    payloadPath->setPlaceholderText("Uses the manifest's payload filename if blank");
    payloadPath->setReadOnly(true);
    addFieldIcon(payloadPath, LineIcon::File);
    browsePayloadButton = new QPushButton("Browse");
    browsePayloadButton->setIcon(lineIcon(LineIcon::File));
    QHBoxLayout *payloadRow = new QHBoxLayout();
    payloadRow->setSpacing(8);
    payloadRow->addWidget(payloadPath, 1);
    payloadRow->addWidget(browsePayloadButton);
    reconstructionLayout->addWidget(payloadLabel);
    reconstructionLayout->addLayout(payloadRow);

    destinationWidget = new QWidget();
    QVBoxLayout *destinationLayout = new QVBoxLayout(destinationWidget);
    destinationLayout->setContentsMargins(0, 8, 0, 0);
    destinationLabel = new QLabel("Destination folder");
    destinationLabel->setObjectName("fieldLabel");
    destinationPath = new QLineEdit();
    destinationPath->setPlaceholderText("Select the output project folder");
    destinationPath->setReadOnly(true);
    addFieldIcon(destinationPath, LineIcon::Folder);
    browseDestinationButton = new QPushButton("Browse");
    browseDestinationButton->setIcon(lineIcon(LineIcon::OpenFolder));
    QHBoxLayout *destinationRow = new QHBoxLayout();
    destinationRow->setSpacing(8);
    destinationRow->addWidget(destinationPath, 1);
    destinationRow->addWidget(browseDestinationButton);
    destinationLayout->addWidget(destinationLabel);
    destinationLayout->addLayout(destinationRow);
    sourceLayout->addWidget(reconstructionWidget);
    sourceLayout->addWidget(destinationWidget);

    networkSendWidget = new QWidget();
    QVBoxLayout *networkSendLayout = new QVBoxLayout(networkSendWidget);
    networkSendLayout->setContentsMargins(0, 8, 0, 0);
    QLabel *scanInputLabel = new QLabel("Existing scan JSON");
    scanInputLabel->setObjectName("fieldLabel");
    scanInputPath = new QLineEdit();
    scanInputPath->setReadOnly(true);
    scanInputPath->setPlaceholderText("Select the scan JSON created earlier");
    addFieldIcon(scanInputPath, LineIcon::File);
    browseScanInputButton = new QPushButton("Browse");
    browseScanInputButton->setIcon(lineIcon(LineIcon::File));
    QHBoxLayout *scanInputRow = new QHBoxLayout();
    scanInputRow->setSpacing(8);
    scanInputRow->addWidget(scanInputPath, 1);
    scanInputRow->addWidget(browseScanInputButton);
    QLabel *sendHostLabel = new QLabel("Receiver IP address");
    sendHostLabel->setObjectName("fieldLabel");
    sendHost = new QLineEdit();
    sendHost->setPlaceholderText("For example, 192.168.1.25");
    QLabel *sendPortLabel = new QLabel("Receiver TCP port");
    sendPortLabel->setObjectName("fieldLabel");
    sendPort = new QLineEdit("48321");
    sendPort->setMaxLength(5);
    QLabel *sendCodeLabel = new QLabel("One-time pairing code");
    sendCodeLabel->setObjectName("fieldLabel");
    sendPairingCode = new QLineEdit();
    sendPairingCode->setMaxLength(16);
    sendPairingCode->setPlaceholderText("Enter the 16-character code shown by the receiver");
    networkSendLayout->addWidget(scanInputLabel);
    networkSendLayout->addLayout(scanInputRow);
    networkSendLayout->addWidget(sendHostLabel);
    networkSendLayout->addWidget(sendHost);
    networkSendLayout->addWidget(sendPortLabel);
    networkSendLayout->addWidget(sendPort);
    networkSendLayout->addWidget(sendCodeLabel);
    networkSendLayout->addWidget(sendPairingCode);
    sourceLayout->addWidget(networkSendWidget);

    networkReceiveWidget = new QWidget();
    QVBoxLayout *networkReceiveLayout = new QVBoxLayout(networkReceiveWidget);
    networkReceiveLayout->setContentsMargins(0, 8, 0, 0);
    QLabel *receivePortLabel = new QLabel("Listen on TCP port");
    receivePortLabel->setObjectName("fieldLabel");
    receivePort = new QLineEdit("48321");
    receivePort->setMaxLength(5);
    QLabel *receiveCodeLabel = new QLabel("One-time pairing code (16 characters)");
    receiveCodeLabel->setObjectName("fieldLabel");
    receivePairingCode = new QLineEdit();
    receivePairingCode->setReadOnly(true);
    receiveAddressLabel = new QLabel();
    receiveAddressLabel->setObjectName("muted");
    receiveAddressLabel->setWordWrap(true);
    networkReceiveLayout->addWidget(receivePortLabel);
    networkReceiveLayout->addWidget(receivePort);
    networkReceiveLayout->addWidget(receiveCodeLabel);
    networkReceiveLayout->addWidget(receivePairingCode);
    networkReceiveLayout->addWidget(receiveAddressLabel);
    sourceLayout->addWidget(networkReceiveWidget);
    leftLayout->addWidget(sourceCard);

    QVBoxLayout *strategyCardLayout = nullptr;
    strategyCard = createCard("Chunking strategy", strategyCardLayout);
    strategyBox = new QComboBox();
    strategyBox->addItem("Adaptive");
    strategyBox->addItem("Fixed");
    strategyBox->addItem("CDC");
    strategyCardLayout->addWidget(strategyBox);
    leftLayout->addWidget(strategyCard);

    QVBoxLayout *outputFilesLayout = nullptr;
    outputFilesCard = createCard("File outputs", outputFilesLayout);
    scanChunkWidget = new QWidget();
    QVBoxLayout *scanChunkLayout = new QVBoxLayout(scanChunkWidget);
    scanChunkLayout->setContentsMargins(0, 0, 0, 0);
    scanChunkLayout->setSpacing(8);
    scanOutputLabel = new QLabel("Scan JSON");
    scanOutputLabel->setObjectName("fieldLabel");
    scanOutputName = new QLineEdit("scan.json");
    addFieldIcon(scanOutputName, LineIcon::File);
    chunksOutputLabel = new QLabel("Chunk manifest JSON");
    chunksOutputLabel->setObjectName("fieldLabel");
    chunksOutputName = new QLineEdit("scan_chunks.json");
    addFieldIcon(chunksOutputName, LineIcon::File);
    scanChunkLayout->addWidget(scanOutputLabel);
    scanChunkLayout->addWidget(scanOutputName);
    scanChunkLayout->addWidget(chunksOutputLabel);
    scanChunkLayout->addWidget(chunksOutputName);
    outputFilesLayout->addWidget(scanChunkWidget);

    payloadOutputWidget = new QWidget();
    QVBoxLayout *payloadOutputLayout = new QVBoxLayout(payloadOutputWidget);
    payloadOutputLayout->setContentsMargins(0, 8, 0, 0);
    payloadOutputLabel = new QLabel("Compressed payload");
    payloadOutputLabel->setObjectName("fieldLabel");
    payloadOutputName = new QLineEdit("missing-chunks.rtpack");
    addFieldIcon(payloadOutputName, LineIcon::File);
    payloadOutputLayout->addWidget(payloadOutputLabel);
    payloadOutputLayout->addWidget(payloadOutputName);
    outputFilesLayout->addWidget(payloadOutputWidget);

    outputDirectoryLabel = new QLabel();
    outputDirectoryLabel->setObjectName("muted");
    outputDirectoryLabel->setWordWrap(true);
    outputFilesLayout->addWidget(outputDirectoryLabel);
    leftLayout->addWidget(outputFilesCard);
    leftLayout->addStretch();

    QVBoxLayout *actionCardLayout = nullptr;
    QFrame *actionCard = createCard("Actions and progress", actionCardLayout);
    startButton = new QPushButton("START TRANSFER");
    startButton->setObjectName("primaryButton");
    startButton->setIcon(lineIcon(LineIcon::Play, QColor("#FFFFFF")));
    startButton->setMinimumHeight(48);
    startButton->setCursor(Qt::PointingHandCursor);
    actionCardLayout->addWidget(startButton);
    statusLabel = new QLabel("[READY] Ready");
    statusLabel->setObjectName("statusLabel");
    actionCardLayout->addWidget(statusLabel);
    progressBar = new QProgressBar();
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setFormat("Ready");
    actionCardLayout->addWidget(progressBar);
    rightLayout->addWidget(actionCard);

    QVBoxLayout *comparisonCardLayout = nullptr;
    comparisonWidget = createCard("Size comparison", comparisonCardLayout);
    originalSizeLabel = new QLabel("Original project: --");
    payloadSizeLabel = new QLabel("Transfer package: --");
    sizeSavedLabel = new QLabel("Size saved: --");
    originalSizeLabel->setObjectName("muted");
    payloadSizeLabel->setObjectName("muted");
    sizeSavedLabel->setObjectName("statusLabel");
    comparisonCardLayout->addWidget(originalSizeLabel);
    comparisonCardLayout->addWidget(payloadSizeLabel);
    comparisonCardLayout->addWidget(sizeSavedLabel);
    comparisonWidget->setVisible(false);
    rightLayout->addWidget(comparisonWidget);

    QVBoxLayout *consoleCardLayout = nullptr;
    QFrame *consoleCard = createCard("Processing output", consoleCardLayout);
    outputArea = new QTextEdit();
    outputArea->setReadOnly(true);
    outputArea->setLineWrapMode(QTextEdit::NoWrap);
    outputArea->setMinimumHeight(360);
    outputArea->setPlaceholderText("Backend messages will appear here.");
    QFont consoleFont("Consolas");
    consoleFont.setStyleHint(QFont::Monospace);
    outputArea->setFont(consoleFont);
    outputArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    outputArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    consoleCardLayout->addWidget(outputArea, 1);
    rightLayout->addWidget(consoleCard, 1);

    dashboard->setStyleSheet(
        "QWidget#dashboard, QWidget#mainPanel {"
        "background-color: #0B1220; color: #E5E7EB;"
        "font-family: Segoe UI; font-size: 13px;"
        "}"
        "QFrame#sidebar {"
        "background-color: #080D16; border-right: 1px solid #1F2A3A;"
        "}"
        "QLabel#brandMark {"
        "background-color: #12372A; color: #4ADE80; border-radius: 12px;"
        "font-size: 15px; font-weight: 700;"
        "}"
        "QLabel#pageTitle {font-size: 24px; font-weight: 700; color: #F8FAFC;}"
        "QLabel#muted {color: #94A3B8;}"
        "QFrame#card {"
        "background-color: #111A29; border: 1px solid #253247; border-radius: 14px;"
        "}"
        "QLabel#cardTitle {font-size: 15px; font-weight: 600; color: #F1F5F9;}"
        "QLabel#fieldLabel {font-size: 12px; color: #A7B4C7;}"
        "QLineEdit, QComboBox {"
        "background-color: #0B1220; color: #E5E7EB; border: 1px solid #2B3A50;"
        "border-radius: 8px; padding: 8px 10px; min-height: 20px;"
        "}"
        "QLineEdit:focus, QComboBox:focus {border: 1px solid #38BDF8;}"
        "QComboBox QAbstractItemView {"
        "background-color: #111A29; color: #E5E7EB; selection-background-color: #1D4ED8;"
        "}"
        "QPushButton, QToolButton#settingsButton {"
        "background-color: #182336; color: #E5E7EB; border: 1px solid #2B3A50;"
        "border-radius: 8px; padding: 8px 10px;"
        "}"
        "QPushButton:hover, QToolButton#settingsButton:hover {"
        "background-color: #22314A;"
        "}"
        "QToolButton#navButton {background: transparent; border: none; border-radius: 10px;}"
        "QToolButton#navButton:hover {background-color: #172235;}"
        "QToolButton#navButton:checked {background-color: #12372A;}"
        "QPushButton#primaryButton {"
        "background-color: #15803D; color: white; border: none; border-radius: 9px;"
        "font-size: 14px; font-weight: 700; letter-spacing: 1px;"
        "}"
        "QPushButton#primaryButton:hover {background-color: #16A34A;}"
        "QPushButton#primaryButton:disabled {background-color: #334155; color: #94A3B8;}"
        "QLabel#statusLabel {font-size: 13px; font-weight: 600; color: #CBD5E1;}"
        "QProgressBar {"
        "background-color: #0B1220; color: white; border: 1px solid #253247;"
        "border-radius: 7px; text-align: center; min-height: 18px;"
        "}"
        "QProgressBar::chunk {background-color: #22C55E; border-radius: 6px;}"
        "QTextEdit {"
        "background-color: #070B12; color: #C9D1D9; border: 1px solid #30363D;"
        "border-radius: 9px; padding: 10px;"
        "}"
        "QCheckBox {color: #CBD5E1; spacing: 8px;}"
        "QScrollArea {border: none; background-color: #0B1220;}"
    );

    outputDirectoryLabel->setText(
        "Generated files: " + QDir(outputDirectory).filePath("collection"));

    navigationGroup->button(0)->setChecked(true);
    connect(navigationGroup, &QButtonGroup::idClicked,
            this, [this](int index) {
                operationBox->setCurrentIndex(index);
            });
    connect(operationBox, &QComboBox::currentIndexChanged,
            this, [navigationGroup](int index) {
                if (QAbstractButton *button = navigationGroup->button(index)) {
                    button->setChecked(true);
                }
            });
    connect(settingsButton, &QToolButton::clicked,
            this, &MainWindow::showSettingsDialog);
    connect(useBaselineBox, &QCheckBox::toggled,
            this, [this](bool checked) {
                baselineLabel->setVisible(checked);
                baselinePath->setVisible(checked);
                browseBaselineButton->setVisible(checked);
            });
    connect(operationBox, &QComboBox::currentTextChanged,
            this, [this](const QString &) {
                comparisonWidget->setVisible(false);
                updateOperationFields();
            });

    // Create backend process
    backendProcess = new QProcess(this);
    backendProcess->setWorkingDirectory(outputDirectory);

#ifdef Q_OS_WIN
    // Prevent a separate console window from appearing.
    backendProcess->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
        }
    );
#endif

    // Display backend standard output.
    connect(backendProcess,
            &QProcess::readyReadStandardOutput,
            this,
            [this]() {
                QString output = QString::fromLocal8Bit(
                    backendProcess->readAllStandardOutput()
                );
                appendConsoleOutput("INFO", output);
            });

    // Display backend errors.
    connect(backendProcess,
            &QProcess::readyReadStandardError,
            this,
            [this]() {
                QString output = QString::fromLocal8Bit(
                    backendProcess->readAllStandardError()
                );
                appendConsoleOutput("ERROR", output);
            });

    // Handle completion.
    connect(backendProcess,
            &QProcess::finished,
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {

                startButton->setEnabled(true);
                browseButton->setEnabled(true);
                strategyBox->setEnabled(true);
                browseBaselineButton->setEnabled(true);
                browseManifestButton->setEnabled(true);
                browsePayloadButton->setEnabled(true);
                browseDestinationButton->setEnabled(true);
                useBaselineBox->setEnabled(true);
                operationBox->setEnabled(true);
                sidebarWidget->setEnabled(true);
                settingsButton->setEnabled(true);
                updateOperationFields();

                progressBar->setRange(0, 100);

                if (exitStatus == QProcess::NormalExit &&
                    exitCode == 0) {
                    progressBar->setValue(100);
                    progressBar->setFormat("Complete");
                    statusLabel->setText("[SUCCESS] " +
                                         operationBox->currentText() + " completed");
                    statusLabel->setStyleSheet("color: #4ADE80; font-weight: 600;");
                    appendConsoleOutput("SUCCESS",
                                        operationBox->currentText() + " completed.");
                    if (operationBox->currentText() == "Compress") {
                        updateCompressionComparison();
                    }
                } else {
                    progressBar->setValue(0);
                    progressBar->setFormat("Failed");
                    statusLabel->setText("[ERROR] Operation failed; see output");
                    statusLabel->setStyleSheet("color: #F87171; font-weight: 600;");
                    comparisonWidget->setVisible(false);
                }
            });

    // Handle failure to start the backend executable.
    connect(backendProcess,
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError error) {

                if (error == QProcess::FailedToStart) {
                    appendConsoleOutput("ERROR", "Could not start RetroTransfer.exe.");
                    appendConsoleOutput("ERROR", backendProcess->errorString());

                    startButton->setEnabled(true);
                    browseButton->setEnabled(true);
                    strategyBox->setEnabled(true);
                    browseBaselineButton->setEnabled(true);
                    browseManifestButton->setEnabled(true);
                    browsePayloadButton->setEnabled(true);
                    browseDestinationButton->setEnabled(true);
                    useBaselineBox->setEnabled(true);
                    operationBox->setEnabled(true);
                    sidebarWidget->setEnabled(true);
                    settingsButton->setEnabled(true);
                    updateOperationFields();

                    progressBar->setRange(0, 100);
                    progressBar->setValue(0);
                    progressBar->setFormat("Failed");
                    statusLabel->setText("[ERROR] Backend could not start");
                    statusLabel->setStyleSheet("color: #F87171; font-weight: 600;");
                }
            });

    // Button connections
    connect(browseButton, &QPushButton::clicked,
            this, &MainWindow::browseFolder);

    connect(browseBaselineButton, &QPushButton::clicked,
            this, &MainWindow::browseBaseline);

    connect(browseManifestButton, &QPushButton::clicked,
            this, &MainWindow::browseManifest);

    connect(browsePayloadButton, &QPushButton::clicked,
            this, &MainWindow::browsePayload);

    connect(browseDestinationButton, &QPushButton::clicked,
            this, &MainWindow::browseDestination);

    connect(browseScanInputButton, &QPushButton::clicked,
            this, &MainWindow::browseScanInput);

    connect(startButton, &QPushButton::clicked,
            this, &MainWindow::startTransfer);

    updateOperationFields();
}

MainWindow::~MainWindow()
{
    if (networkWorker && networkWorker->isRunning()) {
        networkWorker->cancel();
        networkWorker->wait();
    }
}

void MainWindow::browseFolder()
{
    QString selectedFolder = QFileDialog::getExistingDirectory(
        this,
        "Select Folder",
        ""
    );

    if (!selectedFolder.isEmpty()) {
        folderPath->setText(selectedFolder);
    }
}

void MainWindow::browseBaseline()
{
    QString selectedFolder = QFileDialog::getExistingDirectory(
        this, "Select Older Project Folder", ""
    );
    if (!selectedFolder.isEmpty()) {
        baselinePath->setText(selectedFolder);
    }
}

void MainWindow::browseManifest()
{
    QString selectedFile = QFileDialog::getOpenFileName(
        this, "Select Chunk Manifest", "", "JSON files (*.json)"
    );
    if (!selectedFile.isEmpty()) {
        manifestPath->setText(selectedFile);
    }
}

void MainWindow::browsePayload()
{
    QString selectedFile = QFileDialog::getOpenFileName(
        this, "Select Payload", "", "RetroTransfer payloads (*.rtpack)"
    );
    if (!selectedFile.isEmpty()) {
        payloadPath->setText(selectedFile);
    }
}

void MainWindow::browseDestination()
{
    QString selectedFolder = QFileDialog::getExistingDirectory(
        this, "Select Destination Folder", ""
    );
    if (!selectedFolder.isEmpty()) {
        destinationPath->setText(selectedFolder);
    }
}

void MainWindow::browseScanInput()
{
    const QString selectedFile = QFileDialog::getOpenFileName(
        this, "Select Existing Scan JSON", outputDirectory,
        "JSON files (*.json)");
    if (!selectedFile.isEmpty()) {
        scanInputPath->setText(selectedFile);
    }
}

void MainWindow::showSettingsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Settings");
    dialog.setMinimumWidth(520);
    dialog.setStyleSheet(
        "QDialog {background-color: #0B1220; color: #E5E7EB;}"
        "QLabel {color: #CBD5E1;}"
        "QLineEdit {background-color: #111A29; color: #E5E7EB;"
        "border: 1px solid #2B3A50; border-radius: 7px; padding: 8px;}"
        "QPushButton {background-color: #182336; color: #E5E7EB;"
        "border: 1px solid #2B3A50; border-radius: 7px; padding: 8px;}"
    );

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    QLabel *description = new QLabel(
        "Choose where generated files are written. RetroTransfer creates a "
        "\"collection\" folder inside this location."
    );
    description->setWordWrap(true);
    layout->addWidget(description);

    QLineEdit *directoryEdit = new QLineEdit(outputDirectory);
    directoryEdit->setReadOnly(true);
    QPushButton *browseButton = new QPushButton("Browse");
    browseButton->setIcon(lineIcon(LineIcon::OpenFolder));
    QHBoxLayout *directoryRow = new QHBoxLayout();
    directoryRow->addWidget(directoryEdit, 1);
    directoryRow->addWidget(browseButton);
    layout->addLayout(directoryRow);

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    connect(browseButton, &QPushButton::clicked, &dialog, [&]() {
        const QString selected = QFileDialog::getExistingDirectory(
            &dialog, "Select Output Folder", directoryEdit->text());
        if (!selected.isEmpty()) {
            directoryEdit->setText(selected);
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted &&
        !directoryEdit->text().isEmpty() &&
        QDir(directoryEdit->text()).exists()) {
        outputDirectory = QDir(directoryEdit->text()).absolutePath();
        backendProcess->setWorkingDirectory(outputDirectory);
        outputDirectoryLabel->setText(
            "Generated files: " +
            QDir(outputDirectory).filePath("collection"));
    }
}

void MainWindow::appendConsoleOutput(const QString &tag, const QString &text)
{
    const QStringList lines = text.split(
        QRegularExpression("\\r\\n|\\n|\\r"), Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        return;
    }

    QColor tagColor("#93C5FD");
    if (tag == "PROCESSING") {
        tagColor = QColor("#38BDF8");
    } else if (tag == "SUCCESS") {
        tagColor = QColor("#4ADE80");
    } else if (tag == "ERROR") {
        tagColor = QColor("#F87171");
    }

    QTextCharFormat tagFormat;
    tagFormat.setForeground(tagColor);
    tagFormat.setFontWeight(QFont::DemiBold);
    QTextCharFormat textFormat;
    textFormat.setForeground(QColor("#D1D5DB"));

    QTextCursor cursor(outputArea->document());
    cursor.movePosition(QTextCursor::End);
    for (const QString &line : lines) {
        cursor.insertText("[" + tag + "] ", tagFormat);
        cursor.insertText(line + "\n", textFormat);
    }
    outputArea->setTextCursor(cursor);
    outputArea->ensureCursorVisible();
}

void MainWindow::updateCompressionComparison()
{
    QString manifestName = chunksOutputName->text().trimmed();
    if (QFileInfo(manifestName).suffix().isEmpty()) {
        manifestName += ".json";
    }
    const QString manifestPath =
        QDir(outputDirectory).filePath("collection/" + manifestName);
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        appendConsoleOutput("ERROR", "Could not read the chunk manifest for size comparison.");
        return;
    }

    const QJsonDocument document =
        QJsonDocument::fromJson(manifestFile.readAll());
    if (!document.isObject()) {
        appendConsoleOutput("ERROR", "Chunk manifest is not valid JSON.");
        return;
    }

    const QJsonObject compression =
        document.object().value("selectiveCompression").toObject();
    if (compression.isEmpty()) {
        appendConsoleOutput("ERROR", "The chunk manifest has no compression metrics.");
        return;
    }

    const quint64 originalBytes =
        static_cast<quint64>(compression.value("totalSourceBytes").toDouble());
    const quint64 payloadBytes =
        static_cast<quint64>(compression.value("payloadBytes").toDouble());
    const quint64 headerBytes =
        static_cast<quint64>(compression.value("bundleHeaderBytes").toDouble());
    quint64 packageBytes = payloadBytes + headerBytes;
    if (packageBytes < payloadBytes) {
        packageBytes = payloadBytes;
    }

    const quint64 savedBytes =
        originalBytes > packageBytes ? originalBytes - packageBytes : 0;
    const double savedPercent =
        originalBytes == 0 ? 0.0
                           : (100.0 * static_cast<double>(savedBytes)
                              / static_cast<double>(originalBytes));

    originalSizeLabel->setText(
        "Original project: " + formatByteSize(originalBytes)
        + " (" + QLocale().toString(originalBytes) + " bytes)");
    payloadSizeLabel->setText(
        "Transfer package: " + formatByteSize(packageBytes)
        + " (" + QLocale().toString(packageBytes) + " bytes)");
    sizeSavedLabel->setText(
        "Size saved: " + formatByteSize(savedBytes)
        + " (" + QString::number(savedPercent, 'f', 2) + "%)");
    sizeSavedLabel->setStyleSheet("color: #4ADE80; font-weight: 600;");
    comparisonWidget->setVisible(true);

    appendConsoleOutput(
        "SUCCESS",
        "Size comparison: " + formatByteSize(originalBytes)
        + " original -> " + formatByteSize(packageBytes)
        + " package, " + formatByteSize(savedBytes)
        + " saved (" + QString::number(savedPercent, 'f', 2) + "%)");
}

void MainWindow::updateOperationFields()
{
    QString operation = operationBox->currentText();
    if (operation != "Compress") {
        comparisonWidget->setVisible(false);
    }
    bool reconstruct = operation == "Reconstruct";
    bool compress = operation == "Compress";
    bool sending = operation == "Send to Receiver";
    bool receiving = operation == "Receive from Sender";
    bool networkOperation = sending || receiving;

    folderWidget->setVisible(!reconstruct && !networkOperation);
    compressionWidget->setVisible(compress);
    reconstructionWidget->setVisible(reconstruct || sending);
    destinationWidget->setVisible(reconstruct || receiving);
    networkSendWidget->setVisible(sending);
    networkReceiveWidget->setVisible(receiving);
    strategyCard->setVisible(!reconstruct && !networkOperation);
    outputFilesCard->setVisible(!reconstruct && !networkOperation);
    scanChunkWidget->setVisible(!reconstruct && !networkOperation);
    payloadOutputWidget->setVisible(compress);

    folderPath->setPlaceholderText("Select the project to process");
    manifestLabel->setText(sending ? "Chunk manifest (required)" : "Chunk manifest");
    payloadLabel->setText(sending ? "Compressed payload (required)"
                                  : "Payload (optional)");
    payloadPath->setPlaceholderText(
        sending ? "Select the .rtpack payload to send"
                : "Uses the manifest's payload filename if blank");
    destinationLabel->setText(receiving ? "Reconstruction destination folder"
                                        : "Destination folder");

    if (receiving && receivePairingCode->text().isEmpty()) {
        receivePairingCode->setText(QString::number(
            QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0')
                                         .toUpper());

        QStringList addresses;
        const auto interfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface &networkInterface : interfaces) {
            if (!(networkInterface.flags() & QNetworkInterface::IsUp) ||
                !(networkInterface.flags() & QNetworkInterface::IsRunning) ||
                (networkInterface.flags() & QNetworkInterface::IsLoopBack)) {
                continue;
            }
            for (const QNetworkAddressEntry &entry :
                 networkInterface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol &&
                    !entry.ip().isLoopback()) {
                    addresses.append(entry.ip().toString());
                }
            }
        }
        addresses.removeDuplicates();
        receiveAddressLabel->setText(
            addresses.isEmpty()
                ? "No active LAN IPv4 address was found. Check the network connection."
                : "Connect from the sender to one of these addresses: " +
                      addresses.join(", "));
    }

    if (receiving) {
        startButton->setText(networkWorker && networkWorker->isRunning()
                                 ? "STOP LISTENING"
                                 : "START RECEIVER");
    } else if (sending) {
        startButton->setText("SEND PACKAGE");
    } else {
        startButton->setText("START TRANSFER");
    }

    if (useBaselineBox->isChecked()) {
        baselineLabel->setVisible(compress);
        baselinePath->setVisible(compress);
        browseBaselineButton->setVisible(compress);
    } else {
        baselineLabel->setVisible(false);
        baselinePath->setVisible(false);
        browseBaselineButton->setVisible(false);
    }
}

void MainWindow::startTransfer()
{
    comparisonWidget->setVisible(false);

    if (networkWorker && networkWorker->isRunning()) {
        if (operationBox->currentText() == "Receive from Sender") {
            networkWorker->cancel();
            startButton->setEnabled(false);
            statusLabel->setText("[PROCESSING] Stopping receiver...");
            appendConsoleOutput("PROCESSING", "Stopping the TCP receiver.");
        }
        return;
    }

    const QString selectedOperation = operationBox->currentText();
    if (selectedOperation == "Send to Receiver" ||
        selectedOperation == "Receive from Sender") {
        startNetworkTransfer(selectedOperation == "Receive from Sender");
        return;
    }

    if (backendProcess->state() != QProcess::NotRunning) {
        return;
    }

    QString backendPath =
        QDir(QCoreApplication::applicationDirPath())
            .filePath("RetroTransfer.exe");

    if (!QFileInfo::exists(backendPath)) {
        appendConsoleOutput("ERROR", "RetroTransfer.exe was not found beside the GUI.");
        statusLabel->setText("[ERROR] Backend executable not found");
        statusLabel->setStyleSheet("color: #F87171; font-weight: 600;");
        return;
    }

    QStringList arguments;
    QString operation = operationBox->currentText();
    auto rejectInput = [this](const QString &message) {
        statusLabel->setText("[ERROR] " + message);
        statusLabel->setStyleSheet("color: #F87171; font-weight: 600;");
        appendConsoleOutput("ERROR", message);
    };

    if (operation == "Reconstruct") {
        if (manifestPath->text().isEmpty()) {
            rejectInput("Please select a chunk manifest.");
            return;
        }
        if (destinationPath->text().isEmpty()) {
            rejectInput("Please select a destination folder.");
            return;
        }

        arguments << "--reconstruct" << manifestPath->text();
        if (!payloadPath->text().isEmpty()) {
            arguments << "--payload" << payloadPath->text();
        }
        arguments << "--output" << destinationPath->text();
    } else {
        if (folderPath->text().isEmpty()) {
            rejectInput("Please select a project folder.");
            return;
        }

        QString scanName = scanOutputName->text().trimmed();
        QString chunksName = chunksOutputName->text().trimmed();
        if (scanName.isEmpty() || chunksName.isEmpty()) {
            rejectInput("Scan and chunk filenames are required.");
            return;
        }

        arguments << folderPath->text();
        arguments << "--strategy" << strategyBox->currentText().toLower();
        arguments << "--scan-output" << scanName;
        arguments << "--chunks-output" << chunksName;

        if (operation == "Compress") {
            QString payloadName = payloadOutputName->text().trimmed();
            if (payloadName.isEmpty()) {
                rejectInput("Please enter a payload filename.");
                return;
            }
            arguments << "--payload-output" << payloadName;

            if (useBaselineBox->isChecked()) {
                if (baselinePath->text().isEmpty()) {
                    rejectInput("Please select an older project folder.");
                    return;
                }
                arguments << "--baseline" << baselinePath->text();
            } else {
                arguments << "--compress-all";
            }
        }
    }

    outputArea->clear();
    appendConsoleOutput("PROCESSING", "Starting RetroTransfer backend.");
    appendConsoleOutput("INFO", "Operation: " + operation);
    if (operation != "Reconstruct") {
        appendConsoleOutput("INFO", "Selected folder: " + folderPath->text());
        appendConsoleOutput("INFO", "Selected strategy: " + strategyBox->currentText());
    } else {
        appendConsoleOutput("INFO", "Manifest: " + manifestPath->text());
        appendConsoleOutput("INFO", "Destination: " + destinationPath->text());
    }

    statusLabel->setText("[PROCESSING] Running " + operation + "...");
    statusLabel->setStyleSheet("color: #38BDF8; font-weight: 600;");

    progressBar->setRange(0, 0);
    progressBar->setFormat("Processing");

    startButton->setEnabled(false);
    browseButton->setEnabled(false);
    strategyBox->setEnabled(false);
    browseBaselineButton->setEnabled(false);
    browseManifestButton->setEnabled(false);
    browsePayloadButton->setEnabled(false);
    browseDestinationButton->setEnabled(false);
    useBaselineBox->setEnabled(false);
    operationBox->setEnabled(false);
    sidebarWidget->setEnabled(false);
    settingsButton->setEnabled(false);

    backendProcess->setWorkingDirectory(outputDirectory);
    backendProcess->start(backendPath, arguments);
}

void MainWindow::startNetworkTransfer(bool receiving)
{
    auto rejectInput = [this](const QString &message) {
        statusLabel->setText("[ERROR] " + message);
        statusLabel->setStyleSheet("color: #F87171; font-weight: 600;");
        appendConsoleOutput("ERROR", message);
    };

    bool portOk = false;
    const QString portText = receiving ? receivePort->text().trimmed()
                                       : sendPort->text().trimmed();
    const uint port = portText.toUInt(&portOk);
    if (!portOk || port == 0 || port > 65535) {
        rejectInput("Enter a TCP port from 1 to 65535.");
        return;
    }

    TcpTransferOptions options;
    options.port = static_cast<quint16>(port);
    if (receiving) {
        if (destinationPath->text().isEmpty() ||
            !QDir(destinationPath->text()).exists()) {
            rejectInput("Select an existing reconstruction destination folder.");
            return;
        }
        options.mode = TcpTransferOptions::Mode::ReceivePackage;
        options.pairingCode = receivePairingCode->text();
        options.destinationDirectory = destinationPath->text();
        options.incomingDirectory =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (options.incomingDirectory.isEmpty()) {
            rejectInput("Could not determine a persistent application data folder.");
            return;
        }
    } else {
        if (sendHost->text().trimmed().isEmpty()) {
            rejectInput("Enter the receiver's LAN IP address.");
            return;
        }
        if (!QFileInfo(scanInputPath->text()).isFile() ||
            !QFileInfo(manifestPath->text()).isFile() ||
            !QFileInfo(payloadPath->text()).isFile()) {
            rejectInput("Select an existing scan JSON, chunk manifest, and .rtpack payload.");
            return;
        }
        if (!QRegularExpression("^[0-9A-Fa-f]{16}$")
                 .match(sendPairingCode->text().trimmed()).hasMatch()) {
            rejectInput("Enter the receiver's 16-character hexadecimal pairing code.");
            return;
        }
        options.mode = TcpTransferOptions::Mode::SendPackage;
        options.host = sendHost->text().trimmed();
        options.pairingCode = sendPairingCode->text().trimmed();
        options.scanPath = scanInputPath->text();
        options.manifestPath = manifestPath->text();
        options.payloadPath = payloadPath->text();
    }

    outputArea->clear();
    appendConsoleOutput("PROCESSING",
                        receiving ? "Starting the TCP receiver."
                                  : "Preparing the package for TCP transfer.");
    if (receiving) {
        appendConsoleOutput("INFO", "Pairing code: " + options.pairingCode);
        appendConsoleOutput("INFO",
                            "Incoming partial packages are stored in the application data folder.");
    } else {
        appendConsoleOutput("INFO", "Receiver: " + options.host + ":" +
                                        QString::number(options.port));
        appendConsoleOutput("INFO", "Scan JSON: " + options.scanPath);
        appendConsoleOutput("INFO", "Manifest: " + options.manifestPath);
        appendConsoleOutput("INFO", "Payload: " + options.payloadPath);
    }

    statusLabel->setText(receiving ? "[PROCESSING] Waiting for a paired sender..."
                                   : "[PROCESSING] Connecting to receiver...");
    statusLabel->setStyleSheet("color: #38BDF8; font-weight: 600;");
    progressBar->setRange(0, 0);
    progressBar->setFormat(receiving ? "Waiting for sender" : "Connecting");

    const bool receiverMode = receiving;
    networkWorker = new TcpTransferWorker(
        options, this,
        [this](const QString &message) {
            appendConsoleOutput("INFO", message);
        },
        [this](quint64 completed, quint64 total) {
            if (total == 0) return;
            const int percent = static_cast<int>(
                (static_cast<long double>(completed) * 100.0L) /
                static_cast<long double>(total));
            progressBar->setRange(0, 100);
            progressBar->setValue(std::min(percent, 100));
            progressBar->setFormat("%p%");
            statusLabel->setText(
                QString("[PROCESSING] Transferring %1 of %2 bytes")
                    .arg(static_cast<qulonglong>(completed))
                    .arg(static_cast<qulonglong>(total)));
        },
        this);

    setOperationControlsEnabled(false);
    if (receiverMode) {
        startButton->setEnabled(true);
        startButton->setText("STOP LISTENING");
    }
    connect(networkWorker, &QThread::finished, this, [this, receiverMode]() {
        const bool succeeded = networkWorker->succeeded();
        const QString error = networkWorker->errorMessage();
        const QString summary = networkWorker->summary();
        networkWorker->deleteLater();
        networkWorker = nullptr;

        if (receiverMode) {
            receivePairingCode->clear();
        }
        setOperationControlsEnabled(true);
        updateOperationFields();
        progressBar->setRange(0, 100);
        if (succeeded) {
            progressBar->setValue(100);
            progressBar->setFormat("Complete");
            statusLabel->setText("[SUCCESS] " + summary);
            statusLabel->setStyleSheet("color: #4ADE80; font-weight: 600;");
            appendConsoleOutput("SUCCESS", summary);
        } else {
            progressBar->setValue(0);
            progressBar->setFormat("Stopped");
            statusLabel->setText("[ERROR] " +
                                 (error.isEmpty() ? "Network transfer failed." : error));
            statusLabel->setStyleSheet("color: #F87171; font-weight: 600;");
            appendConsoleOutput("ERROR",
                                error.isEmpty() ? "Network transfer failed." : error);
        }
    });
    networkWorker->start();
}

void MainWindow::setOperationControlsEnabled(bool enabled)
{
    startButton->setEnabled(enabled);
    browseButton->setEnabled(enabled);
    strategyBox->setEnabled(enabled);
    browseBaselineButton->setEnabled(enabled);
    browseManifestButton->setEnabled(enabled);
    browsePayloadButton->setEnabled(enabled);
    browseDestinationButton->setEnabled(enabled);
    browseScanInputButton->setEnabled(enabled);
    useBaselineBox->setEnabled(enabled);
    operationBox->setEnabled(enabled);
    sidebarWidget->setEnabled(enabled);
    settingsButton->setEnabled(enabled);
    sendHost->setEnabled(enabled);
    sendPort->setEnabled(enabled);
    sendPairingCode->setEnabled(enabled);
    receivePort->setEnabled(enabled);
}
