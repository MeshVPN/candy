#!/bin/bash -e

# 递归收集 macOS 动态库的 Homebrew 依赖并内嵌进 App 的 Frameworks/ 目录，
# 把 /opt/homebrew（或 /usr/local）的绝对路径引用改写为 @rpath/<库名>，
# 使 .app 脱离 Homebrew 自包含分发。等价于 Linux/Windows 侧 search-deps.sh 的角色。
#
# 用法：bundle-macos-deps.sh <待处理的 dylib 或可执行文件> <Frameworks 目标目录>
#
# 依赖 App 主可执行文件带 @executable_path/../Frameworks 的 LC_RPATH
# （flutter 生成的 macOS Runner 默认已包含），故所有库统一放 Frameworks/ 即可解析。

TARGET="$1"
DEST="$2"

if [ -z "$DEST" ]; then
    echo "Usage: $0 <dylib-or-executable> <frameworks-dir>"
    exit 1
fi

# 只收集第三方（Homebrew）库；系统库(/usr/lib、/System)目标机自带，跳过。
BREW_PREFIX_RE='^/opt/homebrew/|^/usr/local/'

# 已处理过的库（按 basename 判重），避免重复拷贝与循环依赖导致的死递归。
declare -a processed

# 列出某个文件引用的全部 Homebrew 依赖（otool 首行是文件自身，跳过）。
list_brew_deps () {
    otool -L "$1" | awk 'NR>1 {print $1}' | grep -E "$BREW_PREFIX_RE" || true
}

# 把某文件对所有 Homebrew 依赖的引用改写为 @rpath/<basename>。
rewrite_refs () {
    local file="$1" dep
    for dep in $(list_brew_deps "$file"); do
        install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$file"
    done
}

# 递归收集：把依赖拷进 DEST、改其自身 id、再处理它的下游依赖，最后才改引用。
# 注意顺序——必须先递归发现下游（此时引用仍是原始绝对路径），再 rewrite_refs，
# 否则引用被改成 @rpath 后就无法从 otool 输出里发现下游依赖了。
collect () {
    local file="$1" dep base
    for dep in $(list_brew_deps "$file"); do
        base=$(basename "$dep")
        if [[ ! " ${processed[*]} " =~ " ${base} " ]]; then
            processed+=("$base")
            cp -n "$dep" "$DEST/$base"
            chmod u+w "$DEST/$base"
            install_name_tool -id "@rpath/$base" "$DEST/$base"
            collect "$DEST/$base"
            rewrite_refs "$DEST/$base"
        fi
    done
}

collect "$TARGET"
rewrite_refs "$TARGET"
echo "bundled macOS deps into: $DEST"
