# hustNetworkLogin

华中科技大学校园网（深澜 ePortal）自动登录 —— OpenWrt 插件 + LuCI 界面（C 版）。

掉线 15 秒自动重连，开箱即用，支持 GitHub Actions 云端编译（无需本地搭建 OpenWrt 环境）。

## 关于上游

本项目参考了 [@black-binary/hust-network-login](https://github.com/black-binary/hust-network-login)（Rust 版）的认证流程与密码加密算法，感谢原作者。

> 为什么用 C 重写：OpenWrt 原生就是 C 交叉编译工具链（`$(TARGET_CC)` + musl），用 C 可以做到「menuconfig 选架构 → make 直接交叉编译」，不依赖任何预编译二进制；而 Rust 交叉编译在 OpenWrt 里需要额外工具链，通常只能按架构下载预编译产物。

密码加密算法（`BE(密码 ">" mac) ^ 65537 mod n`）已对照上游官方测试用例**逐字节验证一致**；
实现是自带的 `src/modexp.h`（约 100 行模幂），与 Python 的 `pow(m, e, n)` 做 300+ 组随机差分对拍，CI 每跑必过。

## 特性

- 纯 C 实现，**零第三方库依赖**：HTTP 客户端（`src/http.h`）与 RSA 公钥模幂（`src/modexp.h`）都自带实现，只链接 base system 里的 `libubus` / `libubox`；OpenWrt 原生交叉编译，CI 里也不用从源码编 openssl/curl
- 掉线 15 秒检测并自动重连
- LuCI 网页配置（JS 框架，需 LuCI 23.05+，ImmortalWrt 23.05/25.x 均支持）
- **ubus 接口**：`ubus call hust-network-login status` / `reconnect`（**由 C 守护进程自己注册，不需要 rpcd 插件**），脚本或其它服务也能调
- 界面文案走 LuCI 标准 i18n：英文 msgid + `po/zh_Hans` 翻译包，中文界面由 `luci-i18n-hustNetworkLogin-zh-cn` 提供
- GitHub Actions 云端编译，本地零环境

## 快速开始：fork 编译（推荐，无需本地环境）

### 1. Fork 本仓库

点右上角 **Fork**。

### 2. 改成你自己的路由器架构

打开 `.github/workflows/build.yml`，顶部四个变量默认是**小米路由器 3G（MT7621）**：

```yaml
env:
  SDK_VERSION: "25.12.2"   # ImmortalWrt 版本
  TARGET: "ramips"         # 目标平台
  SUBTARGET: "mt7621"      # 子目标
  GCC: "14.3.0"            # gcc 版本
```

如果路由器不是 MT7621，改成对应的值（常见映射见文末[架构表](#架构映射)）。

### 3. 触发编译

- 直接 push 到 `main` 分支，自动触发；或
- 仓库 **Actions** 标签页 → 左侧 `build` → **Run workflow** 手动触发。

约 5~10 分钟完成。

### 4. 下载产物

Actions → 最近一次运行 → **Summary** → 下载 `hustNetworkLogin` artifact，解压得到三个 `.apk`：

```
hustNetworkLogin_0.2.0-1_mipsel_24kc.apk          # 守护进程（C）
luci-app-hustNetworkLogin_1.0.0-1_all.apk         # LuCI 页面
luci-i18n-hustNetworkLogin-zh-cn_*_all.apk        # 中文界面翻译包（可选）
```

> 不装翻译包时界面是英文（msgid 即英文原文），装了就是中文 —— 这是 LuCI 的标准做法。

### 5. 装到路由器

```sh
scp hustNetworkLogin_*.apk luci-app-hustNetworkLogin_*.apk luci-i18n-hustNetworkLogin-*.apk root@192.168.1.1:/tmp/
ssh root@192.168.1.1
apk add --allow-untrusted /tmp/hustNetworkLogin_*.apk
apk add --allow-untrusted /tmp/luci-app-hustNetworkLogin_*.apk
apk add --allow-untrusted /tmp/luci-i18n-hustNetworkLogin-*.apk      # 中文界面（可选）
/etc/init.d/hust-network-login restart    # 必须：装包不会重启服务，跑着的还是旧二进制
```

> `--allow-untrusted` 是因为本地/CI 编译的包没有官方签名。
> **升级/重装后一定要 `restart`**：OpenWrt 装包不会重启服务，跑着的还是旧二进制；旧版二进制不注册 ubus 对象，页面会显示「服务未运行」。详见[排错](#排错)。

## 本地编译（有 OpenWrt / ImmortalWrt 源码树时）

```sh
cp -r package/hustNetworkLogin      immortalwrt/package/
cp -r luci-app-hustNetworkLogin     immortalwrt/package/
cd immortalwrt

./scripts/feeds update -a && ./scripts/feeds install -a   # 首次，拉 feeds 源码

make menuconfig   # 选架构 + 勾选 Network → hustNetworkLogin，LuCI → Applications → luci-app-hustNetworkLogin
make package/hustNetworkLogin/compile V=s
make package/luci-app-hustNetworkLogin/compile V=s
```

### 自测（CI 全跑）

```sh
python3 luci-app-hustNetworkLogin/test/check.py     # 默认值三处一致 / 翻译覆盖 / JSON+JS 语法
node    luci-app-hustNetworkLogin/test/view-test.js # 视图逻辑回归（桩掉 LuCI 环境，跑真实视图源码）
cc package/hustNetworkLogin/test/test_encrypt.c -o /tmp/test_encrypt && /tmp/test_encrypt   # 加密 KAT（上游向量）
python3 package/hustNetworkLogin/test/modexp-diff.py # 加密随机差分（对拍 Python pow）
cc package/hustNetworkLogin/test/http_test.c -o /tmp/http_test && /tmp/http_test            # HTTP 解析单测（URL/头体边界）
sh package/hustNetworkLogin/test/ubus-e2e.sh        # 端到端：真 ubusd + 真 HTTP + 本机假门户
```

`view-test.js` 里 `rpc.declare` 的 `expect` 处理是**逐字照搬** `luci-static/resources/rpc.js`
的 —— 视图拿到的到底是「回复对象」还是被拆开的某个字段，全由那一小段决定（`expect` 写错时
重连成功也会显示成 `unknown error`），桩不忠实就测不出这类问题。

`ubus-e2e.sh` 会自己下载并编译 json-c / libubox / ubus（首次约 1~2 分钟，缓存在 `/tmp/hust-ubus-e2e`），
然后在 `unshare -r` 里以 uid 0 跑真 ubusd —— 因为 **ubusd 只允许 uid 0 发布对象**，这是离线验证注册代码的唯一办法。
环境不允许用户命名空间时它会打印 `SKIP` 退出（CI 与本地都不会因此变红）。

## 使用

打开 LuCI（`http://192.168.1.1`）→ **服务 → HUST Network Login**（中文界面下显示为「华中科技大学校园网登录」），填学号、密码，勾选「启用自动登录」，保存。

**状态区**（页面每几秒自动刷新，只有两行）：

| 项目 | 说明 |
|------|------|
| 连接状态 | 启动中 / 探测中 / 认证中 / 已在线 / 登录失败，重试中 / 服务未运行（读不到 ubus 对象时） |
| 最近错误 | 按下述优先级显示：**缺少必填项提醒** → 守护进程上报的最近错误（如 `all probe urls failed`、`login rejected: ...`）→ RPC 失败原因（读不到 ubus 对象时写明怎么启动服务） |

**缺项提醒**：必填项与 init 脚本的启动条件逐条对齐 —— **启用自动登录 + 用户名 + 密码**，缺任何一项守护进程都不会启动：

- 「最近错误」行直接写 `缺少必填项：用户名、密码 —— 填好后点「保存并应用」。`（按「启用 → 用户名 → 密码」顺序列出缺的项）
- 缺项提醒优先级最高：它一定比历史错误更有用（缺项时守护进程根本没机会启动），所以会盖住守护进程上报的旧错误
- 点「重连」也会先提示缺什么，并且**不会**白跑一次 RPC

**操作按钮**：

| 按钮 | 功能 |
|------|------|
| 重连 | 立刻重新认证：给守护进程发 `SIGHUP`，中断正在进行的请求并马上重跑一轮（**不重启进程**）；被其它设备挤下线、或换了网络环境时用。**改了配置请用「保存并应用」（会重启服务以套用新配置）** |

> 服务未启用时点「重连」会提示先启用并保存配置。刚改过配置请先「保存并应用」，重连用的是已保存的配置。
> 点「重连」成功会弹一条提示，状态区也会自己刷新成「认证中 → 已在线」；失败或配置缺项则给出对应的告警文案。
> 服务没在跑（ubus 里没有 `hust-network-login` 对象）时，状态区会显示「服务未运行」并**禁用**「重连」按钮 —— 见文末[排错](#排错)。

命令行等价：

```sh
uci set hust-network-login.main.username='M2020123123'
uci set hust-network-login.main.password='你的密码'
uci set hust-network-login.main.enabled='1'
uci commit hust-network-login && /etc/init.d/hust-network-login reload   # 改配置后重启以生效
logread | grep -i hust   # 看日志
```

ubus 接口（脚本、其它服务也能用）：

```sh
ubus call hust-network-login status       # 连接状态、最近错误、服务是否启用/运行
ubus call hust-network-login reconnect    # 立刻重新认证（同界面「重连」按钮，不重启进程）
```

### 可配置项

| UCI 项 | 默认值 | 说明 |
|---|---|---|
| `test_url` | `http://connect.rom.miui.com/generate_204` | 在线探测地址，留空用默认。默认用轻量 204 探测（在线时返回空响应，省流量），可改成 `http://www.baidu.com` 等。**只支持 `http://`**（自带 HTTP 实现不做 TLS），写 `https://` 会记一条日志并跳过 |
| `check_interval` | `15` | 在线时的检测周期（秒）。掉线是立即重连，此值只影响「在线时多久探测一次」 |

```sh
uci set hust-network-login.main.test_url='http://www.baidu.com'
uci set hust-network-login.main.check_interval='30'
uci commit hust-network-login && /etc/init.d/hust-network-login reload
```

## 排错

**先看这两个地方**：

```sh
ubus list | grep hust-network-login                 # 对象在不在总线上
cat /tmp/run/hust-network-login.state               # 守护进程自己写的运行状态
logread -e hust-network-login | tail -20            # 它的日志
```

`/tmp/run/hust-network-login.state` 里的 `ubus=` 字段就是控制面的自检结果：

| ubus= | 含义 |
|-------|------|
| `registered` | 对象已发布，页面应该能正常读到状态、重连也能用 |
| `unavailable` | 连不上 ubusd（**会自动每 5 秒重试**，ubusd 起来后自己就恢复了） |
| `reconnecting` | 与 ubusd 的连接断过（比如 ubusd 重启），正在自动重连并重新发布对象 |
| `no-object(N)` | ubusd 拒绝注册，N 是它返回的错误码（6 = 权限不足） |

**症状一：页面显示「服务未运行」，点「重连」报 `Object not found`**

`-32000 Object not found` 是 uhttpd 的 ubus 插件在总线上找不到这个对象时报的（这一步在权限校验之前），所以它只有一个含义：**守护进程没在跑**，不是权限问题。常见原因和对应处理：

| 原因 | 处理 |
|------|------|
| **升级/重装 apk 后没有重启服务**（OpenWrt 装包不会自动重启服务，跑着的还是旧二进制；旧版二进制不注册 ubus 对象，页面就找不到它） | `/etc/init.d/hust-network-login restart`（或直接重启路由器） |
| 服务没启用，或没填账号/密码（init 脚本缺这两样就根本不启动进程） | 界面勾选「启用自动登录」+ 填账号密码 →「保存并应用」；页面「最近错误」行会直接写缺少哪一项（见[上文缺项提醒](#使用)） |
| 进程崩了 / 被手动 kill 掉 | `logread -e hust-network-login \| tail -30` 看原因，再 `/etc/init.d/hust-network-login start` |

```sh
/etc/init.d/hust-network-login restart              # 升级后 / 排错第一步
ubus call hust-network-login status                 # 起来了就能直接读状态
```

**症状二：页面一直「未知 / 登录失败，重试中」**

看 `最近错误` 行和 `logread -e hust-network-login`：

| 日志 | 含义与处理 |
|------|-----------|
| `all probe urls failed` | 探测地址都连不上（没网 / 需要门户认证但门户没响应）。可换 `test_url`，见[可配置项](#可配置项) |
| `extract portal_ip failed` / `extract mac failed` | 门户返回的页面结构变了（学校改了认证页面），需要更新 `src/main.c` 里的解析规则 |
| `login rejected: ...` | 账号密码错、或已在别处登录被挤下线；冒号后是门户原话 |
| `login request failed` | 门户地址能解析但请求发不出去（网关/防火墙问题） |

**症状三：点「重连」弹 `Reconnect failed: unknown error`**

守护进程其实受理了请求，是**旧版页面把回复解析错了**：`rpc.declare()` 的 `expect` 写成了
`{ result: false, ... }`，rpc.js 只会取第一个 key，于是把回复里的 `result` 布尔值当成整个返回值，
`res.result` 变成 undefined → 成功也走失败分支。`luci-app-hustNetworkLogin` **1.0.0-4** 起已修
（`expect: { '': {} }`，与 `status` 一致），升级 luci-app 包后强刷页面（Ctrl+F5，清 JS 缓存）即可。
判据：`ubus call hust-network-login reconnect` 能看到 `"result": true`，而页面偏说失败。

## 目录结构

```
.
├── package/hustNetworkLogin/          # C 软件包
│   ├── Makefile                       #   标准 C 包，$(TARGET_CC) 交叉编译
│   ├── src/main.c                     #   C 源码（登录 + 检测 + ubus 控制面）
│   ├── src/http.h                     #   明文 HTTP 客户端（自带，替代 libcurl）
│   ├── src/modexp.h                   #   RSA 公钥模幂（自带，替代 OpenSSL）
│   ├── test/test_encrypt.c            #   加密 KAT（上游向量 + 边界用例）
│   ├── test/modexp-diff.py            #   加密随机差分（对拍 Python pow）
│   ├── test/http_test.c               #   HTTP 解析单测（URL / 头体边界 / Content-Length）
│   ├── test/fake-portal.py            #   端到端用的本机假门户（校验登录表单里的密文）
│   ├── test/ubus-e2e.sh + stubs/      #   端到端：真 ubusd + 真 HTTP（stubs 只剩 syslog 桩）
│   └── files/
│       ├── etc/init.d/hust-network-login   # procd 服务脚本
│       └── etc/config/hust-network-login   # UCI 默认配置
├── luci-app-hustNetworkLogin/         # LuCI 插件（JS 版）
│   ├── Makefile
│   ├── htdocs/luci-static/resources/view/hustNetworkLogin.js
│   ├── po/zh_Hans/luci-app-hustNetworkLogin.po   # 中文翻译（英文 msgid → 中文）
│   ├── test/check.py                  #   一致性自检（默认值/翻译/JSON/JS 语法）
│   ├── test/view-test.js              #   视图逻辑回归（node，含 reconnect 回复解析）
│   └── root/
│       ├── usr/share/luci/menu.d/luci-app-hustNetworkLogin.json
│       ├── usr/share/rpcd/acl.d/luci-app-hustNetworkLogin.json   # 权限（uci + ubus status/reconnect）
└── .github/workflows/build.yml        # GitHub Actions 云端编译
```

## 架构映射

fork 后按自己路由器改 workflow 顶部 4 个变量（`TARGET` / `SUBTARGET` 尤其重要）：

| 路由器 | TARGET / SUBTARGET |
|---|---|
| 小米 3G / 4A 千兆 / AC2100 等（MT7621） | `ramips` / `mt7621` |
| 小米 4A 百兆 / 4C 等（MT7628） | `ramips` / `mt76x8` |
| 红米 AC2100（MT7621） | `ramips` / `mt7621` |
| x86_64 软路由 | `x86` / `64` |
| 树莓派 4 | `bcm27xx` / `bcm2711` |
| 斐讯 N1 | `rockchip` / `armv8`（或对应） |

> 不确定时：在 [ImmortalWrt 固件下载站](https://downloads.immortalwrt.org) 找到你的机型，看它在 `releases/<版本>/targets/<TARGET>/<SUBTARGET>/` 的哪一层，把这两段填进去即可。`GCC` 版本看该目录下 `immortalwrt-sdk-*.tar.zst` 文件名里的 `gcc-xx.x.x`。

## 说明

- **状态与控制的实现**：C 守护进程自己注册 ubus 对象 `hust-network-login`（`uloop` + `libubus`，方法 `status` / `reconnect`）。登录循环跑在单独的 worker 线程，主线程只跑事件循环 —— 所以**进程在，对象就在**，不需要 rpcd 的 ucode 插件（那类插件一旦没加载，页面就彻底失能而且看不出原因）。控制面自带自愈：连不上 ubusd（或 ubusd 中途重启、对象从总线消失）时，每 5 秒重连一次并重新发布对象 —— 页面上就是「服务未运行 → 自己恢复」的过程，不需要人工干预；`reconnect` 用 `pthread_kill(worker, SIGHUP)` 复用现成的打断链路（`CURLOPT_XFERINFOFUNCTION` 中止传输 + `sleep_interruptible` 提前返回），秒级生效、不重启进程、认证与加密代码零改动。状态同时原子写入 `/tmp/run/hust-network-login.state`（tmpfs，重启清空）供 SSH 直接查看 —— 其中 `ubus=` 字段（`registered` / `unavailable` / `reconnecting` / `no-object(N)`）就是控制面自检结果；PID 写 `/tmp/run/hust-network-login.pid`。
- **两条路径别混**：① 只想立刻重新认证（配置没变）→ 界面「重连」/ `ubus call hust-network-login reconnect`，守护进程收到 `SIGHUP`，经 libcurl 进度回调中断当前请求并马上重跑一轮（**不重启进程**）；② 改了配置（账号/密码/探测地址/检测周期）→ 「保存并应用」或 `uci commit ... && /etc/init.d/hust-network-login reload`，这条会**重启服务**，因为配置是启动时通过环境变量注入进程的。`SIGTERM`/`SIGINT` 会写 `state=stopped`、删 pidfile 后干净退出（配合 procd）。详细过程看 syslog：`logread -e hust-network-login`。
- **包名/显示名是小驼峰 `hustNetworkLogin`**；但 UCI config 名、二进制名、init.d 脚本名保持 kebab-case `hust-network-login`（OpenWrt 系统机制约定）。
- **LuCI 版本**：JS 框架需 LuCI 23.05+。
