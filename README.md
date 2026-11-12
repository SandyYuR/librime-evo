# librime-evo

> **Fork notice**
>
> This repository is a personal fork of [rime/librime](https://github.com/rime/librime),
> whose upstream is [rime/librime](https://github.com/rime/librime).
>
> It is **not** the official Rime project. For the canonical source, releases and
> documentation, please refer to [rime/librime](https://github.com/rime/librime) and
> [rime.im](https://rime.im).
>
> This fork is maintained independently. Changes relative to upstream are listed in
> [Changes from upstream](#changes-from-upstream). Bug reports about unmodified
> upstream behavior should go to the upstream project.

[![Build status](https://github.com/amzxyz/librime-evo/actions/workflows/commit-ci.yml/badge.svg)](https://github.com/amzxyz/librime-evo/actions/workflows/commit-ci.yml)
[![GitHub release](https://img.shields.io/github/release/amzxyz/librime-evo.svg)](https://github.com/amzxyz/librime-evo/releases)
[![License](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

RIME: Rime Input Method Engine
===

Rime with your keystrokes.

Project home
---
Upstream: [rime.im](https://rime.im)  
This fork: [github.com/amzxyz/librime-evo](https://github.com/amzxyz/librime-evo)

Changes from upstream
---
This fork is based on [rime/librime](https://github.com/rime/librime).
The following is a non-exhaustive list of local changes:

- [ ] TODO: 增加辅助码功能
- [ ] TODO: 重构滤镜模块等

Everything not listed here should be assumed to match upstream. When in doubt,
compare against the upstream repository before reporting a bug.

License
---
[The 3-Clause BSD License](https://opensource.org/licenses/BSD-3-Clause)

This fork retains the original copyright notices. See [LICENSE](LICENSE) for details.

Features
===
  - A modular, extensible input method engine in cross-platform C++ code,
    built on top of open-source technologies
  - Covering features found in a large variety of Chinese input methods,
    either shape-based or phonetic-based
  - Rime input schema, a DSL in YAML syntax for fast trying out innovative ideas
    of input method design
  - Spelling Algebra, a mechanism to create variant spelling, especially useful
    for Chinese dialects
  - Support for chord-typing with a generic Qwerty keyboard

Install
===
Follow the instructions to build librime on platforms other than Linux:
  - [macOS](https://github.com/rime/librime/tree/master/README-mac.md)
  - [Windows](https://github.com/rime/librime/tree/master/README-windows.md)

Build dependencies
---
  - compiler with C++17 support
  - cmake>=3.12
  - libboost>=1.74
  - libglog>=0.7 (optional)
  - libleveldb
  - libmarisa
  - libyaml-cpp>=0.5
  - libgtest (optional)

Runtime dependencies
---
  - libboost
  - libglog (optional)
  - libleveldb
  - libmarisa
  - libyaml-cpp

Build and install on Linux
---
```
make
sudo make install
```

Android ARM64 artifacts
---
The [Android ARM64 engine workflow](.github/workflows/android-engine.yml) builds two
artifacts from the current commit and pinned source dependencies: a self-contained
`arm64-v8a` engine DSO and an `org.rime.plugin.MANIFEST` provider APK. See the
[Android build and verification guide](downstream/android/README.zh-CN.md) for artifact
contents, checksums, signing behavior and local reproduction commands.

Packaging status
---
[![Packaging status](https://repology.org/badge/vertical-allrepos/librime.svg?columns=4&minversion=1.10.0)](https://repology.org/project/librime/versions)

Frontends
===

Official:
  - [ibus-rime](https://github.com/rime/ibus-rime): IBus frontend for Linux
  - [Squirrel](https://github.com/rime/squirrel): frontend for macOS
  - [Weasel](https://github.com/rime/weasel): frontend for Windows

<!-- Sort in alphabetical order with :sort in vim -->
Community:
  - [ARIF](https://www.nongnu.org/arif/): frontend for Readline
  - [Hamster](https://github.com/imfuxiao/Hamster): frontend for iOS
  - [My RIME](https://github.com/LibreService/my_rime): frontend for web
  - [PIME](https://github.com/EasyIME/PIME): frontend for Windows
  - [Trime](https://github.com/osfans/trime): frontend for Android
  - [XIME](https://github.com/stackia/XIME): frontend for macOS
  - [YuyanIme](https://github.com/gurecn/YuyanIme): frontend for Android
  - [coc-rime](https://github.com/tonyfettes/coc-rime): frontend for Vim
  - [emacs-rime](https://github.com/DogLooksGood/emacs-rime): frontend for Emacs
  - [fcitx-rime](https://github.com/fcitx/fcitx-rime): Fcitx frontend for Linux
  - [fcitx5-android](https://github.com/fcitx5-android/fcitx5-android): frontend for Android
  - [fcitx5-macos](https://github.com/fcitx-contrib/fcitx5-macos): Fcitx5 frontend for macOS
  - [fcitx5-rime](https://github.com/fcitx/fcitx5-rime): Fcitx5 frontend for Linux
  - [fcitx5-ui.nvim](https://github.com/black-desk/fcitx5-ui.nvim): Fcitx5 frontend for Vim
  - [fcitx5.nvim](https://github.com/tonyfettes/fcitx5.nvim): Fcitx5 frontend for Vim
  - [pyrime](https://github.com/Freed-Wu/pyrime): frontend for Ptpython
  - [rabbit](https://github.com/amorphobia/rabbit): frontend for Windows
  - [rime.nvim](https://github.com/Freed-Wu/rime.nvim): frontend for Vim
  - [rl_custom_rime](https://github.com/Freed-Wu/rl_custom_rime): frontend for Readline
  - [tmux-rime](https://github.com/Freed-Wu/tmux-rime): frontend for Tmux
  - [zsh-rime](https://github.com/Freed-Wu/zsh-rime): frontend for Zsh

Plugins
===
  - [librime-charcode](https://github.com/rime/librime-charcode) (Deprecated) Module that
    deals with character encoding; depends on boost::locale and ICU libraries
  - [librime-legacy](https://github.com/rime/librime-legacy) (Deprecated) Legacy module with
    GPL-licensed code
  - [librime-lua](https://github.com/hchunhui/librime-lua) Lua scripting
  - [librime-octagram](https://github.com/lotem/librime-octagram) Language model
  - [librime-predict](https://github.com/rime/librime-predict) Predict next word
  - [librime-proto](https://github.com/lotem/librime-proto) IPC using CapnProto

Related works
===
  - [plum](https://github.com/rime/plum): Rime configuration (recipe) installer
  - [combo-pinyin](https://github.com/rime/home/wiki/ComboPinyin): an innovative
    chord-typing practice to input Pinyin
  - [rime-essay](https://github.com/rime/rime-essay): the preset vocabulary
  - [SCU](https://github.com/neolee/SCU): Squirrel Configuration Utilities

Credits
===
We are grateful to the makers of the following open source libraries:

  - [Boost C++ Libraries](http://www.boost.org/) (Boost Software License)
  - [google-glog](https://github.com/google/glog) (The 3-Clause BSD License)
  - [Google Test](https://github.com/google/googletest) (The 3-Clause BSD License)
  - [LevelDB](https://github.com/google/leveldb) (The 3-Clause BSD License)
  - [marisa-trie](https://github.com/s-yata/marisa-trie) (BSD 2-Clause License, LGPL 2.1)
  - [yaml-cpp](https://github.com/jbeder/yaml-cpp) (MIT License)

Contributors
===
  - [佛振](https://github.com/lotem)
  - [鄒旭](https://github.com/zouxu09)
  - [Weng Xuetian](http://csslayer.info)
  - [Chongyu Zhu](http://lembacon.com)
  - [Zhiwei Liu](https://github.com/liuzhiwei)
  - [BYVoid](http://www.byvoid.com)
  - [雪齋](https://github.com/LEOYoon-Tsaw)
  - [瑾昀](https://github.com/kunki)
  - [osfans](https://github.com/osfans)
  - [jakwings](https://github.com/jakwings)
  - [Prcuvu](https://github.com/Prcuvu)
  - [hchunhui](https://github.com/hchunhui)
  - [Qijia Liu](https://github.com/eagleoflqj)
  - [WhiredPlanck](https://github.com/WhiredPlanck)
