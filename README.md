# sensme

一个在终端运行按情绪简单分类的程序。

版本 0.1，支持 Linux、Windows 和 Android(Termux)。

## 下载

| 平台 | 文件 |
|---|---|
| Linux x86_64 | `sensme-0.1-linux-x86_64` |
| Linux arm64 | `sensme-0.1-linux-aarch64` |
| Windows x86_64 | `sensme-0.1-windows-x86_64.exe` |

### Android

```sh
# 以下命令在 Termux 里执行
termux-setup-storage          
mkdir -p ~/bin
cp ~/storage/downloads/sensme-0.1-linux-aarch64 ~/bin/sensme
chmod +x ~/bin/sensme
sensme
```

源目录填 `~/storage/shared/Music`，输出目录在共享存储里。

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
输出目录里已经有的文件会跳过，`-f` 才覆盖。用 `-r` 整理已有目录是 rename。

## 手动修正

规则一行一条，匹配串和情绪之间用 TAB 隔开：

```
Flower Dance	03_Calm
米津玄師	06_Emotional
アイネクライネ	06_Emotional
```

匹配串是相对路径的子串，不分大小写。情绪写 `03_Calm`、`3` 或者 `Calm` 都行。
默认读 `~/.config/sensme/overrides.tsv`，存在才读，也可以用 `-O` 指定别的文件。

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

## 已知问题

- 只有 mp3，flac / m4a / ogg 都不支持
- Windows 版有个坑：文件名走系统 ANSI 代码页。中文系统的 GBK 能覆盖中文和
  日文曲名，但韩文、emoji 这类超出代码页的标题会被跳过。曲库里有这种名字的话，
  建议用 WSL 跑 Linux 版
- exe 没有签名，第一次运行 SmartScreen 会提示未知发布者

## 编译

```sh
make            # 需要 libmpg123 开发包,pacman -S libmpg123
make STATIC=1   # 单文件,不依赖系统库
```

三个平台的包是 CI 编的，脚本在 `scripts/` 里，本地也能跑：

```sh
# 静态 x86_64 版，在 Docker / Podman 里跑
docker run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out" \
  alpine:3.20 sh /src/scripts/build-musl.sh x86_64 /out

# 静态 arm64 版（Android 也用这个），用 Zig 自带的 musl 交叉编译
# 需要 pkg-config、xz、bzip2
sh scripts/build-linux-aarch64.sh dist

# Windows 版，先装 mingw-w64
sh scripts/build-windows.sh dist
```

版本号写在 `Makefile` 里。推一个 `v0.1` 这样的 tag，GitHub Actions 就会编三个
平台，自动发 Release。
