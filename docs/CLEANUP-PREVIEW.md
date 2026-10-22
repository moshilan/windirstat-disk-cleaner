# 垃圾清理：第一阶段只读预览

本阶段仅支持本机固定NTFS磁盘上的目录。新增原生独立窗口，不改变现有空间分析、大文件与重复检测算法。菜单入口为“清理→垃圾清理（只读预览）…”，无需先扫描磁盘。打开不自动扫描；本模块没有删除按钮、执行器或文件写操作。

## 当前规则

所有规则均要求创建和修改至少7天，所有候选默认未勾选，风险保守标为中等。7天是项目筛选策略，不是文件无用或可安全删除的保证。

| 类别 | 支持的默认目录和条件 |
| --- | --- |
| 当前用户临时文件 | Known Folder LocalAppData下Temp，仅.tmp/.temp；其他扩展名不列候选 |
| Chrome HTTP缓存 | Google/Chrome/User Data/{Default,Profile 数字}/Cache/Cache_Data |
| Edge HTTP缓存 | Microsoft/Edge/User Data/{Default,Profile 数字}/Cache/Cache_Data |
| Firefox HTTP缓存 | Mozilla/Firefox/Profiles/*/cache2/entries |
| pip HTTP缓存 | pip/Cache/http-v2，适配23.3及以上默认格式；不含wheels、虚拟环境或项目依赖 |

只支持这些精确存在的目录，不追踪策略/命令行指定的缓存位置，不猜测未知软件或未知版本结构。未在本机读取浏览器真实缓存以验证安装版本；规则布局依据官方文档/源码及合成样本验证。浏览器可能正在运行，本阶段不声称文件未被占用；后续执行前需关闭应用并重新核验。缓存减少会造成重新下载和离线访问影响。

官方依据：[Chromium缓存目录源码](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/content/browser/network_service_instance_impl.h)、[Windows路径实现](https://chromium.googlesource.com/chromium/src/+/HEAD/chrome/common/chrome_paths_win.cc)、[Edge默认用户目录](https://learn.microsoft.com/en-us/deployedge/edge-learnmore-create-user-directory-vars)、[Edge缓存重定向策略](https://learn.microsoft.com/en-us/deployedge/microsoft-edge-policies/DiskCacheDir)、[Firefox HTTP缓存格式](https://firefox-source-docs.mozilla.org/networking/cache2/doc.html)、[pip缓存说明](https://pip.pypa.io/en/stable/topics/caching/)。

## 界面与统计

上方分类勾选决定扫描范围，下方候选勾选仅影响选择汇总。文件明细显示路径、分类、磁盘占用、规则版本、原因、风险；选中行可查看完整路径和规则详情。排除框每行一个本机绝对目录，也可用“添加排除目录”。类别或排除变化后旧结果失效，须重新扫描；本阶段配置仅在窗口会话内使用。

扫描显示检查数量、候选数和耗时，不伪造百分比。取消保留部分预览，读取失败列出路径和系统原因，不能当成零占用或完整结果。GUI最多10000候选，达到上限明确标为不完整；后续可扩展为分批/虚拟列表。

占用来自可核验的文件分配大小，单独保留逻辑长度及文件身份。分类按卷和文件身份去重；预计释放是占用上限，不等于实际释放，更不是已释放。实际执行前还需要新鲜快照和复核。

## 保护边界

- 只解析当前用户已知目录及固定相对根。系统/安装根、个人已知文件夹、浏览器Cookie/密码/历史/书签/会话/离线存储、源码仓库和凭据目录不纳入规则。
- 排除优先，按完整目录边界匹配；相对路径、盘根、网络路径、通配或ADS路径不能作为排除输入。
- 不跟随文件与祖先重解析点，跳过云占位/离线文件、压缩、稀疏、加密、系统属性、多硬链接、命名流及无法核验元数据的对象，显示跳过计数。
- 普通权限即可；不接管所有权、不改ACL、不提升权限、不停止软件。文件操作测试中的ACL及链接仅用于新建合成目录。
- 本阶段没有安全执行器，不宣称已防护WinDirStat原有删除/任意命令入口，也不修改它们。

## 验证与交付

结果待云端构建与本机合成复验后回填。原生核心测试覆盖白名单、年龄、个人/未知路径、排除边界、保护、身份及分配统计、取消、截断、ADS、硬链接、链接边界与内容不变；中英文GUI测试覆盖入口、默认未选、选择汇总、失效、排除、权限失败及取消。

GUI测试仅在显式WDS_CLEANUP_TEST_ROOT指向系统Temp/WinDirStatTests-*运行目录、目录链安全且fixture标记匹配时映射LocalAppData；窗口显示合成模式。无效覆盖禁用扫描，不回退真实缓存。该接口只用于只读测试，不代表后续执行授权。

## 下一阶段

自定义规则编辑、排除持久化、应用占用检查、规则兼容样本扩充、缓存配对/分组（例如pip HTTP元数据与.body）、执行前重新核验与安全执行器、操作日志、回收站和其他类别均未开发。路径核验不保证消除外部进程持续修改引发的所有竞态，不可直接复用预览结果执行删除。
