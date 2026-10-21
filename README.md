# C盘清理工具开发基线

基于WinDirStat保留空间分析、大文件识别和重复检测，计划新增垃圾清理及安全保护。

- 固定上游：`49d81479c034cd7dd2cc03bd007a1d41d779840f`，源码版本字段为2.10.0，不等同于已验证正式发行版。
- 当前状态：中文界面与柔和平面树图已实现，云端Release x64编译、双语合成扫描及本机合成数据GUI检查通过；高DPI等扩展验证待补，垃圾清理模块尚未开发。本地未安装大型工具链。
- 来源和许可证：[docs/UPSTREAM.md](docs/UPSTREAM.md)
- 环境、构建阻塞和后续验证：[docs/BUILD-BASELINE.md](docs/BUILD-BASELINE.md)
- 实施方案、阶段和验收：[docs/pm-20261021-cleanup-ac.md](docs/pm-20261021-cleanup-ac.md)
- 工作记录：[WORKLOG.md](WORKLOG.md)
- 云端构建与首次公开操作：[docs/CLOUD-BUILD.md](docs/CLOUD-BUILD.md)
- 公开前检查范围及限制：[docs/PUBLIC-REVIEW.md](docs/PUBLIC-REVIEW.md)
- 首次构建结果与本机只读GUI步骤：[docs/FIRST-CLOUD-BUILD.md](docs/FIRST-CLOUD-BUILD.md)
- 当前中文与树图修改、构建及验收：[docs/UI-REFINEMENT.md](docs/UI-REFINEMENT.md)

不对用户真实C盘和回收站运行破坏性测试；所有文件操作验证须限定在合成数据或隔离虚拟磁盘。

以下保留上游README内容。其兼容性和功能说明是上游声明，不代表本项目本轮运行验证结果。

---

# WinDirStat - Windows Directory Statistics

## Description

WinDirStat is a disk usage analyzer and cleanup assistant for Microsoft Windows. Scan drives, folders, network shares, or connected MTP devices, then explore disk usage through sortable file lists, file-type statistics, and interactive treemaps, sunbursts, or flame graphs.

Find large or duplicate files, inspect permissions, estimate storage costs, export reports, and run cleanup or deleted-file recovery tools. See the [website](https://windirstat.net/) for an overview and the [user guide](https://github.com/windirstat/windirstat/wiki) for usage details.

### Major features

* Scan multiple drives or folders together, with pause/resume, refresh, and accelerated local NTFS scanning when elevated
* Explore largest files, file types, logical versus physical size, hardlinks, and free/unknown space with configurable visualizations
* Search scanned results by name, size, file/folder type, or owner; filter scans by path, name, size, or age, with regular-expression support
* Detect duplicates using configurable hashes and cloud-file safeguards; optional sparse hashing labels sampled matches as **Probable duplicates**
* Inspect file and folder permission entries and estimate storage tier costs by file modification age
* Watch file-system changes, save/load scans as CSV or JSON, and export scan, duplicate, or permission reports from the command line
* [Recover deleted files](https://github.com/windirstat/windirstat/wiki/Recovering-Files) from local NTFS, exFAT, and FAT drives with administrator access; recovery depends on surviving metadata and file data
* Open, move, or delete files; run Windows cleanup tools, NTFS compression, hardlink deduplication, and custom cleanup commands
* Customize layouts, columns, colors, fonts, and toolbar size, with dark mode, portable settings, translations, and Explorer integration

For changes in recent versions, please check out [the change log](CHANGELOG.md).

### Getting started

1. Choose drives or **Individual Folders** in **Select Drives**. Use the add-folder button to scan several folders together. Connected MTP devices appear under **Individual Drives**.
2. Enable **Scan for duplicate files** only when needed; hashing adds file reads. For accelerated full-drive NTFS scans, use **File > Run Elevated**.
3. Sort **All Files** or open **Largest Files**, select an item to locate it in the visualization, and use `Ctrl+F` to search. Right-click column headers to choose details and use **View** to switch graphs.
4. Review items in Explorer before cleanup, then refresh affected results. Use **File > Save Results To CSV/JSON** to keep a scan snapshot.

For scripting, use `/SaveTo`, `/SaveDupesTo`, or `/SavePermsTo` followed by an output filename and scan targets. Save modes write a report and exit; `/LoadFrom` reopens a saved scan. See [command-line examples and export formats](https://github.com/windirstat/windirstat/wiki/Command-Line-and-CSV).

### Installation

The recommended way to install WinDirStat is with a package manager, which also makes future updates easier:

* Install from the [Microsoft Store](https://apps.microsoft.com/detail/9ph1gl95p3wf) for Store-managed installation and updates
* Install with `winget install -e --id WinDirStat.WinDirStat` (or use `winget upgrade` later)
* Install with `choco install windirstat` (or use `choco upgrade windirstat` later)
* Install with `scoop install extras/windirstat` (requires `scoop bucket add extras`)

If you prefer a manual installer, need a portable archive, or want to browse older versions and beta builds, use the [GitHub releases page](https://github.com/windirstat/windirstat/releases/). If you are not sure which file to choose, download the **64-bit MSI installer**.

| Download | Best for | What is inside |
| --- | --- | --- |
| [Microsoft Store app](https://apps.microsoft.com/detail/9ph1gl95p3wf) | Users who want one-click installation and Store-managed updates | Installs WinDirStat through the Microsoft Store app experience on supported Windows systems. |
| [WinDirStat-x64.msi](https://github.com/windirstat/windirstat/releases/latest/download/WinDirStat-x64.msi) | Most users on modern Intel or AMD 64-bit Windows PCs | Standard Windows installer for 64-bit systems. Adds WinDirStat to the Start menu and installs it like a normal app. |
| [WinDirStat-arm64.msi](https://github.com/windirstat/windirstat/releases/latest/download/WinDirStat-arm64.msi) | Windows on ARM devices, including newer Surface devices and other Snapdragon-based laptops | Standard Windows installer built for ARM64 Windows. |
| [WinDirStat-x86.msi](https://github.com/windirstat/windirstat/releases/latest/download/WinDirStat-x86.msi) | Older 32-bit Windows installations | Standard Windows installer for 32-bit systems. |
| [MSIX bundle](https://github.com/windirstat/windirstat/releases/latest) | Windows App Installer or Store-style deployment across different CPU types | If the release includes an `.msixbundle` asset, it can contain packages for multiple CPU types and Windows chooses the right package for your device. |
| [WinDirStat.zip](https://github.com/windirstat/windirstat/releases/latest/download/WinDirStat.zip) | Portable use, testing, or running without an installer | Zip archive containing the WinDirStat executables. Extract it first, then run the executable for your CPU type. |
| [WinDirStat.7z](https://github.com/windirstat/windirstat/releases/latest/download/WinDirStat.7z) | Portable use when you already have 7-Zip installed | Same kind of portable executable archive as the zip file, usually with a smaller download size. |

## Copyright / Licenses

* Copyright © WinDirStat Team ([windirstat.net](https://windirstat.net/))

The application itself is distributed under the terms of the [GPL v3 or later](windirstat/res/license.txt), but parts of the source code are also available under more lenient license terms.

The logo and all derivatives are available under the terms of the Creative
Commons license [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/).

## Compatibility

WinDirStat 2.x has been developed for and tested on the following operating systems. It may work on older or newer operating systems, but those systems are not supported.

* Windows 7
* Windows 8
* Windows 8.1
* Windows 10
* Windows 11
* Windows Server 2008 R2
* Windows Server 2012
* Windows Server 2012 R2
* Windows Server 2016
* Windows Server 2019
* Windows Server 2022
* Windows Server 2025

## Resources

* A [website](https://windirstat.net/)
* The [user guide and reference](https://github.com/windirstat/windirstat/wiki)
* A [blog](https://blog.windirstat.net/)
* Twitter/X as [@windirstat](https://x.com/windirstat)
* SubReddit [r/WinDirStat](https://www.reddit.com/r/WinDirStat/)

Find a more up-to-date list of resources on the website and the blog at any point in time.

## Official Downloads and Malware Warning

WinDirStat's popularity has led to unofficial websites that copy the project's name, branding, or downloads. These sites are not operated by the WinDirStat team, may offer outdated or modified files, and may expose users to malware.

For your safety, install WinDirStat only through the Microsoft Store link and package managers listed above, the official [GitHub releases](https://github.com/windirstat/windirstat/releases/), or links from [windirstat.net](https://windirstat.net/). The team reports impersonation sites when possible, but takedowns are not always successful.

## Building

WinDirStat can be built with Visual Studio 2022 or later. A Visual Studio solution file can be loaded from `windirstat.sln`.

## Contributors

You can contribute by responding to issues, [developing](https://github.com/windirstat/windirstat/wiki/Developers) source code, or developing [translations](https://github.com/windirstat/windirstat/wiki/Contribute-Translation).

Thank you to everyone who has helped shape WinDirStat over the years.

<a href="https://github.com/windirstat/windirstat/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=windirstat/windirstat&max=1000" alt="WinDirStat contributor tiles" />
</a>

For additional historical contributors, testers, and translators, please check out [the contributors page](CONTRIBUTORS.md).

## Logo

![WinDirStat logo](windirstat/logos/logo_256px.png)
