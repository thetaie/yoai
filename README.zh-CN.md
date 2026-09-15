# Body Clock

*[English](README.md)*

对话留在你自己的设备上，模型用你自己的 API key 调用 —— 没有 Body Clock
账号，也没有 Body Clock 服务器。

先做 HarmonyOS，再做 Android。

这是 v0 骨架：一个 C++ 核心、一个 HarmonyOS app，远程 OpenAI 兼容模型端到端
跑通；在 HarmonyOS 上，GGUF 模型也能通过 llama.cpp 直接跑在设备本机。接下来
一件事是本地个人数据存储，而核心现在的形状，决定了它是往上加东西，不是推倒
重来 —— 本机推理引擎就是作为同一层 C ABI 背后的第二个 `llm::Client` 加进去的，
它上面的代码不需要知道有两条路。

## 仓库里有什么

```
src/                  核心 —— 一个静态库加一层 C ABI
  bodyclock.h                全部对外接口(约 150 行的契约)
  llm/                  每种 provider 协议一个流式客户端
  client/               HTTP + TLS
  platform/             日志,以及 C ABI 的实现
cli/                  桌面调试壳 —— 不用设备也能跑核心
harmony/              HarmonyOS app(ArkTS)及其 NAPI 桥
```

### 核心是一个库，不是一个服务

没有端口要监听，没有配置文件，也不存会话状态。宿主 app 负责持久化和设置，告诉
核心用户手上有哪些模型的 key，然后请它跑一轮对话：

```c
bodyclock_provider_set("zhipu/glm-4.7-flash",
                  "https://open.bigmodel.cn/api/paas/v4",
                  api_key, "glm-4.7-flash", NULL);

bodyclock_chat("zhipu/glm-4.7-flash", messages_json, NULL, on_event, user_data);
```

有两个后果值得直说，因为它们是设计本身，不是实现细节：

- **key 不落我们的盘。** 注册表就是进程内存。在 HarmonyOS 上，key 是在交给核心
  的那一刻才从系统的加密资产库里取出来的，每次冷启动之后由 app 重新填回注册表。
- **历史归宿主管。** `bodyclock_chat` 每一轮都接收完整的对话记录。只有一个地方决定
  留什么、裁什么，所以不会出现两套策略互相打架。

`bodyclock_chat` 是阻塞的，在调用线程上通过回调流式返回；回调返回 0 就取消这一轮。
它之上的东西 —— 每轮一个工作线程、回到 UI 的 threadsafe function —— 都是宿主
自己的事，这也是这层 ABI 能这么小的原因。

## 构建

### 桌面（用来跑核心）

需要 CMake、一个 C++11 编译器，以及 libcurl / OpenSSL / RapidJSON。`build.sh`
在有 vcpkg checkout 时从 vcpkg 取，没有时从系统包管理器取 ——
`apt install libcurl4-openssl-dev libssl-dev rapidjson-dev`，或者
`brew install openssl@3 rapidjson`。`build.cmd` 需要 vcpkg，反正 Windows 上这
三个依赖也是从那儿来的。

```
build.cmd            # Windows
./build.sh           # macOS / Linux
```

然后不用手机也能跟真实的 provider 对话：

```
rem Windows
set BODYCLOCK_API_KEY=...
build\bodyclock-cli.exe --base-url https://open.bigmodel.cn/api/paas/v4 --model glm-4.7-flash
```

```
# macOS / Linux
export BODYCLOCK_API_KEY=...
./build/bodyclock-cli --base-url https://open.bigmodel.cn/api/paas/v4 --model glm-4.7-flash
```

不带问题参数就进入交互模式，并保留对话记录，所以多轮上下文也能一起验。
`--usage` 打印 token 汇总，`--debug` 打开核心日志。

任何 OpenAI 兼容的 endpoint 用法都一样，只有 URL、key 和模型名不同。Google
Gemini 值得单独写出来，因为它的兼容层放在一个谁也猜不到的路径下：

```
export BODYCLOCK_API_KEY=...          # 从 https://aistudio.google.com/apikey 获取
./build/bodyclock-cli --base-url https://generativelanguage.googleapis.com/v1beta/openai \
                 --model gemini-3.6-flash
```

在 app 里，同一个 key 填在设置 → 添加模型 → **Google Gemini** 下面，URL 和模型
名会预填好，要粘贴的只有 key 本身 —— 而且它进的是系统的加密资产库，不是配置
文件。内置的厂商列表在
[Provider.ets](harmony/entry/src/main/ets/model/Provider.ets)；其中的 `custom`
一项可以填任何不在列表里的 endpoint。

### HarmonyOS

每个 ABI 把原生这两半交叉编译一次，之后改动只要重新打包：

```
cd harmony
build.cmd           # Windows        -- 依赖、本机推理引擎、app
build.cmd hap       #                -- 之后每次只要重新打包

./build.sh          # macOS / Linux
./build.sh hap
```

不带参数的命令需要仓库旁边有一份 llama.cpp checkout（见下）。想编不带本机推理
引擎的 app，就把另外两个目标写出来：`build.cmd deps hap`。

`hap` 走的就是 DevEco 背后那套 hvigor——同样的任务、同样的签名——产物是
`harmony/entry/build/default/outputs/default/entry-default-signed.hap`，可以直接
`hdc install`。IDE 是用来写代码和调试的，不是构建的必需品。想用的时候再用 DevEco
Studio 打开 `harmony/`；签名和 SDK 要求见
[harmony/README.md](harmony/README.md)。

#### 本机模型

app 也可以把 GGUF 跑在手机本机上，全程不联网。这条路在默认构建里就是开的；脚本
唯一不会替你拉的东西就是 llama.cpp 本身：

```
git clone --depth 1 https://github.com/ggml-org/llama.cpp ../llama.cpp
cd harmony
build.cmd             # Windows       -- 或者 `build.cmd llama` 只编引擎
./build.sh            # macOS / Linux
```

代价是 `libbodyclock.so` 多约 4.3 MiB、一份约 200 MB 的 checkout，以及几分钟交叉编译。
`build.cmd deps hap` 编出来的 app 不带它。

checkout 放在仓库旁边（用 `LLAMA_SRC` 可以改位置），而且不会替你更新：这个构建
想要一个你能锁定、能反复对比测量的源码，不是一个在两次运行之间悄悄变掉的。

它会在 `harmony/prebuilt/llama-sdk/<abi>/` 下装配出 SDK，app 的 CMakeLists 会
自己找到 —— **把它编出来就等于打开了这条路**，把它删掉这条路就关上，app 照样能
编。不需要往 `build-profile.json5` 里贴一个只在你自己机器上成立的路径，也就不用
再想办法不让它进提交。

在 app 里是设置 → **本机模型** → 选一个 `.gguf`。文件留在原处：只是被引用，不会
复制进沙箱，删掉模型也不会删文件。加完之后它在选择器里就是一个普通模型。

## 进度

| 能力 | 状态 |
|---|---|
| 远程模型（OpenAI 兼容，流式） | 已完成 |
| 思考过程与回答分离 | 已完成 |
| 每轮的 token / 费用汇总 | 已完成 |
| 对话持久化到设备 | 已完成 |
| 本机推理（llama.cpp + GGUF） | HarmonyOS 上已可用，默认构建即包含 |
| 本地个人数据存储 | 未开始 |
| Android app | 未开始 |
| 工具 / MCP、附件、Markdown 渲染 | 未开始 |

## 关于名字

**Body Clock** 就是英文里说的生物钟。

始终带空格写。字标依赖这个空格：环替掉的是 *Body* 的 `o`，右边那截文字是
`dy Clock`。

唯一的例外是桌面图标下面那行标签 —— `BodyClock`，闭合写。去掉空格能省回大约半个
字符的宽度，实测占标签宽的 5%~6%，而启动器是名字唯一要硬挤进固定宽度的地方。它是
和应用名分开的资源（`EntryAbility_label`，不是 `app_name`），所以别处不跟着改。

需要单词形式的地方一律小写 `bodyclock`：C ABI 的 `bodyclock_` 前缀、
`libbodyclock.so`、包名 `ai.bodyclock.app`。不要出现 `BodyClock` 这种第二种展示写法。

## 许可

MIT —— 见 [LICENSE](LICENSE)。一个个人项目。
