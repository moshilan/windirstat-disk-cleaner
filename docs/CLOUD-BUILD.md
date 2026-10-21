# 手动云端构建与首次公开操作

日期：2026-10-22。工作流配置及首次远程验证已完成，Release x64构建、合成只读扫描和产物校验通过。实际运行记录见FIRST-CLOUD-BUILD.md。

## 构建配置

- 工作流：`.github/workflows/build-portable.yml`，名称Build portable x64，只支持workflow_dispatch。
- 使用标准`windows-2025-vs2026`，单任务，超时30分钟，不使用larger或self-hosted Runner。
- checkout和upload-artifact固定到官方Action提交；checkout不保留认证信息，令牌仅contents: read。
- 构建的是手动选择分支对应的项目提交，不会永远检出旧上游代码；必须包含固定上游提交49d81479c034cd7dd2cc03bd007a1d41d779840f作为祖先。
- 检查VS2026、v145、ATL及SDK 10.0.26100.0，缺失即报错，不在线安装工具或降级工程设置。
- xxHash锁文件：`.github/dependencies/xxhash.json`，v0.8.3对应commit e626a72bc2321cd320e953a0ccf1584cad60f363，两个源文件和LICENSE均验证SHA256。
- 直接构建原windirstat.sln的Release x64，不执行根build.cmd、签名、MSI、MSIX、WinGet或Chocolatey发布。
- 上游预构建会规范化源码、依赖头文件和语言资源。只在临时Runner发生，不回写仓库；记录源提交、xxHash下载哈希和构建后依赖哈希。固定依赖不等于承诺EXE逐字节可重复，Runner镜像仍会更新。
- 原有发布工作流保留，但本次不创建Release，不配置其发布Secrets，不会由手动构建触发。

## 合成测试与下载内容

成功编译后，测试脚本在RUNNER_TEMP新建独立目录，生成两个65536字节的相同文件、一个1048576字节的唯一文件和一个12字节中文名称文件。只传入该输入目录，通过/SaveTo和/SaveDupesTo导出报告，核对大小、重复分组和扫描前后哈希。

不调用任何删除、清空、压缩、恢复、自定义命令或上游全套测试。测试配置使用独立便携INI、关闭自动提升和快速整卷扫描。只读扫描也会写测试报告与便携设置，但不修改输入数据。

合成扫描不能代替本机GUI的人工验收。测试超时或失败时任务失败，不上传成功的便携包；构建记录仍尽量上传，供排查。没有在本机运行尚未构建出的应用。

| Artifact | 内容 | 保留 |
| --- | --- | --- |
| windirstat-portable-x64-提交号 | WinDirStat.exe、SHA256SUMS.txt、上游及xxHash许可证 | 7天 |
| build-records-提交号 | 源提交与工具版本、xxHash锁文件、MSBuild文本日志、构建变更摘要、依赖列表、合成验证摘要 | 7天 |

不上传整个工作目录、Git目录、PDB、个人配置、真实扫描结果或环境变量全集。合成摘要只含生成样本的文件名和哈希；原始扫描报告留在临时Runner，不作为Artifact上传。

## 首次创建公开仓库与推送

以下为首次配置操作说明；本项目已按用户授权完成一轮，后续核对实际状态时以FIRST-CLOUD-BUILD.md为准，不重复创建Fork或添加origin。

1. 登录GitHub，在官方https://github.com/windirstat/windirstat 创建自己的公开Fork。Fork已具有上游历史，适合当前本地浅仓库。不要把本机整个目录通过网页拖拽上传。
2. 本地确认处于`codex/public-cloud-build`且工作区干净，阅读PUBLIC-REVIEW.md。原开发分支仅保留在本地，不推送。
3. 将下面地址替换成实际Fork地址，再逐条执行。origin只用于自己的仓库，保留upstream官方来源。

```powershell
git rev-parse --show-toplevel
git status --short --branch
git remote add origin https://github.com/YOUR_ACCOUNT/YOUR_REPO.git
git push -u origin codex/public-cloud-build
```

仅推送指定分支，不使用--all、--mirror、--force或无选择地推送标签。不要将旧开发分支合并进公开准备分支，否则会重新带入旧机器路径历史。若origin已存在，先核对URL，不能盲目重复添加；若浅仓库推送受限，停止后先核对Fork关系和基线对象，不强推。

4. 在Fork设置中把默认分支切换为`codex/public-cloud-build`。手动工作流必须在默认分支存在，Run workflow按钮才会显示。
5. 打开Actions，按GitHub提示启用Fork的工作流。允许使用官方checkout、upload-artifact及标准Windows Runner，令牌只需读取仓库；无需PAT、签名证书或发布Secrets。
6. 选择Build portable x64，点击Run workflow，选择该分支。首次以环境检查、编译和合成扫描实际结果为准，不把本地语法检查当作云端成功。
7. 成功后下载便携Artifact，解压并核对SHA256SUMS.txt。普通权限启动EXE，仅用项目合成目录验证GUI。未签名产物可能有系统安全提示，不应关闭系统防护来绕过来源核对。

## 账户与费用边界

使用公开仓库的标准GitHub托管Runner，不申请larger Runner，不使用矩阵、自动push触发或定时任务。计算使用按GitHub公开标准Runner政策处理；Artifact设置7天且不上传PDB以控制存储。首次已确认仓库公开、Actions启用且标准Runner运行成功，未启用任何付费服务。

## 本轮已验证与尚未验证

已验证：YAML解析、PowerShell语法、所有内联PowerShell语法、xxHash真实文件哈希及错误哈希拒绝路径、公开准备分支范围与提交邮箱、上游业务源码未变。

首次远程验证另已确认：工作流调度、Runner预装组件、MSBuild编译、EXE依赖和真实程序的合成扫描。尚未验证本机GUI人工操作、真实用户数据场景和旧系统兼容；未安装本地大型工具链，没有真实清理或删除。

2026-10-22后续中文与树图阶段已修改UI源码，并再次通过同一工作流编译与双语合成扫描；本机合成数据GUI操作已检查，范围及限制见UI-REFINEMENT.md。上述源码未变与GUI未验收结论仅描述首次构建阶段。
