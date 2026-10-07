/* Super+Z 3D 工作空间星系. 由 dwm.c 在 config.h 与 Pertag 之后 #include.
 *
 * 每个窗口在开始时截图一次 (XRender Picture + mipmap, 驻留时轮流刷新), 之后只在离屏 3D 场景里绘制:
 *   世界坐标 (x 右, y 下, z 远离镜头) -> 镜头变换 -> 透视投影 -> 按镜头空间 z 排序 -> 三层合成 -> 全屏遮罩窗口
 * 三层 (见 galaxy-gl.c): 底层 XRender (壁纸 / 深空 / 卡片), 光层 OpenGL (光点 / 光带 / 粒子, 加法混合 + 泛光),
 * 前景层 XRender (文字 / 标签 / 状态栏); 每帧由 GPU 合成.
 * Tag = 星系核心, 窗口 = 沿 3D 轨道环绕核心运行的星体, 所有 tag 组成星系群.
 * 流程: 开场 (约 7.7s: 起飞 / 跃迁 / 点火 / 螺旋旋转 / 俯冲铺满全屏 / 点名 / 弧线回缩; 鼠标默认休眠, 左键唤醒后点击才快进) -> 停在星系轨道态 (不限时, tag 核心沿开普勒椭圆群轨道公转, 导演镜头轮换机位; 鼠标默认休眠, 左键唤醒后才可视差 / 缩放 / 选窗口)
 *       -> Esc: 星系群沿轨道划过一段弧线后坍缩成一个光点, 停在纯壁纸 (再按 Super+Z 恢复)
 *       -> Super+Z: 回程, 窗口星飞回原位置变回截图, 露出真实桌面
 *       -> 点击窗口星: 卡片朝镜头前推后跳到该窗口; 点击核心: 核心亮起后切到该 tag
 * 动画期间不移动 / 隐藏 / 映射 / 重叠 / 聚焦任何真实窗口; 只在结束时恢复 (或按点击切换) tag 和焦点. */

#include <X11/extensions/dpms.h>
#include <X11/extensions/Xrandr.h>
#include <ctype.h>

#define GALAXYMIPS     6
#define GALAXYALPHAS   256
#define GALAXYTRAIL    12
#define GALAXYGAPS     4096
#define GALAXYRINGS    3
#define GALAXYSEG      72
#define GALAXYARCS     8         /* 每段 9 条线, 独立参加深度排序 */
#define GALAXYARCSEG   (GALAXYSEG / GALAXYARCS)
#define GALAXYTILES    4         /* 大截图的透视画面拆成 4x4 仿射块 */
#define GALAXYFPS      120.0
#define GALAXYORBITFPS 60.0      /* 查不到显示器刷新率时的驻留帧率 */
#define GALAXYIDLEFPS  30.0
#define GALAXYIDLE     90.0      /* 驻留时多久无输入后降到 GALAXYIDLEFPS (秒) */
#define GALAXYMOUSEIDLE 8.0     /* 唤醒鼠标后无操作多久恢复纯展示 */
/* 可在 config/dwm.h 里覆盖的参数 (见那里的「Super+Z 星系」一节), 没写时用这里的默认值 */
#ifndef GALAXY_INTRO
#define GALAXY_INTRO    1.35      /* 开场时间拉伸: 1 场景秒 = 1.35 真实秒, 越大开场越慢 */
#endif
#ifndef GALAXY_FXGAP
#define GALAXY_FXGAP    1.0       /* 驻留特效间隔的倍数: 2 = 特效间隔加倍 */
#endif
#ifndef GALAXY_DIAG
#define GALAXY_DIAG     30.0      /* 驻留时镜头翻滚 (度), 轨道盘面沿屏幕对角线铺开; 0 = 水平 */
#endif
#ifndef GALAXY_TOURDIST
#define GALAXY_TOURDIST .5        /* 巡游镜头离星系核心的距离 (焦距的倍数), 越小越近 */
#endif
#ifndef GALAXY_SHOT
#define GALAXY_SHOT     9.0       /* 导演镜头每个机位停留的秒数 */
#endif
#ifndef GALAXY_QUIETFPS
#define GALAXY_QUIETFPS 30.0      /* 安静模式 (省电模式打开时) 驻留帧率 */
#endif
#define GALAXYINTRO    GALAXY_INTRO
#define GALAXYHOLD     4.2       /* 关键帧曲线的驻留点: stage 停在这里 */
#define GALAXYIEND     5.7       /* 开场时钟走到这里进入驻留 (旋转后还有俯冲 / 点名 / 回缩, 真实约 7.7s) */
#define GALAXYEXIT     4.75      /* Esc 坍缩从这里接着播放到 GALAXYEND (4.2~4.8 之间的曲线是平的) */
#define GALAXYEND      6.0
#define GALAXYWARP     .6        /* 开场中按键, 或唤醒鼠标后点击: 快进到驻留态的真实时长 */
#define GALAXYRETURN   1.6       /* Super+Z 飞回原位的真实时长 */
#define GALAXYLAND     .95       /* 进入窗口 (点击窗口星 / 核心): 卡片落到真实窗口位置的时长 */
#define GALAXYBARS     8         /* 落位时实时截取的状态栏 / 托盘 */
#define GALAXYSHOT     GALAXY_SHOT
#define GALAXYSHOTMIX  6.0       /* 机位之间的过渡 (真实秒), 巡游之间是一次飞越; 慢一些, 镜头转角另有上限 */
#define GALAXYDIAG     GALAXY_DIAG
#define GALAXYSTREAK   24        /* 每个星系核心身后的长曝光星轨分段数 */
#define GALAXYSTREAKM  1.5       /* 星轨覆盖的平近点角 (rad): 近点处自然拉长, 远点处变短 */
#define GALAXYCOLLAPSE 2.35      /* Esc 后完整的环绕 / 坍缩演出 (真实秒) */
#define GALAXYEXITSTART 1.25     /* 退场从环绕切换到向中心收束的时刻 */
#define GALAXYSHOCK    2.05      /* Esc 后多久从中心爆出冲击波, 圆形揭开壁纸 (真实秒; 之前透镜把背景拽向中心) */
#define GALAXYSHOCKT   .5        /* 冲击波扩到屏幕四角的时长 */
#define GALAXYAFTER    .3        /* 揭开后中心余晖再散去的时长, 之后才停在壁纸 */
#define GALAXYPREP     .45       /* 开场按 Esc 时平滑进入星系群形态 */
#define GALAXYLANES    3
#define GALAXYNOVAPRE  .3        /* 超新星爆发前核心收缩、变暗的时长 (秒) */
#define GALAXYSPARE    8         /* 星系运行中新开的窗口最多再诞生几颗星 */
#define GALAXYNOTES    6         /* 通知彗星队列 (同时最多 2 颗) */
#define GALAXYRIVER    60        /* 星际尘埃流: 沿三条群轨道流动的粒子数 */
#define GALAXYSHARDX   20        /* 开场 A: 桌面碎成 20x12 块 */
#define GALAXYSHARDY   12
#define GALAXYSHARDA   8         /* 碎块蒙版的透明度级数 */
#define GALAXYBANGN    90        /* 开场 C: 大爆炸喷出的粒子数 */
#define GALAXYPI       3.14159265358979323846

enum { GalaxyOff, GalaxyIntro, GalaxyOrbit, GalaxyCollapse, GalaxyReturn, GalaxyRest };
enum { GalaxyFlyHome, GalaxyLand };    /* 回程: 飞回原位 / 进入选中的窗口 (目标 tag 已在遮罩下切好) */
enum { GalaxyDustItem, GalaxyCoreItem, GalaxyStarItem, GalaxyRingItem, GalaxyClusterItem, GalaxyStreakItem, GalaxySunItem };
enum { GalaxyHalo, GalaxyDisc, GalaxySpike, GalaxyPoint, GalaxySurface, GalaxyShapes };   /* 柔光 / 实心光点 / 衍射芒 / 恒星核心 (点光源) / 近处的恒星表面 */
/* sprite 色调: 暖白 / 冷蓝 / 橙红 (urgent, 高 CPU) / 天象用色 / 每个 tag 一个颜色. sprite 都是白芯彩晕: 中心过曝成白, 颜色在衰减部分 */
enum { GalaxyWarm, GalaxyCool, GalaxyHot, GalaxyGold, GalaxyCyan, GalaxyRose, GalaxyGreen, GalaxyOrange, GalaxyBlue, GalaxyViolet,
    GalaxyTag0, GalaxyTints = GalaxyTag0 + 9 };
/* 极光色系 (颜色就是工作区, 换壁纸也不变): 所有 tag 在 青 -> 蓝 -> 紫 -> 品红 这条渐变上取色;
 * 暖色 (金 / 橙 / 红) 只留给天象和提醒. 0xRRGGBB, 改这里就能换整套配色 */
static const unsigned int galaxytagcolor[9] = {
    0x2de2e6, 0x3cc8ff, 0x4d8bff, 0x6c6cff, 0x8f5bff, 0xb84dff, 0xe04de0, 0xff5fa8, 0x7af0c8 };
/* 暖白 / 冷白 / 红 (urgent, 高 CPU) / 金 / 青 / 玫粉 / 绿 / 橙 / 蓝 / 紫 */
static const unsigned int galaxyfxcolor[10] = {
    0xfff1dc, 0xdbe8ff, 0xff4d5e, 0xffc46b, 0x3cf0ff, 0xff5fa8, 0x5dff9b, 0xff8a4d, 0x4d8bff, 0x8f5bff };
#define GALAXYTAGTINT(tag) (GalaxyTag0 + (tag) % 9)
/* 三条群轨道: 内青 / 中蓝紫 / 外玫粉 (光带用时饱和度压低) */
static const int galaxylanetint[3] = { GalaxyCyan, GalaxyViolet, GalaxyRose };

static const char *galaxymodename[] = { "off", "intro", "orbit", "collapse", "return", "rest" };
/* 开场的三套编排 (随机轮换, 不连续重复; 环境变量 GALAXY_VARIANT=A|B|C 固定一套), 其余节拍共用 */
enum { GalaxyShatter, GalaxyGate, GalaxyBang, GalaxyVariants };
static const char *galaxyvariantname[] = { "A shatter", "B gate", "C bigbang" };

typedef struct { double x, y, z; } GalaxyVec;
typedef struct { double m[3][3]; } GalaxyMat;
typedef struct { double w, x, y, z; } GalaxyQuat;
typedef struct { double t, v; } GalaxyKey;

typedef struct {
    double x, y, z, scale;      /* 屏幕坐标, 镜头空间深度, 透视缩放 */
    int ok;
} GalaxyProj;

typedef struct {
    GalaxyVec pos, target;
    double rx, ry, rz;          /* pitch / yaw / roll */
    double fov, focal, near, far, dist;
    GalaxyMat rot;               /* 镜头 -> 世界 (镜头的朝向) */
    GalaxyMat view;              /* 世界 -> 镜头 */
} GalaxyCamera;

typedef struct {
    int tag, nstars, nrings, hit, lane;
    GalaxyVec home;              /* 星系群坐标系中的位置 */
    GalaxyVec pos, rpos;         /* 当前世界坐标 / 回程开始时冻结的位置 */
    double rx, ry, rz;          /* 轨道平面: 倾角 / 偏航 / 翻滚 */
    double radius, speed, phase, orbitphase, precess, size, alpha, ralpha, hover;
    double anomaly, peri, flare, ripple, flip, eclipse;  /* 平近点角 / 交会 / 掩食 / 涟漪 */
    double ignite, callout, streakreveal;       /* 开场: 点火闪光 / 逐个点名 / 星轨拉出进度 */
    double nova, bridge;                        /* 驻留: 超新星爆闪 / 光桥到达时的闪光 */
    double flashat;             /* 拖进来一个窗口的时刻: 核心闪一下 */
    double fill;                /* 有窗口的程度: 空星系诞生第一颗星时从 0 长到 1 (亮度 / 轨道环半径随之长大) */
    int rank;                   /* 开场点火的先后顺序 */
    GalaxyVec nudge;             /* 交会时互相吸引的表现层偏移 */
    GalaxyMat ring[GALAXYRINGS];  /* 每条轨道环相对星系轨道平面的姿态 (环与环互相倾斜) */
    double ringr[GALAXYRINGS], ringz[GALAXYRINGS][GALAXYARCS];
    int ringn[GALAXYRINGS][GALAXYARCS];
    GalaxyMat plane;             /* 当前轨道平面 (含全局旋转) */
    double hx, hy, hr;          /* 屏幕上的点击范围 */
    GalaxyProj p;
    GalaxyVec trail[48];        /* 粒子尾迹: 现在和过去的世界位置 (回程时冻结) */
} GalaxyCore;

typedef struct {
    Window win;
    Client *c;                  /* 只用于调试日志, 窗口关闭后置空 */
    Monitor *mon;
    unsigned int tags;
    char title[64];
    int galaxy, ring, valid, focused, current, hidden, global, shown, back, base, snap, hit, kmatch;
    int w, h;                   /* 真实窗口大小, 也是卡片的世界尺寸 */
    Pixmap mippix[GALAXYMIPS];
    Picture mip[GALAXYMIPS];
    int mipw[GALAXYMIPS], miph[GALAXYMIPS];
    unsigned int gltex, glsrc;  /* 截图的 GL 纹理 (带 mip 链), 第一次画时建; glsrc / glpix: 读截图用的纹理和 GLX pixmap */
    unsigned long glpix;
    int glfail, gldirty;        /* 建不了 (走 XRender) / 截图刷新过, 下次画时重拷 */
    double pq[4][2];            /* 上一帧画出的屏幕四角 (运动模糊), pqframe: 那一帧的编号 */
    unsigned long pqframe;
    double clickat, clickuv[2]; /* 点中卡片的时刻和卡面上的位置 (涟漪) */
    GalaxyVec trail[48];        /* 粒子尾迹 (同核心) */
    GalaxyVec home, detach;      /* 桌面平面 z=0 上的起点 / 脱离桌面后的位置 */
    double drx, dry, drz;       /* 脱离桌面时的卡片旋转 */
    double radius, angle, speed, rock, delay;
    GalaxyVec pos, vel;
    GalaxyMat orient;
    double size, brightness, alpha, vis, tint, glow, lod, hover, flipcard, constel;
    /* 回程开始时冻结的状态 */
    GalaxyVec rpos;
    GalaxyMat rorient;
    double rsize, rvis, rtint, rglow, rbright;
    double bx0, by0, bx1, by1;  /* 屏幕上的点击范围 */
    /* 天象: 星系运行中新开的窗口诞生 / 关闭的窗口化作流星 (galaxynow 时刻, 0 表示没有) */
    double born, died;
    GalaxyVec dpos, dvel;        /* 关闭时的位置和速度: 流星沿轨道切线飞出 */
    /* 卡片宽高相对截图的拉伸 (落位时窗口大小可能已变); 落点: 目标 tag 里真实窗口的中心和大小 */
    double kw, kh, rkw, rkh, lw, lh;
    int land;
    GalaxyVec lpos;
    double refreshat;           /* 上次刷新截图的时刻 */
    int urgent, pid;            /* 窗口请求关注 (红星) / 进程号 (_NET_WM_PID, 0 表示不知道) */
    double heat, heatt;         /* 进程 (含子进程) CPU 占用 (核数): 平滑后的 / 最近一轮扫描的 */
    unsigned long long cpuprev; /* 上一轮扫描时子树累计的 CPU 时间 (jiffies) */
    double moveat;              /* 拖到别的核心 / 拖完飞回轨道: 开始时刻和起点 (0 表示没有) */
    GalaxyVec movefrom;
    GalaxyProj p;
} GalaxyStar;

typedef struct {
    GalaxyVec pos;
    double size, light;
    int disk;                   /* 盘面尘带: 跟随星系群一起转 */
    double tw, boost;           /* 远景星点的闪烁相位; 超新星附近的提亮 */
    GalaxyProj p;
} GalaxyDust;

typedef struct { int kind, index; double z; } GalaxyItem;

/* 粒子: 世界坐标 (screen 为 1 时是屏幕坐标, 像素 / 秒), 按年龄淡出、缩小; 颜色是色调表里的颜色 */
typedef struct {
    GalaxyVec pos, vel;
    float age, life, size, alpha, drag;
    float temp;                 /* 色温偏移: -1 偏蓝 .. 1 偏橙 */
    unsigned char tint, screen;
} GalaxyParticle;
#define GALAXYPARTICLES 24000

typedef struct {
    int mode, grabkbd, grabptr, w, h, ntags, nstars, ndust, nitems, ntrail, rendermajor;
    int vx, vy, vw, vh;         /* 视口: 发起星系的那块显示器. 投影中心和画面尺度都按它算, 遮罩和画布仍覆盖整个 root */
    int warping, dpms, dpmsoff, hover, hovercore, handon, fulldesk;
    int mouseawake, mousevalid; /* 开场 / 驻留: 首次左键只唤醒, 移动后才开始悬停 */
    double lastmouse;
    int dragging;
    double dragx, dragy, dragtime, dragyaw, dragpitch, dragtyaw, dragtpitch, dragvyaw, dragvpitch;
    int rkind, rstar, rcore;    /* 回程种类: 飞回原位 / 进入窗口; 点中的窗口 / 点中的核心 */
    /* 落位时实时截取的状态栏 / 托盘 (切 tag 之后的样子), 最后淡入 */
    Picture landbar[GALAXYBARS];
    int landbarx[GALAXYBARS], landbary[GALAXYBARS], landbarw[GALAXYBARS], landbarh[GALAXYBARS], nlandbar;
    double tscale, starscale, orbitscale, glowscale;
    /* 时钟: scene 是场景时间, stage 驱动关键帧曲线 (驻留时停住), motion 驱动轨道运动 (一直走) */
    double last, scene, stage, motion, holdw;
    double wstart, wscene, cstart, cstage, celapsed, rstart, lastinput, lastdpms;
    double exitspin, clusteralpha, rcluster, streakalpha, rstreak, sunalpha, rsun, sunpulse, ripple, rippleamp;
    GalaxyMat cworld;            /* Esc 时冻结星系群朝向, 避免叠加全局自转 */
    int nlanes, lanemask, npop, *popord;
    GalaxyProj clusterpts[GALAXYLANES][GALAXYSEG + 1];
    double clusterz[GALAXYLANES][GALAXYARCS], clusterr[GALAXYLANES][GALAXYSEG + 1];
    int clustern[GALAXYLANES][GALAXYARCS];
    GalaxyProj *streakpts, sunp;  /* 每个核心 GALAXYSTREAK + 1 个点 */
    double *streakz;
    /* 导演镜头: dclock 只在无交互时走; 交互 (移动 / 滚轮 / 悬停) 让 dspeed 降到 0, 镜头停在当前机位 */
    double dclock, dspeed, dfit, lastpointer, dtyaw[2];
    double dyoff[2];            /* 当前 / 下一个机位的 yaw 平移: 机位切换时镜头最多转 35° */
    int dshot, dtg[2], lastflip, lastripple, lastnova, lastcomet;
    int tourhist[3];            /* 最近巡游过的星系: 下一站优先选附近、最近没去过的 */
    /* 驻留特效的目标在每次事件开始时按镜头选定 (避开正在巡游的星系, 优先画面里看得见的), 之后不变 */
    int flipk, flipg, novak, novag, constk, constg;
    double flipstart;
    int bridgek, bri, brj;      /* 当前这次核心光桥连接的两个核心 */
    int eclipsepair;            /* 当前掩食的前后核心, 用于一次性日志 */
    /* 开场节拍: iclock 是开场时钟 (场景秒, 坍缩时冻结), beatfade 在快进 / 坍缩时把节拍效果淡掉 */
    double iclock, beatfade, warpfx, lanereveal[GALAXYLANES];
    double space, rspace;       /* 深空背景的不透明度 (开场由壁纸溶解过去) / 回程开始时冻结的值 */
    /* 天象: 新窗口等待截图 (映射后 0.6s, 窗口画好了再截) / 通知彗星 / 整点报时. 时刻都是 galaxynow */
    int maxstars, nbirth, nnote, lasthour, chimehour, pulsar;
    Window birthwin[GALAXYSPARE];
    double birthat[GALAXYSPARE];
    char note[GALAXYNOTES][128];
    double noteat[GALAXYNOTES];  /* 0: 还在排队 */
    double chimeat, fakehour, hushuntil, calm;  /* calm: 天象发生时随机特效让位 (降到 .3) */
    XftFont *notefont, *clockfont, *iconfont;   /* iconfont: 状态栏字体, 显示 tag 图标 */
    /* 前景层 (ARGB, 每帧清空): 文字 / 标签 / 状态栏 / 边框 / 结尾的桌面截图, 由 GPU 盖在光层之上 */
    Pixmap frontpix;
    Picture front;
    Visual *argbvisual;
    Colormap argbcmap;
    int frontused;
    /* 卡片实时刷新: 轮转位置 / 本段统计 */
    int refreshi, refreshn;
    double refreshsum, refreshmax;
    /* CPU 色温: 每帧增量扫描 /proc, 一轮扫完按进程树汇总 */
    void *heatdir;
    struct GalaxyProc { int pid, ppid; unsigned long long t; } *procs;
    int nprocs, cprocs;
    double heatstart, heatlast, heatcost;
    /* 拖动卡片: 按下的候选 / 正在拖的星 (-1 没有), 按下位置, 拖动平面的镜头深度, 是否改过 tag */
    int dragarm, dragstar, moved;
    double dragx0, dragy0, dragz;
    GalaxyVec dragpos;
    /* 确定性时钟 (GALAXY_FAKETIME=帧率): 每帧前进固定步长; GALAXY_DUMP 列出要存成 PPM 的时刻 */
    double lastcost;            /* 上一帧的渲染耗时 (帧率面板用) */
    /* 粒子池 (环形覆盖, 满了就挤掉最老的) / 上一帧的真实间隔 / 随机序号 / 一次性爆发是否已发过 */
    GalaxyParticle *parts;
    int nparts, partnext;
    double pdt, retu;           /* retu: 回程进度 (0..1), 回程中粒子随之淡出 */
    unsigned int pseed;
    int novaburst, birthburst, bangburst;
    unsigned int bandcolor;     /* 当前光带颜色 (0xAARRGGBB), 见 galaxybandcolor */
    double fakestep, fakeclock, dumpt[16];
    int ndump, dumpi;
    /* 开场变体. A: 桌面 (壁纸 + 状态栏) 切成碎块, 旋转着被吸进视口中心的灭点; C: 爆心 (焦点窗口中心, 世界坐标) */
    int variant, shardw, shardh;
    Pixmap shardpix, shardmaskpix[GALAXYSHARDA];
    Picture shardsrc, shardflat, shardmask[GALAXYSHARDA];
    GalaxyVec bang;
    int quiet;                  /* 安静模式: 省电模式打开时特效变少变稀, 帧率降低 */
    int gentle;                 /* 减弱动效 (GALAXY_GENTLE=1 或 ~/.cache/galaxy-gentle): 闪白 / 透镜 / 光晕 / 曝光骤降 / 闪烁都收敛 */
    double refresh;             /* 遮罩所在显示器的刷新率 (Hz): 驻留态按它出帧 */
    double nebseed[2];          /* 星云噪声的偏移: 每次 Super+Z 随机, 浓淡的位置每次不同 */
    double adapt, adaptat;      /* 曝光适应: 平滑后的光层亮度 / 上次更新的时刻 */
    int meteork;                /* 流星雨: 当前是第几次 */
    double meteorat;            /* 下一颗零星流星的时刻 */
    /* 远景 (星云着色器里画, 每次按 nebseed 生成, 坐标是按视口宽归一化的 q): 远方星系 x y 半径 朝向 / 倾斜 类型 亮度 相位,
     * 亮星 x y 闪烁周期 相位 */
    float farg[2][8], fars[3][4];
    float farcomet[4];          /* 远处彗星: 头部 x y (q), 运动方向角, 强度 (0 表示没有) */
    int farcometk;
    double lumsum, lummax, expomin;   /* 本段统计 (日志): 光层亮度的和 / 最大值, 最低曝光 */
    int trace;                  /* 环境变量 GALAXY_TRACE: 每帧把镜头参数写进日志 */
    int saver;                  /* 屏保模式 (无操作时由 bin/galaxysaver.py 触发): 任何输入都回到桌面 */
    double saverx, savery;      /* 屏保开始时的指针位置: 移动超过几像素才算有人回来 */
    double fxgap;               /* 驻留特效间隔的总倍数 (配置 x 安静模式) */
    double divesum, divemax;    /* 俯冲段 (开场时钟 3.2–5.0) 的渲染耗时统计 */
    int divenum;
    GalaxyVec dtour[2];          /* 巡游机位: 当前 / 下一个机位跟踪的核心 (平滑后的位置) */
    /* 驻留交互: 鼠标视差 / 滚轮推拉 (t 开头是目标值, 每帧平滑逼近) */
    double mx, my, tyaw, tpitch, pyaw, ppitch, tzoom, zoom;
    /* 每帧由 update 算出, render 只读这些 */
    double bright, vign, desk, deskover, bar, reveal, ringalpha, trailgain, dustfade, central, pulse;
    /* 回程开始时冻结的全局状态 */
    double rcdist, rcx, rcy, rcz, rring, rdust, rbright, rvign, rdesk;
    GalaxyVec rcampos, rctarget;
    Window overlay, wallwin;
    Cursor hand, blankcursor;
    Pixmap backpix, desktoppix, wallpix, vignettepix, bgpix, tilemaskpix, spinpix;
    Picture back, desktop, wallpaper, live, overlaypic, vignette, bg, tilemask, spinpic;
    int spinw, spinh, projslow;   /* projslow: 整幅透视采样明显慢于仿射, 大卡片改走半分辨率 */
    double probeaffine, probeproj;
    XftFont *titlefont, *queryfont;
    /* 键盘导航: 过滤词 / 选中的星 / 匹配数 / 最近一次键盘操作的时刻 (鼠标移动后交还给鼠标) */
    char kq[64];
    int kqlen, ksel, kn;
    double lastkey;
    XftDraw *titledraw;
    XftColor titlecolor;
    int titlecolorok;
    double bgkey[4], bglast[4];     /* 背景缓存: 驻留时壁纸亮度 / 暗角不变, 每帧只复制一次 */
    int bgok;
    Picture white[GALAXYALPHAS], black[GALAXYALPHAS];
    XRenderPictFormat *argb, *a8, *a1;
    GalaxyCore *galaxies;
    GalaxyStar *stars;
    GalaxyDust *dust;
    GalaxyItem *items;
    GalaxyVec *tgpos;
    GalaxyMat *tgplane;
    GalaxyProj *tpts, *rpts;
    GalaxyCamera cam;
    GalaxyMat world;
    struct timespec start;
    /* 开始前的 dwm 状态, 以及结束时要切换到的状态 (默认就是开始前的状态, 点击窗口星时改成该窗口) */
    Monitor *savedmon, *tmon;
    unsigned int savedtags, ttags;
    Window savedwin, twin;
    int tshow, barx, bary, barw, barh;
    /* 统计 (按阶段) */
    FILE *log;
    unsigned long frames, errors, ngaps;
    double segstart, rendersum, rendermax, *gaps;
    double phasecost[6], qualityavg, qualitylast, qualityvisual;
    int quality, qualitybad, qualitygood;
    double firstgap[24], firstcost[24];    /* 开场前 24 帧的时刻和渲染耗时 */
    int nfirst, firstlogged;
} GalaxyScene;

static GalaxyScene galaxyscene;
static double galaxynow(void);
static void galaxyheatfree(void);
static int galaxyhud;           /* F12 帧率面板, 进程内保持 */

/* 时间轴上的关键帧曲线 (单调分段三次 Hermite), 用于镜头和形态变化.
 * 时间是场景时间; 驻留态停在 GALAXYHOLD, 坍缩从 GALAXYEXIT 接着播放, 所以 4.2~4.8 的值必须相同 */
static const GalaxyKey galaxycamdist[] = {
    {0, 1}, {.15, 1}, {.6, 1.05}, {1, 1.12}, {1.5, 1.22}, {2.2, 1.5}, {2.6, 1.46},
    {2.9, 1.38}, {3.3, 1.3}, {3.8, 1}, {4.2, .72}, {4.8, .72}, {5.5, 1.2}, {6, 1.2}
};
static const GalaxyKey galaxycamyaw[] = {   /* 度 */
    {0, 0}, {.15, 0}, {.6, -3}, {1, -6}, {1.5, -10}, {2.2, -12}, {2.6, -2},
    {2.9, 10}, {3.3, 20}, {3.8, 6}, {4.2, -4}, {4.8, -4}, {5.5, 0}, {6, 0}
};
static const GalaxyKey galaxycampitch[] = { /* 负值: 镜头在上方; 群轨道在水平盘面内, 驻留时低角度斜视 */
    {0, 0}, {.15, 0}, {.6, 2}, {1, 5}, {1.5, 8}, {2.2, 8}, {2.6, 4},
    {2.9, 0}, {3.3, -8}, {3.8, -12}, {4.2, -14}, {4.8, -14}, {5.5, 0}, {6, 0}
};
static const GalaxyKey galaxycamroll[] = {
    {0, 0}, {2.2, 0}, {2.6, 2}, {2.9, 5}, {3.3, 3}, {4.2, GALAXYDIAG}, {4.8, GALAXYDIAG}, {5.5, 0}
};
/* 旋转之后的节拍镜头 (时间轴是开场时钟): 俯冲进星系群内部铺满全屏 -> 逐个点名 -> 沿弧线拉回,
 * 终点与驻留第一个机位 (wide) 的起点相同 */
static const GalaxyKey galaxybeatdist[] = {
    {3.2, 1.32}, {3.55, 1.27}, {3.8, 1.05}, {4.05, .5}, {4.5, .4}, {5.2, .8}, {5.45, .76}, {5.7, .72}
};
static const GalaxyKey galaxybeatpitch[] = {
    {3.2, -7}, {3.6, -8}, {3.95, -25}, {4.3, -34}, {4.5, -32}, {5.2, -17}, {5.7, -14}
};
static const GalaxyKey galaxybeatyaw[] = {
    {3.2, 18}, {3.6, 18}, {3.95, 28}, {4.5, 66}, {5.2, 8}, {5.7, -4}
};
static const GalaxyKey galaxybeatroll[] = {
    {3.2, 2.5}, {3.95, -3}, {4.5, -4}, {5.2, GALAXYDIAG * .75}, {5.7, GALAXYDIAG}
};
static const GalaxyKey galaxybright[] = {   /* 壁纸亮度: 冻结时 70%, 星系阶段沉入深空, 结尾 70% -> 100% */
    {0, 1}, {.15, .7}, {.6, .7}, {1.5, .3}, {4.8, .3}, {5.5, .7}, {5.7, .7}, {6, 1}
};
static const GalaxyKey galaxyspacekeys[] = {   /* 深空背景: 开场由壁纸溶解过去, 坍缩时溶解回壁纸 */
    {.2, 0}, {1.5, 1}, {4.8, 1}, {5, 1}, {5.7, 0}
};
static const GalaxyKey galaxyvignettekeys[] = {
    {.15, 0}, {1.5, .75}, {4.8, .75}, {5.6, .25}, {6, 0}
};
static const GalaxyKey galaxycardvis[] = {  /* 截图可见度 (再乘深度 LOD): 截图 -> 小卡 -> 发光面板, 坍缩时变成光点 */
    {1, 1}, {1.5, .85}, {2.2, .7}, {3, .62}, {3.6, .7}, {4.2, .8}, {4.8, .8}, {5.05, 0}
};
static const GalaxyKey galaxycardsize[] = {
    {.15, 1}, {.6, .74}, {1, .46}, {1.5, .3}, {2.2, .2}, {3.3, .15}, {4.2, .17}, {4.8, .17}, {5.2, .08}
};
static const GalaxyKey galaxycardtint[] = { /* 卡片自身发光: 只轻微提亮, 截图内容始终看得见 (太白会变成一块灰板) */
    {1.2, 0}, {2.2, .07}, {3, .12}, {3.6, .09}, {4.2, .05}, {4.8, .05}, {5, .3}
};
static const GalaxyKey galaxystarglow[] = {
    {.8, 0}, {1.5, .22}, {2.2, .45}, {3, .75}, {3.6, 1}
};
static const GalaxyKey galaxytrailgain[] = {
    {1.4, 0}, {1.9, .35}, {2.3, .6}, {2.7, 1}, {3.1, .9}, {3.5, .55}, {4.2, .5},
    {4.8, .5}, {5.05, .35}, {5.35, .2}, {5.5, 0}
};
static const GalaxyKey galaxyringkeys[] = { /* 轨道环的透明度: 高速旋转时稍明显, 驻留时淡 */
    {1, 0}, {1.8, .14}, {2.4, .24}, {3.3, .24}, {4.2, .13}, {4.8, .13}, {5.1, 0}
};
static const double galaxyinclinations[] = { 8, 28, -36, 54, -22, 42, -50, 16, -62 };
static const double galaxyringtilt[GALAXYRINGS] = { 0, 38, -32 };
/* 三条开普勒椭圆群轨道 (共用焦点 = 中心光源): 半长轴 (屏宽倍数) / 偏心率 / 倾角 / 升交点 / 近点角 (度) / 周期 (真实秒) / 方向 */
typedef struct { double a, e, inc, node, peri, period, dir; } GalaxyLane;
static const GalaxyLane galaxylanes[GALAXYLANES] = {
    { .22, .35,   8,   0,  40, 30,  1 },
    { .33, .45, -10,  70, 165, 44, -1 },
    { .45, .52,  14, -40, 285, 60,  1 },
};

/* ---------- 数学 ---------- */

static double galaxyclamp(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

/* 色调的 RGB: 光带和光点共用同一套颜色. sat < 0 用原色; 否则按 sat 向白色靠 (.6 以上是原色, 0 是白色) */
static void
galaxytintcolor(int tint, double sat, double out[3])
{
    unsigned int c = tint >= GalaxyTag0 ? galaxytagcolor[(tint - GalaxyTag0) % 9] : galaxyfxcolor[tint];
    double k = sat < 0 ? 1 : galaxyclamp(sat / .6);
    int i;

    for (i = 0; i < 3; i++)
        out[i] = 1 + ((c >> (16 - 8 * i) & 255) / 255.0 - 1) * k;
}

/* 光带用的打包颜色 0xAARRGGBB (不透明). sat < 0 用色调的默认饱和度; 亮芯常用一半饱和度 (白芯彩晕) */
static unsigned int
galaxytintrgb(int tint, double sat)
{
    double c[3];

    galaxytintcolor(tint, sat, c);
    return 0xff000000u | (unsigned int)(galaxyclamp(c[0]) * 255 + .5) << 16
        | (unsigned int)(galaxyclamp(c[1]) * 255 + .5) << 8 | (unsigned int)(galaxyclamp(c[2]) * 255 + .5);
}

/* 两个打包颜色按 t 混合 (超新星冲击环蓝紫 -> 橙) */
static unsigned int
galaxymixrgb(unsigned int a, unsigned int b, double t)
{
    unsigned int out = 0xff000000u;
    int k;

    t = galaxyclamp(t);
    for (k = 0; k < 24; k += 8)
        out |= (unsigned int)((a >> k & 255) * (1 - t) + (b >> k & 255) * t + .5) << k;
    return out;
}
static double galaxymix(double a, double b, double t) { return a + (b - a) * t; }
static double galaxyphase(double t, double a, double b) { return galaxyclamp((t - a) / (b - a)); }
static double galaxyeaseincubic(double x) { x = galaxyclamp(x); return x * x * x; }
static double galaxyeaseoutcubic(double x) { x = 1 - galaxyclamp(x); return 1 - x * x * x; }
static double galaxyeaseoutquart(double x) { x = 1 - galaxyclamp(x); return 1 - x * x * x * x; }
static double galaxyeaseoutback(double x) { x = galaxyclamp(x) - 1; return 1 + x * x * (2.2 * x + 1.2); }  /* 略冲过 1 再回落 */
static double galaxysmoothstep(double x) { x = galaxyclamp(x); return x * x * (3 - 2 * x); }

static double
galaxyeaseinoutcubic(double x)
{
    x = galaxyclamp(x);
    return x < .5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2;
}

/* 指数平滑: 与帧率无关 */
static double
galaxyfollow(double cur, double target, double dt, double tau)
{
    return cur + (target - cur) * (1 - exp(-dt / tau));
}

static double
galaxytangent(const GalaxyKey *k, int n, int i)
{
    double a, b;

    if (i == 0 || i == n - 1)
        return 0;
    a = (k[i].v - k[i-1].v) / (k[i].t - k[i-1].t);
    b = (k[i+1].v - k[i].v) / (k[i+1].t - k[i].t);
    return a * b <= 0 ? 0 : 2 * a * b / (a + b);
}

static double
galaxycurve(const GalaxyKey *k, int n, double t)
{
    double h, u, u2, u3, m0, m1;
    int i;

    if (t <= k[0].t)
        return k[0].v;
    if (t >= k[n-1].t)
        return k[n-1].v;
    for (i = 0; t > k[i+1].t; i++);
    h = k[i+1].t - k[i].t;
    u = (t - k[i].t) / h;
    u2 = u * u;
    u3 = u2 * u;
    m0 = galaxytangent(k, n, i) * h;
    m1 = galaxytangent(k, n, i + 1) * h;
    return (2*u3 - 3*u2 + 1) * k[i].v + (u3 - 2*u2 + u) * m0
        + (-2*u3 + 3*u2) * k[i+1].v + (u3 - u2) * m1;
}
#define GALAXYCURVE(k, t) galaxycurve(k, LENGTH(k), t)

/* 开场节拍效果的强度 (开场 / 坍缩中有效, 快进或坍缩时随 beatfade 淡出) */
static double
galaxybeatw(void)
{
    GalaxyScene *r = &galaxyscene;

    return r->mode == GalaxyIntro || r->mode == GalaxyCollapse ? r->beatfade : 0;
}

/* 核心点火的时刻 (开场时钟): 开场 C 里核心先从爆心飞到位, 再依次点火 */
static double
galaxyignitet(GalaxyCore *g)
{
    return galaxyscene.variant == GalaxyBang ? 1.15 + .05 * g->rank : .62 + .06 * g->rank;
}

/* 开场时钟 -> 关键帧 stage: 旋转结束 (3.3) 后平滑停在 3.45, 留出俯冲 / 点名的时间, 4.5 起再落定到 GALAXYHOLD (C1 连续) */
static double
galaxyintrostage(double scene)
{
    double x;

    if (scene <= 3.3)
        return scene;
    if (scene < 3.6) {
        x = scene - 3.3;
        return 3.3 + x - x * x / .6;
    }
    return 3.45 + (GALAXYHOLD - 3.45) * galaxysmoothstep(galaxyphase(scene, 4.5, GALAXYIEND));
}

/* 可复现的伪随机数 [0, 1) */
static double
galaxyhash(unsigned int x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return (x & 0xffffff) / 16777216.0;
}

static GalaxyVec galaxyv(double x, double y, double z) { return (GalaxyVec){x, y, z}; }
static GalaxyVec galaxyadd(GalaxyVec a, GalaxyVec b) { return galaxyv(a.x + b.x, a.y + b.y, a.z + b.z); }
static GalaxyVec galaxysub(GalaxyVec a, GalaxyVec b) { return galaxyv(a.x - b.x, a.y - b.y, a.z - b.z); }
static GalaxyVec galaxyscale(GalaxyVec a, double s) { return galaxyv(a.x * s, a.y * s, a.z * s); }
static double galaxydot(GalaxyVec a, GalaxyVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double galaxylen(GalaxyVec a) { return sqrt(galaxydot(a, a)); }

static GalaxyVec
galaxycross(GalaxyVec a, GalaxyVec b)
{
    return galaxyv(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static GalaxyVec
galaxynormalize(GalaxyVec a)
{
    double l = galaxylen(a);
    return l > 1e-12 ? galaxyscale(a, 1 / l) : galaxyv(0, 0, 1);
}

static GalaxyVec
galaxylerp(GalaxyVec a, GalaxyVec b, double t)
{
    return galaxyadd(a, galaxyscale(galaxysub(b, a), t));
}

/* 二次 Bezier: 3D 曲线轨迹 */
static GalaxyVec
galaxybezier(GalaxyVec a, GalaxyVec c, GalaxyVec b, double t)
{
    return galaxylerp(galaxylerp(a, c, t), galaxylerp(c, b, t), t);
}

static GalaxyMat
galaxyrotx(double a)
{
    double c = cos(a), s = sin(a);
    return (GalaxyMat){{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}

static GalaxyMat
galaxyroty(double a)
{
    double c = cos(a), s = sin(a);
    return (GalaxyMat){{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}

static GalaxyMat
galaxyrotz(double a)
{
    double c = cos(a), s = sin(a);
    return (GalaxyMat){{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
}

static GalaxyMat
galaxymul(GalaxyMat a, GalaxyMat b)
{
    GalaxyMat r;
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

static GalaxyVec
galaxyapply(GalaxyMat m, GalaxyVec v)
{
    return galaxyv(m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
                  m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
                  m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z);
}

static GalaxyMat
galaxytranspose(GalaxyMat a)
{
    GalaxyMat r;
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            r.m[i][j] = a.m[j][i];
    return r;
}

/* yaw (y) -> pitch (x) -> roll (z), 俯仰角不会接近 90°, 不会出现万向节锁 */
static GalaxyMat
galaxyeuler(double rx, double ry, double rz)
{
    return galaxymul(galaxyroty(ry), galaxymul(galaxyrotx(rx), galaxyrotz(rz)));
}

static GalaxyQuat
galaxyquat(GalaxyMat a)
{
    double (*m)[3] = a.m, t = m[0][0] + m[1][1] + m[2][2], s;

    if (t > 0) {
        s = sqrt(t + 1) * 2;
        return (GalaxyQuat){.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        s = sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        return (GalaxyQuat){(m[2][1] - m[1][2]) / s, .25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
    } else if (m[1][1] > m[2][2]) {
        s = sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        return (GalaxyQuat){(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, .25 * s, (m[1][2] + m[2][1]) / s};
    }
    s = sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
    return (GalaxyQuat){(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, .25 * s};
}

static GalaxyMat
galaxyquatmat(GalaxyQuat q)
{
    double w = q.w, x = q.x, y = q.y, z = q.z;

    return (GalaxyMat){{
        {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
        {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
        {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}}};
}

/* 两个朝向之间的插值: 四元数球面插值, 任意角度差都稳定 */
static GalaxyMat
galaxyblend(GalaxyMat a, GalaxyMat b, double t)
{
    GalaxyQuat qa = galaxyquat(a), qb = galaxyquat(b), q;
    double d = qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z, th, sa, sb, l;

    if (t <= 0)
        return a;
    if (t >= 1)
        return b;
    if (d < 0) {
        qb = (GalaxyQuat){-qb.w, -qb.x, -qb.y, -qb.z};
        d = -d;
    }
    if (d > .9995) {
        sa = 1 - t;
        sb = t;
    } else {
        th = acos(d);
        sa = sin((1 - t) * th) / sin(th);
        sb = sin(t * th) / sin(th);
    }
    q = (GalaxyQuat){sa * qa.w + sb * qb.w, sa * qa.x + sb * qb.x, sa * qa.y + sb * qb.y, sa * qa.z + sb * qb.z};
    l = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return galaxyquatmat((GalaxyQuat){q.w / l, q.x / l, q.y / l, q.z / l});
}

/* ---------- 镜头与投影 ---------- */

static GalaxyProj
galaxyproject(GalaxyVec v)
{
    GalaxyCamera *cam = &galaxyscene.cam;
    GalaxyProj p = {0};
    GalaxyVec c = galaxyapply(cam->view, galaxysub(v, cam->pos));

    if (c.z < cam->near || c.z > cam->far)
        return p;
    p.scale = cam->focal / c.z;
    p.x = galaxyscene.vx + galaxyscene.vw * .5 + c.x * p.scale;
    p.y = galaxyscene.vy + galaxyscene.vh * .5 + c.y * p.scale;
    p.z = c.z;
    p.ok = 1;
    return p;
}

/* 深度决定亮度和模糊: 近处更亮更清晰, 远处更暗更模糊 */
static double
galaxydepthlight(double z)
{
    double l = 1.22 - .5 * z / galaxyscene.cam.dist;
    return l < .25 ? .25 : l > 1.15 ? 1.15 : l;
}

/* 贴近镜头时淡出 (.45F -> .25F): 物体穿过镜头附近时不会突然出现或消失. 卡片在 .25F 处被裁掉, 正好已经完全透明 */
static double
galaxynearfade(double z)
{
    return galaxysmoothstep((z - galaxyscene.cam.focal * .25) / (galaxyscene.cam.focal * .2));
}

static double
galaxydepthblur(double z)
{
    return galaxyclamp((z / galaxyscene.cam.dist - 1.05) / .5);
}

/* 镜头绕星系群中心运动: 距离 + 偏航 / 俯仰 / 翻滚 (度) */
static void
galaxysetcameraat(GalaxyVec target, double dist, double pitch, double yaw, double roll)
{
    GalaxyCamera *cam = &galaxyscene.cam;
    double d2r = GALAXYPI / 180;
    GalaxyMat orbit;

    cam->dist = dist;
    cam->rx = pitch * d2r;
    cam->ry = yaw * d2r;
    cam->rz = roll * d2r;
    cam->target = target;
    orbit = galaxymul(galaxyroty(cam->ry), galaxyrotx(cam->rx));
    cam->pos = galaxyadd(cam->target, galaxyapply(orbit, galaxyv(0, 0, -cam->dist)));
    cam->rot = galaxymul(orbit, galaxyrotz(cam->rz));
    cam->view = galaxytranspose(cam->rot);
}

/* 其余部分按依赖顺序 include (同一个编译单元, 都是 static) */
#include "galaxy-gl.c"
#include "galaxy-scene.c"
#include "galaxy-space.c"
#include "galaxy-render.c"
#include "galaxy-build.c"
#include "galaxy-heat.c"
#include "galaxy-control.c"
