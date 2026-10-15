/*
 * ============================================================================
 * main.cpp — i.MX6ULL 智能终端 设备端程序入口
 * ============================================================================
 *
 * 本文件是整个设备端 Qt 应用程序的启动入口。
 * 它做的事情非常少，只有三件：
 *
 *   1. 加载全局样式表 (QSS)
 *   2. 创建并显示 MainWidget 主窗口
 *   3. 进入 Qt 事件循环 (application.exec())
 *
 * 之所以这么精简，是因为所有业务逻辑都在 MainWidget 及其子页面中。
 *
 * 编译目标：
 *   - Desktop (x86_64): 用于 PC 端开发调试，以普通窗口运行
 *   - ARM (i.MX6ULL):   用于开发板部署，全屏运行 (1024x600)
 *
 * 启动流程：
 *   main()
 *     → MainWidget 构造
 *       → initializeNavigation()
 *         → registerPages()    记录 4 个页面的元数据（不创建实例）
 *         → 构建 UI 布局         侧边栏 + QStackedWidget
 *       → show() / showFullScreen()
 *     → 用户点击导航 → showPage() → 惰性创建对应页面
 */

#include "mainwidget.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QListWidget>
#include <QStackedWidget>
#include <QTimer>

/*
 * ============================================================================
 * 匿名命名空间 — 文件内部工具函数
 * ============================================================================
 * 这两个函数只在 main.cpp 内使用，放在匿名 namespace 中等价于 static，
 * 不会污染全局符号表。
 */
namespace
{

    /*
     * 从 Qt 资源文件加载 QSS 样式表并应用到整个应用程序。
     * QSS 语法类似 CSS，可以统一控制所有控件的颜色、字体、边距等。
     */
    void applyApplicationStyleSheet(QApplication &application,
                                    const QString &resourcePath)
    {
        QFile styleSheetFile(resourcePath);
        if (!styleSheetFile.open(QIODevice::ReadOnly))
            return;

        application.setStyleSheet(QString::fromUtf8(styleSheetFile.readAll()));
    }

    /*
     * 页面冒烟测试（Smoke Test）
     * ============================================================================
     *
     * 只在环境变量 IMX6ULL_PAGE_SMOKE_TEST 的值明确为 1、true、yes 或 on
     * 时才执行。设置为 0、false、no、off 或未设置时均保持正常运行。
     * 用途：在 CI/CD 或本地验证时，自动遍历所有导航页面，检测：
     *   1. 每个页面能否正常创建（不会有崩溃或异常）
     *   2. 页面是否被正确嵌入主窗口（不应该弹出独立窗口）
     *   3. 1024x600 布局尺寸是否正确
     *
     * 工作原理：
     *   每隔 150ms 自动切换到下一个页面，触发惰性创建。
     *   全部切换完成后，检查顶层窗口数量是否为 1（只有主窗口）。
     *   如果出现独立弹窗，说明某个页面没有正确嵌入，测试失败。
     *
     * 使用方式：
     *   $ IMX6ULL_PAGE_SMOKE_TEST=1 ./program
     */
    void schedulePageSmokeTest(QApplication &application, MainWidget &mainWindow)
    {
        const QString smokeTestValue = qEnvironmentVariable(
                    "IMX6ULL_PAGE_SMOKE_TEST").trimmed().toLower();
        const bool smokeTestEnabled =
                smokeTestValue == QLatin1String("1") ||
                smokeTestValue == QLatin1String("true") ||
                smokeTestValue == QLatin1String("yes") ||
                smokeTestValue == QLatin1String("on");
        if (!smokeTestEnabled)
            return;

        QListWidget *navigation =
            mainWindow.findChild<QListWidget *>(QStringLiteral("navigationList"));
        if (!navigation)
            return;

        // 每隔 150ms 依次切换到下一个导航项，触发页面惰性创建
        const int pageCreationIntervalMs = 150;
        for (int row = 0; row < navigation->count(); ++row)
        {
            QTimer::singleShot(row * pageCreationIntervalMs,
                               navigation, [navigation, row]()
                               { navigation->setCurrentRow(row); });
        }

        // 所有页面切换完成后 500ms，检查测试结果
        QTimer::singleShot(navigation->count() * pageCreationIntervalMs + 500,
                           &application, [&application, &mainWindow]()
                           {
        // 统计可见的顶层窗口数量
        int visibleTopLevelWindowCount = 0;
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (widget->isVisible())
                ++visibleTopLevelWindowCount;
        }

        qInfo() << "页面冒烟测试完成，可见顶层窗口数："
                << visibleTopLevelWindowCount;
        QStackedWidget *pageStack = mainWindow.findChild<QStackedWidget *>(
                    QStringLiteral("pageStack"));
        qInfo() << "1024x600布局检查：主窗口=" << mainWindow.size()
                << "页面区域=" << (pageStack ? pageStack->size() : QSize());

        // 如果多于 1 个顶层窗口，说明某个页面以独立窗口弹出，测试失败
        if (visibleTopLevelWindowCount != 1) {
            qCritical() << "页面嵌入检查失败：除主界面外仍有页面独立弹出";
            application.exit(2);
            return;
        }
        application.quit(); });
    }

} // namespace

/*
 * ============================================================================
 * main() — 应用程序入口
 * ============================================================================
 */
int main(int argc, char *argv[])
{
    // 1. 创建 QApplication，这是每个 Qt GUI 程序必须的第一步
    QApplication application(argc, argv);

    // 设置组织名和应用名，QSettings 会使用这些信息确定配置文件路径
    QCoreApplication::setOrganizationName(QStringLiteral("IMX6ULL"));
    QCoreApplication::setApplicationName(QStringLiteral("EdgeMonitorTerminal"));

    // 从 Qt 资源文件加载全局样式表（深色主题风格）
    applyApplicationStyleSheet(application, QStringLiteral(":/style/mainstyle.qss"));

    // 2. 创建主窗口
    MainWidget mainWindow;
    mainWindow.setWindowTitle(QStringLiteral("i.MX6ULL 智能终端"));

    // 3. 根据编译目标选择显示模式
#if defined(__arm__)
    /*
     * ARM 开发板：全屏运行
     * 开发板屏幕固定为 1024x600。全屏可避免窗口管理器标题栏额外占用
     * 约 30px 高度，保证页面得到完整的 1024x600 客户区。
     */
    mainWindow.showFullScreen();
#else
    // x86 PC：普通窗口运行，方便开发调试
    mainWindow.show();
#endif

    // 仅当 IMX6ULL_PAGE_SMOKE_TEST=1/true/yes/on 时执行自动化页面测试
    schedulePageSmokeTest(application, mainWindow);

    // 4. 进入 Qt 事件循环，程序在此阻塞，直到窗口关闭
    return application.exec();
}
