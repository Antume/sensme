# sensme

终端版 SensMe：分析 mp3 的节奏/响度/音色/调性，把歌分到 7 个情绪文件夹，
并连同同名 `.lrc` 一起复制。源目录只读，不会修改或删除源文件。

当前版本：**0.1**

- 不知道命令行怎么写？直接运行 `sensme`，它会问你要分析哪个文件夹、
  结果放到哪里。
- 支持 Linux（x86_64 / arm64）、Android（Termux）、Windows（x86_64）。

## 下载现成版本

到 [Releases](../../releases) 下载对应文件，不需要编译：

| 平台 | 文件 | 说明 |
|---|---|---|
| Linux x86_64 | `sensme-0.1-linux-x86_64` | 静态链接，任何发行版都能跑 |
| Linux arm64 | `sensme-0.1-linux-aarch64` | 树莓派、ARM 服务器，**也是 Android 用的那个** |
| Windows x86_64 | `sensme-0.1-windows-x86_64.exe` | 静态链接，无需装 DLL |

Linux：

```sh
chmod +x sensme-0.1-linux-x86_64
./sensme-0.1-linux-x86_64
sudo install -m755 sensme-0.1-linux-x86_64 /usr/local/bin/sensme   # 可选
```

Windows：双击 exe，或在 PowerShell / cmd 里运行。控制台里可以直接把
文件夹**拖进窗口**自动粘贴路径。

### Android

Android 本身不能直接运行 Linux 二进制（内核之外是 bionic，不是 glibc），
需要先装一个终端环境 [Termux](https://f-droid.org/packages/com.termux/)，
然后在 Termux 里跑 **arm64 静态版**——这正是它能脱离 glibc 独立运行的原因：

```sh
# 在 Termux 中
termux-setup-storage          # 允许访问 /sdcard（只弹一次授权）
mkdir -p ~/bin
cp ~/storage/downloads/sensme-0.1-linux-aarch64 ~/bin/sensme
chmod +x ~/bin/sensme
sensme                        # 交互模式：输入 ~/storage/shared/Music
```

分析 300 首大约 2 分钟（手机 CPU 会慢一些），输出目录选在
`~/storage/shared/Music/Music-SensMe` 就能被其他音乐 App 看到。

## 用法

```sh
sensme                              # 交互模式：询问源目录 / 输出目录
sensme -s DIR -o OUT                # 直接指定，不提问
sensme -n                           # 只分析，不复制
sensme -r ~/Music/Music-SensMe      # 原地重排已有输出（移动，不复制）
sensme -O myrules.tsv               # 追加人工规则
sensme -V                           # 版本
sensme -h                           # 完整帮助
```

不带参数时默认是 `~/Music` → `~/Music/Music-SensMe`（交互模式会先确认）。
已有文件会跳过；`-f` 强制覆盖。`-r` 在同一文件系统内用 rename，
305 首约 2 分钟（只有分析耗时，移动是瞬时的）。

## 编译

```sh
make            # 需要 libmpg123 开发包，例如 pacman -S libmpg123
make STATIC=1   # 单文件、不依赖系统库（需要静态 libmpg123）
```

交叉编译脚本（CI 用的就是它们）：

```sh
# 静态 Linux 版（x86_64 / arm64），在 Docker/Podman 里跑
docker run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out" \
  alpine:3.20 sh /src/scripts/build-musl.sh native x86_64 /out
docker run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out" \
  alpine:3.20 sh /src/scripts/build-musl.sh aarch64-linux-musl aarch64 /out

# Windows 版（Debian/Ubuntu/WSL 上，先装 mingw-w64）
sh scripts/build-windows.sh dist
```

版本号在 `Makefile` 里（`VERSION ?= 0.1`）。打 `v0.1` 这样的 tag 并推送，
GitHub Actions 会自动编译三个平台并把产物挂到 Release 上。

## 人工修正：overrides

每行 `匹配串<TAB>情绪`，匹配串是相对路径的大小写不敏感子串：

```
Flower Dance	03_Calm
米津玄師	06_Emotional
アイネクライネ	06_Emotional
```

默认读取 `~/.config/sensme/overrides.tsv`（存在时）；也可用 `-O FILE`。
情绪可写 `03_Calm`、`3` 或 `Calm`。

## 分类原理

- libmpg123 解码歌曲中段 90 s，2048 点 FFT，50% 重叠
- 特征：BPM（自相关）、RMS 响度（dBFS）、频谱重心、频谱通量、
  过零率、谱平坦度、动态范围 p95/p10、低频/高频能量占比、
  onset 密度、节拍清晰度、chroma 调性（大/小调）
- 每个特征用固定锚点映射到 0..1（不再按曲库分位数归一化），
  再合成二维情绪平面：x=愉悦度，y=强度
- 最近情绪中心 + 规则修正：
  - 安静的钢琴/器乐（高频少、噪声低、音色偏暗）→ Calm / Sad；
    这类曲子琶音多，不能按"强度"判成 Dark/Energetic
  - Dark/Intense 必须有真正的强度 + 噪声/打击感，否则退回 Sad/Emotional
  - 中速且不够明亮的"Energetic"→ Emotional（避免所有流行歌挤进 Energetic）
- 结果不再强制每类数量平均；某些类少是正常的（本曲库几乎没有暗黑重型）

## 上次全库结果（305 首）

| 文件夹 | 数量 | 平均 BPM |
|---|---|---|
| 01_Happy_Upbeat | 29 | 103 |
| 02_Energetic | 86 | 104 |
| 03_Calm | 11 | 99 |
| 04_Sad_Melancholy | 90 | 105 |
| 05_Dark_Intense | 2 | 95 |
| 06_Emotional | 76 | 91 |
| 07_Bright | 11 | 101 |

在 101 首人工标注的抽样上，规则版准确率约 83%（7 类，随机基线 14%）。
剩下主要是 Happy/Energetic/Bright 之间的边界，以及抒情歌属于
Sad 还是 Emotional——这类判断本身带主观性，用 overrides 修正最快。

## 局限

- 只用信号处理特征，没有语义模型。人声、歌词、编曲意图都听不到，
  所以"燃""温柔""伤感"的细微差别会有误判。要再上一个台阶需要预训练
  音频模型（如 MTG-Jamendo / DEAM 的 arousal-valence 模型），代价是
  几百 MB 依赖和更慢的分析速度。
- Windows 版走系统 ANSI 代码页：中文系统的 GBK 能覆盖中日文曲名，
  但韩文、emoji 等超出代码页的文件名可能被跳过。这类曲库建议用 WSL 跑
  Linux 版。Windows 版也还没有签名，首次运行 SmartScreen 会提示"未知发布者"。
- 只支持 mp3（不含 flac/m4a/ogg），Windows 版只提供 64 位。
