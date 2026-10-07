/* Super+Z 星系: OpenGL 光层与合成.
 *
 * 画面分三层, 每帧由 GPU 合成后输出到遮罩窗口:
 *   底层 (r->back, XRender): 壁纸 / 深空 / 窗口卡片 / 桌面截图 / 碎块. 与真实桌面逐像素一致的部分都在这里
 *   光层 (本文件, RGBA16F): 光点、光带、闪光、粒子. 加法混合, 叠加处越叠越亮; 再提取泛光 (6 级双重 Kawase)
 *   前景层 (r->front, XRender ARGB): 文字、标签、状态栏、边框、结尾的桌面截图
 * 合成: 底层 与 (光层 + 泛光, 软拐点压缩) 做滤色混合, 再盖上前景层. 光层为空时输出就是底层本身,
 * 所以开场第一帧和回程最后一帧仍与真实桌面一致.
 *
 * 遮挡: 光点和光带按原来的画家顺序提交, 画到一张卡片时先把已经提交的光画掉, 再在光层上用卡片的形状「挖洞」
 * (挡住卡片后面的光) 并写入卡片的深度; 粒子最后统一画, 按深度测试, 被前面的卡片挡住.
 * 底层和前景层用 GLX_EXT_texture_from_pixmap 直接当纹理, 不经过 CPU. */
#include <epoxy/gl.h>
#include <epoxy/glx.h>

#define GALAXYGLINST    16384     /* 光点 / 光带一批的上限, 满了就先画 */
#define GALAXYGLPART    65536     /* 粒子上限 */
#define GALAXYBLOOM     6         /* 泛光的降采样级数 */
#define GALAXYBLOOMW    .75f      /* 泛光逐级权重: 第 i 级 (越往后越宽) 的权重是它的 i 次方 */
#define GALAXYBLOOMK    1.8       /* 加权后总能量变小 (1+.75+.56+... 约 3.3, 原来等权是 6 级), 整体补偿 */

enum { GalaxyGLHalo, GalaxyGLDisc, GalaxyGLSpike, GalaxyGLLine, GalaxyGLRect, GalaxyGLDust, GalaxyGLBokeh, GalaxyGLStar, GalaxyGLSurface, GalaxyGLStream };

typedef struct { float a[4], b[4], c0[4], c1[4]; } GalaxyGLInst;

/* 进程内只建一次的 GL 状态 (上下文 / 程序 / 缓冲); 每次 Super+Z 只重新绑定遮罩窗口和两张 pixmap */
static struct {
    int ready, failed, yinv24, yinv32, yinv32config;
    GLXFBConfig fbwin, fb24, fb32;
    XVisualInfo *vi;
    Colormap cmap;
    GLXContext ctx;
    GLuint proglight, progcut, progdown, progup, progcomp, progprobe, prognebula, progcopy, progcard, progpart;
    GLuint cardfbo, cardtex, cardvao, cardvbo, copyfbo, blurfbo, blurtex;
    float aniso;                      /* 各向异性过滤上限 (不支持时为 0) */
    int ncards;                       /* 这一帧画进卡片层的卡片数 (0 时合成不读卡片层) */
    unsigned long frameid;            /* 帧编号 (每帧 galaxyglframe 加一) */
    int cardups;                      /* 这一帧新建的卡片纹理数 (分摊到多帧, 避免开场卡顿) */
    int texmade, texfreed;            /* 本次 Super+Z 建 / 删的卡片纹理数 (写进日志, 查泄漏) */
    GLuint vao, vbo, fullvao;
    GLuint fbo, lighttex, depthtex, bloomfbo[GALAXYBLOOM], bloomtex[GALAXYBLOOM];
    int fw, fh, bw[GALAXYBLOOM], bh[GALAXYBLOOM];
    GLuint texback, texfront, texwall;
    GLXPixmap gback, gfront, gwall;
    float lens[4], shock[4], glow;    /* 收尾特效 (每帧 galaxyglframe 清零, 坍缩时 galaxyglexitfx 设置) */
    float rays[3], expo;
    float pull, burst, bext, axis[3];  /* Esc 收尾时粒子的吸入 / 爆发 (每帧清零, galaxyglcollapsefx 设置) */
    float rip[4], ripn[3];             /* 涟漪场: 半径 / 强度 / 宽度 (世界), 盘面法线 (每帧清零, galaxyglripple 设置) */              /* 体积光束 (光源像素位置 + 强度) / 光层曝光 (每帧清零, galaxyglpostfx 设置) */
    GLuint pbo[3];                    /* 读回最小一级泛光 (曝光适应), 轮流使用, 晚两帧读 */
    int pboi, pbocap, pbon_[3];
    GLsync pbofence[3];
    double lum;                       /* 最近读回的光层平均亮度 (最亮通道) */
    Window win;
    GalaxyGLInst *inst, *part;
    int ninst, npart;
    double gputime;
} galaxygl;

/* ---------- 着色器 ---------- */

static const char *galaxyglvslight =
    "#version 330 core\n"
    "layout(location=0) in vec4 ia; layout(location=1) in vec4 ib;\n"
    "layout(location=2) in vec4 ic0; layout(location=3) in vec4 ic1;\n"
    "uniform vec2 screen;\n"
    "out vec2 vl; out float vt; flat out vec4 fb; flat out vec4 fc0; flat out vec4 fc1; flat out vec2 flen; flat out float frad;\n"
    "void main() {\n"
    "  vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0 - 1.0, pos;\n"
    "  int kind = int(ib.z + .5);\n"
    "  vt = 0.0; flen = vec2(0.0); frad = ia.z;\n"
    "  if (kind == 3 || kind == 9) {\n"
    "    vec2 d = ia.zw - ia.xy; float L = max(length(d), 1e-3); vec2 dir = d / L, n = vec2(-dir.y, dir.x);\n"
    "    float e = ib.x + 1.5, along = mix(-e, L + e, c.x * .5 + .5), across = c.y * e;\n"
    "    pos = ia.xy + dir * along + n * across; vl = vec2(along, across); flen = vec2(L, ib.x); vt = clamp(along / L, 0.0, 1.0);\n"
    "  } else if (kind == 4) {\n"
    "    pos = ia.xy + (c * .5 + .5) * ia.zw; vl = c;\n"
    "  } else {\n"   /* ia.w: 形状的旋转角 (衍射芒随镜头转; 其他光点为 0) */
    "    pos = ia.xy + c * ia.z; vl = vec2(cos(ia.w) * c.x + sin(ia.w) * c.y, cos(ia.w) * c.y - sin(ia.w) * c.x);\n"
    "  }\n"
    "  gl_Position = vec4(pos.x / screen.x * 2.0 - 1.0, 1.0 - pos.y / screen.y * 2.0, ib.w, 1.0);\n"
    "  fb = ib; fc0 = ic0; fc1 = ic1;\n"
    "}\n";

/* 光晕 / 圆点 / 衍射芒的形状与原来的 sprite 相同; 白芯彩晕: 中心偏白, 颜色在衰减部分 (fc0.a 是着色强度) */
static const char *galaxyglfslight =
    "#version 330 core\n"
    "in vec2 vl; in float vt; flat in vec4 fb; flat in vec4 fc0; flat in vec4 fc1; flat in vec2 flen; flat in float frad;\n"
    "out vec4 o; uniform float time, still;\n"
    "float hs(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }\n"
    "float sn(vec2 p) {\n"
    "  vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(hs(i), hs(i + vec2(1, 0)), f.x), mix(hs(i + vec2(0, 1)), hs(i + vec2(1, 1)), f.x), f.y);\n"
    "}\n"
    "float h2(vec2 p) { return hs(p + 17.31); }\n"
    /* 星尘: 光斑 (像素坐标 P, 局部强度 w(P) 由调用方给出) 里按 4px 一格散布的粒子, 每格出现的概率跟强度走; 缓慢闪烁 */
    "float dust(vec2 P, float cellsz, float dens) {\n"
    "  vec2 g = floor(P / cellsz); float s = 0.0;\n"
    "  for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {\n"
    "    vec2 c = g + vec2(i, j), q = (c + vec2(hs(c), h2(c))) * cellsz;\n"
    "    float pr = hs(c + 3.7); if (pr > dens) continue;\n"
    "    float rd = .6 + .7 * hs(c + 5.1), d = length(P - q);\n"
    "    float tw = still > .5 ? 1.0 : .65 + .35 * sin(time * (1.0 + 2.0 * hs(c + 7.3)) + 6.28 * hs(c + 9.9));\n"
    "    s += exp(-d * d / (rd * rd)) * (.35 + .65 * pow(h2(c + 1.3), 3.0)) * tw;\n"
    "  }\n"
    "  return s;\n"
    "}\n"
    "";
static const char *galaxyglfslight2 =
    "void main() {\n"
    "  int kind = int(fb.z + .5); float a; vec3 col = fc0.rgb;\n"
    "  if (kind == 9) {\n"
    /* 粒子流: 沿线按间距分格, 每格一颗, 顺着线的方向流动, 横向越靠中间越密; 叠一点原来的实心线保持轮廓 */
    "    float hw = max(flen.y, .6), sp = max(2.5, .6 * hw), fl = (still > .5 ? 5.0 : 14.0) * time;\n"
    "    float x = vl.x + fl, g = floor(x / sp), s = 0.0;\n"
    "    for (int i = -1; i <= 1; i++) {\n"
    "      float c = g + float(i), along = (c + hs(vec2(c, 1.0))) * sp - fl;\n"
    "      float off = (hs(vec2(c, 2.0)) - .5) * (hs(vec2(c, 3.0)) * 2.0) * 1.6 * hw;\n"
    "      float rd = .6 + .9 * hs(vec2(c, 4.0)), d = length(vec2(vl.x - along, vl.y - off));\n"
    "      float tw = still > .5 ? 1.0 : .6 + .4 * sin(time * (1.2 + 2.0 * hs(vec2(c, 5.0))) + 6.28 * hs(vec2(c, 6.0)));\n"
    "      s += exp(-d * d / (rd * rd)) * (.35 + .65 * pow(hs(vec2(c, 7.0)), 3.0)) * tw;\n"
    "    }\n"
    "    float t = clamp(vl.x, 0.0, flen.x), dl = length(vec2(vl.x - t, vl.y));\n"
    "    a = clamp(.2 * clamp(flen.y + .5 - dl, 0.0, 1.0) + 1.3 * s * step(vl.x, flen.x + 2.0) * step(-2.0, vl.x), 0.0, 1.0);\n"
    "    col = mix(mix(fc0.rgb, fc1.rgb, vt), vec3(1.0), .35 * smoothstep(.6, 1.0, s));\n"
    "  } else if (kind == 8) {\n"
    /* 近处的恒星表面: 球面 (半径 .55) 有临边昏暗和缓慢流动的米粒纹理, 中间偏白; 球外一圈随角度起伏、缓慢翻涌的日冕 */
    "    float d = length(vl), x = d / .55, th = atan(vl.y, vl.x);\n"
    "    if (x < 1.0) {\n"
    "      float mu = sqrt(1.0 - x * x);\n"
    "      vec2 sp = vl / .55 / (1.0 + mu);\n"
    "      float gran = .7 + .3 * sn(sp * 14.0 + vec2(time * .15, -time * .1)) * (.6 + .4 * sn(sp * 31.0 - time * .2));\n"
    "      a = (.35 + .65 * mu) * gran;\n"
    "      col = mix(fc0.rgb, vec3(1.0), .55 * mu);\n"
    "    } else {\n"
    "      a = exp(-(x - 1.0) * 5.0) * (.5 + .5 * sn(vec2(th * 6.0, time * .3 - x * 2.0))) * .6 * (1.0 - smoothstep(.85, 1.0, d));\n"
    "    }\n"
    "  } else if (kind == 3) {\n"
    "    float t = clamp(vl.x, 0.0, flen.x), d = length(vec2(vl.x - t, vl.y));\n"
    "    a = clamp(flen.y + .5 - d, 0.0, 1.0); col = mix(fc0.rgb, fc1.rgb, vt);\n"
    "  } else if (kind == 4) {\n"
    "    a = 1.0;\n"
    "  } else {\n"
    "    float d = length(vl);\n"
    "    float rot = still > .5 ? 0.0 : time * .15; vec2 P = mat2(cos(rot), sin(rot), -sin(rot), cos(rot)) * vl * frad;\n"
    "    if (kind == 0) {\n"
    /* 柔光 -> 平滑光晕 (35%) + 一团缓慢旋转、闪烁的星尘 */
    "      float w = (.55 * exp(-d * d / .0288) + .3 * exp(-d * d / .1568) + .15 * exp(-d * d / .5)) * (1.0 - smoothstep(.75, 1.0, d));\n"
    "      a = .35 * w + (frad > 6.0 ? dust(P, 4.0, min(1.0, w * 1.6)) * .9 : .65 * w);\n"
    "    }\n"
    "    else if (kind == 2) {\n"
    /* 衍射芒 -> 沿四条射线 (45° 两条较淡) 排布的粒子串, 亮度沿射线衰减, 轻微向外流动 */
    "      vec2 Q = vl * frad; float fo = still > .5 ? 0.0 : time * 6.0, s = 0.0;\n"
    "      for (int k = 0; k < 4; k++) {\n"
    "        vec2 dir = k == 0 ? vec2(1, 0) : k == 1 ? vec2(0, 1) : k == 2 ? vec2(.7071, .7071) : vec2(.7071, -.7071);\n"
    "        float al = dot(Q, dir), pe = dot(Q, vec2(-dir.y, dir.x)), side = sign(al), x = abs(al) - fo;\n"
    "        float g = floor(x / 3.0), wk = k < 2 ? 1.0 : .4;\n"
    "        for (int i = -1; i <= 1; i++) {\n"
    "          float c = g + float(i), at = (c + hs(vec2(c, float(k) + side))) * 3.0 + fo;\n"
    "          float rd = .5 + .6 * hs(vec2(c, 9.0 + float(k))), dd = length(vec2(abs(al) - at, pe - (hs(vec2(c, 4.0 + side)) - .5) * 1.2));\n"
    "          s += wk * exp(-dd * dd / (rd * rd)) * pow(clamp(1.0 - at / frad, 0.0, 1.0), 2.0);\n"
    "        }\n"
    "      }\n"
    "      a = s + .6 * exp(-d * d / .004);\n"
    "    }\n"
    "    else if (kind == 5) a = exp(-d * d / .12) * (1.0 - smoothstep(.8, 1.0, d));\n"
    /* 焦外光斑: 实心圆盘, 边缘略亮 (镜头的散景) */
    "    else if (kind == 6) a = (1.0 - smoothstep(.86, 1.0, d)) * (.6 + .4 * smoothstep(.55, .92, d));\n"
    /* 恒星核心 (点光源): 很亮很小的芯 + 指数衰减的眩光 + 一圈极淡的散射环; 没有平的圆盘边 */
    "    else if (kind == 7) {\n"
    /* 点光源: 很小的芯 + 星尘眩光 */
    "      float w = .4 * exp(-d * 7.0) * (1.0 - smoothstep(.85, 1.0, d));\n"
    "      a = exp(-d * d / .01) + .3 * w + (frad > 6.0 ? dust(P, 4.0, min(1.0, w * 2.5)) * .8 : .7 * w);\n"
    "    }\n"
    "    else a = 1.0 - smoothstep(.4, 1.0, d);\n"
    "    a = clamp(a, 0.0, 1.0);\n"
    "    col = mix(vec3(1.0), fc0.rgb, smoothstep(0.0, kind == 7 ? .22 : .45, d) * fc0.a);\n"
    "  }\n"
    "  float al = a * fb.y;\n"
    "  if (al < .0005) discard;\n"
    "  o = vec4(col * al, al);\n"
    "}\n";

/* 卡片挖洞: 四个角 (屏幕坐标 + 深度), 只输出不透明度 */
static const char *galaxyglvscut =
    "#version 330 core\n"
    "layout(location=0) in vec3 p;\n"
    "uniform vec2 screen;\n"
    "void main() { gl_Position = vec4(p.x / screen.x * 2.0 - 1.0, 1.0 - p.y / screen.y * 2.0, p.z, 1.0); }\n";
static const char *galaxyglfscut =
    "#version 330 core\n"
    "uniform float alpha; out vec4 o;\n"
    "void main() { o = vec4(0.0, 0.0, 0.0, alpha); }\n";

/* 全屏三角形 (后期 / 合成共用) */
static const char *galaxyglvsfull =
    "#version 330 core\n"
    "out vec2 uv;\n"
    "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";

/* 双重 Kawase: 降采样取 5 点, 升采样取 8 点 */
static const char *galaxyglfsdown =
    "#version 330 core\n"
    "in vec2 uv; uniform sampler2D src; uniform vec2 texel; out vec4 o;\n"
    "void main() {\n"
    "  vec4 s = texture(src, uv) * 4.0;\n"
    "  s += texture(src, uv - texel); s += texture(src, uv + texel);\n"
    "  s += texture(src, uv + vec2(texel.x, -texel.y)); s += texture(src, uv - vec2(texel.x, -texel.y));\n"
    "  o = s / 8.0;\n"
    "}\n";
static const char *galaxyglfsup =
    "#version 330 core\n"
    "in vec2 uv; uniform sampler2D src; uniform vec2 texel; out vec4 o;\n"
    "void main() {\n"
    "  vec4 s = texture(src, uv + vec2(-texel.x * 2.0, 0.0));\n"
    "  s += texture(src, uv + vec2(-texel.x, texel.y)) * 2.0; s += texture(src, uv + vec2(0.0, texel.y * 2.0));\n"
    "  s += texture(src, uv + vec2(texel.x, texel.y)) * 2.0; s += texture(src, uv + vec2(texel.x * 2.0, 0.0));\n"
    "  s += texture(src, uv + vec2(texel.x, -texel.y)) * 2.0; s += texture(src, uv + vec2(0.0, -texel.y * 2.0));\n"
    "  s += texture(src, uv + vec2(-texel.x, -texel.y)) * 2.0;\n"
    "  o = s / 12.0;\n"
    "}\n";

/* 远景星空 (光层最先画的一层, 之后卡片挖洞会把它挡住): 两个远方星系、三颗偶尔闪烁的亮星、稀疏的闪烁星点和远处彗星;
 * 作为远景: 视差很小、动作很慢, 四边渐隐、画面中央压暗 (不抢焦点); 位置每次按 seed 随机. view: 视口 (左上原点像素), cam: 镜头偏航 / 俯仰 */
static const char *galaxyglfsnebula =
    "#version 330 core\n"
    "in vec2 uv; out vec4 o;\n"
    "uniform vec2 screen, cam, seed; uniform vec4 view; uniform float t, k, still;\n"
    "uniform vec4 fg0[2], fg1[2], fs[3], comet;\n"
    "float h(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }\n"
    "";
/* 着色器源码分两段 (C99 只保证 4095 字节以内的字符串字面量), 建程序时拼起来 */
static const char *galaxyglfsnebula2 =
    "void main() {\n"
    "  vec2 px = vec2(uv.x * screen.x, (1.0 - uv.y) * screen.y), q = (px - view.xy) / view.z;\n"
    "  if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > view.w / view.z) discard;\n"
    "  vec2 c = q - vec2(.5, .5 * view.w / view.z);\n"
    "  vec3 col = vec3(0.0), grey = vec3(.55, .6, .7);\n"
    "  float hh = view.w / view.z;\n"
    /* 远景: 四边 / 四角渐隐 (不堆在角上), 画面中央压暗 (星系和卡片才是焦点) */
    "  float ed = min(min(q.x, 1.0 - q.x), min(q.y, hh - q.y));\n"
    "  float edge = .4 + .6 * smoothstep(0.0, .18, ed), cd = 1.0 - .45 * exp(-dot(c, c) / .05);\n"
    /* 远景元素 (视差只有星云远层的一半): 远方星系 (旋涡: 核心 + 两条对数螺旋臂, 极慢自转; 椭圆: 柔和光斑) */
    "  vec2 pq = q - cam * .03; vec3 far = vec3(0.0);\n"
    "  for (int i = 0; i < 2; i++) {\n"
    "    vec2 dv = pq - fg0[i].xy; float sz = fg0[i].z;\n"
    "    if (dot(dv, dv) > 9.0 * sz * sz) continue;\n"
    "    float ca = cos(fg0[i].w), sa = sin(fg0[i].w);\n"
    "    vec2 lp = vec2(ca * dv.x + sa * dv.y, (-sa * dv.x + ca * dv.y) / fg1[i].x) / sz;\n"
    "    float rr = length(lp), th = atan(lp.y, lp.x), core = exp(-rr * rr * 18.0), g;\n"
    "    if (fg1[i].y < .5) {\n"
    "      float arm = .5 + .5 * cos(2.0 * th - 4.0 * log(rr + .05) + fg1[i].w + t * .02);\n"
    "      g = core * .9 + exp(-rr * 2.6) * (.15 + .55 * arm * arm) * (1.0 - smoothstep(.3, 1.4, rr));\n"
    "    } else g = exp(-rr * rr * 2.2) * .8 + core * .5;\n"
    "    far += mix(mix(vec3(1.0, .88, .7), vec3(.6, .72, 1.0), smoothstep(0.0, .8, rr)), grey, .3) * g * fg1[i].z;\n"
    "  }\n"
    /* 亮星: 平时一个小亮点, 偶尔短暂闪一下, 闪时有很小的十字星芒 */
    "  for (int i = 0; i < 3; i++) {\n"
    "    vec2 dv = (pq - fs[i].xy) * view.z; float d2 = dot(dv, dv);\n"
    "    if (d2 > 900.0) continue;\n"
    "    float fl = still > .5 ? 0.0 : pow(max(0.0, sin(6.2832 * t / fs[i].z + fs[i].w)), 40.0);\n"
    "    float sp = exp(-abs(dv.y) * 1.2) * exp(-abs(dv.x) / (10.0 + 8.0 * fl)) + exp(-abs(dv.x) * 1.2) * exp(-abs(dv.y) / (10.0 + 8.0 * fl));\n"
    "    far += vec3(.9, .94, 1.0) * (exp(-d2 / 2.2) * .25 * (1.0 + fl) + sp * .5 * fl);\n"
    "  }\n"
    "  col *= edge * cd;\n"
    /* 闪烁星点: 每 22px 一格, 约 2% 的格子里有一颗, 亮度按各自的相位慢慢起伏 */
    "  vec2 cell = floor((px + cam * 60.0) / 22.0), fp = fract((px + cam * 60.0) / 22.0);\n"
    "  float r = h(cell);\n"
    "  if (r < .02) {\n"
    "    vec2 sp = vec2(h(cell + 3.1), h(cell + 7.3)) * .7 + .15;\n"
    "    float tw = still > .5 ? .8 : .55 + .45 * sin(t * (1.1 + 2.5 * h(cell + 1.7)) + 6.28 * h(cell + 9.1));\n"
    "    col += mix(vec3(.75, .85, 1.0), vec3(1.0, .88, .7), h(cell + 5.5)) * exp(-dot(fp - sp, fp - sp) * 22.0 * 22.0 / 1.6) * tw * .045 * cd;\n"
    "  }\n"
    "  col *= k;\n"
    /* k: 深空的淡入淡出 (开场溶解进来, 揭开壁纸时一起淡出) */
    /* 远处彗星: 头部一个小光点带淡晕, 彗尾沿运动反方向延伸约 12% 视口宽, 逐渐张开、指数变淡 */
    "  if (comet.w > .001) {\n"
    "    vec2 dv = pq - comet.xy, dir = vec2(cos(comet.z), sin(comet.z));\n"
    "    float al = dot(dv, -dir), pr = dot(dv, vec2(-dir.y, dir.x)), sg = .002 + max(al, 0.0) * .08;\n"
    "    float tail = al > 0.0 ? exp(-al / .042) * exp(-pr * pr / (sg * sg)) * (1.0 - smoothstep(.1, .14, al)) : 0.0;\n"
    "    float d2 = dot(dv, dv) * view.z * view.z;\n"
    "    float head = exp(-d2 / 16.0) + .3 * exp(-d2 / 400.0);\n"
    "    far += mix(vec3(.8, .92, 1.0), grey, .2) * (head * .35 + tail * .2) * comet.w / max(cd, .01);\n"
    "  }\n"
    "  col += far * edge * cd * k;\n"
    "  o = vec4(col, 0.0);\n"
    "}\n";

/* GPU 粒子 (不存状态: 位置由编号 + 种子 + 时间解析算出, 数量可以很多). mode 0: 核心的吸积盘 (开普勒转速, 内热外冷,
 * 两条淡淡的旋臂密度波); mode 1: 星系群周围的星尘, 本身不发光, 只在靠近核心 / 中心光源时被照亮并染上它的颜色.
 * 投影与 galaxyproject 相同, 深度与 galaxygldepth 相同 (被卡片挡住) */
static const char *galaxyglvspart =
    "#version 330 core\n"
    "uniform mat3 view, plane; uniform vec3 campos, cpos, tint; uniform vec2 center, screen;\n"
    "uniform float focal, cnear, cfar, fnear, time, mode, seed, alpha, psize, extent; uniform vec4 disk;\n"
    "uniform vec4 lights[10]; uniform vec3 lcol[10]; uniform int nl; uniform vec4 trail[48]; uniform int ntrail; uniform float mclock, tlen;\n"
    "uniform float pull, burst, bext; uniform vec3 axis;\n"
    "out vec2 vl; out vec3 vc; out float va;\n"
    "float hs(float n) { return fract(sin(n * 12.9898 + seed * 78.233) * 43758.5453); }\n"
    "";
/* 天象粒子 (mode 4): 通用发射器, 不存状态. eshape 0 球壳爆发 (减速扩张, 纤维结构), 1 双向喷流 (沿 ±z 循环发射),
 * 2 粒子流 (epos 沿二次贝塞尔曲线流向 eb, 控制点 = 中点 + ebasis 第二列), 3 环形波 (ebasis 的 xy 平面上半径 ep.x 的一圈).
 * ep: 速度 (或半径 / 流速), 锥角 (或散开), 寿命 (秒), 发射持续时间 (或可见比例); et: 事件经过的秒数.
 * 涟漪场 rip (半径, 强度, 宽度) 和盘面法线 ripn: 经过处的吸积盘 / 拖尾 / 星尘被托起、点亮 */
static const char *galaxyglvspartfx =
    "uniform vec3 epos, eb, ec0, ec1, ripn; uniform mat3 ebasis; uniform vec4 ep, rip; uniform float et; uniform int eshape;\n"
    "void emitp(float fi, float h1, float h2, float h3, float h4, out vec3 wp, out vec3 col, out float a, out float sz) {\n"
    "  float h5 = hs(fi * 7.31 + .2), h6 = hs(fi * 8.97 + .4), u;\n"
    "  vec3 rd = vec3(h2, h3, h4) * 2.0 - 1.0; rd /= max(length(rd), .05);\n"
    "  a = 0.0; wp = epos; col = ec0; sz = psize * (.5 + .8 * h5);\n"
    "  if (eshape == 0) {\n"
    "    float age = et - h1 * ep.w; u = age / ep.z;\n"
    "    if (age < 0.0 || u > 1.0) return;\n"
    "    float fil = .72 + .28 * sin(9.0 * rd.x + 3.1) * sin(7.0 * rd.y + 1.7) * sin(8.0 * rd.z + .4);\n"
    "    wp = epos + rd * ep.x * ep.z * (1.0 - pow(1.0 - u, 2.5)) * fil * (.82 + .18 * h6);\n"
    "    col = u < .3 ? mix(vec3(1.0, .97, .92), ec0, u / .3) : mix(ec0, ec1, (u - .3) / .7);\n"
    "    a = alpha * pow(1.0 - u, 1.3) * (.3 + .7 * pow(h5, 3.0));\n"
    "  } else if (eshape == 1) {\n"
    "    float age = fract(h1 + et / ep.z) * ep.z, side = h2 < .5 ? -1.0 : 1.0; u = age / ep.z;\n"
    "    vec3 d = normalize(vec3((h3 - .5) * 2.0 * ep.y, (h4 - .5) * 2.0 * ep.y, 1.0)) * side;\n"
    "    wp = epos + ebasis * (d * ep.x * age * (.8 + .4 * h5));\n"
    "    col = mix(ec0, ec1, u);\n"
    "    a = alpha * pow(1.0 - u, 1.5) * (.35 + .65 * pow(h6, 3.0));\n"
    "  } else if (eshape == 2) {\n"
    "    float s = fract(h1 + et * ep.x);\n"
    "    if (s > ep.w) return;\n"
    "    vec3 c1 = (epos + eb) * .5 + ebasis[1];\n"
    /* ep.y > 0: 中段最散 (两端收拢, 用于光桥 / 连线); ep.y < 0: 越往后越散越淡 (彗尾) */
    "    float tail = ep.y < 0.0 ? 1.0 : 0.0;\n"
    "    wp = mix(mix(epos, c1, s), mix(c1, eb, s), s) + rd * abs(ep.y) * mix(sin(3.1416 * s), pow(s, .8), tail) * sqrt(h6);\n"
    "    col = mix(ec0, ec1, s);\n"
    "    a = alpha * (.3 + .7 * pow(h5, 3.0)) * mix(smoothstep(0.0, .06, s), pow(1.0 - s, 1.4), tail) * (1.0 - smoothstep(ep.w - .08, ep.w, s));\n"
    "  } else {\n"
    "    float th = 6.2832 * h1 + ep.w * et, r = ep.x * (1.0 + (h2 - .5) * ep.y);\n"
    "    wp = epos + ebasis * vec3(cos(th) * r, sin(th) * r, (h3 - .5) * ep.y * ep.x * .3);\n"
    "    col = mix(ec0, ec1, h4);\n"
    "    a = alpha * (.3 + .7 * pow(h5, 3.0));\n"
    "  }\n"
    "}\n";
static const char *galaxyglvspart2 =
    "void main() {\n"
    "  float fi = float(gl_InstanceID), a, sz;\n"
    "  vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0 - 1.0;\n"
    "  float h1 = hs(fi * 1.37 + .1), h2 = hs(fi * 2.71 + .3), h3 = hs(fi * 3.13 + .7), h4 = hs(fi * 5.17 + .9);\n"
    "  vec3 wp, col;\n"
    "  if (mode > 3.5) {\n"
    "    emitp(fi, h1, h2, h3, h4, wp, col, a, sz);\n"
    "  } else if (mode > 2.5) {\n"
    /* 尾迹: 星体过去 disk.x (motion) 内走过的路径 (trail[0] 是现在的位置). 每颗粒子在星体经过的那一刻生成,
     * 之后原地 (世界坐标固定) 慢慢散开、变暗, 直到下一轮; 越新越亮、越靠近路径中心 */
    /* trail[0] 是现在的位置, trail[1..] 是固定时间网格上的过去位置 (网格不随时间滑动, 插值误差不会逐帧变化):
     * disk.z = 现在距最近网格点的时间, disk.w = 网格间隔 */
    /* trail[k].w: 该点沿路径到星体的距离. 可见长度按路径长度 tlen 截断 (开场星体跑得快时尾迹也紧跟在身后),
     * 离星体越远散得越开 (彗尾形状), 越暗 */
    "    float age = fract(h1 + mclock / disk.x), back = age * disk.x - disk.z; vec4 pt;\n"
    "    if (back <= 0.0) pt = mix(trail[0], trail[1], age * disk.x / max(disk.z, 1e-6));\n"
    "    else {\n"
    "      float fk = min(1.0 + back / disk.w, float(ntrail - 1)); int k0 = int(floor(fk));\n"
    "      pt = mix(trail[k0], trail[min(k0 + 1, ntrail - 1)], fk - float(k0));\n"
    "    }\n"
    "    float u = clamp(pt.w / max(tlen, 1e-3), 0.0, 1.0);\n"
    "    vec3 rd = vec3(h2, h3, h4) * 2.0 - 1.0; rd /= max(length(rd), .05);\n"
    "    float h5 = hs(fi * 7.31 + .2), h6 = hs(fi * 8.97 + .4);\n"
    "    wp = pt.xyz + rd * disk.y * sqrt(h6) * (.05 + pow(u, 1.1));\n"
    "    col = mix(vec3(1.0, .96, .9), tint, smoothstep(0.0, .3, u));\n"
    "    a = alpha * (1.0 - smoothstep(.55, 1.0, u)) * (1.0 - .5 * u) * pow(1.0 - age, .7) * (.25 + .75 * pow(h5, 4.0));\n"
    "    sz = psize * (.45 + .6 * h5);\n"
    "  } else if (mode > 1.5) {\n"
    /* 粒子恒星: 球形分布, 中心密外围疏, 略扁; 绕自转轴内快外慢地转, 每颗粒子还有一点缓慢的涌动; 中心暖白, 外圈 tag 色 */
    "    float u = pow(h1, .9), rr = disk.x * u;\n"
    "    float z = h2 * 2.0 - 1.0, ph = 6.2832 * h3, s = sqrt(1.0 - z * z);\n"
    "    ph += disk.z * time / (.35 + u);\n"
    "    vec3 lp = vec3(s * cos(ph), s * sin(ph), z * disk.w) * rr;\n"
    "    lp += .06 * disk.x * vec3(sin(time * .7 + fi), sin(time * .6 + fi * 1.7), sin(time * .8 + fi * .9));\n"
    "    wp = cpos + plane * lp;\n"
    "    col = mix(vec3(1.0, .93, .82), tint, smoothstep(.1, .7, u));\n"
    "    a = alpha * (.25 + .75 * pow(h4, 3.0)) * (1.0 - .7 * u);\n"
    "    sz = psize * (.5 + .9 * h4);\n"
    "  } else if (mode < .5) {\n"
    "    float u = pow(h1, 1.6), rr = mix(disk.x, disk.y, u);\n"
    "    float th = 6.2832 * h2 + disk.z * pow(disk.x / rr, 1.5) * time;\n"
    "    float arm = .55 + .45 * cos(2.0 * th - 5.0 * log(rr / disk.x));\n"
    "    wp = cpos + plane * vec3(cos(th) * rr, sin(th) * rr, (h3 - .5) * disk.w * rr);\n"
    "    col = mix(vec3(1.0, .94, .86), tint, smoothstep(0.0, .4, u));\n"
    "    a = alpha * (.12 + .88 * pow(h4, 5.0)) * arm * (1.0 - .75 * u);\n"
    "    sz = psize * (.6 + .9 * pow(h4, 3.0));\n"
    "  } else {\n"
    "    vec3 p0 = vec3(h1, h2, h3) * 2.0 - 1.0;\n"
    "    p0 += .03 * vec3(sin(time * .05 + fi), sin(time * .04 + fi * 1.3), sin(time * .045 + fi * .7));\n"
    "    wp = plane * (p0 * extent * vec3(1.0, .45, 1.0));\n"
    "    col = vec3(0.0);\n"
    "    for (int k = 0; k < 10; k++) {\n"
    "      if (k >= nl) break;\n"
    "      vec3 d = wp - lights[k].xyz;\n"
    "      col += lcol[k] * exp(-dot(d, d) / (lights[k].w * lights[k].w));\n"
    "    }\n"
    "    a = alpha * (.4 + .6 * h4);\n"
    "    sz = psize * (.5 + h4);\n"
    "  }\n"
    /* 涟漪场: 吸积盘 / 星尘 / 拖尾的粒子在波经过时沿盘面法线被托起一点, 并短暂变亮 */
    "  if (rip.y > .001 && (mode < 1.5 || (mode > 2.5 && mode < 3.5))) {\n"
    "    float dw = (length(wp) - rip.x) / rip.z, w = rip.y * exp(-dw * dw);\n"
    "    wp += ripn * w * rip.z * .35; a *= 1.0 + 2.5 * w; col = mix(col, vec3(1.0), .3 * min(w, 1.0));\n"
    "  }\n"
    /* Esc 收尾: 先绕星系群的上轴螺旋着吸进中心 (各自有延迟, 越近越亮越白), 再随冲击波从中心炸开、淡出 */
    "  float hd = hs(fi * 9.71 + .5), he = hs(fi * 6.13 + .8);\n"
    "  if (pull > .001) {\n"
    "    float d = clamp(pull * 1.4 - .4 * hd, 0.0, 1.0); d = d * d * (3.0 - 2.0 * d);\n"
    "    float an = 3.5 * d * (1.0 + .5 * he), ca = cos(an), sa = sin(an);\n"
    "    wp = (wp * ca + cross(axis, wp) * sa + axis * dot(axis, wp) * (1.0 - ca)) * pow(1.0 - d, 1.6);\n"
    "    a *= 1.0 + 1.5 * d; col = mix(col, vec3(1.0, .95, .88), .5 * d);\n"
    "  }\n"
    "  if (burst > .001) {\n"
    "    vec3 dir = vec3(hd, he, h4) * 2.0 - 1.0; dir /= max(length(dir), .05);\n"
    "    wp = dir * bext * (.25 + .75 * h1) * pow(burst, .55);\n"
    "    a *= pow(1.0 - burst, 1.6);\n"
    "  }\n"
    "  vec3 cv = view * (wp - campos);\n"
    "  if (cv.z < cnear || cv.z > cfar || a < .002) { gl_Position = vec4(3.0, 3.0, 3.0, 1.0); vl = c; vc = col; va = 0.0; return; }\n"
    "  float sc = focal / cv.z;\n"
    "  sz = clamp(sz * sqrt(sc), .6, 3.5);\n"
    "  vec2 p = center + cv.xy * sc + c * sz * 2.0;\n"
    "  gl_Position = vec4(p.x / screen.x * 2.0 - 1.0, 1.0 - p.y / screen.y * 2.0, clamp((cv.z - cnear) / (cfar - cnear), 0.0, 1.0) * 2.0 - 1.0, 1.0);\n"
    "  vl = c; vc = col; va = a * smoothstep(fnear, fnear * 1.8, cv.z);\n"
    "}\n";
static const char *galaxyglfspart =
    "#version 330 core\n"
    "in vec2 vl; in vec3 vc; in float va; out vec4 o;\n"
    "void main() { float a = exp(-dot(vl, vl) * 3.0) * va; if (a < .001) discard; o = vec4(vc * a, a); }\n";

/* 截图 pixmap -> 卡片纹理 (逐像素拷贝, 统一成第 0 行在上) */
static const char *galaxyglfscopy =
    "#version 330 core\n"
    "in vec2 uv; uniform sampler2D src; uniform float flip; out vec4 o;\n"
    "void main() { o = texture(src, vec2(uv.x, flip > .5 ? 1.0 - uv.y : uv.y)); }\n";

/* 窗口卡片: 透视校正的四边形 (顶点给裁剪坐标, w 为镜头深度). 纹理是预乘的截图;
 * 不透明度 vis, 发白 tint / 压暗 dark (与原来 XRender 上叠白 / 叠黑的结果一致), 边缘光 rim, 转动时扫过的高光 spec */
static const char *galaxyglvscard =
    "#version 330 core\n"
    "layout(location=0) in vec4 p; layout(location=1) in vec2 t; out vec2 uv;\n"
    "void main() { uv = t; gl_Position = p; }\n";
static const char *galaxyglfscard =
    "#version 330 core\n"
    "in vec2 uv; out vec4 o;\n"
    "uniform sampler2D tex; uniform float vis, tint, dark, rim, spec, specat, bias, aspect, exact, weight; uniform vec3 rimc, ripple, envc;\n"
    "void main() {\n"
    "  vec4 c = exact > .5 ? textureLod(tex, uv, 0.0) : texture(tex, uv, bias);\n"
    "  float m = c.a;\n"
    "  c *= vis;\n"
    "  if (tint > .001) c = vec4(tint * m) + c * (1.0 - tint * m);\n"
    "  if (dark > .001) c = vec4(0.0, 0.0, 0.0, dark * m) + c * (1.0 - dark * m);\n"
    "  if (rim > .001) {\n"
    "    float e = min(min(uv.x, 1.0 - uv.x) * aspect, min(uv.y, 1.0 - uv.y));\n"
    "    c.rgb += rimc * rim * m * (1.0 - smoothstep(0.0, .035, e));\n"
    "  }\n"
    "  if (spec > .001) c.rgb += vec3(spec * m * exp(-pow((uv.x * .75 + uv.y * .25 - specat) / .09, 2.0)));\n"
    /* 环境光: 卡面映出星云 / 所属核心的颜色 */
    "  c.rgb += envc * m * vis;\n"
    /* 点击涟漪: 从点击处扩散的一圈亮环 (ripple.z 是进度 0..1) */
    "  if (ripple.z > 0.0 && ripple.z < 1.0) {\n"
    "    float d = length((uv - ripple.xy) * vec2(aspect, 1.0)), r = ripple.z * 1.4 * max(aspect, 1.0);\n"
    "    c.rgb += vec3(.85, .95, 1.0) * m * (1.0 - ripple.z) * .7 * exp(-pow((d - r) / .03, 2.0));\n"
    "  }\n"
    "  o = c * weight;\n"
    "}\n";

/* 合成: 底层 滤色 光层 (软拐点压缩, 轻微色差和颗粒只作用在光上), 再盖前景层 */
static const char *galaxyglfscomp =
    "#version 330 core\n"
    "in vec2 uv; out vec4 o;\n"
    "uniform sampler2D tbase, tlight, tbloom, tfront, twall, tcards; uniform float cards;\n"
    "uniform float ybase, yfront, bloomk, aberr, grain, seed, glow; uniform vec2 screen;\n"
    "uniform vec4 lens, shock; uniform float expo; uniform vec3 rays; uniform sampler2D tbloom2;\n"
    "vec3 tolin(vec3 c) { return mix(c / 12.92, pow((c + .055) / 1.055, vec3(2.4)), step(.04045, c)); }\n"
    "vec3 tosrgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - .055, step(.0031308, c)); }\n"
    /* 保色相的软肩: 按最亮通道压缩, 各通道等比缩放 (不会像逐通道截断那样先变白); 极亮处才逐渐偏白 */
    "vec3 tone(vec3 L) {\n"
    "  float m = max(max(L.r, L.g), L.b), a = .55, t;\n"
    "  if (m < 1e-6) return vec3(0.0);\n"
    "  t = m < a ? m : a + (1.0 - a) * (1.0 - exp(-(m - a) / (1.0 - a)));\n"
    "  return mix(L * (t / m), vec3(t), smoothstep(1.2, 8.0, m) * .6);\n"
    "}\n"
    "float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233)) + seed) * 43758.5453); }\n"
    "void main() {\n"
    "  vec2 ub = vec2(uv.x, ybase > .5 ? uv.y : 1.0 - uv.y), uf = vec2(uv.x, yfront > .5 ? uv.y : 1.0 - uv.y);\n"
    "  vec2 px = vec2(uv.x * screen.x, (1.0 - uv.y) * screen.y), d = px - lens.xy;\n"
    "  float rd = length(d);\n"
    /* 引力透镜: 越靠近核心, 采样点越往外 (背景被拽向中心), 再带一点旋涡; 到半径 lens.w 处衰减为 0, 采样不会出屏 */
    "  if (lens.z > .001) {\n"
    "    float f = lens.z * pow(max(1.0 - rd / lens.w, 0.0), 2.0), a = 1.2 * f;\n"
    "    vec2 sp = lens.xy + mat2(cos(a), sin(a), -sin(a), cos(a)) * d * (1.0 + f);\n"
    "    ub = vec2(sp.x / screen.x, ybase > .5 ? 1.0 - sp.y / screen.y : sp.y / screen.y);\n"
    "  }\n"
    "  vec3 base = texture(tbase, ub).rgb;\n"
    "  if (cards > .5) { vec4 cd = texture(tcards, uv); base = base * (1.0 - cd.a) + cd.rgb; }\n"
    "  base = tolin(base);\n"
    /* 冲击波揭开壁纸: 圆内是壁纸, 边界三个通道错开一点 (色差) */
    "  if (shock.w > .5) {\n"
    "    vec2 uw = vec2(uv.x, ybase > .5 ? uv.y : 1.0 - uv.y);\n"
    "    vec3 wall = tolin(texture(twall, uw).rgb), in3;\n"
    "    in3 = 1.0 - smoothstep(shock.x - shock.y * vec3(1.15, 1.0, .85), shock.x + shock.y * vec3(.25, .15, .05), vec3(rd));\n"
    "    base = mix(base, wall, in3);\n"
    "  }\n"
    "  vec2 off = (uv - .5) * aberr / screen;\n"
    "  vec3 L = vec3(texture(tlight, uv + off).r, texture(tlight, uv).g, texture(tlight, uv - off).b)\n"
    "         + bloomk * texture(tbloom, uv).rgb;\n"
    /* 光按显示空间的强度画出 (调好的观感不变), 换到线性空间后再压缩、滤色; 光为 0 时输出与底层完全相同 */
    /* 冲击波的光环 (外沿偏暖) 和收尾的余晖 */
    "  if (shock.z > .001) L += vec3(1.0, .9, .78) * shock.z * exp(-pow((rd - shock.x) / (.6 * shock.y), 2.0));\n"
    "  if (glow > .001) L += vec3(1.0, .86, .7) * glow * exp(-rd * rd / (.025 * screen.x * screen.x));\n"
    /* 体积光束: 从光源出发沿径向采样泛光, 逐次衰减 (星云 / 光尘被照出放射状的光柱) */
    "  if (rays.z > .001) {\n"
    "    vec2 c = vec2(rays.x / screen.x, 1.0 - rays.y / screen.y), dl = (uv - c) * (.9 / 24.0), sp = uv;\n"
    "    vec3 acc = vec3(0.0); float dec = 1.0;\n"
    "    for (int i = 0; i < 24; i++) { sp -= dl; acc += max(texture(tbloom2, sp).rgb - .08, 0.0) * dec; dec *= .93; }\n"
    "    L += rays.z * acc * (2.0 / 24.0);\n"
    "  }\n"
    "  L *= expo;\n"
    "  L = tone(pow(max(L * 1.15, 0.0), vec3(2.2)));\n"
    "  vec3 c = tosrgb(1.0 - (1.0 - base) * (1.0 - L));\n"
    "  vec4 f = texture(tfront, uf);\n"
    "  c = c * (1.0 - f.a) + f.rgb;\n"
    /* 三角分布抖动 (约 1 个色阶, 消掉星云 / 光晕渐变的色带), 只在有光的地方: 开场刚开始 (还没有光) 与桌面逐像素一致 */
    "  c += (hash(uv * screen) + hash(uv * screen + 17.0) - 1.0) * (grain / .06) * clamp(max(max(L.r, L.g), L.b) * 40.0, 0.0, 1.0) / 255.0;\n"
    "  o = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
    "}\n";

/* 有些 GLX 驱动报告的 Y_INVERTED 与绑定后的 ARGB pixmap 方向不一致。
 * 用前景层的上下色块在初始化时校准，搜索栏和文字不会整层倒置。 */
static const char *galaxyglfsprobe =
    "#version 330 core\n"
    "in vec2 uv; out vec4 o; uniform sampler2D src;\n"
    "void main() { o = texture(src, uv); }\n";

static GLuint
galaxyglshader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    char log[1024];

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        if (galaxyscene.log)
            fprintf(galaxyscene.log, "galaxy gl: shader error: %s\n", log), fflush(galaxyscene.log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint
galaxyglprogram(const char *vs, const char *fs)
{
    GLuint v = galaxyglshader(GL_VERTEX_SHADER, vs), f = galaxyglshader(GL_FRAGMENT_SHADER, fs), p;
    GLint ok = 0;

    if (!v || !f)
        return 0;
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* ---------- 初始化 ---------- */

/* 选窗口用的 FBConfig (尽量用默认 visual, 遮罩背景能直接用桌面截图) 和两种 pixmap 纹理的 FBConfig */
static int
galaxyglconfigs(void)
{
    static const int winattr[] = {
        GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER, True, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None };
    int pixattr[] = {
        GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT, GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
        GLX_DOUBLEBUFFER, False, GLX_X_RENDERABLE, True, 0, True, None };
    VisualID def = XVisualIDFromVisual(DefaultVisual(dpy, screen));
    GLXFBConfig *c;
    XVisualInfo *v;
    int n, i, k, depth, inv;

    if (!(c = glXChooseFBConfig(dpy, screen, winattr, &n)) || n < 1)
        return 0;
    galaxygl.fbwin = c[0];
    for (i = 0; i < n; i++)
        if ((v = glXGetVisualFromFBConfig(dpy, c[i]))) {
            k = v->visualid == def;
            XFree(v);
            if (k) {
                galaxygl.fbwin = c[i];
                break;
            }
        }
    XFree(c);
    galaxygl.vi = glXGetVisualFromFBConfig(dpy, galaxygl.fbwin);
    for (k = 0; k < 2; k++) {
        pixattr[8] = k ? GLX_BIND_TO_TEXTURE_RGBA_EXT : GLX_BIND_TO_TEXTURE_RGB_EXT;
        if (!(c = glXChooseFBConfig(dpy, screen, pixattr, &n)))
            return 0;
        for (i = 0; i < n; i++) {
            if (!(v = glXGetVisualFromFBConfig(dpy, c[i])))
                continue;
            depth = v->depth;
            XFree(v);
            if (depth != (k ? 32 : DefaultDepth(dpy, screen)))
                continue;
            inv = 0;
            glXGetFBConfigAttrib(dpy, c[i], GLX_Y_INVERTED_EXT, &inv);
            if (k) {
                galaxygl.fb32 = c[i];
                galaxygl.yinv32 = galaxygl.yinv32config = inv;
            } else {
                galaxygl.fb24 = c[i];
                galaxygl.yinv24 = inv;
            }
            break;
        }
        XFree(c);
        if (i == n)
            return 0;
    }
    return 1;
}

/* 进程内第一次进入星系时调用: 建上下文、程序、缓冲. 失败一次以后不再重试 */
static int
galaxyglinit(void)
{
    static const int ctxattr[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None };

    if (galaxygl.ready || galaxygl.failed)
        return galaxygl.ready;
    galaxygl.failed = 1;
    if (!epoxy_has_glx_extension(dpy, screen, "GLX_EXT_texture_from_pixmap")
            || !epoxy_has_glx_extension(dpy, screen, "GLX_ARB_create_context_profile")
            || !galaxyglconfigs() || !galaxygl.vi)
        return 0;
    if (!(galaxygl.ctx = glXCreateContextAttribsARB(dpy, galaxygl.fbwin, NULL, True, ctxattr)))
        return 0;
    galaxygl.cmap = XCreateColormap(dpy, root, galaxygl.vi->visual, AllocNone);
    galaxygl.inst = calloc(GALAXYGLINST, sizeof *galaxygl.inst);
    galaxygl.part = calloc(GALAXYGLPART, sizeof *galaxygl.part);
    if (!galaxygl.inst || !galaxygl.part)
        return 0;
    galaxygl.failed = 0;
    galaxygl.ready = 1;
    return 1;
}

/* 程序和缓冲在上下文第一次可用后再建 (需要 current) */
/* 片元着色器源码分两段 (C99 只保证 4095 字节以内的字符串字面量), 拼起来再建程序 */
static GLuint
galaxyglprogram2(const char *vs, const char *fa, const char *fb)
{
    size_t na = strlen(fa), nb = strlen(fb);
    char *src = malloc(na + nb + 1);
    GLuint p;

    if (!src)
        return 0;
    memcpy(src, fa, na);
    memcpy(src + na, fb, nb + 1);
    p = galaxyglprogram(vs, src);
    free(src);
    return p;
}

static int
galaxyglobjects(void)
{
    GLuint *progs[] = { &galaxygl.proglight, &galaxygl.progcut, &galaxygl.progdown, &galaxygl.progup,
        &galaxygl.progcomp, &galaxygl.progprobe, &galaxygl.prognebula, &galaxygl.progcopy, &galaxygl.progcard, &galaxygl.progpart };
    int i;

    if (galaxygl.proglight)
        return 1;
    galaxygl.proglight = galaxyglprogram2(galaxyglvslight, galaxyglfslight, galaxyglfslight2);
    galaxygl.progcut = galaxyglprogram(galaxyglvscut, galaxyglfscut);
    galaxygl.progdown = galaxyglprogram(galaxyglvsfull, galaxyglfsdown);
    galaxygl.progup = galaxyglprogram(galaxyglvsfull, galaxyglfsup);
    galaxygl.progcomp = galaxyglprogram(galaxyglvsfull, galaxyglfscomp);
    galaxygl.progprobe = galaxyglprogram(galaxyglvsfull, galaxyglfsprobe);
    galaxygl.prognebula = galaxyglprogram2(galaxyglvsfull, galaxyglfsnebula, galaxyglfsnebula2);
    galaxygl.progcopy = galaxyglprogram(galaxyglvsfull, galaxyglfscopy);
    galaxygl.progcard = galaxyglprogram(galaxyglvscard, galaxyglfscard);
    {   /* 顶点着色器源码也分两段 */
        size_t na = strlen(galaxyglvspart), nf = strlen(galaxyglvspartfx), nb = strlen(galaxyglvspart2);
        char *vs = malloc(na + nf + nb + 1);

        if (vs) {
            memcpy(vs, galaxyglvspart, na);
            memcpy(vs + na, galaxyglvspartfx, nf);
            memcpy(vs + na + nf, galaxyglvspart2, nb + 1);
            galaxygl.progpart = galaxyglprogram(vs, galaxyglfspart);
            free(vs);
        }
    }
    for (i = 0; i < (int)LENGTH(progs); i++)
        if (!*progs[i])
            return 0;
    glGenVertexArrays(1, &galaxygl.vao);
    glGenBuffers(1, &galaxygl.vbo);
    glBindVertexArray(galaxygl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, galaxygl.vbo);
    for (i = 0; i < 4; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(GalaxyGLInst), (void *)(sizeof(float) * 4 * i));
        glVertexAttribDivisor(i, 1);
    }
    glGenVertexArrays(1, &galaxygl.fullvao);   /* 全屏三角形不读顶点属性 */
    glGenTextures(1, &galaxygl.texback);
    glGenTextures(1, &galaxygl.texfront);
    glGenTextures(1, &galaxygl.texwall);
    /* 卡片: 每个顶点 (裁剪坐标 xyzw, 纹理坐标 uv) */
    glGenVertexArrays(1, &galaxygl.cardvao);
    glGenBuffers(1, &galaxygl.cardvbo);
    glBindVertexArray(galaxygl.cardvao);
    glBindBuffer(GL_ARRAY_BUFFER, galaxygl.cardvbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(4 * sizeof(float)));
    glGenFramebuffers(1, &galaxygl.copyfbo);
    if (epoxy_has_gl_extension("GL_EXT_texture_filter_anisotropic") || epoxy_has_gl_extension("GL_ARB_texture_filter_anisotropic"))
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &galaxygl.aniso);
    return 1;
}

static GLuint
galaxygltex(int w, int h, GLenum fmt)
{
    GLuint t;

    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt == GL_DEPTH_COMPONENT24 ? GL_DEPTH_COMPONENT : GL_RGBA,
            fmt == GL_DEPTH_COMPONENT24 ? GL_UNSIGNED_INT : GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* 光层和泛光链按屏幕大小建, 大小变了才重建 */
static void
galaxyglbuffers(int w, int h)
{
    int i;

    if (galaxygl.fw == w && galaxygl.fh == h)
        return;
    if (galaxygl.fbo) {
        glDeleteFramebuffers(1, &galaxygl.fbo);
        glDeleteTextures(1, &galaxygl.lighttex);
        glDeleteTextures(1, &galaxygl.depthtex);
        glDeleteFramebuffers(GALAXYBLOOM, galaxygl.bloomfbo);
        glDeleteTextures(GALAXYBLOOM, galaxygl.bloomtex);
        glDeleteFramebuffers(1, &galaxygl.cardfbo);
        glDeleteTextures(1, &galaxygl.cardtex);
        glDeleteFramebuffers(1, &galaxygl.blurfbo);
        glDeleteTextures(1, &galaxygl.blurtex);
        if (galaxygl.pbo[0])
            glDeleteBuffers(3, galaxygl.pbo);
        memset(galaxygl.pbo, 0, sizeof galaxygl.pbo);
        for (i = 0; i < 3; i++)
            if (galaxygl.pbofence[i])
                glDeleteSync(galaxygl.pbofence[i]);
        memset(galaxygl.pbofence, 0, sizeof galaxygl.pbofence);
    }
    galaxygl.fw = w;
    galaxygl.fh = h;
    galaxygl.lighttex = galaxygltex(w, h, GL_RGBA16F);
    galaxygl.depthtex = galaxygltex(w, h, GL_DEPTH_COMPONENT24);
    glGenFramebuffers(1, &galaxygl.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, galaxygl.lighttex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, galaxygl.depthtex, 0);
    /* 卡片层: 预乘的 RGBA8, 合成时盖在底层之上、光层之下 */
    glGenTextures(1, &galaxygl.cardtex);
    glBindTexture(GL_TEXTURE_2D, galaxygl.cardtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &galaxygl.cardfbo);
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.cardfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, galaxygl.cardtex, 0);
    /* 运动模糊的累加层 (半精度, 多份 1/n 相加不出色带) */
    galaxygl.blurtex = galaxygltex(w, h, GL_RGBA16F);
    glGenFramebuffers(1, &galaxygl.blurfbo);
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.blurfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, galaxygl.blurtex, 0);
    glGenFramebuffers(GALAXYBLOOM, galaxygl.bloomfbo);
    for (i = 0; i < GALAXYBLOOM; i++) {
        galaxygl.bw[i] = MAX(1, w >> (i + 1));
        galaxygl.bh[i] = MAX(1, h >> (i + 1));
        galaxygl.bloomtex[i] = galaxygltex(galaxygl.bw[i], galaxygl.bh[i], GL_RGBA16F);
        glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.bloomfbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, galaxygl.bloomtex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static GLXPixmap
galaxyglpixmap(Pixmap pix, int rgba)
{
    const int attr[] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
        GLX_TEXTURE_FORMAT_EXT, rgba ? GLX_TEXTURE_FORMAT_RGBA_EXT : GLX_TEXTURE_FORMAT_RGB_EXT, None };

    return glXCreatePixmap(dpy, rgba ? galaxygl.fb32 : galaxygl.fb24, pix, attr);
}

/* 色块在 GL 里的上下: 1 直接采样 (红在上), 0 需要翻转, -1 看不出来 (沿用 FBConfig). */
static int
galaxyglprobedir(Picture pic, GLXPixmap gp, GLuint tex, int w, int h)
{
    XRenderColor red = {0xffff, 0, 0, 0xffff}, blue = {0, 0, 0xffff, 0xffff};
    unsigned char bottom[4], top[4];
    int dir = -1;

    if (!pic || !gp || w < 16 || h < 16)
        return -1;
    XRenderFillRectangle(dpy, PictOpSrc, pic, &red, 0, 0, 8, 8);
    XRenderFillRectangle(dpy, PictOpSrc, pic, &blue, 0, h - 8, 8, 8);
    glXWaitX();
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(galaxygl.progprobe);
    glBindVertexArray(galaxygl.fullvao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glXBindTexImageEXT(dpy, gp, GLX_FRONT_LEFT_EXT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(glGetUniformLocation(galaxygl.progprobe, "src"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bottom);
    glReadPixels(4, h - 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top);
    glXReleaseTexImageEXT(dpy, gp, GLX_FRONT_LEFT_EXT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (top[0] > 192 && top[2] < 64 && bottom[2] > 192 && bottom[0] < 64)
        dir = 1;
    else if (bottom[0] > 192 && bottom[2] < 64 && top[2] > 192 && top[0] < 64)
        dir = 0;
    return dir;
}

/* 驱动报告的 Y_INVERTED 和绑上 pixmap 之后的实际方向可能不一致.
 * 底层是桌面和窗口卡片, 前景是文字; 两层都用上下色块校准, 否则卡片会相对轨道整屏倒置. */
static void
galaxyglprobeorient(int w, int h)
{
    GalaxyScene *r = &galaxyscene;
    XRenderColor clear = {0, 0, 0, 0};
    int dir;

    dir = galaxyglprobedir(r->front, galaxygl.gfront, galaxygl.texfront, w, h);
    if (dir >= 0)
        galaxygl.yinv32 = dir;
    if (r->front)
        XRenderFillRectangle(dpy, PictOpSrc, r->front, &clear, 0, 0, w, h);
    dir = galaxyglprobedir(r->back, galaxygl.gback, galaxygl.texback, w, h);
    if (dir >= 0)
        galaxygl.yinv24 = dir;
}

/* 每次进入星系: 遮罩窗口建好之后绑定上下文和底层 / 前景层 pixmap. 失败返回 0 */
static int
galaxyglbegin(Window win, Pixmap back, Pixmap front, Pixmap wall, int w, int h)
{
    if (!galaxyglinit() || !glXMakeCurrent(dpy, win, galaxygl.ctx) || !galaxyglobjects())
        return 0;
    if (epoxy_has_glx_extension(dpy, screen, "GLX_EXT_swap_control"))
        glXSwapIntervalEXT(dpy, win, 0);   /* 节奏由星系自己控制, 交换缓冲不等垂直同步 (由合成器同步) */
    galaxyglbuffers(w, h);
    galaxygl.win = win;
    galaxygl.gback = galaxyglpixmap(back, 0);
    galaxygl.gfront = galaxyglpixmap(front, 1);
    galaxygl.gwall = wall ? galaxyglpixmap(wall, 0) : 0;   /* 收尾时冲击波揭开的壁纸 (没有也能运行, 只是没有揭开效果) */
    galaxygl.ninst = galaxygl.npart = 0;
    if (galaxygl.gback && galaxygl.gfront)
        galaxyglprobeorient(w, h);
    return galaxygl.gback && galaxygl.gfront;
}

/* 退出: 先放开纹理、结束绘制、卸下上下文, 再销毁 GLX pixmap.
 * 上一帧通常已经 Release, 再放一次可能 BadMatch, 这段忽略 */
static void
galaxyglend(void)
{
    XErrorHandler old;

    if (!galaxygl.ready || !galaxygl.win)
        return;
    if (glXMakeCurrent(dpy, galaxygl.win, galaxygl.ctx)) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        old = XSetErrorHandler(xerrordummy);
        if (galaxygl.gfront)
            glXReleaseTexImageEXT(dpy, galaxygl.gfront, GLX_FRONT_LEFT_EXT);
        if (galaxygl.gback)
            glXReleaseTexImageEXT(dpy, galaxygl.gback, GLX_FRONT_LEFT_EXT);
        if (galaxygl.gwall)
            glXReleaseTexImageEXT(dpy, galaxygl.gwall, GLX_FRONT_LEFT_EXT);
        glFinish();
        glXMakeCurrent(dpy, None, NULL);
        XSync(dpy, False);
        XSetErrorHandler(old);
    }
    if (galaxygl.gback)
        glXDestroyPixmap(dpy, galaxygl.gback);
    if (galaxygl.gfront)
        glXDestroyPixmap(dpy, galaxygl.gfront);
    if (galaxygl.gwall)
        glXDestroyPixmap(dpy, galaxygl.gwall);
    galaxygl.gback = galaxygl.gfront = galaxygl.gwall = 0;
    galaxygl.win = None;
}

/* ---------- 每帧 ---------- */

/* 新一帧: 清空光层和深度 */
static void
galaxyglframe(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    glViewport(0, 0, galaxygl.fw, galaxygl.fh);
    glClearColor(0, 0, 0, 0);
    glClearDepth(1);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    galaxygl.ninst = galaxygl.npart = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.cardfbo);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    galaxygl.ncards = galaxygl.cardups = 0;
    galaxygl.frameid++;
    memset(galaxygl.lens, 0, sizeof galaxygl.lens);
    memset(galaxygl.shock, 0, sizeof galaxygl.shock);
    galaxygl.glow = 0;
    galaxygl.rays[2] = 0;
    galaxygl.expo = 1;
    galaxygl.pull = galaxygl.burst = 0;
    memset(galaxygl.rip, 0, sizeof galaxygl.rip);
}

static void
galaxygldraw(GalaxyGLInst *inst, int n, int depthtest)
{
    if (n < 1)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    glViewport(0, 0, galaxygl.fw, galaxygl.fh);
    glUseProgram(galaxygl.proglight);
    glUniform2f(glGetUniformLocation(galaxygl.proglight, "screen"), galaxygl.fw, galaxygl.fh);
    glUniform1f(glGetUniformLocation(galaxygl.proglight, "time"), (float)galaxynow());
    glUniform1f(glGetUniformLocation(galaxygl.proglight, "still"), galaxyscene.gentle ? 1 : 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    if (depthtest) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glBindVertexArray(galaxygl.vao);
    glBindBuffer(GL_ARRAY_BUFFER, galaxygl.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * sizeof *inst, inst, GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
    glDisable(GL_DEPTH_TEST);
}

/* 把已经提交的光点 / 光带画进光层 (画卡片之前和每帧结束时调用) */
static void
galaxyglflush(void)
{
    galaxygldraw(galaxygl.inst, galaxygl.ninst, 0);
    galaxygl.ninst = 0;
}

static GalaxyGLInst *
galaxyglpush(void)
{
    if (galaxygl.ninst >= GALAXYGLINST)
        galaxyglflush();
    return &galaxygl.inst[galaxygl.ninst++];
}

/* 镜头空间深度 -> 光层深度 (-1..1); 屏幕空间的光 (z 不在镜头范围内) 放在最前 */
static float
galaxygldepth(double z)
{
    GalaxyCamera *c = &galaxyscene.cam;

    if (z <= c->near)
        return -1;
    return (float)galaxyclamp((z - c->near) / (c->far - c->near)) * 2 - 1;
}

/* 光点: shape 为 GalaxyHalo / GalaxyDisc / GalaxySpike; rgb 0..1; core: 白芯彩晕的着色强度 */
static void
galaxyglglow(int shape, const double rgb[3], double core, double x, double y, double radius, double alpha)
{
    GalaxyGLInst *g;
    double angle = 0;

    /* 衍射芒: 越亮越长, 方向随镜头慢慢转 (像真实镜头里的星芒, 不是贴在屏幕上的十字) */
    if (shape == GalaxySpike) {
        radius *= .75 + .4 * MIN(1, alpha);
        angle = .5 * galaxyscene.cam.ry + .35 * galaxyscene.cam.rx;
    }
    if (!galaxygl.win || alpha < 1.0 / 512 || radius < .25 || x + radius < 0 || y + radius < 0
            || x - radius > galaxygl.fw || y - radius > galaxygl.fh)
        return;
    g = galaxyglpush();
    *g = (GalaxyGLInst){{x, y, radius, angle}, {0, alpha, shape == GalaxySpike ? GalaxyGLSpike : shape == GalaxyDisc ? GalaxyGLDisc : shape == GalaxyPoint ? GalaxyGLStar : shape == GalaxySurface ? GalaxyGLSurface : GalaxyGLHalo, -1},
        {rgb[0], rgb[1], rgb[2], core}, {0}};
}

/* 星云: 每帧光层清空后最先画 (加法), 之后卡片挖洞会把卡片后面的部分擦掉.
 * k: 强度; t: 秒 (流动); oct: fbm 倍频数 (降级时减少); 颜色取极光配色的紫 / 青 / 玫粉 */
static void
galaxyglnebula(double k, double t, const double seed[2], int still)
{
    GalaxyScene *r = &galaxyscene;
    GLuint p = galaxygl.prognebula;

    if (!galaxygl.win || !p || k < .004)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    glViewport(0, 0, galaxygl.fw, galaxygl.fh);
    glUseProgram(p);
    glUniform2f(glGetUniformLocation(p, "screen"), galaxygl.fw, galaxygl.fh);
    glUniform4f(glGetUniformLocation(p, "view"), r->vx, r->vy, r->vw, r->vh);
    glUniform2f(glGetUniformLocation(p, "cam"), (float)r->cam.ry, (float)-r->cam.rx);
    glUniform1f(glGetUniformLocation(p, "t"), (float)t);
    glUniform1f(glGetUniformLocation(p, "k"), (float)k);
    glUniform2f(glGetUniformLocation(p, "seed"), (float)seed[0], (float)seed[1]);
    glUniform1f(glGetUniformLocation(p, "still"), still ? 1 : 0);
    {   /* 远景元素 (galaxyfarinit 生成) */
        float g0[2][4], g1[2][4];
        int i;

        for (i = 0; i < 2; i++) {
            memcpy(g0[i], r->farg[i], sizeof g0[i]);
            memcpy(g1[i], r->farg[i] + 4, sizeof g1[i]);
        }
        glUniform4fv(glGetUniformLocation(p, "fg0"), 2, &g0[0][0]);
        glUniform4fv(glGetUniformLocation(p, "fg1"), 2, &g1[0][0]);
        glUniform4fv(glGetUniformLocation(p, "fs"), 3, &r->fars[0][0]);
        glUniform4fv(glGetUniformLocation(p, "comet"), 1, r->farcomet);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(galaxygl.fullvao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* 光带: 两端颜色可不同 (打包 0xRRGGBB), a 为不透明度, hw 为半宽 */
static void
galaxyglline(const GalaxyProj *pa, const GalaxyProj *pb, unsigned int c0, unsigned int c1, double a, double hw)
{
    GalaxyGLInst *g;

    if (!galaxygl.win || a < 1.0 / 512)
        return;
    g = galaxyglpush();
    *g = (GalaxyGLInst){{pa->x, pa->y, pb->x, pb->y}, {hw, a, GalaxyGLLine, -1},
        {(c0 >> 16 & 255) / 255.0f, (c0 >> 8 & 255) / 255.0f, (c0 & 255) / 255.0f, 0},
        {(c1 >> 16 & 255) / 255.0f, (c1 >> 8 & 255) / 255.0f, (c1 & 255) / 255.0f, 0}};
}

/* 粒子流: 和光带同样的参数, 画成沿线流动、闪烁的细粒子 (轨道 / 星轨 / 尾迹 / 连线等都用它) */
static void
galaxyglstream(const GalaxyProj *pa, const GalaxyProj *pb, unsigned int c0, unsigned int c1, double a, double hw)
{
    GalaxyGLInst *g;

    if (!galaxygl.win || a < 1.0 / 512)
        return;
    g = galaxyglpush();
    *g = (GalaxyGLInst){{pa->x, pa->y, pb->x, pb->y}, {hw, a, GalaxyGLStream, -1},
        {(c0 >> 16 & 255) / 255.0f, (c0 >> 8 & 255) / 255.0f, (c0 & 255) / 255.0f, 0},
        {(c1 >> 16 & 255) / 255.0f, (c1 >> 8 & 255) / 255.0f, (c1 & 255) / 255.0f, 0}};
}

/* 实色矩形 (闪光), 加在光层上 */
static void
galaxyglrect(double x, double y, double w, double h, const double rgb[3], double a)
{
    GalaxyGLInst *g;

    if (!galaxygl.win || a < 1.0 / 512)
        return;
    g = galaxyglpush();
    *g = (GalaxyGLInst){{x, y, w, h}, {0, a, GalaxyGLRect, -1}, {rgb[0], rgb[1], rgb[2], 0}, {0}};
}

/* 粒子 (柔和的小光点), 最后统一画, 按镜头深度被卡片挡住; bokeh: 焦外的大光斑 */
static void
galaxyglparticle(double x, double y, double z, double radius, const double rgb[3], double core, double alpha, int bokeh)
{
    if (!galaxygl.win || galaxygl.npart >= GALAXYGLPART || alpha < 1.0 / 512 || radius < .3
            || x + radius < 0 || y + radius < 0 || x - radius > galaxygl.fw || y - radius > galaxygl.fh)
        return;
    galaxygl.part[galaxygl.npart++] = (GalaxyGLInst){{x, y, radius, 0}, {0, alpha, bokeh ? GalaxyGLBokeh : GalaxyGLDust, galaxygldepth(z)},
        {rgb[0], rgb[1], rgb[2], core}, {0}};
}

static void
galaxyglmat(GLuint p, const char *name, const GalaxyMat *m)
{
    float f[9];
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            f[i * 3 + j] = (float)m->m[i][j];
    glUniformMatrix3fv(glGetUniformLocation(p, name), 1, GL_TRUE, f);
}

/* GPU 粒子的公共状态: 镜头投影, 加法混合, 按卡片深度测试 (卡片挖洞时已写入) */
static GLuint
galaxyglpartbegin(void)
{
    GalaxyScene *r = &galaxyscene;
    GLuint p = galaxygl.progpart;

    if (!galaxygl.win || !p)
        return 0;
    galaxyglflush();
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    glViewport(0, 0, galaxygl.fw, galaxygl.fh);
    glUseProgram(p);
    galaxyglmat(p, "view", &r->cam.view);
    glUniform3f(glGetUniformLocation(p, "campos"), r->cam.pos.x, r->cam.pos.y, r->cam.pos.z);
    glUniform2f(glGetUniformLocation(p, "center"), r->vx + r->vw * .5, r->vy + r->vh * .5);
    glUniform2f(glGetUniformLocation(p, "screen"), galaxygl.fw, galaxygl.fh);
    glUniform1f(glGetUniformLocation(p, "focal"), r->cam.focal);
    glUniform1f(glGetUniformLocation(p, "cnear"), r->cam.near);
    glUniform1f(glGetUniformLocation(p, "cfar"), r->cam.far);
    glUniform1f(glGetUniformLocation(p, "fnear"), r->cam.focal * .25);
    glUniform1f(glGetUniformLocation(p, "time"), (float)galaxynow());
    glUniform1f(glGetUniformLocation(p, "pull"), galaxygl.pull);
    glUniform1f(glGetUniformLocation(p, "burst"), galaxygl.burst);
    glUniform1f(glGetUniformLocation(p, "bext"), galaxygl.bext);
    glUniform3fv(glGetUniformLocation(p, "axis"), 1, galaxygl.axis);
    glUniform4fv(glGetUniformLocation(p, "rip"), 1, galaxygl.rip);
    glUniform3fv(glGetUniformLocation(p, "ripn"), 1, galaxygl.ripn);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glBindVertexArray(galaxygl.fullvao);
    return p;
}

/* 一个核心的吸积盘: rin / rout 世界半径, omega 内缘角速度 (rad/s), n 粒子数 */
static void
galaxygldisk(GalaxyVec pos, const GalaxyMat *plane, double rin, double rout, double omega, const double rgb[3],
        double alpha, double psize, int n, double seed)
{
    GLuint p = galaxyglpartbegin();

    if (!p || n < 1 || alpha < .004 || rout <= rin)
        return;
    galaxyglmat(p, "plane", plane);
    glUniform3f(glGetUniformLocation(p, "cpos"), pos.x, pos.y, pos.z);
    glUniform3f(glGetUniformLocation(p, "tint"), rgb[0], rgb[1], rgb[2]);
    glUniform4f(glGetUniformLocation(p, "disk"), rin, rout, omega, .05);
    glUniform1f(glGetUniformLocation(p, "mode"), 0);
    glUniform1f(glGetUniformLocation(p, "seed"), seed);
    glUniform1f(glGetUniformLocation(p, "alpha"), alpha);
    glUniform1f(glGetUniformLocation(p, "psize"), psize);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
    glDisable(GL_DEPTH_TEST);
}

/* 粒子恒星: 半径 rad (世界), 自转角速度 omega, n 颗粒子; alpha 是单颗粒子的亮度 */
static void
galaxyglstarball(GalaxyVec pos, const GalaxyMat *plane, double rad, double omega, const double rgb[3], double alpha, double psize, int n, double seed)
{
    GLuint p = galaxyglpartbegin();

    if (!p || n < 1 || alpha < .002 || rad <= 0)
        return;
    galaxyglmat(p, "plane", plane);
    glUniform3f(glGetUniformLocation(p, "cpos"), pos.x, pos.y, pos.z);
    glUniform3f(glGetUniformLocation(p, "tint"), rgb[0], rgb[1], rgb[2]);
    glUniform4f(glGetUniformLocation(p, "disk"), rad, 0, omega, .8);
    glUniform1f(glGetUniformLocation(p, "mode"), 2);
    glUniform1f(glGetUniformLocation(p, "seed"), seed);
    glUniform1f(glGetUniformLocation(p, "alpha"), alpha);
    glUniform1f(glGetUniformLocation(p, "psize"), psize);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
    glDisable(GL_DEPTH_TEST);
}

/* 星体身后的粒子尾迹: pts[0..n-1] 是星体现在和过去 (等时间间隔, 共 lm 个 motion 单位) 的世界位置;
 * width: 散开的半径, count 颗粒子, alpha 是单颗粒子的亮度 */
static void
galaxygltrail(const GalaxyVec *pts, int n, double lm, double mclock, double head, double grid, double tlen, double width,
        const double rgb[3], double alpha, double psize, int count, double seed)
{
    GLuint p;
    float f[48][4];
    double arc = 0;
    int i;

    if (n < 2 || count < 1 || alpha < .002 || lm <= 0 || !(p = galaxyglpartbegin()))
        return;
    n = MIN(n, 48);
    for (i = 0; i < n; i++) {
        if (i)
            arc += galaxylen(galaxysub(pts[i], pts[i - 1]));
        f[i][0] = pts[i].x;
        f[i][1] = pts[i].y;
        f[i][2] = pts[i].z;
        f[i][3] = arc;
    }
    glUniform4fv(glGetUniformLocation(p, "trail"), n, &f[0][0]);
    glUniform1f(glGetUniformLocation(p, "tlen"), tlen);
    glUniform1i(glGetUniformLocation(p, "ntrail"), n);
    glUniform1f(glGetUniformLocation(p, "mclock"), (float)fmod(mclock, lm * 4096));
    glUniform3f(glGetUniformLocation(p, "tint"), rgb[0], rgb[1], rgb[2]);
    glUniform4f(glGetUniformLocation(p, "disk"), lm, width, head, grid);
    glUniform1f(glGetUniformLocation(p, "mode"), 3);
    glUniform1f(glGetUniformLocation(p, "seed"), seed);
    glUniform1f(glGetUniformLocation(p, "alpha"), alpha);
    glUniform1f(glGetUniformLocation(p, "psize"), psize);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, count);
    glDisable(GL_DEPTH_TEST);
}

/* Esc 收尾 (这一帧): pull 吸入进度, burst 爆发进度, axis 绕转的轴 (世界), ext 爆发的半径 */
static void
galaxyglcollapsefx(double pull, double burst, GalaxyVec axis, double ext)
{
    double l = galaxylen(axis);

    galaxygl.pull = (float)pull;
    galaxygl.burst = (float)burst;
    galaxygl.bext = (float)ext;
    galaxygl.axis[0] = (float)(l > 0 ? axis.x / l : 0);
    galaxygl.axis[1] = (float)(l > 0 ? axis.y / l : 1);
    galaxygl.axis[2] = (float)(l > 0 ? axis.z / l : 0);
}

/* 涟漪场 (这一帧): 以世界原点为中心、半径 radius 的一圈波, 强度 amp, 宽度 width; normal 是盘面法线 */
static void
galaxyglripple(double radius, double amp, double width, GalaxyVec normal)
{
    double l = galaxylen(normal);

    galaxygl.rip[0] = (float)radius;
    galaxygl.rip[1] = (float)amp;
    galaxygl.rip[2] = (float)MAX(1, width);
    galaxygl.ripn[0] = (float)(l > 0 ? normal.x / l : 0);
    galaxygl.ripn[1] = (float)(l > 0 ? normal.y / l : 1);
    galaxygl.ripn[2] = (float)(l > 0 ? normal.z / l : 0);
}

/* 天象粒子: shape 见 galaxyglvspartfx; basis 的第三列是主方向 (喷流轴 / 环面法线), 粒子流用第二列作弯曲偏移;
 * speed / spread / life / dur 依形状含义不同; et 事件经过的秒数; c0 -> c1 颜色渐变; n 颗粒子 */
static void
galaxyglemitter(int shape, GalaxyVec pos, const GalaxyMat *basis, GalaxyVec b, double speed, double spread, double life,
        double dur, double et, const double c0[3], const double c1[3], double alpha, double psize, int n, double seed)
{
    GLuint p;

    if (n < 1 || alpha < .002 || !(p = galaxyglpartbegin()))
        return;
    glUniform1f(glGetUniformLocation(p, "mode"), 4);
    glUniform1i(glGetUniformLocation(p, "eshape"), shape);
    glUniform3f(glGetUniformLocation(p, "epos"), pos.x, pos.y, pos.z);
    glUniform3f(glGetUniformLocation(p, "eb"), b.x, b.y, b.z);
    galaxyglmat(p, "ebasis", basis);
    glUniform4f(glGetUniformLocation(p, "ep"), speed, spread, MAX(1e-3, life), dur);
    glUniform1f(glGetUniformLocation(p, "et"), (float)et);
    glUniform3f(glGetUniformLocation(p, "ec0"), c0[0], c0[1], c0[2]);
    glUniform3f(glGetUniformLocation(p, "ec1"), c1[0], c1[1], c1[2]);
    glUniform1f(glGetUniformLocation(p, "seed"), seed);
    glUniform1f(glGetUniformLocation(p, "alpha"), alpha);
    glUniform1f(glGetUniformLocation(p, "psize"), psize);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
    glDisable(GL_DEPTH_TEST);
}

/* 星尘: 半径 extent 的扁平区域里 n 颗, 被至多 10 个光源 (位置 + 影响半径, 颜色 * 强度) 照亮 */
static void
galaxygldust(const GalaxyMat *world, double extent, float lights[][4], float lcol[][3], int nl, double alpha, double psize, int n, double seed)
{
    GLuint p = galaxyglpartbegin();

    if (!p || n < 1 || alpha < .004 || nl < 1)
        return;
    galaxyglmat(p, "plane", world);   /* 星尘跟着星系群整体转 */
    glUniform1f(glGetUniformLocation(p, "mode"), 1);
    glUniform1f(glGetUniformLocation(p, "seed"), seed);
    glUniform1f(glGetUniformLocation(p, "alpha"), alpha);
    glUniform1f(glGetUniformLocation(p, "psize"), psize);
    glUniform1f(glGetUniformLocation(p, "extent"), extent);
    glUniform4fv(glGetUniformLocation(p, "lights"), nl, &lights[0][0]);
    glUniform3fv(glGetUniformLocation(p, "lcol"), nl, &lcol[0][0]);
    glUniform1i(glGetUniformLocation(p, "nl"), nl);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n);
    glDisable(GL_DEPTH_TEST);
}

/* 快速粒子的运动拉丝: (x0, y0) 尾 -> (x1, y1) 头, 尾部按 tail 变暗; hw 半宽 */
static void
galaxyglpartline(double x0, double y0, double x1, double y1, double z, double hw, const double rgb[3], double tail, double alpha)
{
    if (!galaxygl.win || galaxygl.npart >= GALAXYGLPART || alpha < 1.0 / 512
            || MAX(x0, x1) < 0 || MAX(y0, y1) < 0 || MIN(x0, x1) > galaxygl.fw || MIN(y0, y1) > galaxygl.fh)
        return;
    galaxygl.part[galaxygl.npart++] = (GalaxyGLInst){{x0, y0, x1, y1}, {hw, alpha, GalaxyGLLine, galaxygldepth(z)},
        {rgb[0] * tail, rgb[1] * tail, rgb[2] * tail, 0}, {rgb[0], rgb[1], rgb[2], 0}};
}

/* 卡片挖洞: 先画掉已提交的光, 再按卡片形状把它后面的光擦掉, 并写入卡片深度 (粒子按它测试) */
static void
galaxyglcutout(double q[4][2], double z[4], double alpha)
{
    float v[4][3];
    static GLuint vao, vbo;
    int i, k[4] = {0, 1, 3, 2};

    if (!galaxygl.win || alpha < .02)
        return;
    galaxyglflush();
    if (!vao) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof v[0], NULL);
    }
    for (i = 0; i < 4; i++) {
        v[i][0] = q[k[i]][0];
        v[i][1] = q[k[i]][1];
        v[i][2] = galaxygldepth(z[k[i]]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    glViewport(0, 0, galaxygl.fw, galaxygl.fh);
    glUseProgram(galaxygl.progcut);
    glUniform2f(glGetUniformLocation(galaxygl.progcut, "screen"), galaxygl.fw, galaxygl.fh);
    glUniform1f(glGetUniformLocation(galaxygl.progcut, "alpha"), MIN(1, alpha));
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(alpha > .5 ? GL_TRUE : GL_FALSE);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_DEPTH_TEST);
}

static void
galaxyglfull(GLuint prog, GLuint src, int w, int h, int sw, int sh)
{
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform1i(glGetUniformLocation(prog, "src"), 0);
    glUniform2f(glGetUniformLocation(prog, "texel"), .5f / sw, .5f / sh);
    glViewport(0, 0, w, h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* 截图 pixmap -> 卡片纹理: 经 GLX pixmap 逐像素拷贝 (统一成第 0 行在上), 再生成 mip 链 */
static void
galaxyglcardcopy(GLXPixmap gp, GLuint src, GLuint t, int w, int h)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glXBindTexImageEXT(dpy, gp, GLX_FRONT_LEFT_EXT, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.copyfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(galaxygl.progcopy);
    glUniform1i(glGetUniformLocation(galaxygl.progcopy, "src"), 0);
    glUniform1f(glGetUniformLocation(galaxygl.progcopy, "flip"), galaxygl.yinv32);
    glBindVertexArray(galaxygl.fullvao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glXReleaseTexImageEXT(dpy, gp, GLX_FRONT_LEFT_EXT);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);   /* 卸下, 删纹理时显存才能立即释放 */
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    glBindTexture(GL_TEXTURE_2D, t);
    glGenerateMipmap(GL_TEXTURE_2D);
}

/* 建卡片纹理 (三线性 / 各向异性过滤). GLX pixmap 和读它用的纹理留着: 截图刷新后用 galaxyglcardcopy 原地重拷,
 * 不必每次分配显存 / 建 GLX pixmap. 失败返回 0 */
static GLuint
galaxyglcardtex(Pixmap pix, int w, int h, unsigned long *keep, unsigned int *srctex)
{
    GLXPixmap gp;
    GLuint src, t;
    GLint maxsz = 0;

    if (!galaxygl.win || !galaxygl.progcopy || w < 1 || h < 1)
        return 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxsz);
    if (w > maxsz || h > maxsz)
        return 0;
    if (!(gp = galaxyglpixmap(pix, 1)))
        return 0;
    glGenTextures(1, &src);
    glBindTexture(GL_TEXTURE_2D, src);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (galaxygl.aniso > 1)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, MIN(8, galaxygl.aniso));
    galaxyglcardcopy(gp, src, t, w, h);
    galaxygl.texmade++;
    *keep = gp;
    *srctex = src;
    return t;
}

/* 释放卡片纹理 (要在释放截图的 X pixmap 之前) */
static void
galaxyglfreecard(unsigned int *t, unsigned int *src, unsigned long *gp)
{
    XErrorHandler old;

    if (!*t && !*gp)
        return;
    if (galaxygl.win && (glXGetCurrentContext() == galaxygl.ctx || glXMakeCurrent(dpy, galaxygl.win, galaxygl.ctx))) {
        if (*t) {
            glDeleteTextures(1, t);
            galaxygl.texfreed++;
        }
        if (*src)
            glDeleteTextures(1, src);
        if (*gp) {
            old = XSetErrorHandler(xerrordummy);
            glXDestroyPixmap(dpy, *gp);
            XSync(dpy, False);
            XSetErrorHandler(old);
        }
    }
    *t = *src = 0;
    *gp = 0;
}

/* 卡片的着色参数: 不透明度 / 发白 / 压暗 / 边缘光 (颜色) / 高光 (位置) / mip 偏移 / 宽高比 / 点击涟漪 (uv + 进度) / 环境光颜色 */
typedef struct {
    double vis, tint, dark, rim, rimc[3], spec, specat, bias, aspect, ripple[3], env[3];
    int exact;
} GalaxyCardFx;

/* 卡片四边形 -> 顶点 (裁剪坐标 xyzw + uv) 并画出; weight 乘在输出上 (运动模糊的每一份) */
static void
galaxyglcardquad(GLuint tex, double q[4][2], double z[4], const GalaxyCardFx *fx, double weight)
{
    static const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    static const int order[4] = {0, 1, 3, 2};
    float v[4][6], w;
    GLuint p = galaxygl.progcard;
    int i, k;

    for (i = 0; i < 4; i++) {
        k = order[i];
        w = fx->exact ? 1 : (float)MAX(1e-3, z[k]);
        v[i][0] = (float)(q[k][0] / galaxygl.fw * 2 - 1) * w;
        v[i][1] = (float)(1 - q[k][1] / galaxygl.fh * 2) * w;
        v[i][2] = 0;
        v[i][3] = w;
        v[i][4] = uvs[k][0];
        v[i][5] = uvs[k][1];
    }
    glUseProgram(p);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(p, "tex"), 0);
    glUniform1f(glGetUniformLocation(p, "vis"), (float)fx->vis);
    glUniform1f(glGetUniformLocation(p, "tint"), (float)fx->tint);
    glUniform1f(glGetUniformLocation(p, "dark"), (float)fx->dark);
    glUniform1f(glGetUniformLocation(p, "rim"), (float)fx->rim);
    glUniform3f(glGetUniformLocation(p, "rimc"), (float)fx->rimc[0], (float)fx->rimc[1], (float)fx->rimc[2]);
    glUniform1f(glGetUniformLocation(p, "spec"), (float)fx->spec);
    glUniform1f(glGetUniformLocation(p, "specat"), (float)fx->specat);
    glUniform1f(glGetUniformLocation(p, "bias"), (float)fx->bias);
    glUniform1f(glGetUniformLocation(p, "aspect"), (float)fx->aspect);
    glUniform1f(glGetUniformLocation(p, "exact"), fx->exact ? 1 : 0);
    glUniform3f(glGetUniformLocation(p, "ripple"), (float)fx->ripple[0], (float)fx->ripple[1], (float)fx->ripple[2]);
    glUniform3f(glGetUniformLocation(p, "envc"), (float)fx->env[0], (float)fx->env[1], (float)fx->env[2]);
    glUniform1f(glGetUniformLocation(p, "weight"), (float)weight);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(galaxygl.cardvao);
    glBindBuffer(GL_ARRAY_BUFFER, galaxygl.cardvbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* 画一张卡片进卡片层: q 屏幕四角 (左上 右上 右下 左下), z 镜头深度 (透视校正).
 * pq: 上一帧的四角 (NULL 表示没有); 位移大时沿 上一帧 -> 这一帧 画 n 份做运动模糊 (先加进模糊层再合上来) */
static void
galaxyglcard(GLuint tex, double q[4][2], double z[4], const GalaxyCardFx *fx, double pq[4][2])
{
    double qi[4][2], d = 0, x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, t;
    int i, k, n = 1;

    if (!galaxygl.win || !galaxygl.progcard || !tex)
        return;
    if (pq && !fx->exact && galaxygl.blurfbo)
        for (i = 0; i < 4; i++)
            d = MAX(d, hypot(q[i][0] - pq[i][0], q[i][1] - pq[i][1]));
    if (d > 8)
        n = MIN(6, (int)ceil(d / 8));
    glEnable(GL_BLEND);
    glViewport(0, 0, galaxygl.fw, galaxygl.fh);
    if (n == 1) {
        glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.cardfbo);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        galaxyglcardquad(tex, q, z, fx, 1);
    } else {
        for (i = 0; i < 4; i++) {
            x0 = MIN(x0, MIN(q[i][0], pq[i][0]));
            x1 = MAX(x1, MAX(q[i][0], pq[i][0]));
            y0 = MIN(y0, MIN(q[i][1], pq[i][1]));
            y1 = MAX(y1, MAX(q[i][1], pq[i][1]));
        }
        x0 = MAX(0, floor(x0) - 2);
        y0 = MAX(0, floor(y0) - 2);
        x1 = MIN(galaxygl.fw, ceil(x1) + 2);
        y1 = MIN(galaxygl.fh, ceil(y1) + 2);
        if (x1 <= x0 || y1 <= y0)
            return;
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)x0, (int)(galaxygl.fh - y1), (int)(x1 - x0), (int)(y1 - y0));
        glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.blurfbo);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBlendFunc(GL_ONE, GL_ONE);
        for (k = 0; k < n; k++) {
            t = (k + .5) / n;
            for (i = 0; i < 4; i++) {
                qi[i][0] = pq[i][0] + (q[i][0] - pq[i][0]) * t;
                qi[i][1] = pq[i][1] + (q[i][1] - pq[i][1]) * t;
            }
            galaxyglcardquad(tex, qi, z, fx, 1.0 / n);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.cardfbo);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(galaxygl.progcopy);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, galaxygl.blurtex);
        glUniform1i(glGetUniformLocation(galaxygl.progcopy, "src"), 0);
        glUniform1f(glGetUniformLocation(galaxygl.progcopy, "flip"), 0);
        glBindVertexArray(galaxygl.fullvao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    galaxygl.ncards++;
}

/* 半精度浮点 -> float (读回的泛光是 RGBA16F) */
static float
galaxyhalf(unsigned short h)
{
    int e = h >> 10 & 31, m = h & 1023;
    float v = e == 0 ? m / 16777216.0f : e == 31 ? 65504.0f : ldexpf(1 + m / 1024.0f, e - 15);

    return h & 0x8000 ? -v : v;
}

/* 曝光适应: 每两帧把最小一级泛光 (只做过降采样, 是光层的平均) 按原格式读进 PBO 并插一个 fence;
 * 之后只在 fence 已经完成时才映射读出, 绝不等 GPU (等的话每帧要多 2~3ms) */
static void
galaxyglreadlum(int level)
{
    unsigned short *px;
    double sum = 0, m;
    GLenum st;
    int i, k, n, w = galaxygl.bw[level], h = galaxygl.bh[level];

    /* 按降级后可能用到的最大一级 (第 2 级, 级数最少剩 3) 分配 */
    if (!galaxygl.pbo[0]) {
        galaxygl.pbocap = galaxygl.bw[2] * galaxygl.bh[2];
        glGenBuffers(3, galaxygl.pbo);
        for (i = 0; i < 3; i++) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, galaxygl.pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)galaxygl.pbocap * 4 * sizeof(unsigned short), NULL, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    /* 已经完成的读回: 取出来算平均 */
    for (k = 0; k < 3; k++) {
        if (!galaxygl.pbofence[k])
            continue;
        st = glClientWaitSync(galaxygl.pbofence[k], 0, 0);
        if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED)
            continue;
        glDeleteSync(galaxygl.pbofence[k]);
        galaxygl.pbofence[k] = 0;
        n = galaxygl.pbon_[k];
        glBindBuffer(GL_PIXEL_PACK_BUFFER, galaxygl.pbo[k]);
        if (n > 0 && (px = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)n * 4 * sizeof(unsigned short), GL_MAP_READ_BIT))) {
            for (i = 0; i < n; i++) {
                m = MAX(galaxyhalf(px[i * 4]), MAX(galaxyhalf(px[i * 4 + 1]), galaxyhalf(px[i * 4 + 2])));
                sum += m;
            }
            galaxygl.lum = sum / n;
            sum = 0;
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (++galaxygl.pboi % 2 || w * h > galaxygl.pbocap)
        return;
    /* 找一个空闲的 PBO 发起新的读回 */
    for (k = 0; k < 3 && galaxygl.pbofence[k]; k++);
    if (k == 3)
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, galaxygl.bloomfbo[level]);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, galaxygl.pbo[k]);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_HALF_FLOAT, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    galaxygl.pbofence[k] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    galaxygl.pbon_[k] = w * h;
}

/* 后期 (这一帧): 体积光束的光源位置 (像素) 和强度, 光层曝光 */
static void
galaxyglpostfx(double rx, double ry, double rays, double expo)
{
    galaxygl.rays[0] = (float)rx;
    galaxygl.rays[1] = (float)ry;
    galaxygl.rays[2] = (float)rays;
    galaxygl.expo = (float)expo;
}

/* 收尾特效 (这一帧): 透镜中心 / 强度 / 半径, 冲击波半径 / 宽度 / 光环亮度, 是否揭开壁纸, 余晖亮度 (像素, 左上原点) */
static void
galaxyglexitfx(double cx, double cy, double lens, double lensr, double sr, double sw, double sk, int reveal, double glow)
{
    galaxygl.lens[0] = (float)cx;
    galaxygl.lens[1] = (float)cy;
    galaxygl.lens[2] = (float)lens;
    galaxygl.lens[3] = (float)MAX(1, lensr);
    galaxygl.shock[0] = (float)sr;
    galaxygl.shock[1] = (float)MAX(1, sw);
    galaxygl.shock[2] = (float)sk;
    galaxygl.shock[3] = reveal ? 1 : 0;
    galaxygl.glow = (float)glow;
}

/* 每帧最后: 画完剩下的光和粒子, 泛光, 与底层 / 前景层合成, 交换缓冲.
 * bloom: 泛光强度; fx: 色差和颗粒的强度 (开场第一帧 / 回程最后为 0, 保证与真实桌面一致) */
static void
galaxyglpresent(double bloom, double fx, int levels)
{
    GalaxyScene *r = &galaxyscene;
    struct timespec t0, t1;
    GLuint p = galaxygl.progcomp;
    int i, wall;

    if (!galaxygl.win)
        return;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    galaxyglflush();
    galaxygldraw(galaxygl.part, galaxygl.npart, 1);
    galaxygl.npart = 0;
    glDisable(GL_BLEND);
    glBindVertexArray(galaxygl.fullvao);
    levels = MAX(1, MIN(GALAXYBLOOM, levels));
    if (bloom > .001) {
        for (i = 0; i < levels; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.bloomfbo[i]);
            galaxyglfull(galaxygl.progdown, i ? galaxygl.bloomtex[i - 1] : galaxygl.lighttex, galaxygl.bw[i], galaxygl.bh[i],
                    i ? galaxygl.bw[i - 1] : galaxygl.fw, i ? galaxygl.bh[i - 1] : galaxygl.fh);
        }
        /* 升采样时每一级乘 GALAXYBLOOMW 再叠到上一级: 第 i 级的总权重是 GALAXYBLOOMW^i, 越宽的光晕越淡, 衰减连续不成圈 */
        glEnable(GL_BLEND);
        glBlendColor(GALAXYBLOOMW, GALAXYBLOOMW, GALAXYBLOOMW, GALAXYBLOOMW);
        glBlendFunc(GL_CONSTANT_COLOR, GL_ONE);
        for (i = levels - 1; i > 0; i--) {
            glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.bloomfbo[i - 1]);
            galaxyglfull(galaxygl.progup, galaxygl.bloomtex[i], galaxygl.bw[i - 1], galaxygl.bh[i - 1], galaxygl.bw[i], galaxygl.bh[i]);
        }
        glDisable(GL_BLEND);
        galaxyglreadlum(levels - 1);
    }
    /* 底层 / 前景层的 XRender 绘制必须先完成 */
    glXWaitX();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, galaxygl.fw, galaxygl.fh);
    glUseProgram(p);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, galaxygl.texback);
    glXBindTexImageEXT(dpy, galaxygl.gback, GLX_FRONT_LEFT_EXT, NULL);
    /* 平时逐像素对齐采样 (首帧与桌面一致); 透镜扭曲时要插值, 否则有锯齿 */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, galaxygl.lens[2] > .001 ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, galaxygl.lens[2] > .001 ? GL_LINEAR : GL_NEAREST);
    wall = galaxygl.gwall && galaxygl.shock[3] > .5;
    if (wall) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, galaxygl.texwall);
        glXBindTexImageEXT(dpy, galaxygl.gwall, GLX_FRONT_LEFT_EXT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, galaxygl.lighttex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, galaxygl.bloomtex[0]);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, galaxygl.texfront);
    glXBindTexImageEXT(dpy, galaxygl.gfront, GLX_FRONT_LEFT_EXT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(glGetUniformLocation(p, "tbase"), 0);
    glUniform1i(glGetUniformLocation(p, "tlight"), 1);
    glUniform1i(glGetUniformLocation(p, "tbloom"), 2);
    glUniform1i(glGetUniformLocation(p, "tfront"), 3);
    glUniform1i(glGetUniformLocation(p, "twall"), 4);
    glUniform1i(glGetUniformLocation(p, "tcards"), 5);
    glUniform1i(glGetUniformLocation(p, "tbloom2"), 6);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, galaxygl.bloomtex[MIN(2, levels - 1)]);
    glUniform1f(glGetUniformLocation(p, "cards"), galaxygl.ncards > 0 ? 1 : 0);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, galaxygl.cardtex);
    glActiveTexture(GL_TEXTURE0);
    glUniform4fv(glGetUniformLocation(p, "lens"), 1, galaxygl.lens);
    glUniform4f(glGetUniformLocation(p, "shock"), galaxygl.shock[0], galaxygl.shock[1], galaxygl.shock[2], wall ? 1 : 0);
    glUniform1f(glGetUniformLocation(p, "glow"), galaxygl.glow);
    glUniform3f(glGetUniformLocation(p, "rays"), galaxygl.rays[0], galaxygl.rays[1], bloom > .001 ? galaxygl.rays[2] : 0);
    glUniform1f(glGetUniformLocation(p, "expo"), galaxygl.expo > 0 ? galaxygl.expo : 1);
    glUniform1f(glGetUniformLocation(p, "ybase"), galaxygl.yinv24);
    glUniform1f(glGetUniformLocation(p, "yfront"), galaxygl.yinv32);
    glUniform1f(glGetUniformLocation(p, "bloomk"), bloom > .001 ? bloom * GALAXYBLOOMK : 0);
    glUniform1f(glGetUniformLocation(p, "aberr"), 3 * fx);
    glUniform1f(glGetUniformLocation(p, "grain"), .06 * fx);
    glUniform1f(glGetUniformLocation(p, "seed"), (float)fmod(galaxynow() * 7.31, 100));
    glUniform2f(glGetUniformLocation(p, "screen"), galaxygl.fw, galaxygl.fh);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE3);
    glXReleaseTexImageEXT(dpy, galaxygl.gfront, GLX_FRONT_LEFT_EXT);
    if (wall) {
        glActiveTexture(GL_TEXTURE4);
        glXReleaseTexImageEXT(dpy, galaxygl.gwall, GLX_FRONT_LEFT_EXT);
    }
    glActiveTexture(GL_TEXTURE0);
    glXReleaseTexImageEXT(dpy, galaxygl.gback, GLX_FRONT_LEFT_EXT);
    glXSwapBuffers(dpy, galaxygl.win);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    galaxygl.gputime += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    (void)r;
}
