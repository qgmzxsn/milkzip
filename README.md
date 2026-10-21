# MilkZip

离线打开、浏览和解压本机压缩包的 Windows 桌面程序。文件和密码都在本机处理，无需账号，不上传文件，也不会运行解压出的程序。

当前版本：[**v0.6.0**](https://github.com/qgmzxsn/milkzip/releases/tag/v0.6.0)。

[**下载 Windows 64 位便携版**](https://github.com/qgmzxsn/milkzip/releases/download/v0.6.0/milkzip-0.6.0-windows-x64-portable.zip) · [校验文件](https://github.com/qgmzxsn/milkzip/releases/download/v0.6.0/SHA256SUMS.txt)

完整解压 ZIP 后运行 `milkzip.exe`，保留旁边所有 DLL、platforms 和许可文件。无需另装 Qt 或 7-Zip。v0.3.0 保留在历史发行版中。

![Windows 实际界面](docs/screenshots/main-window.png)

## v0.6.0 格式兼容改进

- 按内容识别已支持格式；`.txt`、`.bin`、无后缀和错误后缀的压缩包无需改名。普通文本保持为普通文件。
- 内置 LZ4 1.10.0 解码，读取标准帧、旧版帧、串联帧、可跳过帧及 tar.lz4；支持校验、取消和实际输出量限制。无需另装 LZ4 程序。
- 接通 7-Zip 已有的独立 LZMA 和 Unix compress（`.Z`）处理器。
- 内层扫描使用同一套内容识别，媒体附加包的探测范围与单独打开一致。附加包搜索读取开头 8 MiB 和末尾 32 MiB；ZIP 还查找末尾 1 MiB 中的目录结束记录。归档起点完全落在这些范围外的载体仍可能无法识别。
- 自动递归保留 Office/OpenDocument 文档、EPUB、JAR/APK 等成品容器；显式打开这些文件仍可解压。无后缀的压缩流没有原始文件名时，输出名使用 `.out`，以免与内层源文件冲突。
- “全部分支”模式下，损坏、缺卷、暂不支持或缺少密码的内层包保留，并继续处理其他独立分支。部分成功会明确提示；“工具 → 上次解压结果”可查看清单。主动取消、路径安全错误、引擎错误和累计资源上限仍立即停止。
- C API 不改变选项结构；新增返回值 `ARCHIVER_ERR_PARTIAL`（13）。只有混合成功与失败的分支才返回部分成功；单个失败仍保留原错误码。

## 解压

- ZIP、7z、RAR/RAR5、tar，以及 tar.gz/bz2/xz/zst。
- 独立 GZ、BZ2、XZ、Zstandard 流；CAB、ISO、UDF、WIM/SWM、ARJ、LZH/LHA、CPIO、AR/DEB、RPM、DMG、XAR 交给内置 7-Zip 对应处理器读取。具体变体、编码方式和路径仍受引擎及安全规则限制，不承诺任意文件都能打开。
- `.7z.001`、`.zip.001`、`.001`、`.partN.rar`、`.rNN`、`.zNN` 等分卷；选择后续卷会寻找首卷。各卷需放在同一文件夹中，缺卷时显示所缺文件名。
- 图片／视频改后缀伪装的 ZIP（含 ZIP64，可带媒体前缀或尾随数据）、7z、RAR，以及附加在媒体尾部的压缩包。可打开 MP4/M4V/MOV/MKV/AVI/WebM、JPG/PNG/GIF/BMP/WebP 等载体。**不包括像素隐写、专有加密隐藏算法或从普通视频中凭空恢复文件。**
- 自解压 `.exe`／`.sfx`（ZIP/7z/RAR SFX）以及 `.zipsc` 等自定义后缀按内容识别其中的归档；程序只读取，**不会运行 exe**。
- 默认自动解开内层包直到没有压缩包（最多 32 层）。多个内层包各自进入 `原包名.unpacked/`，避免同名内容互相覆盖。
- 可只解一层、限制层数、逐层选择继续／停止，以及保留中间包。原始输入始终保留；分卷输入保留整组。
- 内层需要新密码时，先试密码本及本次已验证密码，再弹窗；输入后在当前层继续，无需重新解开外层。

嵌套解压累计输出可选 4、16、64 GiB，累计最多 10 万个条目，压缩比超过限制会停止。大小不明确的流也检查实际写出量。普通单次解压最多 64 GiB，打开的包最多 100 万条目。同名文件不覆盖；密码取消、缺卷或其他错误时，已完成的内容可能保留，请检查后选择新的空目录重试。Windows 暂不创建压缩包中的符号链接。

## 本地密码本

工具栏或“工具 → 密码本”可添加、修改、删除、排序，也可导入 UTF-8 文本（每行一个密码，保留前后空格，忽略空行和重复项）。点击“保存”才生效。

最多保存 64 条，每条最多 1024 UTF-8 字节。使用 **Windows 凭据管理器**，每条密码单独保存；原有 0.3.0 密码本会在下次保存时迁移。密码保存在本机当前 Windows 用户的凭据管理器中，**不在程序文件夹内**；拷走程序文件夹不会带走密码。先按顺序快速验证候选密码，全部不匹配后才询问。解压密码输入框以明文显示；弹窗中勾选保存的密码仅在验证成功后写入。密码不写入日志或命令行。不进行密码破解。

其他系统可编译核心与 Qt 界面，但尚不提供持久密码本，也没有正式发布包。

## 界面

参考 7-Zip 的菜单、图标工具栏、地址栏和详细文件列表：双击目录或按 Enter 进入目录，Alt+↑ 返回，点击表头排序，Ctrl+O 打开，Ctrl+A 全选。**在文件列表中双击压缩包或伪装文件会直接解压到同目录的同名文件夹**（已存在时自动加序号），默认嵌套解压到底，只有需要新密码时才弹窗；工具栏“打开”仍用于浏览压缩包内容。在压缩包内容列表中双击内层压缩包，也会直接开始同样的默认嵌套解压。工具栏提供添加、解压、测试、打开、信息和密码本。解压与测试在后台运行；解压支持进度和取消。“添加”目前仅创建无压缩 ZIP，不能创建 RAR。

## 构建与验证

构建环境需要联网取得依赖；打包后的应用运行无需网络，也不要求安装 Qt 或 7-Zip。

```sh
sh scripts/fetch-7zip.sh
# 按平台编译独立 7-Zip Format7zF 动态库，并安装 Qt 6 Widgets/Concurrent/Test。
cmake -S . -B build -DSEVENZIP_SRC="$PWD/third_party/7zip/src" \
  -DCMAKE_PREFIX_PATH=/path/to/Qt \
  -DARCHIVER_TEST_ENGINE=/path/to/7z.dll-or-7z.so \
  -DARCHIVER_TEST_7Z=/path/to/7z
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows 可用 `scripts/build-windows-native.ps1`（MinGW Qt）；PR 的 Windows 工作流使用 MSVC 构建真实引擎和界面，运行核心／GUI／凭据迁移测试，再上传便携包和实际截图。工作流产物用于测试；日常使用请从上面的 Release 下载。本版本未经代码签名。

命令行默认保留兼容模式；`archiver-cli nested 输入包 输出目录 --all` 启用多分支自动解压，`--depth 1` 只解一层，`--keep` 保留中间包。C API 的新流程见 `archiver_extract_nested_ex`。

## 许可证

项目原创代码为 MIT。7-Zip 作为独立 DLL／动态库运行时加载，适用 LGPL 和其第三方许可；RAR 仅用于解压。Qt 与运行时许可随包提供，详见 `THIRD-PARTY-NOTICES.md` 和 `LICENSES/`。

项目图标（`assets/milkzip-mascot.jpg` 及生成的 `assets/milkzip.ico`）由维护者提供，未单独声明开源许可。
