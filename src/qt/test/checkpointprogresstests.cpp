// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/checkpointprogresstests.h>

#include <qt/bitcoingui.h>
#include <qt/clientmodel.h>
#include <qt/networkstyle.h>
#include <qt/platformstyle.h>
#include <test/test_bitcoin.h>
#include <ui_interface.h>

#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QStatusBar>
#include <QTest>

#include <memory>
#include <thread>

void CheckpointProgressTests::progress()
{
    TestingSetup setup;
    const QLocale oldLocale;
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    std::unique_ptr<const PlatformStyle> platform(PlatformStyle::instantiate("other"));
    std::unique_ptr<const NetworkStyle> network(NetworkStyle::instantiate("main"));
    ClientModel model(nullptr);
    BitcoinGUI gui(platform.get(), network.get());
    gui.setClientModel(&model);
    QLabel* label = nullptr;
    for (auto* candidate : gui.statusBar()->findChildren<QLabel*>()) {
        if (candidate->text() == "Connecting to peers...") label = candidate;
    }
    QVERIFY(label);
    auto* bar = gui.statusBar()->findChild<QProgressBar*>();
    QVERIFY(bar);

    // Deliver core notifications from a worker: widgets only change after the
    // GUI processes the queued ClientModel signal.
    const auto notify = [](int start, int height, int target, bool replay) {
        std::thread worker([=] { uiInterface.NotifyCheckpointHeaderProgress(start, height, target, replay); });
        worker.join();
    };
    notify(0, 0, 37500000, false);
    QCOMPARE(label->text(), QString("Connecting to peers..."));
    QTRY_COMPARE(label->text(), QString("Presyncing headers... 0 / 37,500,000 (0.0%)"));
    notify(0, 31801497, 37500000, false);
    QTRY_COMPARE(label->text(), QString("Presyncing headers... 31,801,497 / 37,500,000 (84.8%)"));
    QVERIFY(!label->isHidden());
    QVERIFY(!bar->isHidden());
    QCOMPARE(bar->value(), 848039920);

    // Normal header/block notifications must not overwrite the active phase.
    gui.setNumBlocks(100, QDateTime::currentDateTime(), 0.5, true);
    gui.setNumBlocks(100, QDateTime::currentDateTime(), 0.5, false);
    QVERIFY(label->text().startsWith("Presyncing headers..."));

    notify(0, 0, 37500000, true);
    QTRY_COMPARE(label->text(), QString("Replaying headers... 0 / 37,500,000 (0.0%)"));
    notify(0, 17601497, 37500000, true);
    QTRY_COMPARE(label->text(), QString("Replaying headers... 17,601,497 / 37,500,000 (46.9%)"));
    QCOMPARE(bar->value(), 469373253);

    // A nonzero checkpoint start (including resumed replay) uses the actual range.
    notify(10000000, 23750000, 37500000, true);
    QTRY_COMPARE(label->text(), QString("Replaying headers... 23,750,000 / 37,500,000 (50.0%)"));
    QCOMPARE(bar->value(), 500000000);
    notify(10000000, 37500000, 37500000, true);
    QTRY_COMPARE(bar->value(), 1000000000);

    // Completion and abandoned sessions both restore the ordinary sync UI.
    notify(0, 0, 0, false);
    QTRY_COMPARE(label->text(), QString("Connecting to peers..."));
    notify(0, 100, 200, false);
    QTRY_VERIFY(label->text().startsWith("Presyncing headers..."));
    notify(0, 0, 0, false);
    QTRY_COMPARE(label->text(), QString("Connecting to peers..."));
    gui.setNumBlocks(100, QDateTime::currentDateTime(), 1.0, false);
    QVERIFY(bar->isHidden());
    gui.setClientModel(nullptr);
    QLocale::setDefault(oldLocale);
}
