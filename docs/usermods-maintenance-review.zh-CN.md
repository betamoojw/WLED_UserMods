# KNX_IP、Xiaozhi_MCP 和 License_Mgnt 维护审查

本次审查涵盖 `UserModsDev` 分支上的三个用户模块，包括 KNX 传输与转换辅助代码、MCP 传输与信任存储代码，以及 WLED 核心调用的许可证验证器。本次清理提高可读性并移除未使用的实现代码，保留现有协议、配置、诊断、LED 和许可证行为。以下发现描述的是需要通过单独的行为变更解决的现有缺陷或风险；本次清理不代表安全审核通过。

初始工作区的五个文件中已存在暂存的修改。这些暂存内容保持完整。新增维护修改未暂存。未创建提交、推送、PR、问题或审查回复。源码的重构代码块中已披露 AI 辅助情况。

## 架构与兼容性边界

| 用户模块 | 职责 | 需要保留的行为 |
| --- | --- | --- |
| KNX_IP | KNXnet/IP 发现和组播路由；按地址回调；全局和分段 LED 控制；相对调整；时间更新；温度反馈；设置和 GA 表 | 地址解析和零地址哨兵值、注册顺序、线上传输载荷、舍入、LED 能力判断、主分段与零号分段的选择、发布时机、重复发送/去重设置、JSON 键、标签和诊断 |
| Xiaozhi_MCP | 连接后注册六个 MCP 工具；在 WLED 主循环中分派 JSON-RPC；基于队列的网络工作线程；证书验证和重连处理 | 工具名称/模式/回复、初始化时捕获的别名、回调顺序、队列/会话所有权、15 KiB 传输限制、现有较小的 JSON 文档容量、TLS 策略、重试/心跳间隔、电源和亮度效果 |
| License_Mgnt | 显示设备标识和密钥状态；定期验证 `/DEVICE_KEY`；累计未授权时间区间；警告和重启 | 只读配置值、密钥文件字节和状态码、严格的 61 秒轮询边界、无效区间累计计数器、警告效果及下次检查时重启、待执行重启优先于重新验证 |

KNX 会在分派前验证传入帧长度和大多数处理函数的载荷长度。MCP 通过队列和会话标签将网络操作与 LED 修改隔离，并在启动替代工作线程前停用旧线程。这些边界值得在后续重构中保留。许可证验证是核心依赖：仅修改用户模块无法彻底解决其内存分配和日志问题。

## 清理计划与实施

1. **建立基线。** 阅读 `AGENTS.md` 以及贡献、构建、C++、Web 和安全指南，检查暂存修改，追踪配置、输入入口、状态修改、持久化和关闭路径，并编译现有的组合固件。
2. **统一格式。** 对 13 个 C++ 源文件/头文件应用两空格缩进、同行左花括号、一致的空格和易读的语句布局。保留包含顺序、预处理条件和字符串内容。在进一步编辑前检查词法单元是否一致。
3. **移除已确认未使用的实现。** 删除无调用方的 KNX 私有时钟/白光辅助函数、未使用的 KNX 字段和仅写入的预设跟踪变量；删除 MCP 未使用的私有转义辅助函数和未使用的局部亮度计算；删除 License_Mgnt 的空 MQTT 轮询占位函数。保留公共兼容性辅助函数和可选测试入口。
4. **明确约定。** 为现有 KNX 和 Xiaozhi 用户模块钩子添加 `override`，将不可变超时常量设为 `static constexpr`，将 KNX 发现相关注释翻译成英文，并修正有关传输队列接收、温度提供方和现有注册行为的过时注释。保留现有各模块的日志开关，并解释为何将其改为 `WLED_DEBUG` 会改变输出。
5. **验证与审查。** 将实际使用的函数体与初始源码比较，审查所有非格式修改，运行 Node 测试，重新构建 Web 资源，并编译 `esp32dev` 以及现有组合发布/调试环境。设备级验收与编译分开进行。

保留了已暂存的解析器/限幅逻辑、MCP 效果响应和周期性许可证辅助函数提取。源码标记指出了这些重构代码块。未从外部来源引入算法；清理以本仓库现有实现为基础。现有源码署名和依赖引用均予以保留。

本次清理有意不引入 KNX/MCP 共用的 LED 修改辅助函数：两个模块目前在自动上电行为、白光通道处理和通知调用方面存在差异。在未明确策略的情况下统一它们会改变行为。同样，将三处许可证检查替换为缓存状态，可能改变新导入密钥被识别的时机。

## 需要单独变更的现有问题

以下所有发现均基于源码路径追踪。尚未在硬件上复现设备故障。源码链接指向清理后的文件；提供行号是为了便于具体审查。严重程度遵循 `docs/hardening.instructions.md` 和 `docs/securecode.instructions.md`。

### 严重：凭据处理

**C1 — 默认配置中嵌入了含凭据的 MCP 端点。** `Xiaozhi_MCP.h` 使用包含令牌的完整 URL 初始化 `mcpEndpoint`。启用用户模块而不替换该值时，会使用共享端点。应将该凭据视为已泄露；未测试其当前在服务端是否仍有效。应轮换/撤销凭据，将其从默认值中移除，并要求配置专用端点。这会改变首次使用行为，超出本次仅保持兼容性的清理范围。本报告有意不复制该令牌。来源：[Xiaozhi_MCP.h:54](E:/WLED_Projs/WLED_UserMods/usermods/Xiaozhi_MCP/Xiaozhi_MCP.h#L54)。

**C2 — 调试输出泄露端点凭据和许可证密钥。** `Xiaozhi_MCP::checkMcpConfig()` 通过 `MCP_UM_DEBUG` 打印完整端点；`License_Mgnt::setup()` 通过 `LM_UM_DEBUG` 打印已存储密钥。核心的 `validateDeviceKey()` 还通过 `WLED_DEBUG` 打印计算所得密钥和已存储密钥，包括导入文件无效时应有的密钥。组合调试环境启用了这些路径。应移除敏感值，仅保留状态诊断，核心验证器也应如此。传输层自身的日志防护未覆盖这些用户模块/核心日志。来源：[Xiaozhi_MCP.cpp:174](E:/WLED_Projs/WLED_UserMods/usermods/Xiaozhi_MCP/Xiaozhi_MCP.cpp#L174)、[License_Mgnt.cpp:15](E:/WLED_Projs/WLED_UserMods/usermods/License_Mgnt/License_Mgnt.cpp#L15)、[util.cpp:1617](E:/WLED_Projs/WLED_UserMods/wled00/util.cpp#L1617)。

### 重要：许可证健壮性

**I1 — 许可证读取根据未经检查的文件大小分配内存，并忽略读取不足。** `License_Mgnt::getStoredDeviceKey()` 和核心的 `validateDeviceKey()` 分配 `new char[size + 1]`，不检查分配结果便使用它，并忽略 `readBytes()` 的返回值。大文件可能耗尽堆；读取不足会使末尾终止符之前的部分字节保持未初始化状态。文件由本地配置，因此这是受限内存/数据损坏场景下的健壮性问题，而非未经身份验证即可利用的网络漏洞。应在核心中集中实现有界读取，强制执行实际密钥格式的最大大小限制，检查分配结果和读取长度，并在不应用部分数据的情况下返回明确定义的错误。除非单独的迁移方案明确允许空白字符，否则应保留逐字节精确比较。来源：[License_Mgnt.cpp:179](E:/WLED_Projs/WLED_UserMods/usermods/License_Mgnt/License_Mgnt.cpp#L179)、[util.cpp:1591](E:/WLED_Projs/WLED_UserMods/wled00/util.cpp#L1591)。

### 重要：KNX 正确性与资源处理

**I2 — HSV 应用使用了超出转换器归一化定义域的值。** `applyHSV()` 调用 `hsvToRgb(hDeg, 101 - s01, 101 - v01, ...)`，但调用方构造的饱和度/明度范围是 `0..1`。转换器直接使用这些值。例如，色相为 0、饱和度为 0、明度为 0 时，限幅后得到饱和红色，而不是黑色。应在确定预期映射后传入归一化值，并通过实际命令路径添加测试。修正会造成可见颜色变化，因此本次保留该表达式。来源：[usermod_knx_ip.cpp:1450](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/usermod_knx_ip.cpp#L1450)。

**I3 — 地址变更可能使运行时注册信息过时。** `readFromConfig()` 加载分段偏移量以及时间/温度地址，但 `anyGAChanged` 仅比较部分 GA 设置，不包含分段偏移量。如果 KNX 正在运行且被比较字段未变，`rebuildNeeded` 就为 false。此时仅修改被排除的设置，会走轻量级套接字路径，使回调或缓存的温度 GA 仍沿用旧映射。应建立一个明确的注册快照，包含所有影响注册的设置，并在重建前进行比较。生命周期设计应包含分段数量变化，而不能假定每次变化都是灯光能力变化。来源：[usermod_knx_ip.cpp:2950](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/usermod_knx_ip.cpp#L2950)、[usermod_knx_ip.cpp:3177](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/usermod_knx_ip.cpp#L3177)、[usermod_knx_ip.cpp:3271](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/usermod_knx_ip.cpp#L3271)。

**I4 — 冲突通知可能将已注册分段地址判定为与自身冲突。** `getAllUsedGAs()` 包含当前分段数组。`hasGAConflicts()` 将拟使用的分段地址与这个合并列表比较，仅豁免与中央地址集合匹配的地址。因此，在多分段注册成功后，`checkGAConflictsAndNotifyGUI()` 可能报告零号以外的每个分段与其自身现有地址冲突。应比较预期的地址归属记录，而非单纯的 GA 值；仅允许地址自身的所有者以及有意设置的零号分段别名。应在注册前后测试同一套有效方案。来源：[usermod_knx_ip.cpp:213](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/usermod_knx_ip.cpp#L213)、[usermod_knx_ip.cpp:350](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/usermod_knx_ip.cpp#L350)。

**I5 — 冲突错误使用了错误的核心错误编号。** 三处赋值和清除条件使用字面值 `33`，但 `ERR_KNX_GA_CONFLICT` 为 `89`，且 Web UI 在 `case 89` 下处理 KNX 详细信息。修复行为时应统一使用具名常量，并验证 Info/UI 消息及清除路径。本次清理保留 `33`，避免在未明确说明的情况下改变错误显示行为。来源：[usermod_knx_ip.cpp:454](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/usermod_knx_ip.cpp#L454)、[const.h:504](E:/WLED_Projs/WLED_UserMods/wled00/const.h#L504)、[index.js:1598](E:/WLED_Projs/WLED_UserMods/wled00/data/index.js#L1598)。

**I6 — RX 增强在检查注册状态之前跟踪任意目标地址。** 启用增强功能后，`_handleIncoming()` 会在 `_gos.find(dst)` 之前，为任何单字节写入插入 `_rxBitCache[dst]`。不同的未注册目标地址可能使一个没有淘汰机制的映射持续增长，直至地址空间上限，这对 ESP32 来说负担过大。同一分支还将单比特切换节流应用于每种单字节 DPT，因此连续的亮度/通道值可能仅因最低位变化而被丢弃。应在跟踪前检查注册状态和 DPT，并限制状态数量或使状态过期。由于会改变接收流量的处理，应作为单独变更。来源：[esp-knx-ip.cpp:668](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/esp-knx-ip.cpp#L668)、[esp-knx-ip.cpp:679](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/esp-knx-ip.cpp#L679)。

**I7 — 相对增加的 STOP 命令当前会执行一次正向步进。** `DPT.h` 将 `0x08` 命名为 `DPT_3_007_INCREASE_STOP`，但 `knx_step_delta()` 仅在完整半字节为 `0x00` 时提前返回。对于 `0x08`，步进百分比为零，而最小幅度规则将其变为 `+1`。应测试两种 STOP 编码，并在应用最小步进前检查步进位。本次清理记录并保留当前映射。来源：[DPT.h:28](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/DPT.h#L28)、[usermod_knx_ip.cpp:1027](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/usermod_knx_ip.cpp#L1027)。

**I8 — KNX 发布截止时间不能安全处理计时器回绕。** `_nextTxAt = now + txRateLimitMs` 通过 `millis() >= _nextTxAt` 判断。截止时间若跨过 32 位时钟边界回绕，可能被认为立即到期；计算结果为零时，还会与“未调度”哨兵值冲突。应采用经过时间表示法，或使用可安全处理回绕的截止时间并显式记录待执行状态，同时添加边界测试。这会改变回绕附近的调度行为，仍留待后续处理。来源：[usermod_knx_ip.cpp:2672](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/usermod_knx_ip.cpp#L2672)、[usermod_knx_ip.cpp:2834](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/usermod_knx_ip.cpp#L2834)。

**I9 — 分段分配失败没有明确定义的恢复路径。** `registerSegmentKOs()` 删除旧的六个数组，使用未经检查的 `new[]` 分配替代数组，然后通过各指针写入。在堆内存紧张时，分配失败可能导致程序中止或空指针解引用，具体取决于工具链。应先使用带检查的 WLED 分配器/所有权管理机制分配完整替代结构，失败时保留原有可用注册信息，并发布适当错误。这需要生命周期设计和故障注入测试，不能仅通过格式层面的替换解决。来源：[usermod_knx_ip.cpp:806](E:/WLED_Projs/WLED_UserMods/usermods/KNX_IP/usermod_knx_ip.cpp#L806)。

### 重要：MCP 互操作性

**I10 — MCP JSON 序列化会丢失 ID 信息，且未转义值。** `handleJsonRpcMessage()` 将转换为字符串的 ID 插入手工拼接的 JSON，而未按 JSON 规则加引号，并将工具调用 ID 转换为 `int`。它拼接包含可配置别名的工具描述时没有进行转义。`formatJsonString()` 也仅为字符串值加引号，不转义其内容。因此，字符串请求 ID，以及包含引号/反斜杠的别名，可能造成 ID 不匹配或无效 JSON。应使用保留类型信息的 JSON 序列化，并保留原始 ID 类型，然后测试字符串/数字 ID 和需要转义的别名。仅接入已删除的未使用转义辅助函数无法解决所有情况。现有 README 已记录其中部分限制。来源：[WebSocketMCP.cpp:729](E:/WLED_Projs/WLED_UserMods/usermods/Xiaozhi_MCP/WebSocketMCP.cpp#L729)、[WebSocketMCP.cpp:768](E:/WLED_Projs/WLED_UserMods/usermods/Xiaozhi_MCP/WebSocketMCP.cpp#L768)、[WebSocketMCP.cpp:780](E:/WLED_Projs/WLED_UserMods/usermods/Xiaozhi_MCP/WebSocketMCP.cpp#L780)、[WebSocketMCP.cpp:937](E:/WLED_Projs/WLED_UserMods/usermods/Xiaozhi_MCP/WebSocketMCP.cpp#L937)。

**I11 — MCP 配置变更被保存，但活动客户端不会重启。** `readFromConfig()` 更新端点和别名，但 `loop()` 仅在 `isSetupDone` 为 false 时调用 `setup()`。因此，已连接实例会继续使用旧端点和初始化时的别名，直到重启或禁用后重新启用。如果需要实时配置，应比较当前生效设置与请求设置，并在主循环中通过现有停用机制完成替换；不要直接从设置回调调用传输生命周期 API。重新注册工具时还必须更新元数据，因为 `registerTool()` 目前对已有名称只替换回调。来源：[Xiaozhi_MCP.cpp:151](E:/WLED_Projs/WLED_UserMods/usermods/Xiaozhi_MCP/Xiaozhi_MCP.cpp#L151)、[Xiaozhi_MCP.cpp:98](E:/WLED_Projs/WLED_UserMods/usermods/Xiaozhi_MCP/Xiaozhi_MCP.cpp#L98)、[WebSocketMCP.cpp:842](E:/WLED_Projs/WLED_UserMods/usermods/Xiaozhi_MCP/WebSocketMCP.cpp#L842)。

## 进一步审查的边界

KNX 发送 API 使用固定的 64 字节 cEMI 存储区，没有通用的载荷大小保护，但已追踪的用户模块发送路径使用较小的固定载荷。应将更广泛的公共 API 边界检查视为纵深防御，而非已证实的远程溢出。RX 解析会忽略非零的 cEMI 附加信息长度；协议变更应添加填充该字段的数据包测试样本。修改当前设备/日期解析器的映射前，也需要协议专用测试样本。

传输层的信任存储和工作线程所有权代码仅接受了源码审查和格式调整。这并不证明 TLS 行为、工作线程栈余量、堆稳定性，或硬件/网络故障下的互操作性。与 WLED 已记录的威胁模型一致，本次未对 KNX 组播一概施加 TLS 或身份验证要求。

现有 `test/knx_pure_test.cpp` 复制算法，而非编译生产实现；其相对步进计算使用截断，而生产代码使用舍入。这些测试无法证明实际命令路径的等价性。未来的行为重构应将纯生产辅助函数放到可测试接口之后，随后直接测试它们，避免编译整个固件或维护平行实现。

## 验证

| 检查 | 结果 |
| --- | --- |
| 按锁定版本安装 Node 依赖并构建 Web | 通过 |
| `npm test` | 16 项测试通过；首次运行未通过现有的 850 ms 文件年龄断言，之后单独重跑通过，未修改测试 |
| `pio run -e esp32dev` | 通过 |
| 组合发布构建 | 通过；确认请求的全部三个用户模块均已链接 |
| 组合调试构建 | 通过；确认请求的全部三个用户模块均已链接 |
| 格式验证 | 在仅调整格式的阶段，全部 13 个源文件/头文件的非注释词法单元和字面量内容均保持一致 |
| 活动代码比较 | 132 个实际使用的函数体保持相同词法单元；删除了五个未使用函数，另行审查了七处局部函数体修改 |
| 协议/工作线程比较 | 完整 KNX 传输、DPT 转换/声明、MCP 网络工作线程和信任存储的非注释词法单元均保持一致 |
| 暂存工作保留 | Git 索引与初始索引逐字节一致 |
| 空白验证 | `git diff --check` 通过 |

七处局部函数体修改分别是：移除一个空 MQTT 分支、移除两处未使用的局部亮度计算、移除一处仅写入的预设赋值、简化两个分支结果相同的条件表达式，以及移除 MCP 配置检查中的冗余括号。原有字符串相等检查和长度检查均被保留：Arduino 字符串可包含嵌入的空字符，因此仅保留长度检查无法保持所有输入情况的行为。

组合发布构建使用 88,256 字节静态 RAM 和 1,524,791 字节闪存，初始构建分别为 88,280 和 1,524,835 字节。这些是链接器统计总量，并非运行时堆或栈使用量的测量值。组合调试构建使用了 1,572,864 字节闪存中的 1,569,379 字节：仅剩 3,485 字节。现有 `esp_dmx` 字符串边界警告不在本次清理范围内。KNX 调试标志/日志宏的重定义通过在相同的函数式宏定义前添加 `#undef` 来明确表达；这保留了日志行为。

构建配置和依赖版本未变更。仅 `esp32dev` 构建成功并不能覆盖这些用户模块；两个组合环境提供了相应编译覆盖。三个模块均明确不支持 ESP8266。未刷写固件，也未执行硬件 KNX/MCP/许可证冒烟测试。

本地验证脚本、源码快照和构建日志位于被忽略的 `.cache/usermod-cleanup/` 目录。仓库的 Node 测试会临时修改 Web 源文件并将其还原；交付前已检查还原后的内容。

## 发布前的设备验收

部署本次清理前，应使用备用开发板和现有设置。检查 KNX 发现、全局/分段电源和亮度、绝对/相对颜色/CCT/效果/预设命令、重复处理、网络重连和时间更新。将 GA 反馈和 Info 输出与基线固件比较。测试一套有效的多分段地址方案，以区分现有冲突问题与回归。

对于 MCP，应针对开发板运行现有 `tools/xiaozhi-mcp-smoke.js`，检查初始化、列表查询、只读调用、分片和重连循环；还应检查已配置的安全端点、证书/时间故障、禁用/重新启用，以及 LED 负载下的内存。灯带处于关闭状态时，颜色/效果工具必须使其保持关闭。

对于许可证，应检查密钥文件缺失、无效和有效的情况；验证现有轮询边界处的状态更新，以及警告/重启序列。在兼容性测试中，发出警告后导入有效密钥必须保留当前待执行重启行为。格式错误/超大文件的测试应作为未来健壮性修复的单独故障注入测试进行。

_您正在与 AI 系统交互_
