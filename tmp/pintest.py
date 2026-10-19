# pintest.py — 校验 kk.c 内置拼音首字母表的正确性（抽查常用应用词）
import re, sys

src = open(r"D:\vs2022cs\cpro\claunch\kk.c", encoding="utf-8").read()
m = re.search(r"PINYIN_GROUPS\[26\] = \{(.*?)\};", src, re.S)
lines = re.findall(r'L"([^"]*)"', m.group(1))
assert len(lines) == 26, f"分组行数 {len(lines)} != 26"

gmap = {}
for l, grp in enumerate(lines):
    for ch in grp:
        c = ord(ch)
        if 0x4E00 <= c <= 0x9FA5 and ch not in gmap:
            gmap[ch] = chr(ord('a') + l)

def initials(name):
    out = []
    for ch in name:
        c = ch.lower()
        if 'a' <= c <= 'z' or '0' <= c <= '9':
            out.append(c)
        elif 0x4E00 <= ord(ch) <= 0x9FA5 and ch in gmap:
            out.append(gmap[ch])
    return ''.join(out)

tests = {
    "记事本": "jsb", "惠普管家": "hpgj", "微信": "wx", "腾讯会议": "txhy",
    "网易云音乐": "wyyyy", "浏览器": "llq", "计算器": "jsq", "播放器": "bfq",
    "压缩软件": "ysrj", "钉钉": "dd", "网易邮箱": "wyyx", "中文字幕": "zwzm",
    "记事本++": "jsb", "惠普": "hp", "虚拟机": "xnj", "远程控制": "yckz",
}
bad = 0
for name, want in tests.items():
    got = initials(name)
    flag = "OK " if got == want else "FAIL"
    if got != want: bad += 1
    print(f"{flag} {name}: got={got} want={want}")
print(f"表内汉字数: {len(gmap)}")
sys.exit(1 if bad else 0)
