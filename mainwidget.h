#ifndef MAINWIDGET_H
#define MAINWIDGET_H

#include <functional>

#include <QVector>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QListWidget;
class QStackedWidget;

/*
 * ============================================================================
 * MainWidget — 应用程序主窗口（导航外壳）
 * ============================================================================
 *
 * 这是整个 i.MX6ULL 智能终端设备端程序的主界面框架。
 * 它不是具体业务页面，而是一个"导航外壳"：
 *
 *   ┌──────────────┬──────────────────────────────────────────┐
 *   │   侧边栏      │          内容区域                         │
 *   │  (168px)     │  ┌────────────────────────────────────┐  │
 *   │              │  │  页面标题 / 页面描述 (52px)          │  │
 *   │  QListWidget │  ├────────────────────────────────────┤  │
 *   │  (导航列表)   │  │                                    │  │
 *   │              │  │  QStackedWidget (页面栈)             │  │
 *   │  · 边缘监控   │  │  ├─ CameraStreamingPage (边缘监控)   │  │
 *   │  · 图像分析   │  │  ├─ OpenCvDemoPage    (图像分析)    │  │
 *   │  · 事件录像   │  │  ├─ VideoPlayer       (事件录像)    │  │
 *   │  · 板级设备   │  │  └─ BoardDiagnosticsPage (板级设备) │  │
 *   │              │  │                                    │  │
 *   └──────────────┴──────────────────────────────────────────┘
 *
 * 核心设计理念：
 * 1. 惰性加载（Lazy Loading）：页面不在启动时全部创建，而是在用户
 *    第一次点击导航项时才构造，之后复用同一实例。
 *    这样做的好处是启动速度快、内存占用低。
 *
 * 2. 工厂模式：每个页面用一个 std::function 工厂函数描述"如何创建"，
 *    而不是直接持有页面实例指针。
 *
 * 3. final 类：禁止被继承，因为这个导航框架设计上不需要扩展。
 */
class MainWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit MainWidget(QWidget *parent = nullptr);
    ~MainWidget() override = default;

private slots:
    /*
     * 导航列表点击响应：切换到第 pageIndex 个页面。
     * 如果该页面尚未创建（loadedPages[pageIndex] 为 nullptr），
     * 则调用对应工厂函数创建并缓存。
     */
    void showPage(int pageIndex);

private:
    /*
     * PageFactory：页面工厂函数类型
     * 接受 parent widget，返回构造好的页面指针。
     * 使用 std::function 而非函数指针，可以捕获 lambda 闭包，
     * 方便在 registerPages() 中灵活定义每个页面的创建逻辑。
     */
    using PageFactory = std::function<QWidget *(QWidget *parent)>;

    /*
     * PageDefinition：页面元数据定义
     * - navigationTitle: 侧边栏导航列表显示的文字
     * - pageTitle:       内容区顶部大标题
     * - pageDescription: 内容区顶部描述文字
     * - createPage:      页面工厂函数（惰性调用）
     */
    struct PageDefinition
    {
        QString navigationTitle;
        QString pageTitle;
        QString pageDescription;
        PageFactory createPage;
    };

    void initializeNavigation();
    void registerPages();

    /*
     * 布局相关成员
     */
    QHBoxLayout *mainLayout;      // 顶层水平布局：侧边栏 + 内容区
    QListWidget *navigationList;  // 左侧 168px 导航列表（侧边栏）
    QStackedWidget *pageStack;    // 右侧页面栈，同一时间只显示一个页面
    QLabel *pageTitleLabel;       // 内容区顶部的页面标题
    QLabel *pageDescriptionLabel; // 内容区顶部的页面描述

    /*
     * 页面管理
     * pageDefinitions: 在 registerPages() 中填充，记录所有页面的元数据
     * loadedPages:     与 pageDefinitions 下标一一对应，
     *                  存储已惰性创建的页面实例（nullptr 表示尚未创建）
     */
    QVector<PageDefinition> pageDefinitions;
    QVector<QWidget *> loadedPages;
};

#endif // MAINWIDGET_H