# hustNetworkLogin

华中科技大学校园网（深澜 ePortal）自动登录 —— OpenWrt 插件 + LuCI 界面（C 版）。

掉线 15 秒自动重连，开箱即用，支持 GitHub Actions 云端编译（无需本地搭建 OpenWrt 环境）。

## 关于上游

本项目参考了 [@black-binary/hust-network-login](https://github.com/black-binary/hust-network-login)（Rust 版）的认证流程与密码加密算法，感谢原作者。

> 为什么用 C 重写：OpenWrt 原生就是 C 交叉编译工具链（`$(TARGET_CC)` + musl），用 C 可以做到「menuconfig 选架构 → make 直接交叉编译」，不依赖任何预编译二进制；而 Rust 交叉编译在 OpenWrt 里需要额外工具链，通常只能按架构下载预编译产物。

密码加密算法（`BE(密码 ">" mac) ^ 65537 mod n`）已对照上游官方测试用例**逐字节验证一致**。

## 特性

- 纯 C 实现（`libcurl` + `libopenssl`），OpenWrt 原生交叉编译
- 掉线 15 秒检测并自动重连
- LuCI 网页配置（JS 框架，需 LuCI 23.05+，ImmortalWrt 23.05/25.x 均支持）
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

Actions → 最近一次运行 → **Summary** → 下载 `hustNetworkLogin` artifact，解压得到两个 `.apk`：

```
hustNetworkLogin_0.2.0-1_mipsel_24kc.apk
luci-app-hustNetworkLogin_1.0.0-1_all.apk
```

### 5. 装到路由器

```sh
scp hustNetworkLogin_*.apk luci-app-hustNetworkLogin_*.apk root@192.168.1.1:/tmp/
ssh root@192.168.1.1
apk add --allow-untrusted /tmp/hustNetworkLogin_*.apk
apk add --allow-untrusted /tmp/luci-app-hustNetworkLogin_*.apk
```

> `--allow-untrusted` 是因为本地/CI 编译的包没有官方签名。

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

## 使用

打开 LuCI（`http://192.168.1.1`）→ **服务 → hustNetworkLogin**，填学号、密码，勾选「启用自动登录」，保存。

命令行等价：

```sh
uci set hust-network-login.main.username='M202674581'
uci set hust-network-login.main.password='你的密码'
uci set hust-network-login.main.enabled='1'
uci commit hust-network-login && /etc/init.d/hust-network-login reload
logread | grep -i hust   # 看日志
```

## 目录结构

```
.
├── package/hustNetworkLogin/          # C 软件包
│   ├── Makefile                       #   标准 C 包，$(TARGET_CC) 交叉编译
│   ├── src/main.c                     #   C 源码（登录 + RSA + 检测）
│   ├── test/test_encrypt.c            #   加密自测（对照上游用例）
│   └── files/
│       ├── etc/init.d/hust-network-login   # procd 服务脚本
│       └── etc/config/hust-network-login   # UCI 默认配置
├── luci-app-hustNetworkLogin/         # LuCI 插件（JS 版）
│   ├── Makefile
│   ├── htdocs/luci-static/resources/view/hustNetworkLogin.js
│   └── root/usr/share/luci/menu.d/luci-app-hustNetworkLogin.json
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

- **包名/显示名是小驼峰 `hustNetworkLogin`**；但 UCI config 名、二进制名、init.d 脚本名保持 kebab-case `hust-network-login`（OpenWrt 系统机制约定）。
- **LuCI 版本**：JS 框架需 LuCI 23.05+。
- **默认网关**：认证程序只负责「登录」，解决不了「WAN 口没有默认路由」——那是网络配置，与认证无关。
