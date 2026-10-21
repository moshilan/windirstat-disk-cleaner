# 构建和运行基线记录

检查日期：2026-10-21，Asia/Shanghai。源码：49d81479c034cd7dd2cc03bd007a1d41d779840f。

## 实际结果

| 检查 | 本轮结果 |
| --- | --- |
| 独立仓库 | Git根为当前独立项目根 |
| 源码版本 | 上游version.h为2.10.0，提交已固定 |
| 主机 | Windows内核10.0.26200，X64 |
| Git、PowerShell | Git 2.54.0.windows.1；PowerShell 7.6.5 |
| .NET | 仅Microsoft.NETCore.App 8.0.21运行时；无SDK，与C++构建要求不同 |
| C++工具链 | PATH未找到cl、msbuild；标准VS、BuildTools、SDK目录及安装登记未找到可用工具链；独立主机只读复核结果相同 |
| 构建尝试 | 调用下述命令时提示msbuild无法识别；没有进入MSBuild、预构建或编译阶段 |
| EXE | 源码树没有可运行EXE，本轮没有生成程序 |
| 原程序启动和GUI | 未执行；被缺失工具链阻断 |
| 空间分析、大文件、重复检测 | 原程序运行验收均未执行；不能记为通过 |
| 删除、清理和安装 | 未执行；没有安装任何组件 |

尝试的命令：

```powershell
msbuild .\windirstat\windirstat.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /m
```

本机原始证据均不提交Git：

- `.local/evidence/environment.json`
- `.local/evidence/dotnet-info.txt`
- `.local/evidence/build-attempt.txt`
- `.local/evidence/baseline-fixture-manifest.json`

以上路径相对于本项目根。构建入口失败不代表上游源码编译失败，也没有运行其他发行版替代该提交的运行验收。

## 必要组件及安装边界

实际工程指定v145、stdcpp23、Windows SDK 10.0，pch.h使用ATL；不用MFC。建议仅安装能提供v145的Visual Studio Build Tools 2026及以下组件：

1. MSBuild和MSVC x64/x86 C++编译工具、标准库及匹配的v145工具集。
2. 与所选MSVC版本匹配的ATL x64/x86。
3. 一个受该工具链支持的Windows 10或Windows 11 SDK，包含资源编译与链接所需头文件、库和工具。

这是必要组件清单，不是已执行的安装。当前不需要完整IDE、MFC、ARM工具、WiX、MSIX打包组件、签名证书或.NET SDK。安装器实际显示的下载量和占用尚未测得，不给出假定大小。

v145归属已核对微软说明：
https://learn.microsoft.com/en-us/cpp/overview/what-s-new-for-msvc?view=msvc-180

用户要求不自动安装大型工具链，因此等待是否安装上述最小组件的决定。未获确认前保持环境不变。

## 后续构建方法

- 使用固定上游标签的隔离构建副本，先记录编译器和SDK版本；不直接运行根build.cmd。
- 根build.cmd含RD、DEL、发布和签名动作；本轮未运行。第一次只构建x64 Debug可执行文件，不做Clean、安装或发布。
- 预构建的Format Sources.ps1会写源码，Compress Strings.ps1会写语言资源和生成文件；在隔离副本构建，完成后检查diff，不能把自动格式化结果合入正式源码。
- 先固定xxHash依赖，再执行上述项目构建命令。保留完整构建日志、EXE SHA256和实际产物路径。
- 若出现编译错误，先区分缺组件、SDK或编译器版本、下载失败与源码错误。不得未经证据随意降工具集或修改上游业务代码。

## 已准备的合成数据与待执行验收

数据根：项目内`.local/fixtures/baseline-smoke`。

| 文件 | 逻辑字节数 | 用途 |
| --- | ---: | --- |
| alpha/duplicate-a.bin | 65536 | 与beta中的副本内容相同 |
| beta/duplicate-b.bin | 65536 | 重复检测 |
| beta/large-unique.bin | 1048576 | 唯一最大文件 |
| alpha/中文样本.txt | 12 | 中文路径 |

另有空目录empty。预期4个文件、逻辑总大小1179660字节；alpha为65548字节，beta为1114112字节。已通过文件长度和SHA256检查合成数据；这不等同于应用扫描通过。

构建后只扫描此合成目录：

| 验收 | 操作和预期 | 本轮状态 |
| --- | --- | --- |
| 启动 | 普通权限启动固定源码编译产物，记录版本、截图和哈希 | 未执行 |
| 目录占用 | 只扫描合成数据，逻辑大小与上述合计一致；物理大小另列不混用 | 未执行 |
| 大文件 | 最大文件为beta/large-unique.bin | 未执行 |
| 重复检测 | 开启全量检测，发现一组两个65536字节文件，不将唯一大文件归入 | 未执行 |
| 展示 | 目录列表、可视化与文件定位可操作；中文名称可读 | 未执行 |
| 非破坏性 | 不点击删除、清空、去重、压缩或自定义命令；扫描前后样本哈希不变 | 待运行验证 |

取消、拒绝访问、硬链接、junction、长路径及文件变化等扩展用例在隔离数据中后续补充；本轮不运行上游全套文件系统、恢复或删除测试。

## 2026-10-22后续决定

用户确认暂不安装本地大型工具链，改用公开GitHub仓库及标准托管Runner。新增工作流构建Release x64，前面的Debug命令仅保留为历史尝试记录，不是当前推荐入口。

工作流和本地静态校验已准备；首次云端构建、云端合成扫描以及本机GUI启动仍未执行。后续操作见CLOUD-BUILD.md。

## 2026-10-22首次云端验证结果

上述本机工具链阻塞通过GitHub标准Windows Runner绕开，未安装本地工具。第一次云端Release x64编译、合成只读扫描和Artifact上传均成功，0警告、0错误，未修改上游业务源码或构建配置。本机GUI仍未启动验收，详见FIRST-CLOUD-BUILD.md。

## 2026-10-22中文与树图阶段

本轮修改语言资源及UI默认渲染参数，新增双语合成导出复验，云端构建再次通过。本机独立合成目录的空间、大文件、重复组、语言往返切换、平面视觉、定位、缩放和隐藏恢复检查通过；具体范围与未验证项见UI-REFINEMENT.md。前文未执行状态为早期基线记录。
