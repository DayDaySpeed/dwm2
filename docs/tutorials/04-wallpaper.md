# 教程 4：壁纸

**你会做到：** 在静态、视频、场景三种壁纸之间切换；加入自己的图片和视频；导入 Wallpaper Engine 壁纸；调整切换动画。
**需要多久：** 15 分钟。
**前提：** 已进入桌面。壁纸都由 `bin/livewall.sh` 管理。

---

## 1. 三种壁纸

| 类型 | 是什么 | 文件放哪 |
|---|---|---|
| 静态 | 图片，每 5 分钟随机换一张 | `wallpaper/static/`，可以按题材分子目录（jpg / jpeg / png / webp） |
| 视频 | mpv 循环播放，嵌在桌面最底层（xwinwrap） | `wallpaper/live/` 下的 mp4 / mkv / webm / mov |
| 场景 | Wallpaper Engine 的场景壁纸，由 linux-wallpaperengine 实时渲染 | `wallpaper/live/` 下含 `project.json` 的目录 |

视频和场景合称"动态壁纸"。每次切换（包括每 5 分钟换图）都会用 GPU 播放一段随机的转场动画。

接了几块屏时，每块屏显示同一张壁纸，各自完整铺满，不会一张画面横跨几块屏；动态模式下每块屏各开一个视频播放器或场景，转场也在各屏同时播放。

## 2. 切换和换一张

- **动态 / 静态切换**：`Super + P` → `switch wallpaper mode to live`（或 `to static`）。切过去后随机挑一个；选择会记住，下次开机沿用。
- **立即换一张**：左键点状态栏最左边的菜单图标 `󰍜`。动态模式下换一个视频或场景，静态模式下换一张图（几块屏一起换）。
- **省电模式**（`Super + P` → `open power save`，拔电时也会自动打开）会切到静态壁纸，关掉省电后恢复。

命令行也可以：

```sh
$DWM/bin/livewall.sh next     # 换一张
$DWM/bin/livewall.sh toggle   # 动态 / 静态切换
```

## 3. 加自己的图片和视频

1. 图片放进 `wallpaper/static/`，例如：

   ```sh
   mkdir -p $DWM/wallpaper/static/风景
   cp ~/Downloads/*.jpg $DWM/wallpaper/static/风景/
   ```

   下一次换图时就可能选到，也可以点状态栏图标立即换。
2. 视频放进 `wallpaper/live/`。低于屏幕分辨率的视频（如 1080p 放在 1440p 屏上）在 N 卡上会自动用 Anime4K 放大。
3. 不想要的直接删掉。

`~/Pictures/wallpaper` 也链接到这个目录，用文件管理器从这里拖进去也行。

### 没有私有壁纸仓库时

`wallpaper/` 是私有子模块 `dwm2-wallpaper`，没有访问权限时它是空目录。照上面的方法建好 `static/`、`live/` 子目录、放进自己的文件，用法完全一样。

想让自己的壁纸也能跟着仓库走：在 `wallpaper/` 里建一个自己的 Git 仓库（大文件用 Git LFS），再把 `.gitmodules` 里的地址改成它。

## 4. 导入 Wallpaper Engine 壁纸

需要在 Steam 上拥有 Wallpaper Engine。播放时不依赖 Steam，Steam 只负责下载。

1. 在 Steam 创意工坊订阅喜欢的壁纸，等 Steam 下载完。
2. 运行：

   ```sh
   $DWM/bin/livewall.sh sync
   ```

   把新订阅的壁纸复制进 `wallpaper/live/`；每次登录时也会自动 sync 一次。
3. 复制完之后，可以在 Steam 上取消订阅，不影响播放。

支持视频类和场景类壁纸，**网页类不支持**。场景壁纸要装 `linux-wallpaperengine-git`（在 `packages/core-aur.txt` 里）；没装时自动跳过场景，只播视频。

## 5. 调整转场动画

在 `bin/livewall.sh` 顶部修改：

```bash
TRANSITION=${TRANSITION:-random}           # random: 每次随机 / fade: 淡入 / 着色器名: 固定一种
TRANSITION_SAVE=${TRANSITION_SAVE:-random} # 省电模式下用的转场
TRANS_SEC=0.8                              # 转场时长 (秒)
```

可选的着色器在 `config/transitions/`，文件名去掉 `.glsl` 就是名字，如 `ripple`、`morph`。来自 [gl-transitions](https://github.com/gl-transitions/gl-transitions)，同格式的着色器可以直接放进去用。

## 6. 和其他功能的关系

- **锁屏背景**：每次换壁纸后，后台会把当前画面模糊调暗，生成锁屏背景（每块屏用它自己的壁纸）。
- **星系**：`Esc` 退出星系时，壁纸从黑洞外圈浮现。
- **显卡**：混合模式下视频用独显渲染（画质更好，整机多耗 10～20W），场景用核显，省电模式下全部只用核显；独显直连时都用独显。两块屏同时放动态壁纸时，解码和渲染的负担大约翻倍。
- **焦点**：场景壁纸的窗口设成了鼠标事件直接穿过，鼠标停在空桌面上时也能正常切换显示器、`Super + 滚轮` 切 tag。

---

**下一步：** [教程 5：多显示器](05-monitors.md)
