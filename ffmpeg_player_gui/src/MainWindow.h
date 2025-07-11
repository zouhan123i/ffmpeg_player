#pragma once
#include <QMainWindow>
#include <QImage>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class PlayerCore;
class QLabel;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onOpenFile();
    void onPlayPause();
    void onSeek(int value);
    void onSeekSliderPressed();
    void onSeekSliderReleased();
    void onFrameReady(const QImage &img);
    void onDurationChanged(int duration);
    void onPositionChanged(int pos);
    void onLogLevelChanged(int index);
    void onFrameIndexReady(); // 新增：帧索引完成槽
    void onFrameIndexProgress(int percent); // 新增：索引进度槽

private:
    QString formatTime(int seconds);
    void updateTimeDisplay();
    
    Ui::MainWindow *ui;
    PlayerCore* player = nullptr;
    QLabel* videoLabel = nullptr;
    int currentPosition = 0;
    QString currentFile; // 新增：用于主线程重建流
    int currentDuration = 0;
    bool seekSliderPressed = false;  // 标记用户是否正在操作进度条
    bool indexReady = false; // 新增：UI侧索引状态
}; 