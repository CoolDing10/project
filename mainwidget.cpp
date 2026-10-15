/*
 * ============================================================================
 * mainwidget.cpp — 主窗口导航外壳实现
 * ============================================================================
 *
 * 本文件是 MainWidget 类的实现。MainWidget 是设备端程序的"外壳"，
 * 负责侧边栏导航 + 页面栈管理，不包含任何具体业务逻辑。
 *
 * 核心设计：惰性加载 (Lazy Loading)
 * ─────────────────────────────────
 * 程序启动时只创建导航框架（侧边栏 + 空白 QStackedWidget），
 * 具体的业务页面（CameraStreamingPage 等）不会立刻创建。
 * 只有当用户第一次点击对应导航项时，才会调用工厂函数创建页面，
 * 创建后缓存到 loadedPages 数组中，下次切换直接复用。
 *
 * 好处：
 *   - 启动速度快（不需要等所有页面初始化完）
 *   - 内存占用低（只保留当前可见页 + 已访问过的页面）
 *   - 对于 ARM 嵌入式设备（内存有限）特别重要
 *
 * 页面注册表：
 *   ┌──────────┬──────────────────────┬─────────────┐
 *   │  导航名称  │      类名             │  包装方式     │
 *   ├──────────┼──────────────────────┼─────────────┤
 *   │  边缘监控  │  CameraStreamingPage  │  直接嵌入     │
 *   │  图像分析  │  OpenCvDemoPage       │  包一层ScrollArea│
 *   │  事件录像  │  VideoPlayer          │  直接嵌入     │
 *   │  板级设备  │  BoardDiagnosticsPage │  直接嵌入     │
 *   └──────────┴──────────────────────┴─────────────┘
 */

#include "mainwidget.h"

#include "imx6ulltest.h"
#include "opencv.h"
#include "video_monitor.h"
#include "videoplayer.h"

#include <QDebug>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

/*
 * ============================================================================
 * 构造函数 — 只做最少的初始化：设固定尺寸，然后委托给 initializeNavigation()
 * ============================================================================
 *
 * 1024x600 是 i.MX6ULL 开发板屏幕的物理分辨率。
 * x86 PC 上也用同样尺寸，方便开发时模拟真实设备效果。
 */
MainWidget::MainWidget(QWidget *parent)
    : QWidget(parent),
      mainLayout(new QHBoxLayout(this)),
      navigationList(new QListWidget(this)),
      pageStack(new QStackedWidget(this)),
      pageTitleLabel(new QLabel(this)),
      pageDescriptionLabel(new QLabel(this))
{
    resize(1024, 600);
    setObjectName(QStringLiteral("project"));
    initializeNavigation();
}

/*
 * ============================================================================
 * registerPages() — 注册所有业务页面的元数据（不创建实例！）
 * ============================================================================
 *
 * 这个函数只向 pageDefinitions 数组中填充 PageDefinition 结构体，
 * 描述每个页面"叫什么"和"如何创建"。真正的页面实例在这里还不存在。
 *
 * PageDefinition 四元组：
 *   1. navigationTitle  — 侧边栏显示的文字
 *   2. pageTitle        — 内容区顶部大标题
 *   3. pageDescription  — 内容区顶部描述文字
 *   4. createPage       — 工厂 lambda，惰性调用时才构造页面
 *
 * 工厂 lambda 的设计要点：
 *   - 捕获列表为空 []，不持有任何外部引用
 *   - 接受 parent 参数（QStackedWidget），页面作为其子控件
 *   - "图像分析"页面额外包了一层 QScrollArea，因为 OpenCV 分析页内容较长、
 *     1024x600 屏幕放不下，需要滚动
 *
 * 旧版轮播图、独立人脸识别、音频播放器、MQTT Demo 和 SQLite 闹钟
 * 已从工程中移除；相关业务能力已经收敛到当前四个页面。
 */
void MainWidget::registerPages()
{
    pageDefinitions = {
        /*
         * 页面1：边缘监控
         * CameraStreamingPage 是整个系统最核心的页面，集成了：
         * 摄像头采集、画面变化检测、TCP推流、事件录像、事件存储和 MQTT
         */
        {QStringLiteral("边缘监控"),
         QStringLiteral("边缘视觉监控中心"),
        QStringLiteral("视频预览 · 画面变化检测 · 事件录像 · 远程管理"),
         [](QWidget *parent)
         { return new CameraStreamingPage(parent); }},

        /*
         * 页面2：图像分析
         * 用 QScrollArea 包裹，因为 OpenCV 分析页有很多参数控件和结果显示区，
         * 在 1024x600 屏幕上一屏显示不下。
         * setWidgetResizable(false) 保持内容的原始大小，不强制拉伸。
         */
        {QStringLiteral("图像分析"),
         QStringLiteral("OpenCV 图像分析"),
         QStringLiteral("自动载入最新抓拍，也可手动选择图片进行算法测试"),
         [](QWidget *parent)
         {
             QScrollArea *scrollArea = new QScrollArea(parent);
             scrollArea->setWidget(new OpenCvDemoPage(scrollArea));
             scrollArea->setWidgetResizable(false); // 不拉伸内容，显示滚动条
             scrollArea->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
             scrollArea->setFrameShape(QFrame::NoFrame);
             scrollArea->setStyleSheet(QStringLiteral(
                 "QScrollArea { background:#1b2028; border:none; }"
                 "QScrollArea > QWidget > QWidget { background:#1b2028; }"));
             return scrollArea;
         }},

        /*
         * 页面3：事件录像
         * 自动扫描录像目录，列出并播放边缘监控产生的事件录像文件。
         */
        {QStringLiteral("事件录像"),
         QStringLiteral("事件录像中心"),
         QStringLiteral("自动发现、播放和管理边缘监控生成的录像"),
         [](QWidget *parent)
         { return new VideoPlayer(parent); }},

        /*
         * 页面4：板级设备
         * 显示 i.MX6ULL 板载硬件状态：AP3216C 传感器、ICM20608 IMU、
         * LED、蜂鸣器等设备的实时诊断信息。
         */
        {QStringLiteral("板级设备"),
         QStringLiteral("i.MX6ULL 板级设备"),
         QStringLiteral("传感器、LED与蜂鸣器硬件状态诊断"),
         [](QWidget *parent)
         { return new BoardDiagnosticsPage(parent); }}};

}

/*
 * ============================================================================
 * initializeNavigation() — 构建完整的导航外壳 UI
 * ============================================================================
 *
 * 布局结构（对应 mainwidget.h 中的 ASCII 图）：
 *
 *   ┌─────────────────┬───────────────────────────────────────┐
 *   │   sideBar 168px │          contentArea                  │
 *   │                 │  ┌───────────────────────────────────┐│
 *   │  brandTitle     │  │  contentHeader (52px)              ││
 *   │  brandSubTitle  │  │  ├─ pageTitleLabel                 ││
 *   │                 │  │  └─ pageDescriptionLabel            ││
 *   │  navigationList │  ├───────────────────────────────────┤│
 *   │  (导航列表)      │  │  pageStack (QStackedWidget)       ││
 *   │                 │  │  当前显示的页面                     ││
 *   │  versionLabel   │  │                                   ││
 *   └─────────────────┴───────────────────────────────────────┘
 *
 * 关键设计决策：
 *   1. sideBar 固定 168px 宽度 — 给导航项足够空间又不浪费屏幕
 *   2. contentHeader 固定 52px — 在 1024x600 上为业务页面多保留 12px 垂直空间
 *   3. 占位页（placeholder）— 起初在 QStackedWidget 中插入空白 QWidget，
 *      保持导航列表索引和页面栈索引一致，惰性加载时再替换为真实页面
 */
void MainWidget::initializeNavigation()
{
    // 第一步：注册页面元数据（不创建实例）
    registerPages();

    // 初始化 loadedPages 数组，全部置为 nullptr（表示尚未创建）
    loadedPages.fill(nullptr, pageDefinitions.size());

    /*
     * ========== 全局样式表 ==========
     * 深色主题，用 objectName 选择器限定各区域的样式。
     * QSS 语法类似 CSS，支持 #id 选择器和 ::item 伪状态。
     */
    setStyleSheet(QStringLiteral(
        "#mainShell { background: #11151c; }"
        "#sideBar { background: #111820; border-right: 1px solid #293341; }"
        "#brandTitle { color: #ffffff; font-size: 18px; font-weight: 700; }"
        "#brandSubTitle { color: #6dc7ff; font-size: 11px; }"
        "#navigationList { background: transparent; border: none; outline: none; }"
        "#navigationList::item { color: #c8d0da; border-radius: 7px; "
        "margin: 3px 8px; padding: 8px 12px; }"
        "#navigationList::item:hover { background: #202c39; color: white; }"
        "#navigationList::item:selected { background: #1677b8; color: white; "
        "font-weight: 600; border-left: 3px solid #65d5ff; }"
        "#contentHeader { background: #181e27; border-bottom: 1px solid #303946; }"
        "#pageTitle { color: #f5f8fb; font-size: 18px; font-weight: 700; }"
        "#pageDescription { color: #94a2b3; font-size: 11px; }"
        "#pageStack { background: #1b2028; }"));
    setObjectName(QStringLiteral("mainShell"));

    /*
     * ========== 左侧侧边栏 ==========
     */
    navigationList->setObjectName(QStringLiteral("navigationList"));
    navigationList->setFixedWidth(168);

    /*
     * 为每个页面在导航列表中创建一项，同时在 pageStack 中插入一个占位页。
     * 占位页的作用：保证 navigationList 的第 N 行对应 pageStack 的第 N 页。
     * 惰性加载时会用真实页面替换占位页。
     */
    for (const PageDefinition &definition : pageDefinitions)
    {
        QListWidgetItem *navigationItem =
            new QListWidgetItem(definition.navigationTitle, navigationList);
        navigationItem->setTextAlignment(Qt::AlignCenter);
        navigationItem->setSizeHint(QSize(160, 55));

        /* 占位页只用于保持导航行号与 QStackedWidget 索引一致。 */
        pageStack->addWidget(new QWidget(pageStack));
    }

    // 品牌标识：标题 + 副标题
    QLabel *brandTitle = new QLabel(QStringLiteral("i.MX6ULL EDGE"), this);
    brandTitle->setObjectName(QStringLiteral("brandTitle"));
    brandTitle->setAlignment(Qt::AlignCenter);
    QLabel *brandSubTitle = new QLabel(QStringLiteral("智能视觉管理终端"), this);
    brandSubTitle->setObjectName(QStringLiteral("brandSubTitle"));
    brandSubTitle->setAlignment(Qt::AlignCenter);

    // 组装侧边栏
    QWidget *sideBar = new QWidget(this);
    sideBar->setObjectName(QStringLiteral("sideBar"));
    sideBar->setFixedWidth(168);
    QVBoxLayout *sideLayout = new QVBoxLayout(sideBar);
    sideLayout->setContentsMargins(0, 18, 0, 12);
    sideLayout->setSpacing(3);
    sideLayout->addWidget(brandTitle);    // "i.MX6ULL EDGE"
    sideLayout->addWidget(brandSubTitle); // "智能视觉管理终端"
    sideLayout->addSpacing(18);
    sideLayout->addWidget(navigationList, 1); // stretch=1，占满剩余空间
    QLabel *versionLabel = new QLabel(QStringLiteral("EDGE TERMINAL 1.0"), sideBar);
    versionLabel->setStyleSheet(QStringLiteral("color:#586779; font-size:10px;"));
    versionLabel->setAlignment(Qt::AlignCenter);
    sideLayout->addWidget(versionLabel); // 版本号在底部

    /*
     * ========== 右侧内容区 ==========
     */

    // 内容区顶部：页面标题 + 描述（固定 52px 高度）
    QWidget *contentHeader = new QWidget(this);
    contentHeader->setObjectName(QStringLiteral("contentHeader"));
    contentHeader->setFixedHeight(52);
    pageTitleLabel->setObjectName(QStringLiteral("pageTitle"));
    pageDescriptionLabel->setObjectName(QStringLiteral("pageDescription"));
    QVBoxLayout *headerLayout = new QVBoxLayout(contentHeader);
    headerLayout->setContentsMargins(16, 5, 16, 5);
    headerLayout->setSpacing(0);
    headerLayout->addWidget(pageTitleLabel);
    headerLayout->addWidget(pageDescriptionLabel);

    // 内容区主体：QStackedWidget 页面栈
    pageStack->setObjectName(QStringLiteral("pageStack"));

    // 组装内容区：标题头 + 页面栈
    QWidget *contentArea = new QWidget(this);
    QVBoxLayout *contentLayout = new QVBoxLayout(contentArea);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    contentLayout->addWidget(contentHeader); // 固定 52px
    contentLayout->addWidget(pageStack, 1);  // 占满剩余空间

    /*
     * ========== 顶层布局：侧边栏 + 内容区 ==========
     */
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(sideBar);        // 固定 168px
    mainLayout->addWidget(contentArea, 1); // 占满剩余空间

    // 导航列表的行切换 → 触发页面切换
    connect(navigationList, &QListWidget::currentRowChanged,
            this, &MainWidget::showPage);

    // 默认选中第一个页面（边缘监控），触发惰性创建
    if (!pageDefinitions.isEmpty())
    {
        navigationList->setCurrentRow(0);
        navigationList->setFocus();
    }
}

/*
 * ============================================================================
 * showPage() — 页面切换 + 惰性加载的核心实现
 * ============================================================================
 *
 * 这是整个导航框架最关键的函数。每次用户点击导航列表时被调用。
 *
 * 执行流程：
 *   1. 边界检查（pageIndex 合法性）
 *   2. 更新顶部标题和描述文字
 *   3. 检查 loadedPages[pageIndex]：
 *      - 如果已创建（非 nullptr）→ 直接跳转显示（O(1)）
 *      - 如果未创建（nullptr）→ 进入惰性加载流程
 *   4. 惰性加载子流程：
 *      a) 调用工厂 lambda 创建页面实例
 *      b) 安全检查：如果页面被误创建为独立窗口，强制转为嵌入控件
 *      c) 确保页面的 parent 是 QStackedWidget
 *      d) 用真实页面替换占位页（removeWidget + insertWidget）
 *      e) 缓存到 loadedPages 数组
 *   5. setCurrentWidget(page) 显示页面
 *
 * 为什么需要 "占位页替换" 而不是直接 addWidget？
 *   因为导航列表索引和 QStackedWidget 索引必须保持一一对应。
 *   如果用 addWidget，新页面会被追加到末尾，索引就对不上了。
 */
void MainWidget::showPage(int pageIndex)
{
    // 边界检查
    if (pageIndex < 0 || pageIndex >= pageDefinitions.size())
        return;

    // 更新顶部标题栏
    pageTitleLabel->setText(pageDefinitions.at(pageIndex).pageTitle);
    pageDescriptionLabel->setText(pageDefinitions.at(pageIndex).pageDescription);

    QWidget *page = loadedPages.at(pageIndex);
    if (!page)
    {
        /*
         * ===== 惰性加载：页面首次被访问，需要创建实例 =====
         */

        // 获取当前位置的占位页（后续会被替换掉）
        QWidget *placeholder = pageStack->widget(pageIndex);

        // 调用工厂函数创建真实页面，parent 设为 pageStack
        page = pageDefinitions.at(pageIndex).createPage(pageStack);
        if (!page)
            return;

        /*
         * 防御性检查：防止页面被误创建为顶层窗口。
         * 例如某页面构造函数中调用了 show() 或者设置了 Qt::Window 标志，
         * 会导致页面以独立窗口弹出，主界面内容区显示空白。
         * 这里强制将其降级为普通 Widget 嵌入显示。
         */
        if (page->isWindow())
        {
            qWarning() << "页面被创建成顶层窗口，已自动改为嵌入显示："
                       << pageDefinitions.at(pageIndex).navigationTitle;
            page->setWindowFlags(Qt::Widget);
        }

        // 确保页面的父对象是 pageStack（工厂可能没有正确设置 parent）
        if (page->parentWidget() != pageStack)
            page->setParent(pageStack);

        /*
         * 替换占位页：先移除空白占位页，再在相同索引位置插入真实页面。
         * 这样 QStackedWidget 的索引和导航列表的索引保持一致。
         *
         * 操作顺序：
         *   1. removeWidget(placeholder) — 从布局中移除，但不 delete
         *   2. placeholder->deleteLater() — 安全删除占位页
         *   3. insertWidget(pageIndex, page) — 在同位置插入真实页面
         */
        pageStack->removeWidget(placeholder); // 移除的是placeholder地址里的东西
        placeholder->deleteLater();
        pageStack->insertWidget(pageIndex, page);

        // 缓存已创建的页面实例，下次直接复用
        loadedPages[pageIndex] = page;
        qInfo() << "惰性加载页面完成："
                << pageDefinitions.at(pageIndex).navigationTitle;
    }

    // 切换到目标页面（已缓存的 或 刚刚创建的）
    pageStack->setCurrentWidget(page);
}
