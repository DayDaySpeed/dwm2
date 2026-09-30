#!/usr/bin/env python3
"""为没有封面的本地音乐补上专辑封面 (从网易云搜索)

用法: music_covers.py [音乐目录, 默认 ~/Music] [--dry-run]

- 只处理没有内嵌封面的文件 (mp3/flac/m4a 等 ffmpeg 支持的格式)
- 按标签中的 歌手 + 歌名 搜索, 只接受歌手与歌名都对得上的结果, 对不上则跳过 (不贴错封面)
- 写入前后校验音频数据的 MD5, 一致才替换原文件, 音频内容不会被改动
- 依赖: ffmpeg / ffprobe
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

EXTS = {'.mp3', '.flac', '.m4a', '.ogg', '.opus', '.wav', '.ape'}
HEADERS = {'Referer': 'https://music.163.com', 'User-Agent': 'Mozilla/5.0'}


def get_json(url, retries=3):
    for i in range(retries):
        try:
            req = urllib.request.Request(url, headers=HEADERS)
            with urllib.request.urlopen(req, timeout=15) as r:
                return json.load(r)
        except Exception:
            time.sleep(1 + i)
    return None


def download(url, path):
    req = urllib.request.Request(url, headers=HEADERS)
    with urllib.request.urlopen(req, timeout=30) as r, open(path, 'wb') as f:
        f.write(r.read())


def run(args):
    return subprocess.run(args, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=120)


def has_cover(path):
    out = run(['ffprobe', '-v', 'error', '-select_streams', 'v',
               '-show_entries', 'stream=codec_name', '-of', 'csv=p=0', path]).stdout
    return bool(out.strip())


def tags(path):
    out = run(['ffprobe', '-v', 'error', '-show_entries', 'format_tags=artist,title',
               '-of', 'json', path]).stdout
    t = {k.lower(): v for k, v in json.loads(out or '{}').get('format', {}).get('tags', {}).items()}
    artist, title = t.get('artist', ''), t.get('title', '')
    if not (artist and title):  # 没有标签时从文件名 "歌手 - 歌名" 推断
        m = re.match(r'(.+?)\s+-\s+(.+)', os.path.splitext(os.path.basename(path))[0])
        if m:
            artist, title = artist or m.group(1), title or m.group(2)
    return artist.strip(), title.strip()


def norm(s):
    s = s.lower()
    s = re.sub(r'[\(\（\[【].*?[\)\）\]】]', '', s)  # 去掉括号内的版本说明
    return re.sub(r'[\s\-_·・.,，。\'"!！?？&、/]', '', s)


def matches(want, got):
    a, b = norm(want), norm(got)
    return bool(a and b) and (a in b or b in a)


def find_cover(artist, title):
    q = urllib.parse.urlencode({'s': f'{artist} {title}', 'type': 1, 'limit': 10})
    data = get_json(f'https://music.163.com/api/search/get?{q}')
    songs = ((data or {}).get('result') or {}).get('songs') or []
    for s in songs:
        artists = [a.get('name', '') for a in s.get('artists', [])]
        if matches(title, s.get('name', '')) and any(matches(artist, a) or matches(a, artist) for a in artists) \
                or matches(title, s.get('name', '')) and any(norm(a) and norm(a) in norm(artist) for a in artists):
            detail = get_json('https://music.163.com/api/song/detail/?ids=' + urllib.parse.quote(f'[{s["id"]}]'))
            pic = (((detail or {}).get('songs') or [{}])[0].get('album') or {}).get('picUrl')
            if pic:
                return pic, f'{s["name"]} - {"/".join(artists)} 《{s["album"]["name"]}》'
    return None, None


def audio_md5(path):
    return run(['ffmpeg', '-nostdin', '-v', 'error', '-i', path, '-map', '0:a', '-c', 'copy', '-f', 'md5', '-']).stdout.strip()


def embed(path, cover):
    ext = os.path.splitext(path)[1]
    fd, out = tempfile.mkstemp(suffix=ext, dir=os.path.dirname(path))
    os.close(fd)
    args = ['ffmpeg', '-nostdin', '-v', 'error', '-y', '-i', path, '-i', cover, '-map', '0:a', '-map', '1',
            '-c', 'copy', '-disposition:v', 'attached_pic',
            '-metadata:s:v', 'title=Album cover', '-metadata:s:v', 'comment=Cover (front)']
    if ext == '.mp3':
        args += ['-id3v2_version', '3']
    try:
        if run(args + [out]).returncode != 0 or not has_cover(out):
            raise RuntimeError('ffmpeg 写入失败')
        if audio_md5(out) != audio_md5(path):
            raise RuntimeError('音频校验不一致, 已放弃')
        st = os.stat(path)
        os.replace(out, path)
        os.utime(path, (st.st_atime, st.st_mtime))  # 保留原修改时间
    finally:
        if os.path.exists(out):
            os.remove(out)


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith('--')]
    dry = '--dry-run' in sys.argv
    root = os.path.expanduser(argv[0] if argv else '~/Music')
    files = sorted(os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs
                   if os.path.splitext(f)[1].lower() in EXTS)
    todo = [f for f in files if not has_cover(f)]
    print(f'共 {len(files)} 首, 缺少封面 {len(todo)} 首{" (试运行, 不修改文件)" if dry else ""}\n')
    ok = skipped = 0
    for i, f in enumerate(todo, 1):
        name = os.path.relpath(f, root)
        artist, title = tags(f)
        pic, found = find_cover(artist, title) if title else (None, None)
        if not pic:
            skipped += 1
            print(f'[{i}/{len(todo)}] 跳过 (未找到匹配): {name}')
            continue
        try:
            if not dry:
                with tempfile.NamedTemporaryFile(suffix='.jpg') as c:
                    download(pic + '?param=500y500', c.name)
                    embed(f, c.name)
            ok += 1
            print(f'[{i}/{len(todo)}] 完成: {name}  ←  {found}')
        except Exception as e:
            skipped += 1
            print(f'[{i}/{len(todo)}] 失败 ({e}): {name}')
        time.sleep(0.3)
    print(f'\n补上封面 {ok} 首, 跳过 {skipped} 首')


if __name__ == '__main__':
    main()
