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

enum { GalaxyGLHalo, GalaxyGLDisc, GalaxyGLSpike, GalaxyGLLine, GalaxyGLRect, GalaxyGLDust };

typedef struct { float a[4], b[4], c0[4], c1[4]; } GalaxyGLInst;

/* 进程内只建一次的 GL 状态 (上下文 / 程序 / 缓冲); 每次 Super+Z 只重新绑定遮罩窗口和两张 pixmap */
static struct {
    int ready, failed, yinv24, yinv32, yinv32config;
    GLXFBConfig fbwin, fb24, fb32;
    XVisualInfo *vi;
    Colormap cmap;
    GLXContext ctx;
    GLuint proglight, progcut, progdown, progup, progcomp, progprobe;
    GLuint vao, vbo, fullvao;
    GLuint fbo, lighttex, depthtex, bloomfbo[GALAXYBLOOM], bloomtex[GALAXYBLOOM];
    int fw, fh, bw[GALAXYBLOOM], bh[GALAXYBLOOM];
    GLuint texback, texfront;
    GLXPixmap gback, gfront;
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
    "out vec2 vl; out float vt; flat out vec4 fb; flat out vec4 fc0; flat out vec4 fc1; flat out vec2 flen;\n"
    "void main() {\n"
    "  vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2.0 - 1.0, pos;\n"
    "  int kind = int(ib.z + .5);\n"
    "  vt = 0.0; flen = vec2(0.0);\n"
    "  if (kind == 3) {\n"
    "    vec2 d = ia.zw - ia.xy; float L = max(length(d), 1e-3); vec2 dir = d / L, n = vec2(-dir.y, dir.x);\n"
    "    float e = ib.x + 1.5, along = mix(-e, L + e, c.x * .5 + .5), across = c.y * e;\n"
    "    pos = ia.xy + dir * along + n * across; vl = vec2(along, across); flen = vec2(L, ib.x); vt = clamp(along / L, 0.0, 1.0);\n"
    "  } else if (kind == 4) {\n"
    "    pos = ia.xy + (c * .5 + .5) * ia.zw; vl = c;\n"
    "  } else {\n"
    "    pos = ia.xy + c * ia.z; vl = c;\n"
    "  }\n"
    "  gl_Position = vec4(pos.x / screen.x * 2.0 - 1.0, 1.0 - pos.y / screen.y * 2.0, ib.w, 1.0);\n"
    "  fb = ib; fc0 = ic0; fc1 = ic1;\n"
    "}\n";

/* 光晕 / 圆点 / 衍射芒的形状与原来的 sprite 相同; 白芯彩晕: 中心偏白, 颜色在衰减部分 (fc0.a 是着色强度) */
static const char *galaxyglfslight =
    "#version 330 core\n"
    "in vec2 vl; in float vt; flat in vec4 fb; flat in vec4 fc0; flat in vec4 fc1; flat in vec2 flen;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  int kind = int(fb.z + .5); float a; vec3 col = fc0.rgb;\n"
    "  if (kind == 3) {\n"
    "    float t = clamp(vl.x, 0.0, flen.x), d = length(vec2(vl.x - t, vl.y));\n"
    "    a = clamp(flen.y + .5 - d, 0.0, 1.0); col = mix(fc0.rgb, fc1.rgb, vt);\n"
    "  } else if (kind == 4) {\n"
    "    a = 1.0;\n"
    "  } else {\n"
    "    float d = length(vl);\n"
    "    if (kind == 0) a = (.55 * exp(-d * d / .0288) + .3 * exp(-d * d / .1568) + .15 * exp(-d * d / .5)) * (1.0 - smoothstep(.75, 1.0, d));\n"
    "    else if (kind == 2) a = max(max(exp(-vl.y * vl.y / .0006) * pow(max(1.0 - abs(vl.x), 0.0), 3.0), exp(-vl.x * vl.x / .0006) * pow(max(1.0 - abs(vl.y), 0.0), 3.0)),\n"
    "                            .35 * max(exp(-(vl.x - vl.y) * (vl.x - vl.y) / .0008), exp(-(vl.x + vl.y) * (vl.x + vl.y) / .0008)) * pow(clamp(1.0 - d, 0.0, 1.0), 3.0)) + .6 * exp(-d * d / .004);\n"
    "    else if (kind == 5) a = exp(-d * d / .12) * (1.0 - smoothstep(.8, 1.0, d));\n"
    "    else a = 1.0 - smoothstep(.4, 1.0, d);\n"
    "    a = clamp(a, 0.0, 1.0);\n"
    "    col = mix(vec3(1.0), fc0.rgb, smoothstep(0.0, .45, d) * fc0.a);\n"
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

/* 合成: 底层 滤色 光层 (软拐点压缩, 轻微色差和颗粒只作用在光上), 再盖前景层 */
static const char *galaxyglfscomp =
    "#version 330 core\n"
    "in vec2 uv; out vec4 o;\n"
    "uniform sampler2D tbase, tlight, tbloom, tfront;\n"
    "uniform float ybase, yfront, bloomk, aberr, grain, seed; uniform vec2 screen;\n"
    "vec3 knee(vec3 x) { return mix(x, 1.0 - .4 * exp(-(x - .6) / .4), step(.6, x)); }\n"
    "float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233)) + seed) * 43758.5453); }\n"
    "void main() {\n"
    "  vec2 ub = vec2(uv.x, ybase > .5 ? uv.y : 1.0 - uv.y), uf = vec2(uv.x, yfront > .5 ? uv.y : 1.0 - uv.y);\n"
    "  vec3 base = texture(tbase, ub).rgb;\n"
    "  vec2 off = (uv - .5) * aberr / screen;\n"
    "  vec3 L = vec3(texture(tlight, uv + off).r, texture(tlight, uv).g, texture(tlight, uv - off).b)\n"
    "         + bloomk * texture(tbloom, uv).rgb;\n"
    "  L = clamp(knee(max(L, 0.0)), 0.0, 1.0);\n"
    "  float lum = dot(L, vec3(.3, .59, .11));\n"
    "  L = clamp(L + (hash(uv * screen) - .5) * grain * lum, 0.0, 1.0);\n"
    "  vec3 c = 1.0 - (1.0 - base) * (1.0 - L);\n"
    "  vec4 f = texture(tfront, uf);\n"
    "  o = vec4(c * (1.0 - f.a) + f.rgb, 1.0);\n"
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
static int
galaxyglobjects(void)
{
    GLuint *progs[] = { &galaxygl.proglight, &galaxygl.progcut, &galaxygl.progdown, &galaxygl.progup,
        &galaxygl.progcomp, &galaxygl.progprobe };
    int i;

    if (galaxygl.proglight)
        return 1;
    galaxygl.proglight = galaxyglprogram(galaxyglvslight, galaxyglfslight);
    galaxygl.progcut = galaxyglprogram(galaxyglvscut, galaxyglfscut);
    galaxygl.progdown = galaxyglprogram(galaxyglvsfull, galaxyglfsdown);
    galaxygl.progup = galaxyglprogram(galaxyglvsfull, galaxyglfsup);
    galaxygl.progcomp = galaxyglprogram(galaxyglvsfull, galaxyglfscomp);
    galaxygl.progprobe = galaxyglprogram(galaxyglvsfull, galaxyglfsprobe);
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
    }
    galaxygl.fw = w;
    galaxygl.fh = h;
    galaxygl.lighttex = galaxygltex(w, h, GL_RGBA16F);
    galaxygl.depthtex = galaxygltex(w, h, GL_DEPTH_COMPONENT24);
    glGenFramebuffers(1, &galaxygl.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, galaxygl.lighttex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, galaxygl.depthtex, 0);
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

/* 直接检查前景 pixmap 在 GL 纹理中的上下方向，避免搜索框随驱动配置倒置。
 * 探针只在进入星系时运行一次；下一帧开始前把标记清掉。 */
static void
galaxyglprobefront(int w, int h)
{
    GalaxyScene *r = &galaxyscene;
    XRenderColor red = {0xffff, 0, 0, 0xffff}, blue = {0, 0, 0xffff, 0xffff}, clear = {0, 0, 0, 0};
    unsigned char bottom[4], top[4];

    if (!r->front || !galaxygl.gfront || w < 16 || h < 16)
        return;
    XRenderFillRectangle(dpy, PictOpSrc, r->front, &red, 0, 0, 8, 8);
    XRenderFillRectangle(dpy, PictOpSrc, r->front, &blue, 0, h - 8, 8, 8);
    glXWaitX();
    glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(galaxygl.progprobe);
    glBindVertexArray(galaxygl.fullvao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, galaxygl.texfront);
    glXBindTexImageEXT(dpy, galaxygl.gfront, GLX_FRONT_LEFT_EXT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(glGetUniformLocation(galaxygl.progprobe, "src"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bottom);
    glReadPixels(4, h - 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top);
    glXReleaseTexImageEXT(dpy, galaxygl.gfront, GLX_FRONT_LEFT_EXT);
    /* 直接采样时红色在上方，合成着色器就无需翻转前景纹理。 */
    if (top[0] > 192 && top[2] < 64 && bottom[2] > 192 && bottom[0] < 64)
        galaxygl.yinv32 = 1;
    else if (bottom[0] > 192 && bottom[2] < 64 && top[2] > 192 && top[0] < 64)
        galaxygl.yinv32 = 0;
    if (r->log)
        fprintf(r->log, "galaxy gl: front orientation %s\n", galaxygl.yinv32 ? "direct" : "flipped");
    XRenderFillRectangle(dpy, PictOpSrc, r->front, &clear, 0, 0, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* 每次进入星系: 遮罩窗口建好之后绑定上下文和底层 / 前景层 pixmap. 失败返回 0 */
static int
galaxyglbegin(Window win, Pixmap back, Pixmap front, int w, int h)
{
    if (!galaxyglinit() || !glXMakeCurrent(dpy, win, galaxygl.ctx) || !galaxyglobjects())
        return 0;
    if (epoxy_has_glx_extension(dpy, screen, "GLX_EXT_swap_control"))
        glXSwapIntervalEXT(dpy, win, 0);   /* 节奏由星系自己控制, 交换缓冲不等垂直同步 (由合成器同步) */
    galaxyglbuffers(w, h);
    galaxygl.win = win;
    galaxygl.gback = galaxyglpixmap(back, 0);
    galaxygl.gfront = galaxyglpixmap(front, 1);
    galaxygl.ninst = galaxygl.npart = 0;
    if (galaxygl.gback && galaxygl.gfront)
        galaxyglprobefront(w, h);
    return galaxygl.gback && galaxygl.gfront;
}

static void
galaxyglend(void)
{
    if (!galaxygl.ready || !galaxygl.win)
        return;
    if (galaxygl.gback)
        glXDestroyPixmap(dpy, galaxygl.gback);
    if (galaxygl.gfront)
        glXDestroyPixmap(dpy, galaxygl.gfront);
    galaxygl.gback = galaxygl.gfront = 0;
    glXMakeCurrent(dpy, None, NULL);
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

    if (!galaxygl.win || alpha < 1.0 / 512 || radius < .25 || x + radius < 0 || y + radius < 0
            || x - radius > galaxygl.fw || y - radius > galaxygl.fh)
        return;
    g = galaxyglpush();
    *g = (GalaxyGLInst){{x, y, radius, 0}, {0, alpha, shape == GalaxySpike ? GalaxyGLSpike : shape == GalaxyDisc ? GalaxyGLDisc : GalaxyGLHalo, -1},
        {rgb[0], rgb[1], rgb[2], core}, {0}};
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

/* 粒子 (柔和的小光点), 最后统一画, 按镜头深度被卡片挡住 */
static void
galaxyglparticle(double x, double y, double z, double radius, const double rgb[3], double core, double alpha)
{
    if (!galaxygl.win || galaxygl.npart >= GALAXYGLPART || alpha < 1.0 / 512 || radius < .3
            || x + radius < 0 || y + radius < 0 || x - radius > galaxygl.fw || y - radius > galaxygl.fh)
        return;
    galaxygl.part[galaxygl.npart++] = (GalaxyGLInst){{x, y, radius, 0}, {0, alpha, GalaxyGLDust, galaxygldepth(z)},
        {rgb[0], rgb[1], rgb[2], core}, {0}};
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

/* 每帧最后: 画完剩下的光和粒子, 泛光, 与底层 / 前景层合成, 交换缓冲.
 * bloom: 泛光强度; fx: 色差和颗粒的强度 (开场第一帧 / 回程最后为 0, 保证与真实桌面一致) */
static void
galaxyglpresent(double bloom, double fx, int levels)
{
    GalaxyScene *r = &galaxyscene;
    struct timespec t0, t1;
    GLuint p = galaxygl.progcomp;
    int i;

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
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        for (i = levels - 1; i > 0; i--) {
            glBindFramebuffer(GL_FRAMEBUFFER, galaxygl.bloomfbo[i - 1]);
            galaxyglfull(galaxygl.progup, galaxygl.bloomtex[i], galaxygl.bw[i - 1], galaxygl.bh[i - 1], galaxygl.bw[i], galaxygl.bh[i]);
        }
        glDisable(GL_BLEND);
    }
    /* 底层 / 前景层的 XRender 绘制必须先完成 */
    glXWaitX();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, galaxygl.fw, galaxygl.fh);
    glUseProgram(p);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, galaxygl.texback);
    glXBindTexImageEXT(dpy, galaxygl.gback, GLX_FRONT_LEFT_EXT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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
    glUniform1f(glGetUniformLocation(p, "ybase"), galaxygl.yinv24);
    glUniform1f(glGetUniformLocation(p, "yfront"), galaxygl.yinv32);
    glUniform1f(glGetUniformLocation(p, "bloomk"), bloom > .001 ? bloom : 0);
    glUniform1f(glGetUniformLocation(p, "aberr"), 3 * fx);
    glUniform1f(glGetUniformLocation(p, "grain"), .06 * fx);
    glUniform1f(glGetUniformLocation(p, "seed"), (float)fmod(galaxynow() * 7.31, 100));
    glUniform2f(glGetUniformLocation(p, "screen"), galaxygl.fw, galaxygl.fh);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE3);
    glXReleaseTexImageEXT(dpy, galaxygl.gfront, GLX_FRONT_LEFT_EXT);
    glActiveTexture(GL_TEXTURE0);
    glXReleaseTexImageEXT(dpy, galaxygl.gback, GLX_FRONT_LEFT_EXT);
    glXSwapBuffers(dpy, galaxygl.win);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    galaxygl.gputime += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    (void)r;
}
