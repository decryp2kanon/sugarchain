// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_CHECKPOINTPROGRESSTESTS_H
#define BITCOIN_QT_TEST_CHECKPOINTPROGRESSTESTS_H

#include <QObject>

class CheckpointProgressTests : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void progress();
};

#endif
