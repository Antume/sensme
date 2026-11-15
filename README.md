# sensme

mp3 情绪分类工具。读取音轨特征，判断属于七类情绪中的哪一类，按类别复制到对应
目录，源文件只读，不做修改。

类别目录：`01_Happy_Upbeat`、`02_Energetic`、`03_Calm`、`04_Sad_Melancholy`、
`05_Dark_Intense`、`06_Emotional`、`07_Bright`。

支持 Linux、Windows、Android（Termux），当前版本 0.1。

## 下载

Releases 提供预编译文件，无需自行编译。

| 平台 | 文件 | 说明 |
|---|---|---|
| Linux x86_64 | `sensme-0.1-linux-x86_64` | 静态链接，不依赖发行版与 glibc 版本 |
| Linux aarch64 | `sensme-0.1-linux-aarch64` | ARM 服务器、树莓派 |
| Windows x86_64 | `sensme-0.1-windows-x86_64.exe` | 无需额外 DLL |

Linux 下 `chmod +x` 后直接运行，需要全局安装时复制到 `/usr/local/bin`。
Windows 控制台支持把文件夹拖入窗口，自动填入路径。

### Android

Android 使用 bionic 而非 glibc，普通 Linux 二进制无法运行，需要 Termux 加
aarch64 静态版本：

```sh
termux-setup-storage
mkdir -p ~/bin
cp ~/storage/downloads/sensme-0.1-linux-aarch64 ~/bin/sensme
chmod +x ~/bin/sensme
~/bin/sensme
```

Termux 的 PATH 不含 `~/bin`，所以用 `~/bin/sensme` 调用。想直接敲 `sensme`：

```sh
echo 'export PATH="$HOME/bin:$PATH"' >> ~/.bashrc && . ~/.bashrc
```

不要放在 `~/storage`、`/sdcard` 下运行：共享存储是 noexec，`chmod` 无效。
源目录一般位于 `~/storage/shared/` 下；输出目录同样放在共享存储内，其他播放器
才能读取。手机端分析较慢，数百首需要数分钟。

## 用法

| 命令 | 说明 |
|---|---|
| `sensme` | 交互模式，询问源目录与输出目录 |
| `sensme -s DIR -o DIR` | 指定源目录与输出目录 |
| `sensme -n` | 只输出分类结果，不复制文件 |
| `sensme -r DIR` | 整理已有输出目录，移动而不复制 |
| `sensme -O FILE` | 使用指定的规则文件 |
| `sensme -f` | 覆盖输出目录中已存在的文件 |
| `sensme -V` | 版本号 |
| `sensme -h` | 完整帮助 |

无参数时默认源目录 `~/Music`、输出目录 `~/Music/Music-SensMe`，交互模式会先
确认。输出目录中已存在的文件默认跳过。

## 分类规则覆盖

自动分类存在误差，可用规则文件修正。每行一条，匹配串与类别以 TAB 分隔：

```
Flower Dance	03_Calm
米津玄師	06_Emotional
```

匹配串为相对路径的子串，不区分大小写；类别可写 `03_Calm`、`3` 或 `Calm`。
默认读取 `~/.config/sensme/overrides.tsv`（存在时），也可用 `-O` 指定其他文件。

## 判定方式

用 libmpg123 解码每首歌中间 90 秒，做 2048 点 FFT、50% 重叠窗口，提取以下特征：

- BPM（自相关）、RMS 响度、动态范围 p95/p10
- 频谱重心、频谱通量、谱平坦度、过零率
- 低频与高频能量占比、onset 密度、节拍清晰度
- chroma 调性（区分大小调）

特征按固定锚点映射到 0..1，不依赖曲库分位数，再合成愉悦度与强度两个维度，取
最近的类别中心，并按规则修正：

- 安静钢琴曲偏向 Calm / Sad，避免因子音密集被判为 Dark / Energetic
- Dark / Intense 需要足够的强度与打击感，否则降为 Sad / Emotional
- 中速且偏暗的 Energetic 归入 Emotional

各类别数量不做均衡，分布不均可正常。人工标注测试集上与自动分类的一致率约
83%，随机分配约 14%；误差集中在 Happy / Energetic / Bright 的边界，以及 Sad
与 Emotional 的区分。

## 已知问题

- 仅支持 mp3，不支持 flac、m4a、ogg
- Windows 版文件名使用系统 ANSI 代码页。GBK 环境可覆盖中文与日文曲名，韩文、
  emoji 等超出代码页的标题会被跳过；此类曲库建议用 WSL 运行 Linux 版
- Windows 版未签名，首次运行会出现 SmartScreen 未知发布者提示
- 仅使用信号处理，无语义模型，不分析人声与歌词。细分情绪（如“燃”与“温柔”）
  

## 编译

```sh
make            # 需要 libmpg123 开发包
make STATIC=1   # 单文件静态链接，不依赖系统库
```

三个平台的构建脚本位于 `scripts/`：

```sh
# 静态 x86_64 版，在 Docker / Podman 中执行
docker run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out" \
  alpine:3.20 sh /src/scripts/build-musl.sh x86_64 /out

# 静态 aarch64 版，Zig 自带 musl 交叉编译，需要 pkg-config、xz、bzip2
sh scripts/build-linux-aarch64.sh dist

# Windows 版，需要 mingw-w64
sh scripts/build-windows.sh dist
```

版本号写在 `Makefile` 中。推送 `v0.1` 形式的 tag 会触发 GitHub Actions，构建
三个平台并发布 Release。
