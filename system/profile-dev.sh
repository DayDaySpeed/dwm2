# /etc/profile.d/dev.sh (./setup.sh shared 安装): 多用户共享的开发工具, 装在 /opt/dev (dev 组可写)
# condabin 里只有 conda 一个命令, 不会让 conda 的 python 盖住系统的; conda activate 由 /etc/zsh/zshrc 提供
[ -d /opt/dev/miniconda3/condabin ] && append_path '/opt/dev/miniconda3/condabin'
# Android SDK 和 Flutter (各用户的 ~/Android/Sdk、~/development/flutter 可以是指向这里的软链接)
if [ -d /opt/dev/android-sdk ]; then
    export ANDROID_HOME=/opt/dev/android-sdk ANDROID_SDK_ROOT=/opt/dev/android-sdk
    append_path '/opt/dev/android-sdk/platform-tools'
    append_path '/opt/dev/android-sdk/emulator'
fi
[ -d /opt/dev/flutter/bin ] && append_path '/opt/dev/flutter/bin'
# Gradle 只读依赖缓存 (目录下是 modules-2): 先从这里找依赖, 找不到才下载到各自的 ~/.gradle
[ -d /opt/dev/gradle-ro-cache/modules-2 ] && export GRADLE_RO_DEP_CACHE=/opt/dev/gradle-ro-cache
# ollama 系统服务监听 11435 (11434 可能被 docker 里的 ollama 占着), ollama 命令默认连 11434, 这里改过来
export OLLAMA_HOST=127.0.0.1:11435
