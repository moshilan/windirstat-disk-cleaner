# 垃圾清理：第一阶段只读预览

本阶段仅支持本机固定NTFS磁盘上的目录。新增原生独立窗口，不改变现有空间分析、大文件与重复检测算法。菜单入口为“清理→垃圾清理（只读预览）…”，无需先扫描磁盘。打开不自动扫描；本模块没有删除按钮或执行器，不写入扫描目标，仅保存用户授权的排除设置。

## 当前规则

除VS Code编译缓存采用90天外，规则均要求创建和修改至少7天。所有候选默认未勾选，风险保守标为中等；另列可再生缓存或需人工判断的说明，并解释受保护范围。年龄是项目筛选策略，不是文件无用或可安全删除的保证。

| 类别 | 支持的默认目录和条件 |
| --- | --- |
| 当前用户临时文件 | Known Folder LocalAppData下Temp，仅.tmp/.temp；其他扩展名不列候选 |
| Chrome HTTP缓存 | Google/Chrome/User Data/{Default,Profile 数字}/Cache/Cache_Data |
| Edge HTTP缓存 | Microsoft/Edge/User Data/{Default,Profile 数字}/Cache/Cache_Data |
| Firefox HTTP缓存 | Mozilla/Firefox/Profiles/*/cache2/entries |
| pip HTTP缓存 | LocalAppData下pip/Cache/http-v2及旧格式http，合并同一类别；不含wheels、虚拟环境或项目依赖 |
| Brave HTTP缓存 | LocalAppData下BraveSoftware/Brave-Browser/User Data/{Default,Profile 数字}/Cache/Cache_Data |
| Vivaldi HTTP缓存 | LocalAppData下Vivaldi/User Data/{Default,Profile 数字}/Cache/Cache_Data |
| VS Code编译缓存 | RoamingAppData下Code/CachedData/40位十六进制commit子目录，文件创建及修改均至少90天；仅稳定版默认位置，仍可能属于当前版本，需人工判断 |
| npm下载缓存 | LocalAppData下npm-cache/_cacache；不含_npx、node_modules、配置或项目 |
| NuGet HTTP缓存 | LocalAppData下NuGet/v3-cache；不含项目直接引用的.nuget/packages、plugins-cache或NuGet.Config |

只支持这些精确存在的目录，不追踪策略/命令行指定的缓存位置，不猜测未知软件或未知版本结构。未在本机读取浏览器真实缓存以验证安装版本；规则布局依据官方文档/源码及合成样本验证。浏览器可能正在运行，本阶段不声称文件未被占用；后续执行前需关闭应用并重新核验。缓存减少会造成重新下载和离线访问影响。

官方依据：[Chromium缓存目录源码](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/content/browser/network_service_instance_impl.h)、[Windows路径实现](https://chromium.googlesource.com/chromium/src/+/HEAD/chrome/common/chrome_paths_win.cc)、[Edge默认用户目录](https://learn.microsoft.com/en-us/deployedge/edge-learnmore-create-user-directory-vars)、[Edge缓存重定向策略](https://learn.microsoft.com/en-us/deployedge/microsoft-edge-policies/DiskCacheDir)、[Firefox HTTP缓存格式](https://firefox-source-docs.mozilla.org/networking/cache2/doc.html)、[pip缓存说明](https://pip.pypa.io/en/stable/topics/caching/)。

新增来源：[Brave官方安装常量](https://raw.githubusercontent.com/brave/brave-core/master/chromium_src/chrome/install_static/chromium_install_modes.h)确定BraveSoftware与Brave-Browser；[Vivaldi默认配置路径](https://help.vivaldi.com/desktop/privacy/preventing-vivaldi-profiles-from-being-uploaded-to-git-repositories/)确定根目录，其Cache_Data子树依据Chromium实现推断，仅识别确切存在的目录；[npm官方缓存说明](https://docs.npmjs.com/cli/v11/commands/npm-cache/)、[NuGet缓存与全局包区分](https://learn.microsoft.com/en-us/nuget/consume-packages/managing-the-global-packages-and-cache-folders)确定两个开发工具的精确下载缓存。[VS Code用户目录实现](https://raw.githubusercontent.com/microsoft/vscode/main/src/vs/platform/environment/node/userDataPath.ts)及[缓存清理源码](https://raw.githubusercontent.com/microsoft/vscode/main/src/vs/code/electron-utility/sharedProcess/contrib/codeCacheCleaner.ts)说明APPDATA、按commit保存及稳定版约90天策略；本工具不判断当前commit，采用文件创建和修改年龄筛选，不能等同于VS Code自身清理条件，也不推断便携、自定义或Insiders位置。

浏览器HTTP、pip、npm与NuGet缓存通常可重新获取，但可能影响离线使用、后续下载与安装速度；Temp和VS Code编译数据另标需人工判断。钱包、密码、Cookie、会话、站点存储、编辑器User/Backups/workspaceStorage、源码、项目依赖与配置始终不因类别扩展而纳入。未知软件、其他缓存位置和整个软件数据目录不扫描。

## 界面与统计

上方分类勾选决定扫描范围，下方候选勾选仅影响选择汇总。文件明细显示路径、分类、磁盘占用、实际匹配规则版本、原因、风险；选中行可查看完整路径和规则详情，选中分类可查看本类全部规则。排除框每行一个本机绝对目录，也可用“添加排除目录”。类别或排除变化后旧结果失效，须重新扫描。

有效排除目录复用软件现有INI或注册表配置存储，在独立CleanupPreview设置中保存并读回核验；关闭窗口或正常重启后恢复，打开仍不自动扫描。规范化路径并按大小写不敏感去重，支持中文、空格及分号；清空也会保存。无效草稿保留上一份有效设置，保存失败明确提示并阻止本次扫描；损坏的保存值不静默丢弃，须由用户更正或清空。该设置与原空间分析的排除项独立，不保存候选勾选或扫描快照。保存内容使用ASCII版本化编码，以兼容普通便携INI的Unicode路径。

窗口支持最大化、恢复及四边四角拖动缩放，最小尺寸按按钮和区域所需空间计算，不再锁定初始尺寸。可拖动分隔线位于分类列表与排序栏之间、候选列表与详情之间，以及排除框与诊断日志之间，命中间隙均为8DLU；日志上边界可调整日志高度。点击占用列或排序按钮按分配大小升降排列。隐藏空文件（0 字节）仅隐藏逻辑长度为0的候选，不改变总候选、分类或勾选统计；有内容但分配占用为0的文件仍保留。排序和筛选保留文件对应的选择，全选可见候选只影响显示项，全部取消同时清除隐藏项的选择。

分类新增覆盖状态，区分未发现规则支持的精确目录、目录存在但无合格文件、匹配候选、排除或保护跳过、失败、部分扫描及未扫描。诊断保留各类目录、文件、失败及跳过计数，读取元数据或流失败不再静默归为普通跳过。选中分类可查看完整规则范围；支持路径仍限上表，不探测真实个人目录或扩大规则。

扫描显示检查数量、候选数和耗时，不伪造百分比。取消保留部分预览，读取失败列出路径和系统原因，不能当成零占用或完整结果。GUI最多10000候选，达到上限明确标为不完整；候选控件使用虚拟列表，排序与筛选仅更新索引，按需提供文本，不扩大候选上限。

占用来自可核验的文件分配大小，单独保留逻辑长度及文件身份。分类按卷和文件身份去重；预计释放是占用上限，不等于实际释放，更不是已释放。实际执行前还需要新鲜快照和复核。

## 保护边界

- 只解析当前用户已知目录及固定相对根。系统/安装根、个人已知文件夹、浏览器Cookie/密码/历史/书签/会话/离线存储、源码仓库和凭据目录不纳入规则。
- 排除优先，按完整目录边界匹配；相对路径、盘根、网络路径、通配或ADS路径不能作为排除输入。
- 不跟随文件与祖先重解析点，跳过云占位/离线文件、压缩、稀疏、加密、系统属性、多硬链接、命名流及无法核验元数据的对象，显示跳过计数。
- 普通权限即可；不接管所有权、不改ACL、不提升权限、不停止软件。文件操作测试中的ACL及链接仅用于新建合成目录。
- 本阶段没有安全执行器，不宣称已防护WinDirStat原有删除/任意命令入口，也不修改它们。

## 验证与交付

2026-10-23列表性能修复：原FillFiles每次删除全部行，再插行、设勾选并写五个子列，万条恢复约7万次控件更新；分段计时确认控件重建占处理时间99%以上。e4028ff改为原生虚拟列表，复用原m_visible、m_selected和统计口径；按需返回六列和勾选状态，批量勾选仅更新数组，鼠标状态图标、双击和空格显式更新同一候选身份。未改规则、排除持久化、原扫描、布局或执行能力。原生虚拟列表的按需数据模式依据[Win32说明](https://learn.microsoft.com/en-us/windows/win32/controls/list-view-controls-overview)，窗口计时属性和绘制观察仅在有效合成模式发布，不写扫描输入。

同机2560×1552、100%字体、实时拖动；保留旧算法的计时基线dc6fdb7与正式版1f74aa3均每语种测2次恢复10000候选，4次均值如下，单位毫秒：

| 阶段 | 旧算法基线 | 优化后 |
| --- | ---: | ---: |
| 数据准备，旧版含全部文本格式化 | 9.758 | 0.025 |
| 筛选 | 0.011 | 0.012 |
| 排序 | 0.185 | 0.175 |
| 控件更新 | 3839.641 | 4.573 |
| 汇总及尾部更新 | 3.914 | 1.117 |
| 刷新处理总耗时 | 3857.747 | 5.904 |
| 实际WM_PAINT累计，另计 | 6.174 | 8.283 |

新版文本改在显示回调中生成，正常显示的格式化成本计入绘制，不将0.025ms解读为全量格式化耗时；总耗时不包含随后异步WM_PAINT。4次恢复处理范围3736.496～3929.494ms降至3.658～8.791ms，绘制5.992～14.172ms；鼠标按下至状态稳定258～274ms，包含驱动80ms按键及稳定等待。万条排序完整按钮调用3～5ms。仅用旧算法诊断副本延长等待采集优化前数据，仓库正式驱动的3秒消息限时不变，新交互还显式断言小于3秒。

[最终标准Runner运行39828041163](https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39828041163)全部通过，源码1f74aa3，Release x64编译2分34.59秒、0警告/0错误；核心142、双语GUI各238及原扫描319共937项，双语合成导出和内容哈希检查通过。本机正式中英文各238项、共476项通过（109.7秒），0失败/跳过；实际鼠标缩放、分隔、候选勾选和筛选、空格勾选、大小排序、隐藏候选恢复、原选择/类别/空间统计、排除配置重启及输入不变检查通过。源规则、排除配置和云端工作流未改；本轮未验证200%字体、其他DPI或更大上限。

交付便携2.10.0.2207，3247616字节，SHA256为`20957e72e39d5281838303a2f9ce23293592aaf0f83220106581a22f5a18db4d`，两个Artifact与EXE均校验匹配，程序位于.local/cloud-build/39828041163/portable/WinDirStat.exe。正式通过记录位于.local/refresh-optimized-local-retry-39828041163，分段比较及万条合成启动脚本位于.local/performance-2207。首次优化云端鼠标检查因驱动把首条可见行误当行0而失败，1f74aa3补滚入行0及顶行断言；本机首次新版运行有窗口意外关闭及点击未切换，用户确认期间操作过鼠标，保持桌面不动后原样重跑全通过。失败记录仍保留，未靠放宽超时或改产品绕过。以下保留此前阶段记录。

2026-10-23规则扩展及排除持久化：源码2a1193d已推送既有公开分支，[标准Runner运行39811323245](https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39811323245)全部通过。Release x64编译2分钟，0警告/0错误；核心142项、双语GUI各220项及原扫描319项，共901项，另有双语合成导出和输入哈希不变检查。10类别/11规则的14个候选正样本、受保护负样本、双锚点独立失败、身份去重、pip多规则部分状态均已验证；候选默认不勾选。

本机正式双语GUI测试各在第157项后的10000候选恢复操作出现BM_GETCHECK的3秒消息超时，结果为2失败，不能记为通过。保留生产代码和正式测试不变，仅在.local诊断副本将跨进程Message等待延为30秒并记录耗时；同一鼠标操作下四次读取为3907/3829/3815/3846毫秒，之后完成双语各220项、共440项检查（129.3秒，无跳过）。说明本次超时后可以恢复，但万条列表刷新存在约4秒延迟，未在本轮修复，也不能据此证明所有本机卡顿原因。实际鼠标缩放、三条分隔线、筛选及选择/统计保持均已运行；两轮均为2560×1552、100%字体、实时内容拖动，未覆盖新版200%字体或其他DPI。

诊断续测实际退出和重启应用，确认ASCII便携INI可恢复中文/空格/分号排除目录并去重，Local/Roaming排除均生效且保留同前缀兄弟目录；清空重启保留、无效草稿不覆盖最后有效值、只读INI保存失败和坏编码明确阻止扫描，原空间分析排除值保持。载荷哈希不变，合成ACL恢复；注册表配置模式仅复用现有引擎，未单独做进程重启验证。正式失败记录位于.local/preview-expansion-local-39811323245，诊断副本及结果位于.local/preview-diagnostic-project和.local/preview-expansion-diagnostic-39811323245，不作为正式用例全通过的替代。

交付便携2.10.0.2203，3245056字节，EXE SHA256为`f9b015ad49aa74f3f62e8faad7f0adde6e73f98b0c721b8ff66ff834be188625`；两个Artifact及EXE摘要匹配，依赖仅系统DLL。本机程序位于.local/cloud-build/39811323245/portable/WinDirStat.exe；.local/preview-expansion-local-39811323245/Start-SyntheticPreview.cmd启动14文件的小型合成目录，可人工复核新规则说明、未默认勾选、Unicode排除后退出重启和清空。未知版本布局、自定义缓存位置及当前VS Code commit仍不推断，未扫描个人目录或新增删除能力。用户已确认上一版2201人工交互验收通过，以下保留历史记录。

2026-10-22人工验收再排查：2199的版本、路径、哈希与交付一致，原用户实例没有合成启动参数。确认旧日志框固定24DLU，既有两条分隔均不含日志上边界；旧测试只覆盖这两条和8条候选，不能代表日志拖动或10000条候选的实际操作。修复022c83f仅增加现有日志边界的捕获、布局偏移和高度约束，未改筛选逻辑、规则、扫描或删除能力。

[云端运行39794623773](https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39794623773)通过：Release x64编译2分31秒，0警告/0错误，核心75项、原扫描319项、深色中英文GUI各167项，共728项；新增实际鼠标日志拖动及还原、筛选文字命中和10000条候选反复切换检查。便携2.10.0.2201，3230208字节，SHA256为`9855cce7a1236cd1fde198c64bf4728a91c18616267b797be0aa70c025421666`，两份Artifact与EXE摘要匹配，程序位于项目`.local/cloud-build/39794623773/portable/WinDirStat.exe`。

本机新版通过Computer Use实际鼠标操作并逐步观察截图：日志高约44→130→70像素，右下角拖动令982×795窗口缩为922×745，筛选文字两轮8→5→8条；总候选8、占用40KiB和第二轮原勾选8/预计上限40KiB均保持。8个合成文件内容仍符合生成规格；证据记录及合成启动脚本在`.local/preview-live-after/`，未上传本机截图。用户也确认旧版10000条隔离实例能反复切换。原个人目录结果的“无法取消”根因仍未确认，不能宣称已修复：窗口工具曾激活超时、模态被遮挡，合成忙线程采样含UI Automation调用链，这些工具访问失败不构成筛选逻辑故障证据。以下保留前轮记录及其覆盖范围。

交互修复1111c00及测试修正af04123的[云端构建39773311892](https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39773311892)全部通过：Release x64编译2分18.65秒，0警告/0错误；核心75项、原扫描319项、中英文GUI各135项，共664项检查通过，另有双语合成导出通过。本机下载版中英文GUI共270项通过（41.8秒），无失败/跳过、输入哈希不变、合成ACL恢复。真实鼠标验证四边四角缩小、两条分隔线的实际命中与拖动、筛选反复切换及原选择和统计保持。云端1024×720、轮廓拖动，本机2560×1552、实时内容拖动，均为100%应用字体；本轮新版200%字体与其他系统DPI未验证。

便携版2.10.0.2199，EXE为3228672字节，SHA256为`84089207edee45f4cdf3caf00ea39ad766a4b16300f47e0c52c251288ca2cb82`。[便携包Artifact](https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39773311892/artifacts/12423372271)与记录Artifact摘要均匹配，依赖仅Windows系统DLL。本机程序位于项目`.local/cloud-build/39773311892/portable/WinDirStat.exe`，本机复验记录及合成模式启动脚本位于`.local/preview-interaction-local-39773311892/`。人工验收使用该脚本，核对缩放和恢复、两条主要分隔线及筛选反复开关；普通装饰边框不作为分隔线。

旧版真实拖角不能缩小已复现，原测试直接设置尺寸绕过了最小尺寸约束；新测试保留系统最小尺寸、非客户区命中、捕获及松手后最终尺寸断言，兼容Windows只显示轮廓的缩放模式。默认配置下未复现原筛选无法取消的独立根因，本轮显式保存视图开关并重绘，反复原生点击已通过。前几轮失败与诊断修正见WORKLOG，不作为通过记录。原扫描、重复检测、清理核心和云端构建逻辑未修改，未扫描真实个人目录或执行清理。以下保留旧版验证记录。

本轮实现db9a649及验证修正57c91e6已获授权推送。[云端运行39752041080](https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39752041080)全部通过：Release x64编译2分33.52秒，0警告、0错误；核心75项、原扫描319项、中英文GUI各111项，共616项检查通过，无失败或跳过；另有双语合成导出通过，输入哈希不变。本机下载后中英文合成GUI共222项通过，耗时21.9秒，0失败或跳过；源文件内容不变、合成ACL恢复，未扫描个人目录或执行删除。

新版便携2.10.0.2194，EXE为3226624字节，SHA256为`496957eddf1b95c919c1eb6731ca08180f26d3b3b91a90fed47c576bd8a492c0`。[便携包](https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39752041080/artifacts/12414347228)和构建记录Artifact摘要均已匹配，依赖仅Windows系统DLL。下载保存在项目`.local/cloud-build/39752041080/portable/WinDirStat.exe`，本机复验记录及合成模式启动脚本保存在`.local/preview-validation-local-39752041080/`；本机证据不提交或上传。

两轮失败均来自新增测试样本或驱动：文件ReadAttributes拒绝未实际阻断元数据打开，改用合成目录枚举拒绝；分隔拖动的注入坐标与真实光标竞争，改为同步原生光标并等待精确位移稳定。只修现有测试，不改生产功能或云端体系。文件元数据独立权限拒绝、高DPI、多显示器、真实软件各版本和自定义缓存位置仍未验证。人工验收使用合成模式启动脚本，进入清理预览后核对长路径/规则/原因、列宽和两条分隔线、升降序及筛选后勾选与统计。后续原始版本记录保留如下。

源码81121517e87366b5cdd3a7901757e75febcc0a72的[云端构建39744867758](https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39744867758)全部通过。Release x64编译2分21秒，0警告、0错误；原生核心43项、原有扫描回归319项、新增中英文GUI各70项，共502项检查通过，无失败或跳过。另有双语合成导出核验目录总量1179660字节、最大文件和一组重复文件，输入哈希不变。

原生核心测试覆盖白名单、年龄、个人/未知路径、排除边界、保护、身份及分配统计、取消、截断、ADS、硬链接、链接边界与内容不变。GUI测试覆盖入口、不自动扫描、默认未选、汇总、排除失效、边界、空结果、权限失败优先解释、取消、ACL恢复及无效测试目录禁止回退。取消状态回归仅在云端创建10000文件，观察扫描活跃后同步Stop；原暂停生命周期用例未纳入此次通过范围，生产扫描算法不变。

本机下载后校验两个Artifact摘要及EXE SHA256，中英文小型合成GUI各70项通过（22.1秒，0失败/跳过），样本内容不变。仅新建合成目录中设置/恢复测试ACL，全部测试保留样本，不执行删除。本机报告位于不提交的.local/cleanup-preview-local，云端合成截图保存在下载记录中；未上传本机截图或原始报告。

交付版本2.10.0.2189，EXE 3202560字节，SHA256为`0c56489de3ba9cb541c61185d87c72c485e7de288c76cbb47e0d876144dbc10a`。[便携包Artifact](https://github.com/moshilan/windirstat-disk-cleaner/actions/runs/39744867758/artifacts/12410564876)保留至2026-10-29；解压同目录保留INI和许可证，普通权限启动即可，无需开发环境。dumpbin依赖仅列Windows系统DLL，无单独VC运行库DLL；其他Windows版本未实际启动验证。

云端中英文截图复核未发现明显乱码、重叠和按钮裁切，完整路径和原因可滚动/详情查看；多显示器、不同高DPI及真实软件各版本布局尚未覆盖。迭代失败与最小修正记于WORKLOG，不将失败轮次记为通过。多行文本WM_SETTEXT不产生[EN_CHANGE](https://learn.microsoft.com/en-us/windows/win32/controls/en-change)，故“添加排除目录”程序设置后显式使预览失效，测试也模拟用户编辑通知。

GUI测试仅在显式WDS_CLEANUP_TEST_ROOT指向系统Temp/WinDirStatTests-*运行目录、目录链安全且fixture标记匹配时映射LocalAppData及同级RoamingAppData；窗口显示合成模式。无效覆盖禁用扫描，不回退真实缓存；坏Roaming锚点单独标记相关类别失败，不丢失已完成的Local结果。该接口只用于只读测试，不代表后续执行授权。

## 下一阶段

自定义规则编辑、应用占用检查、真实版本兼容样本扩充、缓存配对/分组（例如pip HTTP元数据与.body）、执行前重新核验与安全执行器、操作日志、回收站和其他类别均未开发。路径核验不保证消除外部进程持续修改引发的所有竞态，不可直接复用预览结果执行删除。
