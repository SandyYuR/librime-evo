# Android ARM64 引擎产物

本目录从当前 `librime-evo` 提交和固定的 Android 依赖源码构建两个同源产物：

- `librime-evo-<提交>-arm64-v8a.so`：自包含的 Rime 引擎，可从中州鹅“Rime引擎变体”页面导入；
- `librime-evo-engine-<提交>-arm64-v8a.apk`：实现 `org.rime.plugin.MANIFEST` 协议的引擎提供 APK。

两者使用上游拆分的 `amzxyz/librime-evo-lua`，将其固定的 LuaJIT 静态编入引擎。LuaJIT 的 VM 生成工具在构建主机运行，运行时由 Android NDK 交叉编译。日志使用上游选定的 spdlog。Lua 插件、LuaJIT、spdlog、octagram、LevelDB predict 及其他依赖均按 `sources.lock.json` 的提交和 SHA-256 获取源码，产物附带各自的许可证及 NDK NOTICE。Android 构建直接读取锁文件和 Git 中记录的模块提交，不需要在本地初始化 submodule。

LuaJIT 支持 Lua 5.1 和部分 Lua 5.2 扩展。此前外置引擎使用 Lua 5.4，依赖 Lua 5.4 专用语法、API 或预编译字节码的方案需要分别验证。优先以源码形式分发 Lua 脚本。

`org.rime.plugin.MANIFEST` 是中州鹅兼容的社区提供协议，不是 Rime 上游标准。普通 Android SDK 中的静态 `librime.a` 不能直接导入；本流程完成全部静态依赖的最终共享库链接，并把动态导出限制为 `rime_get_api`。

## GitHub Actions

`.github/workflows/android-engine.yml` 在 `main`、`build/android-engine-*`、Pull Request 及手动触发时构建 ARM64。Actions 页面提供两个下载项：

- `librime-evo-arm64-so-*`
- `librime-evo-arm64-apk-*`

两项都包含各自的 `SHA256SUMS`、`engine-manifest.json` 和第三方许可证；APK 项另含 `apk-manifest.json`，用于核对签名证书和封装后的文件摘要。下载并解压任一 artifact 后，都可以在其根目录单独运行 `sha256sum -c SHA256SUMS`，不依赖另一个 artifact。

若仓库配置以下四个 Actions Secrets，`main` 分支的非 PR 构建使用固定发行证书并标记为 `release`：

- `ANDROID_ENGINE_KEYSTORE_BASE64`
- `ANDROID_ENGINE_KEYSTORE_PASSWORD`
- `ANDROID_ENGINE_KEY_ALIAS`
- `ANDROID_ENGINE_KEY_PASSWORD`

功能分支、Pull Request，或未配置 Secrets 的 `main` 构建只生成本次运行专用的测试证书并标记为 `test`，不会接触发行身份。测试 APK 可以安装和授权，但下一次运行的证书会变化，升级时需要卸载旧提供包并重新授权；不得把它当作稳定发行签名。Actions artifacts 保留 90 天；长期保存仍应使用不可变标签与 Release assets。

## 本地构建

需要 Python 3.12+、GNU Make、原生 C/C++ 编译器、JDK 17、Android SDK platform 36、build-tools 36.1.0、NDK `28.0.13004108` 和 CMake `3.31.6`：

```sh
export ANDROID_NDK_ROOT="$ANDROID_SDK_ROOT/ndk/28.0.13004108"
export CMAKE_BIN="$ANDROID_SDK_ROOT/cmake/3.31.6/bin/cmake"
python3 downstream/android/build.py --abi arm64-v8a --work build/android --jobs 2
python3 downstream/android/package_engine.py build/android/product \
  --commit "$(git rev-parse HEAD)" --abi arm64-v8a --output dist/android-engine/so
```

APK 封装还需设置 `KEYSTORE_FILE`、`KEYSTORE_PASSWORD`、`KEY_ALIAS`、`KEY_PASSWORD`，再运行：

```sh
engine_commit="$(git rev-parse HEAD)"
engine_short="$(printf '%.8s' "$engine_commit")"
python3 downstream/android/package_plugin_apk.py \
  --engine "dist/android-engine/so/librime-evo-${engine_short}-arm64-v8a.so" \
  --engine-manifest dist/android-engine/so/engine-manifest.json \
  --output dist/android-engine/apk --channel test
```

引擎文件必须按本次提交和 ABI 精确选择。重复构建会保留旧提交命名的 `.so`，不能从输出目录任取第一个文件与最新清单配对。APK 输出目录须与引擎及其清单目录分离，不能互相包含；脚本会在写入前拒绝重叠目录，保护原有引擎和许可证。生成的 XML 与元数据固定使用 UTF-8，不依赖 Windows 的系统代码页。

`.so` 必须是 Android ARM64 ELF、16 KiB 页面兼容、无 RPATH/TEXTREL、只依赖 Android 系统库，并且只导出 `rime_get_api`。APK 将同一字节载荷放入 `lib/arm64-v8a/librime-evo.so`，开启安装时原生库解压，供宿主从 `nativeLibraryDir` 打开。
