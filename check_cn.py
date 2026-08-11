#!/usr/bin/env python3
"""FloatingDamageNG 汉化检查脚本
用法:
  python check_cn.py          # 全面检查
  python check_cn.py keys     # 仅检查 Key 覆盖
  python check_cn.py encoding # 仅检查文件编码
  python check_cn.py hardcode # 仅扫描遗漏硬编码
  python check_cn.py fix      # 自动修复可修复的问题
"""

import json, re, glob, os, sys

# 强制 UTF-8 输出（Windows GBK 终端兼容）
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# 自动检测：优先用脚本所在目录，否则用硬编码路径
_script_dir = os.path.dirname(os.path.abspath(__file__))
# 源码项目根目录（编译环境）
SRC_DIR = "E:/FloatingDamageNG-main1.50"
# JSON 翻译文件路径
JSON_PATH = os.path.join(SRC_DIR, "Skyrim", "Data", "SKSE", "Plugins", "FloatingDamageNG.json")
SRC_GLOB = os.path.join(SRC_DIR, "src", "*.cpp")
SRC_H_GLOB = os.path.join(SRC_DIR, "src", "*.h")

# 启动时验证路径
def _check_paths():
    errors = []
    if not os.path.isdir(SRC_DIR):
        errors.append(f"源码目录不存在: {SRC_DIR}")
    if not os.path.isfile(JSON_PATH):
        errors.append(f"翻译文件不存在: {JSON_PATH}")
    if errors:
        print("[ERR] 路径错误，请确认以下文件/目录存在:")
        for e in errors:
            print(f"  {e}")
        return False
    return True

OK = "[OK]"
WARN = "[WARN]"
ERR = "[ERR]"
INFO = "[INFO]"


def flatten_json(j):
    """展平嵌套 JSON 为 a.b.c 格式"""
    def _flatten(d, prefix=""):
        r = {}
        for k, v in d.items():
            nk = f"{prefix}.{k}" if prefix else k
            if isinstance(v, dict):
                r.update(_flatten(v, nk))
            elif isinstance(v, str):
                r[nk] = v
        return r
    return _flatten(j)


def load_json():
    with open(JSON_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


def extract_i18n_keys():
    """从源码提取所有 I18n::Get("key") 字符串字面量"""
    keys = set()
    for fpath in glob.glob(SRC_GLOB) + glob.glob(SRC_H_GLOB):
        with open(fpath, "r", encoding="utf-8") as f:
            content = f.read()
        for m in re.finditer(r'I18n::Get\s*\(\s*"([^"]+)"', content):
            keys.add(m.group(1))
    return keys


def check_keys():
    """检查 I18n Key 覆盖率"""
    print("=" * 50)
    print("  1. I18n Key 覆盖率检查")
    print("=" * 50)

    j = load_json()
    flat = flatten_json(j)
    src_keys = extract_i18n_keys()

    missing = sorted(k for k in src_keys if k not in flat)
    extra = sorted(k for k in flat if k not in src_keys)

    print(f"  源码 I18n Key: {len(src_keys)}")
    print(f"  JSON 翻译条目: {len(flat)}")

    ok = True
    if missing:
        print(f"\n  {ERR} 缺失翻译 ({len(missing)}):")
        for k in missing:
            fb = ""
            for fpath in glob.glob(SRC_GLOB):
                with open(fpath, "r", encoding="utf-8") as f:
                    mm = re.search(rf'I18n::Get\s*\(\s*"{re.escape(k)}"\s*,\s*"([^"]*)"', f.read())
                    if mm:
                        fb = mm.group(1)
                        break
            print(f"     {k}  ->  \"{fb}\"")
        ok = False
    else:
        print(f"\n  {OK} 所有 Key 均已翻译")

    if extra:
        real_extra = []
        for k in extra:
            leaf = k.split(".")[-1]
            found = False
            for fpath in glob.glob(SRC_GLOB):
                with open(fpath, "r", encoding="utf-8") as f:
                    if leaf in f.read():
                        found = True
                        break
            if not found:
                real_extra.append(k)

        if real_extra:
            print(f"\n  {WARN} JSON 中多余 Key ({len(real_extra)})，源码中未引用:")
            for k in real_extra:
                print(f"     {k}")
        else:
            extra_dynamic = len(extra) - len(real_extra)
            if extra_dynamic > 0:
                print(f"\n  {INFO} JSON 中 {extra_dynamic} 个 Key 通过 kColorTable/kPerKindMeta 动态引用（正常）")

    return ok


def check_encoding():
    """检查源码文件编码"""
    print("\n" + "=" * 50)
    print("  2. 文件编码检查 (UTF-8 BOM)")
    print("=" * 50)

    issues = []
    ok_count = 0
    for fpath in sorted(glob.glob(SRC_GLOB) + glob.glob(SRC_H_GLOB)):
        fname = os.path.basename(fpath)
        with open(fpath, "rb") as f:
            raw = f.read()
        has_bom = raw[:3] == b"\xef\xbb\xbf"
        try:
            text = raw.decode("utf-8")
            cjk = sum(1 for c in text if "一" <= c <= "鿿")
        except UnicodeDecodeError:
            issues.append(f"  {ERR} {fname}: 不是 UTF-8 编码!")
            continue

        if cjk > 0 and not has_bom:
            issues.append(f"  {WARN} {fname}: 含 {cjk} 个中文字符，无 BOM（编译 C4819 警告）")
        elif cjk > 0 and has_bom:
            ok_count += 1

    print(f"  {OK} 编码正确: {ok_count} 个文件")
    if issues:
        print(f"\n  发现 {len(issues)} 个问题:")
        for i in issues:
            print(i)
        return False
    else:
        print(f"  {OK} 所有文件编码正确")
        return True


def check_hardcoded():
    """扫描可能遗漏的硬编码英文字符串"""
    print("\n" + "=" * 50)
    print("  3. 遗漏硬编码英文扫描（仅供参考）")
    print("=" * 50)

    exclude_patterns = [
        r'\\Fonts\\',
        r's(Motion|Font|Pattern|Label)\d*',
        r'##fdng_',
        r'=== Session',
        r'Drill-down',
        r'FloatingDamageNG\.(log|jsonl|csv)',
        r'std::format\(\"s[A-Z]',
        r'#include\s+"',        # include 头文件
        r'logger::',            # 日志输出
        r'\.ini"',              # INI 文件路径
        r'\.json"',             # JSON 文件路径
    ]

    found_count = 0
    for fpath in sorted(glob.glob(SRC_GLOB)):
        with open(fpath, "r", encoding="utf-8") as f:
            lines = f.readlines()

        for lineno, line in enumerate(lines, 1):
            if line.strip().startswith("//") or line.strip().startswith("/*"):
                continue
            if "I18n::Get" in line:
                continue
            if "#include" in line:
                continue
            if "logger::" in line:
                continue

            for m in re.finditer(r'"([A-Z][a-z]{2,}[^"]{3,})"', line):
                text = m.group(1)
                if text.startswith("##") or text.startswith("0x"):
                    continue
                if any(re.search(pat, line) for pat in exclude_patterns):
                    continue
                print(f"  {os.path.basename(fpath)}:{lineno}: \"{text}\"")
                found_count += 1

    if found_count == 0:
        print(f"  {OK} 未发现遗漏的 UI 硬编码英文")
    else:
        print(f"\n  {INFO} 共 {found_count} 处，可能为日志/路径/ID 等非 UI 文本")

    return found_count == 0


def fix_encoding():
    """自动为含中文但无 BOM 的文件添加 BOM"""
    print("=" * 50)
    print("  自动修复: 添加 UTF-8 BOM")
    print("=" * 50)

    fixed = 0
    for fpath in sorted(glob.glob(SRC_GLOB) + glob.glob(SRC_H_GLOB)):
        fname = os.path.basename(fpath)
        with open(fpath, "rb") as f:
            raw = f.read()
        if raw[:3] == b"\xef\xbb\xbf":
            continue
        try:
            text = raw.decode("utf-8")
            cjk = sum(1 for c in text if "一" <= c <= "鿿")
            if cjk > 0:
                with open(fpath, "wb") as f:
                    f.write(b"\xef\xbb\xbf" + raw)
                print(f"  {OK} {fname}: 已添加 BOM ({cjk} 个中文字符)")
                fixed += 1
        except Exception as e:
            print(f"  {ERR} {fname}: 失败 - {e}")

    if fixed == 0:
        print("  无需修复")
    return True


def main():
    if not _check_paths():
        input("\n按回车键关闭...")
        return

    cmd = sys.argv[1] if len(sys.argv) > 1 else "all"

    if cmd == "fix":
        fix_encoding()
        return

    all_ok = True

    if cmd in ("all", "keys"):
        if not check_keys():
            all_ok = False

    if cmd in ("all", "encoding"):
        if not check_encoding():
            all_ok = False

    if cmd in ("all", "hardcode"):
        check_hardcoded()

    print("\n" + "=" * 50)
    if all_ok:
        print(f"  {OK} 汉化检查全部通过！可以编译发布。")
    else:
        print(f"  {ERR} 发现问题，请修复后再编译。")
    print("=" * 50)
    input("\n按回车键关闭...")
    print("=" * 50)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"\n[ERR] 脚本运行出错: {e}")
        import traceback
        traceback.print_exc()
        input("\n按回车键关闭...")
