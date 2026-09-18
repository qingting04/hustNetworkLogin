# hustNetworkLogin — OpenWrt 插件 + LuCI 界面（C 版，源码编译）

华中科技大学校园网（深澜 ePortal）自动登录的 OpenWrt 集成包。

- 核心程序：**C 语言实现**（`src/main.c`），用 libcurl 做 HTTP、libcrypto 做 RSA，掉线 15 秒自动重连
- 本包：OpenWrt 软件包 + procd 服务 + LuCI 网页配置（LuCI 23.05+ JS 框架）
- 加密算法与上游 Rust 版逐字节一致（已对照其官方测试用例验证）

## 为什么用 C 重写

OpenWrt **原生就是 C 交叉编译工具链**（`$(TARGET_CC)` + musl），用 C 写意味着：

- `make menuconfig` 选好架构 → `make` 就是**真正的源码交叉编译**，全程不下载任何预编译二进制
- 依赖只有 `libcurl` + `libopenssl`（OpenWrt 现成软件包）

## 目录结构

```
.
├── package/hustNetworkLogin/           # C 软件包
│   ├── Makefile                          #   标准 C 包，$(TARGET_CC) 交叉编译
│   ├── src/main.c                        #   C 源码（登录 + RSA + 检测）
│   └── files/
│       ├── etc/init.d/hust-network-login #   procd 服务脚本
│       └── etc/config/hust-network-login #   UCI 默认配置
└── luci-app-hustNetworkLogin/          # LuCI 插件（JS 版）
    ├── Makefile
    ├── htdocs/luci-static/resources/view/
    │   └── hustNetworkLogin.js         #   JS 视图模块（view.extend + form.Map）
    └── root/usr/share/luci/menu.d/
        └── luci-app-hustNetworkLogin.json  # 菜单「服务 → 校园网登录」
```

## 编译（放进 immortalwrt 源码树）

```sh
# 1. 放进源码树
cp -r package/hustNetworkLogin      immortalwrt/package/
cp -r luci-app-hustNetworkLogin     immortalwrt/package/
cd immortalwrt

# 2. 更新安装 feeds（首次需要，为 luci.mk 和 libcurl/libopenssl 依赖）
./scripts/feeds update -a
./scripts/feeds install -a

# 3. 选目标架构（小米3G = Target System: MediaTek Ralink MIPS → MT7621）
make menuconfig
#    Network → hustNetworkLogin
#    LuCI → Applications → luci-app-hustNetworkLogin

# 4. 编译（原生交叉编译，自动出对应架构 ipk）
make package/hustNetworkLogin/compile V=s
make package/luci-app-hustNetworkLogin/compile V=s
```

生成的 ipk 在 `bin/packages/*/base/` 下，传到路由器 `opkg install` 即可。

## 使用

LuCI →「服务 → 校园网登录」：填学号、密码，勾选「启用自动登录」，保存。

命令行等价：

```sh
uci set hust-network-login.main.username='M202674581'
uci set hust-network-login.main.password='密码'
uci set hust-network-login.main.enabled='1'
uci commit hust-network-login && /etc/init.d/hust-network-login reload
logread | grep -i hust
```

## 手动安装（备选，不编译 ipk）

若只想快速装到现有 ImmortalWrt，先编译/拿到 `hust-network-login` 二进制，再用 `install.sh`：

```sh
scp hust-network-login root@192.168.1.1:/tmp/
scp install.sh root@192.168.1.1:/tmp/
ssh root@192.168.1.1 "sh /tmp/install.sh /tmp/hust-network-login"
```

## 配置方式

程序支持两种配置来源（优先级从高到低）：

1. 命令行配置文件（两行：用户名、密码）：`hust-network-login /etc/xxx.conf`
2. 环境变量：`HUST_NETWORK_LOGIN_USERNAME` / `HUST_NETWORK_LOGIN_PASSWORD`（procd 脚本用这种）

## 说明

- **LuCI 版本**：JS 框架需 LuCI 23.05+（ImmortalWrt 23.05/25.x 均支持）。
- **默认网关**：认证程序只负责「登录」，解决不了「WAN 口没有默认路由」——那是网络配置，与认证无关。
