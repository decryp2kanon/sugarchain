# Sugarchain Yumekawa

![GitHub All
Releases](https://img.shields.io/github/downloads/sugarchain-project/sugarchain/total)

https://sugarchain.org

## The Meaning of Yumekawa

Sugarchain's first node software is called `Yumekawa (夢川)`. It can be interpreted in several ways.

- "Yume (夢)" means dream and "Kawa (川)" means river, so it can mean `Dream River` in Japanese.
- "Kawa" can also stand for "Kawaii (可愛い)". In this case, the meaning is `Dreamy Cute`.
- Yumekawa also replaces the word `Core` (e.g. Bitcoin Core), which we think sounds a bit centralized.

## License

Sugarchain Yumekawa is released under the terms of the MIT license. See
[COPYING](COPYING) for more information or see
https://opensource.org/licenses/MIT.
- Copyright (c) 2009-2010 Satoshi Nakamoto
- Copyright (c) 2009-2018 The Bitcoin Core developers
- Copyright (c) 2013-2019 Alexander Peslyak - Yespower 1.0.1
- Copyright (c) 2016-2018 The Zcash developers - DigiShieldZEC
- Copyright (c) 2018-2026 The Sugarchain Yumekawa developers

## Minimum Requirement

- CPU: 2 cores
- RAM: 4 GB (at least 4 GB [swap](https://github.com/sugarchain-project/doc/blob/master/swap.md))
- Storage: At least 20 GB of free space

Low-spec systems may still run Sugarchain, but initial synchronization and startup can take significantly longer.

## Depends on Bitcoin Core

Exactly the same as dependencies of [Bitcoin Core v0.16.3](https://github.com/bitcoin/bitcoin/tree/49e34e288005a5b144a642e197b628396f5a0765).

- Ubuntu 22.04

```
sudo apt install \
build-essential software-properties-common libtool autotools-dev automake pkg-config \
libssl-dev libevent-dev bsdmainutils libboost-all-dev \
libminiupnpc-dev libzmq3-dev libqt5gui5 libqt5core5a \
libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev \
protobuf-compiler libqrencode-dev help2man
```

<details>
<summary>Old OS</summary>

- Debian 10

```bash
sudo apt-get install -y \
software-properties-common build-essential libtool autotools-dev automake pkg-config \
libssl-dev libevent-dev bsdmainutils libboost-all-dev \
libminiupnpc-dev libzmq3-dev libqt5gui5 libqt5core5a \
libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev \
protobuf-compiler libqrencode-dev help2man
```

- PPA is *only* for Ubuntu. No `libdb4.8-dev` and `libdb4.8++-dev` packages on Debian.

- Ubuntu 16.04

```bash
sudo add-apt-repository -y ppa:bitcoin/bitcoin && \
sudo apt-get update && \
sudo apt-get install -y \
libdb4.8-dev libdb4.8++-dev \
software-properties-common build-essential libtool autotools-dev automake pkg-config \
libssl-dev libevent-dev bsdmainutils libboost-all-dev \
libminiupnpc-dev libzmq3-dev libqt5gui5 libqt5core5a \
libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev \
protobuf-compiler libqrencode-dev help2man
```

- Ubuntu 18.04+

```bash
sudo add-apt-repository -y ppa:luke-jr/bitcoincore && \
sudo apt-get update && \
sudo apt-get install -y \
libdb4.8-dev libdb4.8++-dev \
software-properties-common build-essential libtool autotools-dev automake pkg-config \
libssl-dev libevent-dev bsdmainutils libboost-all-dev \
libminiupnpc-dev libzmq3-dev libqt5gui5 libqt5core5a \
libqt5dbus5 qttools5-dev qttools5-dev-tools libprotobuf-dev \
protobuf-compiler libqrencode-dev help2man
```

</details>

## Build

-   Ubuntu 22.04.1

``` bash
./autogen.sh && \
./contrib/install_db4.sh $(pwd) && \
export BDB_PREFIX="$PWD/db4" && \
export BDB_CFLAGS="-I${BDB_PREFIX}/include" && \
export BDB_LIBS="-L${BDB_PREFIX}/lib -ldb_cxx-4.8" && \
./configure && \
printf '#include <deque>\n#include <boost/bind/bind.hpp>\nusing namespace boost::placeholders;\n' >/tmp/sugar_compat.h && \
printf '#!/bin/sh\ncase " $* " in\n  *"qt/trafficgraphwidget.cpp"*)\n    exec g++ -include /tmp/sugar_compat.h -include QPainterPath "$@"\n    ;;\n  *)\n    exec g++ -include /tmp/sugar_compat.h "$@"\n    ;;\nesac\n' >/tmp/sugar-cxx && \
chmod +x /tmp/sugar-cxx && \
make -j$(nproc) CXX=/tmp/sugar-cxx && \
strip ./src/sugarchain-cli && \
strip ./src/sugarchaind && \
strip ./src/qt/sugarchain-qt && \
strip ./src/sugarchain-tx && \
strip ./src/test/test_sugarchain && \
./src/test/test_sugarchain test_bitcoin --log_level=test_suite && \
./src/sugarchaind --version
```

-   (optional) The following files can be deleted:
    `rm -rf db4/ && rm -f db-4.8.30.NC.tar.gz`

    <details>
    <summary>Old OS</summary>

    -   Ubuntu 16.04+

    ``` bash
    ./autogen.sh && \
    ./configure && \
    make -j$(nproc) && \
    make check -j$(nproc)
    ```

    -   Debian 10+

    ``` bash
    ./autogen.sh && \
    ./contrib/install_db4.sh `pwd` && \
    export BDB_PREFIX=$PWD/db4 && \
    ./configure BDB_LIBS="-L${BDB_PREFIX}/lib -ldb_cxx-4.8" BDB_CFLAGS="-I${BDB_PREFIX}/include" && \
    make -j$(nproc) && \
    make check -j$(nproc)
    ```

    </details>

## Options after Build

-   (optional) Reduce binary size using strip (about 90% file size
    reduction)

``` bash
strip ./src/sugarchain-cli && \
strip ./src/sugarchaind && \
strip ./src/qt/sugarchain-qt && \
strip ./src/sugarchain-tx && \
strip ./src/test/test_sugarchain
```

-   (optional) After bump version on `configure.ac`, update binary docs
    (manpages) using help2man `.1` files

``` bash
make -j$(nproc) && ./contrib/devtools/gen-manpages.sh
```

-   (optional) When building for Windows or macOS, you may need the
    `--disable-shared` option.

-   (optional) Add seeds/nodes from
    [DNSSEED](https://github.com/sugarchain-project/sugarchain-seeder)\
    https://github.com/sugarchain-project/sugarchain/tree/master-v0.16.3/contrib/seeds

## Unit Test

All Sugarchain Yumekawa developers should run these unit tests. Some
updates may cause these tests to fail.

-   Test All

``` bash
./src/test/test_sugarchain test_bitcoin --log_level=test_suite
```

-   (optional) Run a specific test, e.g. `blockencodings_tests`

``` bash
./src/test/test_sugarchain test_bitcoin --log_level=test_suite --run_test=blockencodings_tests
```

-   (optional) Test QT (GUI)

``` bash
./src/qt/test/test_sugarchain-qt
```

-   (optional) Estimate full IBD time without running a complete IBD
    -   Requires a stopped, fully synced unpruned datadir and a separate
        synced RPC node.
    -   See `contrib/bench/estimate-ibd.md` for usage.

## Run

The options `-rpcuser`, `-rpcpassword`, and `-printtoconsole` are
optional. `-server=1` is required for RPC access or cpuminer when solo
mining.

-   Mainnet: debug mode: `net` for Network \> ./src/qt/sugarchain-qt
    -server=1 -rpcuser=rpcuser -rpcpassword=rpcpassword **-debug=net**
    -printtoconsole

-   Testnet \> ./src/qt/sugarchain-qt **-testnet**

-   Regtest \> ./src/qt/sugarchain-qt **-regtest**

-   Reference\
    https://en.bitcoin.it/w/index.php?title=Running_Bitcoin&oldid=66644

## CLI

- `-prunedebuglogfile`: Prune (limit) the size of `debug.log` (default).

  > ./src/qt/sugarchain-qt -prunedebuglogfile

- `-noprunedebuglogfile`: Disable `debug.log` pruning.

  > ./src/qt/sugarchain-qt -noprunedebuglogfile

- `-fast-ibd=1`: Enable Fast IBD (default). Uses checkpoint-authenticated header synchronization and parallel Yespower verification to significantly reduce initial sync time.

  > ./src/qt/sugarchain-qt -fast-ibd=1

- `-fast-ibd=0`: Disable Fast IBD. Historical headers are fully verified with Yespower.

  > ./src/qt/sugarchain-qt -fast-ibd=0

## Known Issues

-   Transaction too large:
    -   This behavior is inherited from Bitcoin Core and will hopefully
        be fixed in a future *Taproot* soft fork.
-   Slow update balance on wallet:
    -   This behavior is inherited from Bitcoin Core.
    -   Update total balance *every minute (12 blocks)* interval.
    -   A workaround at this moment.
        [source](https://github.com/sugarchain-project/sugarchain/commit/72436c90b29844cf507895df053103f9b6840776#diff-2e3836af182cfb375329c3463ffd91f8)
-   Poor performance on ARM CPUs (32/64-Bit):
    -   No ARM optimization for Yespower yet.
-   Poor performance on 32-Bit OS:
    -   No SSE2 optimization for Yespower yet.
        [source](https://github.com/sugarchain-project/sugarchain/blob/d977987a83aba115d50a9130f0d7914330d1bc75/src/crypto/yespower-1.0.1/yespower-opt.c#L59)
-   Slow startup on low memory machines:
    -   Startup can take up to some hours on 1cpu 1024ram (+swap 3GB)
        VPS.
    -   A workaround is to increase RAM to at least 2 GB.

## Release Process

-   All Sugarchain Yumekawa developers should follow the Gitian release
    process below. It is the safest way to distribute binaries.
-   Please use a Gitian release with a verified PGP signature, or
    compile it yourself.

https://gist.github.com/cryptozeny/3501c77750541208b9dd1a9e9719fc53
