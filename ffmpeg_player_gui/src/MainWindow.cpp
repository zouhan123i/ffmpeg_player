#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "PlayerCore.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QLabel>
#include <QDebug> // Added for qDebug
#include <QStatusBar> // 修复QStatusBar不完整类型错误
#include <QTimer> // Added for QTimer
#include <QVBoxLayout>
#include <QPixmap>
#include <Qt>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    player = new PlayerCore(this);
    videoLabel = new QLabel(this);
    videoLabel->setAlignment(Qt::AlignCenter);
    ui->videoWidget->setLayout(new QVBoxLayout);
    ui->videoWidget->layout()->addWidget(videoLabel);

    connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::onOpenFile);
    connect(ui->playPauseButton, &QPushButton::clicked, this, &MainWindow::onPlayPause);
    //connect(ui->seekSlider, &QSlider::sliderMoved, this, &MainWindow::onSeek);
    connect(ui->seekSlider, &QSlider::sliderMoved, this, &MainWindow::onSeek);
    connect(ui->seekSlider, &QSlider::sliderPressed, this, &MainWindow::onSeekSliderPressed);
    connect(ui->seekSlider, &QSlider::sliderReleased, this, &MainWindow::onSeekSliderReleased);

    connect(player, &PlayerCore::frameReady, this, &MainWindow::onFrameReady);
    connect(player, &PlayerCore::durationChanged, this, &MainWindow::onDurationChanged);
    connect(player, &PlayerCore::positionChanged, this, &MainWindow::onPositionChanged);
    connect(player, &PlayerCore::stateChanged, this, [this](bool playing){
        ui->playPauseButton->setText(playing ? "Pause" : "Play");
    });
    // 删除 preciseFrameSeek/requestPreciseSeek 相关信号连接和旧逻辑
    connect(player, &PlayerCore::frameIndexReady, this, &MainWindow::onFrameIndexReady);
    connect(player, &PlayerCore::frameIndexProgress, this, &MainWindow::onFrameIndexProgress);

    ui->seekSlider->setEnabled(true);
    ui->seekSlider->setMaximum(226);
    qDebug() << "[Ctor] seekSlider maximum:" << ui->seekSlider->maximum();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::onOpenFile()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Open Video"), "", tr("Video Files (*.mp4 *.mkv *.avi)"));
    if (!fileName.isEmpty()) {
        currentFile = fileName; // 新增：记录当前文件名
        indexReady = false; // 新增：打开新文件时重置索引状态
        //ui->seekSlider->setEnabled(false); // 可选：禁用进度条
        ui->seekSlider->setValue(0); // 新增：打开新文件时重置进度条位置
        if (!player->openFile(fileName)) {
            QMessageBox::warning(this, tr("Error"), tr("Failed to open file!"));
        }
    }
}

void MainWindow::onPlayPause()
{
    if (player->isPlaying()) {
        player->pause();
    } else {
        player->play();
    }
}

void MainWindow::onSeek(int value)
{
    player->seek(value);
}

void MainWindow::onSeekSliderPressed() {
    seekSliderPressed = true;
    player->prepareForSeek();
}

void MainWindow::onSeekSliderReleased() {
    seekSliderPressed = false;
    int value = ui->seekSlider->value();
    player->pause();
    player->clearAllQueues();
    player->flushAllDecoders();
    // 1. 先精准渲染一帧（可选，UI预览）
    if (indexReady) {
       // player->renderPreciseFrame(value);
    }
    // 2. 真正 seek 并恢复播放
    player->seek(value);   // 让主流 seek 并恢复解码/播放
    player->play();
}

void MainWindow::onFrameReady(const QImage &img)
{
    if (PlayerCore::getLogLevel() >= LOG_DEBUG) {
        qDebug() << "[DEBUG] onFrameReady: called, img isNull=" << img.isNull() << ", size=" << img.size();
    }
    videoLabel->setPixmap(QPixmap::fromImage(img).scaled(videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (PlayerCore::getLogLevel() >= LOG_DEBUG) {
        qDebug() << "[DEBUG] onFrameReady: setPixmap done";
    }
}

void MainWindow::onFrameIndexReady() {
    indexReady = true;
    statusBar()->showMessage("帧索引已建立，可精准seek");
    ui->seekSlider->setEnabled(true); // 只要索引好就可用
}

void MainWindow::onFrameIndexProgress(int percent) {
    statusBar()->showMessage(QString("正在建立帧索引：%1% ").arg(percent));
}

QString MainWindow::formatTime(int seconds) {
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    int secs = seconds % 60;
    
    if (hours > 0) {
        return QString("%1:%2:%3").arg(hours, 2, 10, QChar('0')).arg(minutes, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'));
    } else {
        return QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'));
    }
}

void MainWindow::onDurationChanged(int duration)
{
    //this->duration = duration; // 确保成员变量同步
    ui->seekSlider->setMaximum(duration > 0 ? duration : 1);
    ui->seekSlider->setEnabled(true);
    updateTimeDisplay();
}

void MainWindow::onPositionChanged(int pos)
{
    // 避免用户拖动时被强制刷新
    if (!seekSliderPressed) {
        ui->seekSlider->setValue(pos);
        currentPosition = pos;
        updateTimeDisplay();
    }
    if (PlayerCore::getLogLevel() >= LOG_DEBUG) {
        qDebug() << "[DEBUG] onPositionChanged: position:" << pos << "formatted:" << formatTime(pos);
    }
}

void MainWindow::updateTimeDisplay() {
    int pos = ui->seekSlider->value();
    int total = ui->seekSlider->maximum();
    ui->timeLabel->setText(QString("%1 / %2")
        .arg(formatTime(pos))
        .arg(formatTime(total)));
}

void MainWindow::onLogLevelChanged(int index)
{
    // Set the global log level based on the combo box selection
    switch (index) {
        case 0: // Error
            PlayerCore::setLogLevel(LOG_ERROR);
            break;
        case 1: // Warning
            PlayerCore::setLogLevel(LOG_WARN);
            break;
        case 2: // Info
            PlayerCore::setLogLevel(LOG_INFO);
            break;
        case 3: // Debug
            PlayerCore::setLogLevel(LOG_DEBUG);
            break;
        default:
            PlayerCore::setLogLevel(LOG_INFO);
            break;
    }
} 