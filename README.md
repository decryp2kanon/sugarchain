# Sugarchain Komorebi — Core31 branding baseline

This branch rebrands the application and executable names from Bitcoin Core
v31.1. It does **not** implement the Sugarchain network: network parameters,
consensus, units, wire identities, and existing data/configuration paths remain
at the Bitcoin baseline. Use an isolated regtest data directory when evaluating
this port. Do not use an existing Sugarchain wallet or data directory.

User executables use the `sugarchain` prefix, including the `sugarchain` launcher,
`sugarchaind`, `sugarchain-cli`, `sugarchain-qt`, `sugarchain-tx`,
`sugarchain-wallet`, `sugarchain-util`, and IPC `sugarchain-node`/`sugarchain-gui`.
Internal CMake target names and test/benchmark executable names remain unchanged.
Upstream links below are retained as upstream references, not new support endpoints.

Build and run
-------------

```sh
cmake -S . -B build -DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=/usr/local
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
build/bin/sugarchain --help
```

See `doc/build-*.md` for platform dependencies and `doc/multiprocess.md` for IPC.
The source repository is https://github.com/decryp2kanon/sugarchain.
No Komorebi binary release or replacement support/security endpoint is announced here.

License
-------

Released under the MIT license; see [COPYING](COPYING). The original Bitcoin Core
copyright and upstream attribution are retained.

Upstream Bitcoin Core development reference
------------------------------------------

The following workflow and service links describe upstream Bitcoin Core.

Development Process
-------------------

The `master` branch is regularly built (see `doc/build-*.md` for instructions) and tested, but it is not guaranteed to be
completely stable. [Tags](https://github.com/bitcoin/bitcoin/tags) are created
regularly from release branches to indicate new official, stable release versions of Bitcoin Core.

The https://github.com/bitcoin-core/gui repository is used exclusively for the
development of the GUI. Its master branch is identical in all monotree
repositories. Release branches and tags do not exist, so please do not fork
that repository unless it is for development reasons.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md)
and useful hints for developers can be found in [doc/developer-notes.md](doc/developer-notes.md).

Testing
-------

Testing and code review is the bottleneck for development; we get more pull
requests than we can review and test on short notice. Please be patient and help out by testing
other people's pull requests, and remember this is a security-critical project where any mistake might cost people
lots of money.

### Automated Testing

Developers are strongly encouraged to write [unit tests](src/test/README.md) for new code, and to
submit new unit tests for old code. Unit tests can be compiled and run
(assuming they weren't disabled during the generation of the build system) with: `ctest`. Further details on running
and extending unit tests can be found in [/src/test/README.md](/src/test/README.md).

There are also [regression and integration tests](/test), written
in Python.
These tests can be run (if the [test dependencies](/test) are installed) with: `build/test/functional/test_runner.py`
(assuming `build` is your build directory).

The CI (Continuous Integration) systems make sure that every pull request is tested on Windows, Linux, and macOS.
The CI must pass on all commits before merge to avoid unrelated CI failures on new pull requests.

### Manual Quality Assurance (QA) Testing

Changes should be tested by somebody other than the developer who wrote the
code. This is especially important for large or high-risk changes. It is useful
to add a test plan to the pull request description if testing the changes is
not straightforward.

Translations
------------

Changes to translations as well as new translations can be submitted to
[Bitcoin Core's Transifex page](https://explore.transifex.com/bitcoin/bitcoin/).

Translations are periodically pulled from Transifex and merged into the git repository. See the
[translation process](doc/translation_process.md) for details on how this works.

**Important**: We do not accept translation changes as GitHub pull requests because the next
pull from Transifex would automatically overwrite them again.
