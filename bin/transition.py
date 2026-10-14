#! /usr/bin/env python3
# 壁纸转场 (bin/livewall.sh 调用): 每块屏各播一段 GPU 转场动画, 从这块屏的图片 A 过渡到图片 B, 播完退出
#   transition.py 着色器.glsl 时长秒 区域1 A1.jpg B1.jpg [区域2 A2.jpg B2.jpg ...]     区域: WxH+X+Y (同 xrandr)
# 着色器用 gl-transitions 的格式 (config/transitions/, 只定义 vec4 transition(vec2 uv)); 图片按 feh --bg-fill 的方式铺满所在屏
# 窗口: 铺满整个桌面, 每块屏一个视口各画各的 (特效以各屏为中心); 映射前就设成 override-redirect (dwm 不接管, 不会闪一下),
#   放到最底层; 播完后 livewall.sh 再把新壁纸垫到它下面
# 和 livewall.sh 的握手 (每步等标准输入读到一行再继续):
#   1. 画好 A 并显示后打印 "ready", 收到回复开始播放
#   2. 播完停在最后一帧 B, 打印 "done"; livewall.sh 这时才把新壁纸 (和 B 一样) 设为不透明垫到下面, 回一行
#   3. 再等几帧让合成器画出下面的新壁纸, 然后退出
#   新壁纸要等转场播完才露出: 窗口刚映射时合成器还没画出它 (要等下一次合成), 播放前就垫好新壁纸会先露出新壁纸,
#   再跳回 A 播转场; 而播放开始时根窗口上已是定格的 A, 转场窗口晚几帧出现也看不出来
# 依赖: 系统的 libglfw / libGL / libX11 (ctypes 直接调用) + python-pillow, 不需要其他 Python 包
# 退出前在 stderr 打印帧数和最大帧间隔, 用来检查是否丢帧

import ctypes as C
import re
import sys
import time

from PIL import Image, ImageOps

glfw = C.CDLL("libglfw.so.3")
x11 = C.CDLL("libX11.so.6")

GLFW_VISIBLE, GLFW_DECORATED, GLFW_FOCUS_ON_SHOW = 0x20004, 0x20005, 0x2000C
GLFW_CONTEXT_VERSION_MAJOR, GLFW_CONTEXT_VERSION_MINOR = 0x22002, 0x22003
GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE = 0x22008, 0x32001
GLFW_X11_CLASS_NAME, GLFW_X11_INSTANCE_NAME = 0x24001, 0x24002
CW_OVERRIDE_REDIRECT = 1 << 9


class XSetWindowAttributes(C.Structure):
    _fields_ = [("background_pixmap", C.c_ulong), ("background_pixel", C.c_ulong),
                ("border_pixmap", C.c_ulong), ("border_pixel", C.c_ulong),
                ("bit_gravity", C.c_int), ("win_gravity", C.c_int), ("backing_store", C.c_int),
                ("backing_planes", C.c_ulong), ("backing_pixel", C.c_ulong), ("save_under", C.c_int),
                ("event_mask", C.c_long), ("do_not_propagate_mask", C.c_long),
                ("override_redirect", C.c_int), ("colormap", C.c_ulong), ("cursor", C.c_ulong)]


glfw.glfwCreateWindow.restype = C.c_void_p
glfw.glfwCreateWindow.argtypes = [C.c_int, C.c_int, C.c_char_p, C.c_void_p, C.c_void_p]
glfw.glfwGetProcAddress.restype = C.c_void_p
glfw.glfwGetProcAddress.argtypes = [C.c_char_p]
glfw.glfwGetX11Display.restype = C.c_void_p
glfw.glfwGetX11Window.restype = C.c_ulong
glfw.glfwGetX11Window.argtypes = [C.c_void_p]
glfw.glfwWindowHintString.argtypes = [C.c_int, C.c_char_p]
for f in ("glfwMakeContextCurrent", "glfwShowWindow", "glfwSwapBuffers", "glfwDestroyWindow"):
    getattr(glfw, f).argtypes = [C.c_void_p]
glfw.glfwSetWindowPos.argtypes = [C.c_void_p, C.c_int, C.c_int]
x11.XChangeWindowAttributes.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong, C.POINTER(XSetWindowAttributes)]
x11.XDefaultScreen.argtypes = [C.c_void_p]
x11.XDisplayWidth.argtypes = x11.XDisplayHeight.argtypes = [C.c_void_p, C.c_int]
for f in ("XLowerWindow", "XMapWindow"):
    getattr(x11, f).argtypes = [C.c_void_p, C.c_ulong]
x11.XMoveWindow.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int]
x11.XFlush.argtypes = [C.c_void_p]
x11.XInternAtom.restype = C.c_ulong
x11.XInternAtom.argtypes = [C.c_void_p, C.c_char_p, C.c_int]
x11.XChangeProperty.argtypes = [C.c_void_p, C.c_ulong, C.c_ulong, C.c_ulong, C.c_int, C.c_int, C.c_void_p, C.c_int]


def gl(name, restype, *argtypes):
    """取 OpenGL 函数 (要在建好 GL 上下文之后)"""
    return C.CFUNCTYPE(restype, *argtypes)(glfw.glfwGetProcAddress(name.encode()))


VERT = """#version 330 core
out vec2 uv;
void main() {                              // 一个盖住整个屏幕的大三角形, 不用顶点缓冲
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"""

FRAG = """#version 330 core
in vec2 uv;
out vec4 fragColor;
uniform sampler2D texFrom, texTo;
uniform float progress, ratio;
vec4 getFromColor(vec2 p) { return texture(texFrom, p); }
vec4 getToColor(vec2 p) { return texture(texTo, p); }
%s
void main() { fragColor = transition(uv); }
"""


def load_transition(path):
    """gl-transitions 着色器: 可调参数 "uniform 类型 名字; // = 默认值" 换成带默认值的常量"""
    src = open(path, encoding="utf-8").read()
    return re.sub(r"^\s*uniform\s+(\w+)\s+(\w+)\s*;\s*//\s*=\s*([^\n;]+?)\s*;?\s*$",
                  lambda m: f"const {m[1]} {m[2]} = {m[1]}({m[3]});", src, flags=re.M)


def ease(t):                               # easeInOutCubic: 先慢后快再慢
    return 4 * t * t * t if t < 0.5 else 1 - (-2 * t + 2) ** 3 / 2


def main():
    shader, duration, rest = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
    mons = []                              # 每块屏 (x, y, w, h, A, B)
    for g, a, b in zip(rest[0::3], rest[1::3], rest[2::3]):
        m = re.fullmatch(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", g)
        if not m:
            sys.exit("区域格式不对: " + g)
        w, h, x, y = map(int, m.groups())
        mons.append((x, y, w, h, a, b))
    if not mons:
        sys.exit("用法: transition.py 着色器.glsl 时长秒 WxH+X+Y A.jpg B.jpg [...]")

    if not glfw.glfwInit():
        sys.exit("glfwInit 失败")
    dpy = glfw.glfwGetX11Display()
    scr = x11.XDefaultScreen(dpy)
    w, h = x11.XDisplayWidth(dpy, scr), x11.XDisplayHeight(dpy, scr)   # 窗口铺满整个桌面
    for k, v in ((GLFW_VISIBLE, 0), (GLFW_DECORATED, 0), (GLFW_FOCUS_ON_SHOW, 0),
                 (GLFW_CONTEXT_VERSION_MAJOR, 3), (GLFW_CONTEXT_VERSION_MINOR, 3),
                 (GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE)):
        glfw.glfwWindowHint(k, v)
    glfw.glfwWindowHintString(GLFW_X11_CLASS_NAME, b"livewall-transition")    # picom 按这个 class 排除特效
    glfw.glfwWindowHintString(GLFW_X11_INSTANCE_NAME, b"livewall-transition")
    win = glfw.glfwCreateWindow(w, h, b"livewall-transition", None, None)
    if not win:
        sys.exit("创建窗口失败")
    glfw.glfwMakeContextCurrent(win)
    glfw.glfwSwapInterval(1)

    # 映射前设成 override-redirect: dwm 不接管
    xwin = glfw.glfwGetX11Window(win)
    attrs = XSetWindowAttributes(override_redirect=1)
    x11.XChangeWindowAttributes(dpy, xwin, CW_OVERRIDE_REDIRECT, C.byref(attrs))

    GLuint, GLint, GLenum, GLsizei, GLfloat = C.c_uint, C.c_int, C.c_uint, C.c_int, C.c_float
    glCreateShader = gl("glCreateShader", GLuint, GLenum)
    glShaderSource = gl("glShaderSource", None, GLuint, GLsizei, C.POINTER(C.c_char_p), C.c_void_p)
    glCompileShader = gl("glCompileShader", None, GLuint)
    glGetShaderiv = gl("glGetShaderiv", None, GLuint, GLenum, C.POINTER(GLint))
    glGetShaderInfoLog = gl("glGetShaderInfoLog", None, GLuint, GLsizei, C.c_void_p, C.c_char_p)
    glCreateProgram = gl("glCreateProgram", GLuint)
    glAttachShader = gl("glAttachShader", None, GLuint, GLuint)
    glLinkProgram = gl("glLinkProgram", None, GLuint)
    glGetProgramiv = gl("glGetProgramiv", None, GLuint, GLenum, C.POINTER(GLint))
    glGetProgramInfoLog = gl("glGetProgramInfoLog", None, GLuint, GLsizei, C.c_void_p, C.c_char_p)
    glUseProgram = gl("glUseProgram", None, GLuint)
    glGetUniformLocation = gl("glGetUniformLocation", GLint, GLuint, C.c_char_p)
    glUniform1i = gl("glUniform1i", None, GLint, GLint)
    glUniform1f = gl("glUniform1f", None, GLint, GLfloat)
    glGenTextures = gl("glGenTextures", None, GLsizei, C.POINTER(GLuint))
    glBindTexture = gl("glBindTexture", None, GLenum, GLuint)
    glActiveTexture = gl("glActiveTexture", None, GLenum)
    glTexParameteri = gl("glTexParameteri", None, GLenum, GLenum, GLint)
    glTexImage2D = gl("glTexImage2D", None, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, C.c_char_p)
    glGenVertexArrays = gl("glGenVertexArrays", None, GLsizei, C.POINTER(GLuint))
    glBindVertexArray = gl("glBindVertexArray", None, GLuint)
    glDrawArrays = gl("glDrawArrays", None, GLenum, GLint, GLsizei)
    glViewport = gl("glViewport", None, GLint, GLint, GLsizei, GLsizei)
    glClearColor = gl("glClearColor", None, GLfloat, GLfloat, GLfloat, GLfloat)
    glClear = gl("glClear", None, C.c_uint)

    def compile_shader(kind, src):
        s = glCreateShader(kind)
        p = C.c_char_p(src.encode())
        glShaderSource(s, 1, C.byref(p), None)
        glCompileShader(s)
        ok = GLint()
        glGetShaderiv(s, 0x8B81, C.byref(ok))  # GL_COMPILE_STATUS
        if not ok.value:
            log = C.create_string_buffer(4096)
            glGetShaderInfoLog(s, 4096, None, log)
            sys.exit("着色器编译失败: " + log.value.decode(errors="ignore"))
        return s

    prog = glCreateProgram()
    glAttachShader(prog, compile_shader(0x8B31, VERT))                       # GL_VERTEX_SHADER
    glAttachShader(prog, compile_shader(0x8B30, FRAG % load_transition(shader)))  # GL_FRAGMENT_SHADER
    glLinkProgram(prog)
    ok = GLint()
    glGetProgramiv(prog, 0x8B82, C.byref(ok))  # GL_LINK_STATUS
    if not ok.value:
        log = C.create_string_buffer(4096)
        glGetProgramInfoLog(prog, 4096, None, log)
        sys.exit("着色器链接失败: " + log.value.decode(errors="ignore"))
    glUseProgram(prog)

    # 每块屏两张图: 铺满裁剪到这块屏的大小 (同 feh --bg-fill), 上下翻转 (OpenGL 纹理原点在左下)
    tex = (GLuint * (2 * len(mons)))()
    glGenTextures(len(tex), tex)
    for i, (x, y, mw, mh, a, b) in enumerate(mons):
        for j, path in enumerate((a, b)):
            img = Image.open(path).convert("RGB")
            if img.size != (mw, mh):
                img = ImageOps.fit(img, (mw, mh), Image.LANCZOS)
            img = img.transpose(Image.FLIP_TOP_BOTTOM)
            glBindTexture(0x0DE1, tex[2 * i + j])  # GL_TEXTURE_2D
            for pname, val in ((0x2801, 0x2601), (0x2800, 0x2601),   # MIN / MAG_FILTER = LINEAR
                               (0x2802, 0x812F), (0x2803, 0x812F)):  # WRAP_S / T = CLAMP_TO_EDGE
                glTexParameteri(0x0DE1, pname, val)
            glTexImage2D(0x0DE1, 0, 0x8051, mw, mh, 0, 0x1907, 0x1401, img.tobytes())  # RGB8, RGB, UNSIGNED_BYTE
    glUniform1i(glGetUniformLocation(prog, b"texFrom"), 0)
    glUniform1i(glGetUniformLocation(prog, b"texTo"), 1)
    u_ratio = glGetUniformLocation(prog, b"ratio")
    u_progress = glGetUniformLocation(prog, b"progress")
    vao = GLuint()
    glGenVertexArrays(1, C.byref(vao))
    glBindVertexArray(vao)
    glClearColor(0.0, 0.0, 0.0, 1.0)

    def draw(p):
        glClear(0x4000)                        # GL_COLOR_BUFFER_BIT: 不属于任何屏的角落 (屏幕大小不一时) 涂黑
        glUniform1f(u_progress, p)
        for i, (x, y, mw, mh, _, _) in enumerate(mons):
            glViewport(x, h - y - mh, mw, mh)  # 视口原点在左下
            glUniform1f(u_ratio, mw / mh)
            for j in range(2):
                glActiveTexture(0x84C0 + j)    # GL_TEXTURE0 + j
                glBindTexture(0x0DE1, tex[2 * i + j])
            glDrawArrays(0x0004, 0, 3)         # GL_TRIANGLES
        glfw.glfwSwapBuffers(win)
        glfw.glfwPollEvents()

    # 先画好 A 再显示 (和根窗口上的定格帧一样, 看不出变化), 放到最底层, 通知 livewall.sh
    draw(0.0)
    glfw.glfwShowWindow(win)
    x11.XMoveWindow(dpy, xwin, 0, 0)
    x11.XLowerWindow(dpy, xwin)
    x11.XFlush(dpy)
    draw(0.0)
    print("ready", flush=True)
    sys.stdin.readline()

    start = last = time.monotonic()
    frames, worst = 0, 0.0
    while True:
        now = time.monotonic()
        t = min((now - start) / duration, 1.0)
        draw(ease(t))
        frames += 1
        worst = max(worst, now - last)
        last = now
        if t >= 1.0:
            break
    name = shader.rsplit("/", 1)[-1].removesuffix(".glsl")
    print(f"{name} frames={frames} max_interval={worst * 1000:.1f}ms", file=sys.stderr)
    print("done", flush=True)                  # 停在最后一帧 B, livewall.sh 把新壁纸垫到下面后回一行
    sys.stdin.readline()
    time.sleep(0.05)                           # 等合成器画出下面已经不透明的新壁纸
    # 关窗口前先设为全透明: picom 关闭窗口时的动画 (squeeze, 上下合拢) 不受 animation-exclude 管,
    # 透明的窗口播动画也看不见; 底下的新壁纸和最后一帧一样, 隐藏时看不出变化
    opacity = C.c_ulong(0)
    x11.XChangeProperty(dpy, xwin, x11.XInternAtom(dpy, b"_NET_WM_WINDOW_OPACITY", 0), 6, 32, 0,  # XA_CARDINAL, PropModeReplace
                        C.byref(opacity), 1)
    x11.XFlush(dpy)
    time.sleep(0.03)                           # 等 picom 画完隐藏后的一帧
    glfw.glfwDestroyWindow(win)
    glfw.glfwTerminate()


if __name__ == "__main__":
    main()
