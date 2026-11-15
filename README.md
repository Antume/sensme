# sensme

把 mp3 按情绪分到 7 个文件夹里。先分析节奏、响度、音色、调性，再猜这首歌是
Happy、Calm 还是 Sad，然后连同同名的 `.lrc` 一起复制过去。原文件不动，只读。

命令行不会用也没关系，直接运行 `sensme`，它会问你两个问题：歌在哪个文件夹，
结果放哪里。

版本 0.1，支持 Linux、Windows 和 Android(Termux)。

## 下载

不用编译，Releases 里拿现成的：

| 平台 | 文件 |
|---|---|
| Linux x86_64 | `sensme-0.1-linux-x86_64`，静态编译，哪个发行版都能跑 |
| Linux arm64 | `sensme-0.1-linux-aarch64`，树莓派和 ARM 服务器用这个 |
| Windows x86_64 | `sensme-0.1-windows-x86_64.exe`，双击就能用，不用装 DLL |

Linux 上 `chmod +x` 之后就能跑，想装进系统就 `install` 到 `/usr/local/bin`。
Windows 版可以在控制台里把文件夹直接拖进窗口，路径会自动粘贴。

### Android

Android 虽然用 Linux 内核，但 C 库是 bionic 不是 glibc，普通 Linux 程序拖进去
是跑不起来的，得先装 Termux。装好之后用 arm64 那个静态版本：

```sh
# 以下命令在 Termux 里执行
termux-setup-storage          # 授权访问 /sdcard,只弹一次
mkdir -p ~/bin
cp ~/storage/downloads/sensme-0.1-linux-aarch64 ~/bin/sensme
chmod +x ~/bin/sensme
sensme
```

源目录填 `~/storage/shared/Music`，输出目录也放在共享存储里，别的播放器才看得到。
手机 CPU 慢一些，300 首要几分钟。

## 用法

```sh
sensme                        # 交互模式,问路径
sensme -s ~/Music -o ~/out    # 直接指定
sensme -n                     # 只看分类结果,不复制文件
sensme -r ~/out               # 重新整理已有的输出目录(移动,不复制)
sensme -O myrules.tsv         # 加自己的规则
sensme -V                     # 版本号
sensme -h                     # 完整帮助
```

不带参数时默认 `~/Music` → `~/Music/Music-SensMe`，交互模式会先确认一遍。
输出目录里已经有的文件会跳过，`-f` 才覆盖。用 `-r` 整理已有目录是 rename，
不复制文件，305 首大概两分钟，时间基本都花在分析上。

## 手动修正

自动分类肯定有错，改规则比重新跑省事。规则一行一条，匹配串和情绪之间用 TAB 隔开：

```
Flower Dance	03_Calm
米津玄師	06_Emotional
アイネクライネ	06_Emotional
```

匹配串是相对路径的子串，不分大小写。情绪写 `03_Calm`、`3` 或者 `Calm` 都行。
默认读 `~/.config/sensme/overrides.tsv`，存在才读，也可以用 `-O` 指定别的文件。

## 它是怎么分的

用 libmpg123 解码每首歌中间 90 秒，做 2048 点 FFT，窗口重叠 50%，然后算这些特征：

- BPM(自相关)、RMS 响度、动态范围 p95/p10
- 频谱重心、频谱通量、谱平坦度、过零率
- 低/高频能量占比、onset 密度、节拍清晰度
- chroma 调性(大致判断大调小调)

每个特征用固定的锚点映射到 0..1，不再按曲库分位数归一化，再合成一个二维平面：
x 是愉悦度，y 是强度。取最近的几个情绪中心，然后按规则修正：

- 安静的钢琴曲往 Calm / Sad 走。这种曲子琶音多，按“强度”很容易判成
  Dark / Energetic，所以要单独压一下
- Dark / Intense 必须有真正的强度和打击感，否则退回 Sad / Emotional
- 中速又不够亮的 Energetic 改成 Emotional，不然所有流行歌都挤在 Energetic

不强制每类数量平均，所以有的类少是正常的，我这个曲库几乎就没有暗黑重型。

## 实际效果

305 首的结果：

| 文件夹 | 数量 | 平均 BPM |
|---|---|---|
| 01_Happy_Upbeat | 29 | 103 |
| 02_Energetic | 86 | 104 |
| 03_Calm | 11 | 99 |
| 04_Sad_Melancholy | 90 | 105 |
| 05_Dark_Intense | 2 | 95 |
| 06_Emotional | 76 | 91 |
| 07_Bright | 11 | 101 |

抽了 101 首人工标注过，准确率大概 83%，随机分的话是 14%。错的基本集中在
Happy / Energetic / Bright 这三类的边界上，还有抒情歌到底算 Sad 还是 Emotional，
这种本来就见仁见智，写 overrides 最快。

## 已知问题

- 只有 mp3，flac / m4a / ogg 都不支持
- Windows 版有个坑：文件名走系统 ANSI 代码页。中文系统的 GBK 能覆盖中文和
  日文曲名，但韩文、emoji 这类超出代码页的标题会被跳过。曲库里有这种名字的话，
  建议用 WSL 跑 Linux 版
- exe 没有签名，第一次运行 SmartScreen 会提示未知发布者
- 纯信号处理，没有语义模型。人声、歌词、编曲意图它都听不到，“燃”和“温柔”
  这种细微差别会误判。想再准一点得上预训练的 arousal-valence 模型
  (MTG-Jamendo、DEAM 那类)，代价是几百 MB 依赖和慢好几倍的分析速度

## 自己编译

```sh
make            # 需要 libmpg123 开发包,pacman -S libmpg123
make STATIC=1   # 单文件,不依赖系统库
```

三个平台的包是 CI 编的，脚本在 `scripts/` 里，本地也能跑：

```sh
# 静态 Linux 版,在 Docker / Podman 里
docker run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out" \
  alpine:3.20 sh /src/scripts/build-musl.sh native x86_64 /out
docker run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out" \
  alpine:3.20 sh /src/scripts/build-musl.sh aarch64-linux-musl aarch64 /out

# Windows 版,在 Debian / Ubuntu / WSL 上,先装 mingw-w64
sh scripts/build-windows.sh dist
```

版本号写在 `Makefile` 里。推一个 `v0.1` 这样的 tag，GitHub Actions 就会编三个
平台，自动发 Release。
