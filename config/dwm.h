#include <X11/XF86keysym.h>

static int showsystray                   = 1;         /* 是否显示托盘栏 */
static const int newclientathead         = 0;         /* 定义新窗口在栈顶还是栈底 */
static const int warppointer             = 0;         /* 键盘切换焦点时是否把鼠标移到目标窗口中间; 0: 同一显示器内不移动 (跨显示器仍会移过去) */
static const int managetransientwin      = 1;         /* 是否管理临时窗口 */
static const unsigned int borderpx       = 3;         /* 窗口边框大小 */
static const unsigned int systraypinning = 1;         /* 托盘跟随的显示器 0代表不指定显示器 */
static const unsigned int systrayspacing = 1;         /* 托盘间距 */
static const unsigned int systrayspadding = 5;        /* 托盘和状态栏的间隙 */
static int gappi                         = 12;        /* 窗口与窗口 缝隙大小 */
static int gappo                         = 12;        /* 窗口与边缘 缝隙大小 */
static const int _gappo                  = 12;        /* 窗口与窗口 缝隙大小 不可变 用于恢复时的默认值 */
static const int _gappi                  = 12;        /* 窗口与边缘 缝隙大小 不可变 用于恢复时的默认值 */
static const int vertpad                 = 5;         /* vertical padding of bar */
static const int sidepad                 = 5;         /* horizontal padding of bar */
static const int showbar                 = 1;         /* 是否显示状态栏 */
static const int topbar                  = 1;         /* 指定状态栏位置 0底部 1顶部 */
static const float mfact                 = 0.6;       /* 主工作区 大小比例 */
static const int   nmaster               = 1;         /* 主工作区 窗口数量 */
static const unsigned int snap           = 10;        /* 边缘依附宽度 */
static const unsigned int baralpha       = 0xc0;      /* 状态栏透明度 */
static const unsigned int borderalpha    = 0xdd;      /* 边框透明度 */
static const char *fonts[]               = { "JetBrainsMono Nerd Font Mono:style=medium:size=13", "monospace:size=13" };
static const char *colors[][3]           = {          /* 颜色设置 ColFg, ColBg, ColBorder */ 
    [SchemeNorm] = { "#bbbbbb", "#333333", "#444444" },
    [SchemeSel] = { "#ffffff", "#37474F", "#ffffff" },
    [SchemeSelGlobal] = { "#ffffff", "#37474F", "#ffffff" }, /* 全局窗口 (super g) 不靠颜色区分: 窗口右上角有图钉徽章, 状态栏标题前有图钉图标 */
    [SchemeHid] = { "#dddddd", NULL, NULL },
    [SchemeSystray] = { NULL, "#7799AA", NULL },
    [SchemeNormTag] = { "#bbbbbb", "#333333", NULL },
    [SchemeSelTag] = { "#eeeeee", "#333333", NULL },
    [SchemeBarEmpty] = { NULL, "#111111", NULL },
};
static const unsigned int alphas[][3]    = {          /* 透明度设置 ColFg, ColBg, ColBorder */ 
    [SchemeNorm] = { OPAQUE, baralpha, borderalpha }, 
    [SchemeSel] = { OPAQUE, baralpha, borderalpha },
    [SchemeSelGlobal] = { OPAQUE, baralpha, borderalpha },
    [SchemeNormTag] = { OPAQUE, baralpha, borderalpha }, 
    [SchemeSelTag] = { OPAQUE, baralpha, borderalpha },
    [SchemeBarEmpty] = { 0, 0x11, 0 },
    [SchemeStatusText] = { OPAQUE, 0x88, 0 },
};

/* 自定义脚本位置 */
static const char *autostartscript = "$DWM/bin/autostart.sh";
static const char *statusbarscript = "$DWM/bin/statusbar/statusbar.sh";

/* 自定义 scratchpad instance */
static const char scratchpadname[] = "scratchpad";

/* 自定义tag名称 */
/* 自定义特定实例的显示状态 */
// 󰎡 󰎤 󰎧 󰎪 󰎭 󰎱 󰎳 󰎶 󰎹 󰎼 󰀽 󰚺 󰎃 󰙯 切
static const char *tags[] = { 
    "", // tag:0  key:1  desc:terminal1
    "󰎧", // tag:1  key:2  desc:terminal2
    "󰎪", // tag:2  key:3  desc:terminal3
    "󰕧", // tag:4  key:9  desc:obs
    "", // tag:5  key:c  desc:chrome
    "󰎄", // tag:6  key:m  desc:music
    "", // tag:7  key:0  desc:steam
    "󰇩", // tag:8  key:w  desc:edge
    "󰨞", // tag:9  key:v  desc:vscode
};

/* 自定义窗口显示规则 */
/* class instance title 主要用于定位窗口适合哪个规则 */
/* tags mask 定义符合该规则的窗口的tag 0 表示当前tag */
/* isfloating 定义符合该规则的窗口是否浮动 */
/* isglobal 定义符合该规则的窗口是否全局浮动 */
/* isnoborder 定义符合该规则的窗口是否无边框 */
/* monitor 定义符合该规则的窗口显示在哪个显示器上 -1 为当前屏幕 */
/* floatposition 定义符合该规则的窗口显示的位置 0 中间，1到9分别为9宫格位置，例如1左上，9右下，3右上 */
static const Rule rules[] = {
    /* class                 instance              title             tags mask     isfloating  isglobal    isnoborder monitor floatposition */
    /** 优先级高 越在上面优先度越高 */
    { NULL,                  NULL,                "保存文件",        0,            1,          0,          0,        -1,      0}, // 浏览器保存文件      浮动
    { NULL,                  NULL,                "图片查看器",      0,            1,          0,          0,        -1,      0}, // qq图片查看器        浮动
    { NULL,                  NULL,                "图片查看",        0,            1,          0,          0,        -1,      0}, // 微信图片查看器      浮动
    { NULL,                  NULL,                "预览",            0,            1,          0,          0,        -1,      0}, // 企业微信图片查看器  浮动
    { NULL,                  NULL,                "Media viewer",    0,            1,          0,          0,        -1,      0}, // tg图片查看器        浮动

    /** 普通优先度 */
    {"obs",                  NULL,                 NULL,             1 << 3,       0,          0,          0,        -1,      0}, // obs        tag -> 󰕧
    {"chrome",               NULL,                 NULL,             1 << 4,       0,          0,          0,        -1,      0}, // chrome     tag -> 
    {"Chromium",             NULL,                 NULL,             1 << 4,       0,          0,          0,        -1,      0}, // Chromium   tag -> 
    {"music",                NULL,                 NULL,             1 << 5,       1,          0,          1,        -1,      0}, // music      tag -> 󰎄 浮动、无边框
    {"splayer",              NULL,                 NULL,             1 << 5,       0,          0,          0,        -1,      0}, // 主窗口 class 是小写 splayer, 固定在 Super+M
    {"SPlayer",              NULL,                 NULL,             1 << 5,       1,          0,          1,        -1,      0}, // Electron 的 10x10 / 200x200 小窗, 浮动无边框, 不参与平铺
    {"steam",                NULL,                 NULL,             1 << 6,       0,          0,          0,        -1,      0}, // steam      tag -> 
    {"Microsoft-edge",       NULL,                 NULL,             1 << 7,       0,          0,          0,        -1,      0}, // edge       tag -> 󰇩
    {"Code",                 NULL,                 NULL,             1 << 8,       0,          0,          0,        -1,      0}, // vscode     tag -> 󰨞
    {"Vncviewer",            NULL,                 NULL,             0,            1,          0,          1,        -1,      2}, // Vncviewer           浮动、无边框 屏幕顶部
    {"flameshot",            NULL,                 NULL,             0,            1,          0,          0,        -1,      0}, // 火焰截图            浮动
    {"scratchpad",          "scratchpad",         "scratchpad",      TAGMASK,      1,          1,          1,        -1,      2}, // scratchpad          浮动、全局、无边框 屏幕顶部
    {"Pcmanfm",              NULL,                 NULL,             0,            1,          0,          1,        -1,      0}, // pcmanfm             浮动、无边框 居中
    {"wemeetapp",            NULL,                 NULL,             TAGMASK,      1,          1,          0,        -1,      0}, // !!!腾讯会议在切换tag时有诡异bug导致退出 变成global来规避该问题

    /** 部分特殊class的规则 */
    {"float",                NULL,                 NULL,             0,            1,          0,          0,        -1,      0}, // class = float       浮动
    {"linux-wallpaperengine",NULL,                 NULL,             0,            1,          1,          1,        -1,      1}, // 场景壁纸窗口刚出现时浮动在左上角 (随后 livewall.sh 把它改成不受 dwm 管理的最底层窗口), 不打乱平铺
    {"global",               NULL,                 NULL,             TAGMASK,      0,          1,          0,        -1,      0}, // class = gloabl      全局
    {"noborder",             NULL,                 NULL,             0,            0,          0,          1,        -1,      0}, // class = noborder    无边框
    {"FGN",                  NULL,                 NULL,             TAGMASK,      1,          1,          1,        -1,      0}, // class = FGN         浮动、全局、无边框
    {"FG",                   NULL,                 NULL,             TAGMASK,      1,          1,          0,        -1,      0}, // class = FG          浮动、全局
    {"FN",                   NULL,                 NULL,             0,            1,          0,          1,        -1,      0}, // class = FN          浮动、无边框
    {"GN",                   NULL,                 NULL,             TAGMASK,      0,          1,          1,        -1,      0}, // CLASS = GN          全局、无边框

    /** 优先度低 越在上面优先度越低 */
    { NULL,                  NULL,                "crx_",            0,            1,          0,          0,        -1,      0}, // 错误载入时 会有crx_ 浮动
    { NULL,                  NULL,                "broken",          0,            1,          0,          0,        -1,      0}, // 错误载入时 会有broken 浮动
};

/* 自定义布局 */
static const Layout layouts[] = {
    { "󰙀",  tile },         /* 主次栈 */
    { "󰕰",  magicgrid },    /* 网格 */
};

#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }
#define MODKEY Mod4Mask
#define TAGKEYS(KEY, TAG, cmd, once) \
    { MODKEY,              KEY, view,       {.ui = 1 << TAG, .i = once, .v = cmd} }, \
    { MODKEY|ShiftMask,    KEY, tag,        {.ui = 1 << TAG} }, \
    { MODKEY|ControlMask,  KEY, toggleview, {.ui = 1 << TAG} }, \

static Key keys[] = {
    /* modifier            key              function          argument */
    { MODKEY,              XK_equal,        toggletoplayer,   {0} },                     /* super +            |  平铺层/浮动层交替放到最上层 */

    { MODKEY,              XK_Tab,          focuslast,        {0} },                     /* super tab          |  切到上一个用过的窗口 (连按来回切) */
    { MODKEY,              XK_Up,           focusstack,       {.i = -1} },               /* super up           |  本tag内切换聚焦窗口 */
    { MODKEY,              XK_Down,         focusstack,       {.i = +1} },               /* super down         |  本tag内切换聚焦窗口 */

    { MODKEY,              XK_Left,         viewtoleft,       {0} },                     /* super left         |  聚焦到左边的tag */
    { MODKEY,              XK_Right,        viewtoright,      {0} },                     /* super right        |  聚焦到右边的tag */
    { MODKEY,              XK_grave,        view,             {0} },                     /* super `            |  在最近两个tag之间来回切换 */
    { MODKEY|ShiftMask,    XK_Left,         tagtoleft,        {0} },                     /* super shift left   |  将本窗口移动到左边tag */
    { MODKEY|ShiftMask,    XK_Right,        tagtoright,       {0} },                     /* super shift right  |  将本窗口移动到右边tag */

    { MODKEY,              XK_a,            overview,         {0} },                     /* super a            |  窗口总览 (点选进入) */
    { MODKEY,              XK_z,            galaxy,           {0} },                     /* super z            |  3D 工作空间星系 (停在轨道态) / 回到桌面 */

    { MODKEY,              XK_comma,        setmfact,         {.f = -0.05} },            /* super ,            |  缩小主工作区 */
    { MODKEY,              XK_period,       setmfact,         {.f = +0.05} },            /* super .            |  放大主工作区 */

    { MODKEY,              XK_i,            hidewin,          {0} },                     /* super i            |  隐藏 窗口 */
    { MODKEY|ShiftMask,    XK_i,            restorewin,       {0} },                     /* super shift i      |  取消隐藏 窗口 */

    { MODKEY|ShiftMask,    XK_Return,       zoom,             {0} },                     /* super shift enter  |  将当前聚焦窗口置为主窗口 */

    { MODKEY,              XK_t,            togglefloating,   {0} },                     /* super t            |  聚焦窗口 浮动/平铺 (浮动时回到上次位置) */
    { MODKEY|ShiftMask,    XK_t,            tileall,          {0} },                     /* super shift t      |  本tag 全部窗口回到平铺 */
    { MODKEY,              XK_space,        selectlayout,     {.v = &layouts[1]} },      /* super space        |  网格/平铺布局切换 */
    { MODKEY,              XK_f,            fullscreen,       {0} },                     /* super f            |  开启/关闭 全屏 */
    { MODKEY|ShiftMask,    XK_f,            togglebarglobal,  {0} },                     /* super shift f      |  开启/关闭 状态栏 (所有 tag 和显示器) */
    { MODKEY,              XK_g,            toggleglobal,     {0} },                     /* super g            |  开启/关闭 全局 */
    { MODKEY,              XK_u,            toggleborder,     {0} },                     /* super u            |  开启/关闭 边框 */
    { MODKEY,              XK_e,            incnmaster,       {.i = +1} },               /* super e            |  改变主工作区窗口数量 (1 2中切换) */

    { MODKEY,              XK_b,            focusmon,         {.i = +1} },               /* super b            |  光标移动到另一个显示器 */
    { MODKEY|ShiftMask,    XK_b,            tagmon,           {.i = +1} },               /* super shift b      |  将聚焦窗口移动到另一个显示器 */

    { MODKEY,              XK_q,            killclient,       {0} },                     /* super q            |  关闭窗口 */
    { MODKEY|ControlMask,  XK_q,            forcekillclient,  {0} },                     /* super ctrl q       |  强制关闭窗口(处理某些情况下无法销毁的窗口) */
    { MODKEY|ShiftMask,    XK_Escape,       spawn,            SHCMD("$DWM/bin/power.sh") }, /* super shift esc    |  电源菜单: 锁屏 / 睡眠 / 休眠 / 注销 / 重启 / 关机 (注销等需确认) */
    { MODKEY|ShiftMask,    XK_r,            spawn,            SHCMD("$DWM/bin/reload.sh") }, /* super shift r   |  编译安装 dwm 并原地重启(窗口与tag保留) */

	{ MODKEY,              XK_o,            showonlyorall,    {0} },                     /* super o            |  切换 只显示一个窗口 / 全部显示 */

    { MODKEY|ControlMask,  XK_equal,        setgap,           {.i = -6} },               /* super ctrl +       |  减小窗口间距 (窗口变大) */
    { MODKEY|ControlMask,  XK_minus,        setgap,           {.i = +6} },               /* super ctrl -       |  增大窗口间距 (窗口变小) */
    { MODKEY|ControlMask,  XK_space,        setgap,           {.i = 0} },                /* super ctrl space   |  重置窗口间距 */

    { MODKEY|ControlMask,  XK_Up,           movewin,          {.ui = UP} },              /* super ctrl up      |  移动窗口 */
    { MODKEY|ControlMask,  XK_Down,         movewin,          {.ui = DOWN} },            /* super ctrl down    |  移动窗口 */
    { MODKEY|ControlMask,  XK_Left,         movewin,          {.ui = LEFT} },            /* super ctrl left    |  移动窗口 */
    { MODKEY|ControlMask,  XK_Right,        movewin,          {.ui = RIGHT} },           /* super ctrl right   |  移动窗口 */

    { MODKEY|Mod1Mask,     XK_Up,           resizewin,        {.ui = V_REDUCE} },        /* super alt up       |  调整窗口 */
    { MODKEY|Mod1Mask,     XK_Down,         resizewin,        {.ui = V_EXPAND} },        /* super alt down     |  调整窗口 */
    { MODKEY|Mod1Mask,     XK_Left,         resizewin,        {.ui = H_REDUCE} },        /* super alt left     |  调整窗口 */
    { MODKEY|Mod1Mask,     XK_Right,        resizewin,        {.ui = H_EXPAND} },        /* super alt right    |  调整窗口 */

  	{ MODKEY,              XK_k,            focusdir,         {.i = UP } },              /* super k            | 同层聚焦上方窗口 */
  	{ MODKEY,              XK_j,            focusdir,         {.i = DOWN } },            /* super j            | 同层聚焦下方窗口 */
  	{ MODKEY,              XK_h,            focusdir,         {.i = LEFT } },            /* super h            | 同层聚焦左侧窗口 */
  	{ MODKEY,              XK_l,            focusdir,         {.i = RIGHT } },           /* super l            | 同层聚焦右侧窗口 */
    { MODKEY|ShiftMask,    XK_k,            exchange_client,  {.i = UP } },              /* super shift k      | 平铺: 二维交换窗口 / 浮动: 贴边 */
    { MODKEY|ShiftMask,    XK_j,            exchange_client,  {.i = DOWN } },            /* super shift j      | 平铺: 二维交换窗口 / 浮动: 缩回默认大小并居中 */
    { MODKEY|ShiftMask,    XK_h,            exchange_client,  {.i = LEFT} },             /* super shift h      | 平铺: 二维交换窗口 / 浮动: 贴边 */
    { MODKEY|ShiftMask,    XK_l,            exchange_client,  {.i = RIGHT } },           /* super shift l      | 平铺: 二维交换窗口 / 浮动: 贴边 */

    /* spawn + SHCMD 执行对应命令(已下部分建议完全自己重新定义) */
    { MODKEY,              XK_s,      togglescratch, SHCMD("tabbed -n scratchpad -c -r 2 st -w ''") },          /* super s          | 打开st scratchpad      */
    { MODKEY,              XK_Return, spawn, SHCMD("tabbed -n st -C tabbed -c -r 2 st -w ''") },                /* super enter      | 打开st                 */
    { MODKEY,              XK_minus,  spawn, SHCMD("tabbed -n st -C float -c -r 2 st -w ''") },                 /* super -          | 打开浮动st终端         */
    { MODKEY,              XK_r,      spawn, SHCMD("killall pcmanfm || pcmanfm") },                             /* super r          | 打开/关闭pcmanfm       */
    { MODKEY,              XK_d,      spawn, SHCMD("rofi -show drun") },                                        /* super d          | rofi: 启动应用 (带图标) */
    { MODKEY|ShiftMask,    XK_d,      spawn, SHCMD("rofi -show run") },                                         /* super shift d    | rofi: 执行命令         */
    { MODKEY,              XK_p,      spawn, SHCMD("$DWM/bin/rofi.sh") },                                       /* super p          | rofi: 执行自定义脚本   */
    { MODKEY|ControlMask,  XK_l,      spawn, SHCMD("$DWM/bin/blurlock.sh") },                                   /* super ctrl l     | 锁定屏幕               */
    { MODKEY|ShiftMask,    XK_Up,     focuslayer, {.i = 1} },                                                    /* super shift up   | 进入浮动层 (已在浮动层则轮换并抬到最前) */
    { MODKEY|ShiftMask,    XK_Down,   focuslayer, {.i = 0} },                                                    /* super shift down | 进入平铺层 (被盖住时把浮动窗口挪开) */
    { MODKEY|ShiftMask,    XK_a,      spawn, SHCMD("flameshot gui") },                   /* super shift a    | 截图                   */
    { MODKEY,              XK_y,      spawn, SHCMD("$DWM/bin/translate.sh") },                                  /* super y          | 翻译选中的文字         */
    { MODKEY|ShiftMask,    XK_y,      spawn, SHCMD("$DWM/bin/translate.sh input") },                            /* super shift y    | 输入文字翻译并复制     */
    { MODKEY,              XK_x,      spawn, SHCMD("CM_LAUNCHER=rofi clipmenu -p 剪贴板") },                    /* super x          | 剪贴板历史 (选中的放进剪贴板) */
    { MODKEY,              XK_n,      spawn, SHCMD("dunstctl history-pop") },                                   /* super n          | 重新显示上一条通知     */
    { MODKEY|ShiftMask,    XK_n,      spawn, SHCMD("dunstctl close-all") },                                     /* super shift n    | 关闭所有通知           */
    { MODKEY,              XK_slash,  spawn, SHCMD("$DWM/bin/keys.sh") },                                       /* super /          | 快捷键速查             */
    { 0,                   XF86XK_AudioRaiseVolume, spawn, SHCMD("$DWM/bin/set_vol.sh up") },                    /* 音量加键        | 音量加                 */
    { 0,                   XF86XK_AudioLowerVolume, spawn, SHCMD("$DWM/bin/set_vol.sh down") },                  /* 音量减键        | 音量减                 */
    { 0,                   XF86XK_AudioMute,  spawn, SHCMD("pactl set-sink-mute @DEFAULT_SINK@ toggle; $DWM/bin/statusbar/statusbar.sh update vol; bash $DWM/bin/statusbar/packages/vol.sh notify") }, /* 静音键          | 静音 / 取消静音        */
    { 0,                   XF86XK_AudioPlay,  spawn, SHCMD("playerctl play-pause; $DWM/bin/statusbar/statusbar.sh update music") }, /* 播放键          | 播放 / 暂停            */
    { 0,                   XF86XK_AudioPause, spawn, SHCMD("playerctl play-pause; $DWM/bin/statusbar/statusbar.sh update music") }, /* 暂停键          | 播放 / 暂停            */
    { 0,                   XF86XK_AudioNext,  spawn, SHCMD("playerctl next; $DWM/bin/statusbar/statusbar.sh update music") },       /* 下一首键        | 下一首                 */
    { 0,                   XF86XK_AudioPrev,  spawn, SHCMD("playerctl previous; $DWM/bin/statusbar/statusbar.sh update music") },   /* 上一首键        | 上一首                 */

    /* super key : 跳转到对应tag. 没有窗口时执行命令; 已在该 tag 且还有可见窗口时再按无反应.
     * once=1: 窗口都隐藏了则取消隐藏, 不多开. once=0: 窗口都隐藏了仍执行命令, 再开一个. */
    /* super shift key : 将聚焦窗口移动到对应tag */
    /* key tag cmd */
    TAGKEYS(XK_1, 0, 0, 0)
    TAGKEYS(XK_2, 1, 0, 0)
    TAGKEYS(XK_3, 2, 0, 0)
    TAGKEYS(XK_9, 3, "obs", 1)
    TAGKEYS(XK_c, 4, "google-chrome-stable --new-window https://www.youtube.com", 0)
    TAGKEYS(XK_m, 5, "~/.local/share/splayer/SPlayer.AppImage --no-sandbox", 1)
    TAGKEYS(XK_0, 6, "~/.local/bin/steam", 1)
    TAGKEYS(XK_w, 7, "microsoft-edge-stable --new-window https://www.github.com", 0)
    TAGKEYS(XK_v, 8, "code", 0)
};

static Button buttons[] = {
    /* click               event mask       button            function       argument  */
    /* 点击窗口标题栏操作 */
    { ClkWinTitle,         0,               Button1,          hideotherwins, {0} },                                   // 左键        |  点击标题     |  隐藏其他窗口仅保留该窗口
    { ClkWinTitle,         0,               Button3,          togglewin,     {0} },                                   // 右键        |  点击标题     |  切换窗口显示状态
    /* 点击窗口操作 */
    { ClkClientWin,        MODKEY,          Button1,          movemouse,     {0} },                                   // super+左键  |  拖拽窗口     |  拖拽窗口
    { ClkClientWin,        MODKEY,          Button3,          resizemouse,   {0} },                                   // super+右键  |  拖拽窗口     |  改变窗口大小
    /* 点击状态栏布局图标操作 */
    { ClkLtSymbol,         0,               Button1,          selectlayout,  {.v = &layouts[1]} },                    // 左键        |  点击布局图标 |  网格/平铺布局切换
    { ClkLtSymbol,         0,               Button3,          tileall,       {0} },                                   // 右键        |  点击布局图标 |  本tag 全部窗口回到平铺
    { ClkLtSymbol,         0,               Button4,          cyclelayout,   {.i = -1} },                             // 鼠标滚轮上  |  布局图标     |  上一个布局 (平铺/网格)
    { ClkLtSymbol,         0,               Button5,          cyclelayout,   {.i = +1} },                             // 鼠标滚轮下  |  布局图标     |  下一个布局 (平铺/网格)
    /* 点击tag操作 */
    { ClkTagBar,           0,               Button1,          view,          {0} },                                   // 左键        |  点击tag      |  切换tag
	{ ClkTagBar,           0,               Button3,          toggleview,    {0} },                                   // 右键        |  点击tag      |  切换是否显示tag
    { ClkTagBar,           MODKEY,          Button1,          tag,           {0} },                                   // super+左键  |  点击tag      |  将窗口移动到对应tag
    { ClkTagBar,           0,               Button4,          viewtoleft,    {0} },                                   // 鼠标滚轮上  |  tag          |  向前切换tag
	{ ClkTagBar,           0,               Button5,          viewtoright,   {0} },                                   // 鼠标滚轮下  |  tag          |  向后切换tag
    /* 点击状态栏操作 */
    { ClkStatusText,       0,               Button1,          clickstatusbar,{0} },                                   // 左键        |  点击状态栏   |  根据状态栏的信号执行 $DWM/bin/statusbar/statusbar.sh $signal L
    { ClkStatusText,       0,               Button2,          clickstatusbar,{0} },                                   // 中键        |  点击状态栏   |  根据状态栏的信号执行 $DWM/bin/statusbar/statusbar.sh $signal M
    { ClkStatusText,       0,               Button3,          clickstatusbar,{0} },                                   // 右键        |  点击状态栏   |  根据状态栏的信号执行 $DWM/bin/statusbar/statusbar.sh $signal R
    { ClkStatusText,       0,               Button4,          clickstatusbar,{0} },                                   // 鼠标滚轮上  |  状态栏       |  根据状态栏的信号执行 $DWM/bin/statusbar/statusbar.sh $signal U
    { ClkStatusText,       0,               Button5,          clickstatusbar,{0} },                                   // 鼠标滚轮下  |  状态栏       |  根据状态栏的信号执行 $DWM/bin/statusbar/statusbar.sh $signal D
                                                                                                                      //
    /* 点击bar空白处 */
    { ClkBarEmpty,         0,               Button1,          spawn, SHCMD("rofi -show window") },                    // 左键        |  bar空白处    |  rofi 执行 window
    { ClkBarEmpty,         0,               Button3,          spawn, SHCMD("rofi -show drun") },                      // 右键        |  bar空白处    |  rofi 执行 drun
                                                                                                                      //
    /* 鼠标在空白处或任意窗口上 上下滚动 切换tag */
    { ClkRootWin,          MODKEY,          Button4,          viewtoleft,    {0} },                                   // super+滚轮上  |  Any          |  向前切换tag
    { ClkRootWin,          MODKEY,          Button5,          viewtoright,   {0} },                                   // super+滚轮下  |  Any          |  向后切换tag
    { ClkWinTitle,         MODKEY,          Button4,          viewtoleft,    {0} },                                   // super+滚轮上  |  Any          |  向前切换tag
    { ClkWinTitle,         MODKEY,          Button5,          viewtoright,   {0} },                                   // super+滚轮下  |  Any          |  向后切换tag
    { ClkClientWin,        MODKEY,          Button4,          viewtoleft,    {0} },                                   // super+滚轮上  |  Any          |  向前切换tag
    { ClkClientWin,        MODKEY,          Button5,          viewtoright,   {0} },                                   // super+滚轮下  |  Any          |  向后切换tag
    { ClkTagBar,           MODKEY,          Button4,          viewtoleft,    {0} },                                   // super+滚轮上  |  Any          |  向前切换tag
    { ClkTagBar,           MODKEY,          Button5,          viewtoright,   {0} },                                   // super+滚轮下  |  Any          |  向后切换tag
    { ClkStatusText,       MODKEY,          Button4,          viewtoleft,    {0} },                                   // super+滚轮上  |  Any          |  向前切换tag
    { ClkStatusText,       MODKEY,          Button5,          viewtoright,   {0} },                                   // super+滚轮下  |  Any          |  向后切换tag
};

/* Super+Z 星系 (dwm/galaxy*.c). 不写时用 galaxy.c 里的默认值; 改完按 Super+Shift+R 重新编译生效.
 * 省电模式 (Super+P / 拔电自动) 打开时星系自动进入安静模式: 特效间隔 x2.5, 不显示流星 / 彗星 / 轨道光流, 帧率降低 */
#define GALAXY_INTRO    1.35    /* 开场时间拉伸: 越大开场越慢 (1.35 约 7.7 秒) */
#define GALAXY_FXGAP    1.0     /* 驻留特效间隔的倍数: 2 = 特效少一半 */
#define GALAXY_DIAG     30.0    /* 驻留时镜头翻滚 (度), 轨道盘面沿屏幕对角线铺开; 0 = 水平 */
#define GALAXY_TOURDIST .5      /* 巡游镜头离星系的距离: 越小越近 */
#define GALAXY_SHOT     9.0     /* 每个机位停留的秒数 */
#define GALAXY_QUIETFPS 30.0    /* 安静模式的驻留帧率 */
