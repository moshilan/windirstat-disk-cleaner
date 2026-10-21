# 上游来源与版本记录

| 项目 | 记录 |
| --- | --- |
| 官方仓库 | https://github.com/windirstat/windirstat.git |
| 固定提交 | 49d81479c034cd7dd2cc03bd007a1d41d779840f |
| 提交标题 | Add Git Worktree Support For Build Revision Detection (#740) |
| 上游提交时间 | 2026-10-21T03:42:13-04:00 |
| 源码版本字段 | windirstat/version.h声明2.10.0；构建号另从Git计算 |
| 导入日期 | 2026-10-21，Asia/Shanghai |
| 公开准备分支 | codex/public-cloud-build |
| 上游基线标签 | upstream-baseline/49d81479 |
| Remote | upstream指向官方仓库；未建立本项目发布远程，未push |
| 历史完整性 | 从上一轮已核验的本地官方源码克隆，保留提交对象；当前是浅仓库，不能声称包含完整历史 |
| 许可证 | GPL-3.0-or-later；保留LICENSE.md、源码声明及第三方声明 |

基线标签只表示固定源码，不表示编译、运行或安全验收通过。后续项目提交以该上游提交为祖先，文档记录项目增量。

导入来源为上一轮已核验的官方源码本地快照，导入前核对HEAD和干净状态；公开记录不包含本机临时目录标识。

项目增量仅包括README、.gitignore、项目约定、文档和云端构建配置；上游C++、资源、工程及原构建脚本保持原样。可以使用下列只读命令核对：

```powershell
git diff upstream-baseline/49d81479 -- windirstat setup tests build.cmd windirstat.sln
```

## 可重复构建的已知限制

- 浅历史会影响上游Git提交计数产生的构建号；正式发布前须决定并记录历史补全或版本策略，不静默改上游代码。
- vcxproj的DownloadXXHash目标会查询latest，失败后回退v0.8.3，因此只锁主仓库SHA仍不足以固定全部构建输入。
- 云端工作流使用.github/dependencies/xxhash.json锁定xxHash v0.8.3提交及两个源文件、许可证的SHA256，在contrib/xxhash预置并校验，避免改上游下载逻辑。本地只为校验下载了小型源文件，没有安装编译工具。
- 本轮没有验证签名、安装包、发布或旧系统兼容；不能将源码版本字段当成发布验证结果。

## 公开准备的历史范围

公开准备分支从上述固定上游提交重新接入去标识后的项目文档与CI配置，保留上游SHA，不改写原开发分支。原开发分支仅留本地；它的初始化提交含有旧本机路径，不在公开准备分支的祖先链中。

首次发布仅推送明确的公开准备分支，不使用--all、--mirror或无选择地推送标签。完整步骤见CLOUD-BUILD.md。
