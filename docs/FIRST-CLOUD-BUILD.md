# 首次GitHub构建验证

日期：2026-10-22，Asia/Shanghai。

## 仓库与公开范围

- 公开Fork：https://github.com/moshilan/windirstat-disk-cleaner
- 官方父仓库：windirstat/windirstat，Fork关系已通过API确认。
- 默认分支：codex/public-cloud-build。
- 实际推送与编译的审核提交：ab323d72bf3dad95c4c558fecdec87c8fd3da923。
- 只显式推送上述分支，关闭跟随标签；旧开发分支和其他本地分支、标签未推送。远程另有服务器Fork继承的官方master，内容为固定上游49d81479，不是上传的本机分支。
- 用户GitHub登录身份已核对；凭据只由已有Git凭据管理器提供，未写入项目或构建参数。没有发布Secrets、PAT文件或个人数据。

## 实际构建

运行：https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39642821395

| 项目 | 结果 |
| --- | --- |
| Workflow / Attempt | Build portable x64 / 1 |
| 结论 | success，首次通过，无需修复或重试 |
| Job时间 | 2026-10-22 01:19:56至01:22:40，UTC+08:00 |
| Runner | 标准GitHub-hosted windows-2025-vs2026，未使用larger或self-hosted |
| 镜像 | 20261018.275.1 |
| MSVC | 14.51.36231 |
| MSBuild | 18.11.0.47511 |
| Windows SDK | 10.0.26100.0 |
| 目标 | Release x64 |
| 编译日志 | Build succeeded，0 Warning(s)，0 Error(s)；编译耗时00:02:09.55 |
| EXE版本 | 2.10.0.2175，产品版本2.10.0；构建号来自云端可达Git历史计数 |

xxHash下载校验通过，云端锁文件与本地审核锁文件一致。上游预构建造成的规范化修改仅发生在Runner工作目录，记录中保留摘要和构建后依赖哈希，没有回写本机源码。没有修改工程或工作流来解决失败，因为首次即成功。

## 合成只读扫描

程序实际在Runner中运行，只扫描本次生成的独立合成目录。验证四个文件的逻辑大小，总计1179660字节；两个65536字节文件归为同一重复组；1048576字节文件为唯一最大样本，另有12字节中文名称样本。扫描前后输入文件名、长度和SHA256一致。

记录为synthetic-smoke.json：status=passed、duplicate_groups=1、input_unchanged=true。未扫描真实个人目录、执行文件清理、删除或回收站操作。此结果验证CLI导出路径，不等同于已完成本机GUI人工验收。

## Artifact及本地校验

| Artifact | 下载位置 | ZIP大小 | 到期时间（UTC+08:00） |
| --- | --- | ---: | --- |
| 便携EXE与许可证 | https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39642821395/artifacts/12365617334 | 1578287字节 | 2026-10-29 01:22:33 |
| 构建记录 | https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39642821395/artifacts/12365717260 | 6069字节 | 2026-10-29 01:22:35 |

下载需要GitHub登录及仓库读取权限，过期后可从同一工作流重新手动构建，但新产物哈希可能不同。

两个ZIP已下载并与GitHub提供的Artifact摘要匹配。便携包只含EXE、SHA256SUMS.txt及许可证；构建记录只含云端构建与合成数据证据。

- 便携ZIP SHA256：412559845cd559fe32062cd1035bdeb1122c13bfb29521000b7324f3f1cf01ed
- 记录ZIP SHA256：c644405e21cc69e17ff7ebbcd613d0c6cd8a692b78282234e418205e52fe5063
- WinDirStat.exe大小：3121664字节。
- WinDirStat.exe SHA256：4643ace969d9e211db3e2bf62edb0c6ba305ea0c7216588ecd8632d968431514。

EXE哈希与包内SHA256SUMS.txt一致，PE头Machine为AMD64（0x8664）。dumpbin列出的导入及延迟加载依赖均为Windows系统DLL，未见MSVCP或VCRUNTIME运行库DLL；不需要安装开发工具链。产物未签名，不能将成功编译当作可信发布签名。

本机下载位于项目忽略目录`.local/cloud-build/39642821395/`，便携文件在其portable子目录，记录在records子目录。原始证据、下载脚本和GitHub认证配置均未提交。

## 本机只读GUI验收步骤

以下步骤仅提供给用户，本轮没有自动执行或启动本机程序。

1. 在项目根打开PowerShell，核对EXE哈希，结果应为上述值。

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath .local/cloud-build/39642821395/portable/WinDirStat.exe
```

2. 本机便携目录已准备WinDirStat.ini，关闭自动提升、备份恢复权限及整卷快速扫描，并启用重复检测、关闭采样。不使用机器上其他已有WinDirStat配置。该INI仅在.local中，不属于云端Artifact。
3. 普通权限启动，并只传入已校验的合成目录；不选择C盘或用户根目录。

```powershell
$sampleRoot = (Resolve-Path -LiteralPath .local/fixtures/baseline-smoke).Path
& ./.local/cloud-build/39642821395/portable/WinDirStat.exe $sampleRoot
```

4. 检查目录逻辑总大小1179660字节；alpha为65548字节，beta为1114112字节。大文件首位为beta/large-unique.bin，1048576字节。重复列表应只有duplicate-a.bin和duplicate-b.bin这一组。检查中文文件名和图形定位。
5. 不点击删除、清空、去重、压缩、恢复或自定义命令。关闭程序后核对样本哈希是否与`.local/evidence/baseline-fixture-manifest.json`一致。若系统拦截未签名程序，先核对来源和哈希，不关闭防护绕过。

本机合成大文件内容与云端生成方式不同，但文件大小、唯一性和重复组预期相同；两套哈希各自独立保存，不能混用。

## 遗留项与交付边界

- 本机GUI人工验收、旧系统兼容、大规模目录性能和真实用户场景尚未验证。
- 本轮未开发垃圾清理模块，未修改上游业务源码，未安装本地大型工具链或启用付费服务。
- 本轮后续结果文档与工作日志只创建本地checkpoint，不额外推送；远程构建来源仍是已审核的ab323d7，避免混淆产物对应源码。
