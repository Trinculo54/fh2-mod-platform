# Checks the platform's own text (the table in src/lang.cpp):
# - that every English text the code passes to tr(), trf(), tr_w(), later() and loc_string() is in the table, so no
#   message quietly stays in English because its template drifted from the table's (one changed character is enough);
# - that every translation keeps the English text's placeholders ({0}, {1}...), each as many times, so no number or
#   name goes missing;
# - with the game's Fonts.zip, that every translation only uses characters the game's fonts for that language have,
#   so none of them shows as a box on the Mods page or the MODS tab.
#   python scripts/check_fonts.py ["<Mount>/media/ui/Fonts.zip"]
#
# The game picks fonts by language (fontsettings.xml in Fonts.zip): the Anthem fonts for Latin text, with Verdana
# behind them for Russian, and a Japanese, Korean or Chinese font that holds only the characters the game's own
# text uses. A .vfont has its glyph count at 0x80 and, from 0xC0, a 28-byte record per glyph starting with its code
# point (see ANNA Drone Mode's gamefont.cpp).
import re
import struct
import sys
import zipfile
from pathlib import Path

FALLBACK = {"JP": "DFGHSGothic-W5", "KO": "MalgunBdGm", "CHT": "DFYuanMedium-B5", "RU": "verdana"}

# The calls that translate their first argument. Their own definitions (lang.cpp, text.cpp) aren't calls.
CALL = re.compile(r"(?<![\w.:>])(tr|trf|tr_w|later|loc_string)\s*\(")
ENGINE = {"lang.cpp", "lang.hpp", "text.cpp", "text.hpp"}
# Text that reaches tr_w() another way: the title screen's lists add Back as their last choice (settings.cpp).
ALSO = ["Back"]


def code_points(data):
    count = struct.unpack_from("<I", data, 0x80)[0]
    return {struct.unpack_from("<I", data, 0xC0 + 28 * i)[0] for i in range(count)}


def c_string(literal):   # the table only escapes quotes and backslashes
    return re.sub(r'\\(["\\])', r"\1", literal)


def placeholders(text):
    return sorted(re.findall(r"\{[0-9]+\}", text))


# ---- the English text the code translates ----------------------------------------------------------------------

def skip_quoted(src, i):
    """The index just past the string or character literal that starts at src[i]."""
    j = i + 1
    while j < len(src) and src[j] != src[i] and src[j] != "\n":
        j += 2 if src[j] == "\\" else 1
    return j + 1


def without_comments(src):
    """The source with its comments blanked (lines kept), and its strings and character literals as they are."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
        elif src.startswith('R"', i) and not (i and (src[i - 1].isalnum() or src[i - 1] == "_")):
            m = re.match(r'R"([^(]*)\(', src[i:])
            end = src.find(")" + m.group(1) + '"', i) if m else -1
            j = n if end < 0 else end + len(m.group(1)) + 2
            out.append(src[i:j])
        elif c in "\"'":
            j = skip_quoted(src, i)
            out.append(src[i:j])
        else:
            j = i + 1
            out.append(c)
        i = j
    return "".join(out)


def first_argument(src, at):
    """The text of the first argument of the call whose '(' is at src[at]."""
    depth, i = 0, at + 1
    while i < len(src):
        c = src[i]
        if c in "\"'":
            i = skip_quoted(src, i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                break
            depth -= 1
        elif c == "," and depth == 0:
            break
        i += 1
    return src[at + 1:i]


LITERAL = re.compile(r'(?:L|u8)?"((?:[^"\\\n]|\\.)*)"')
GROUP = re.compile(r'(?:(?:L|u8)?"(?:[^"\\\n]|\\.)*"\s*)+')


def unescape(body):
    """A C string literal's text, with its escapes (\\xC5\\xBC, \\\\, \\") turned into what they stand for."""
    out, i = bytearray(), 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        e = body[i + 1]
        if e == "x":
            digits = re.match(r"[0-9A-Fa-f]+", body[i + 2:]).group(0)
            out.append(int(digits, 16) & 0xFF)
            i += 2 + len(digits)
        else:
            out += {"n": b"\n", "t": b"\t", "r": b"\r", "0": b"\0"}.get(e, e.encode("utf-8"))
            i += 2
    return out.decode("utf-8", "replace")


def literals(expression):
    """Each run of adjacent string literals in an expression, joined as the compiler joins them."""
    return ["".join(unescape(b) for b in LITERAL.findall(g.group(0))) for g in GROUP.finditer(expression)]


def initializers(src, name):
    """The initialisers of `name`'s definitions in one source file (char, wchar_t or GameTile constants)."""
    found = []
    for m in re.finditer(r"\b(?:char|wchar_t|GameTile)\s*\*?\s*(?:const\s+)?" + re.escape(name) +
                         r"\s*(?:\[[^\]]*\])?\s*=\s*", src):
        depth, i = 0, m.end()
        while i < len(src) and not (src[i] == ";" and depth == 0):
            if src[i] in "\"'":
                i = skip_quoted(src, i)
                continue
            if src[i] in "([{":
                depth += 1
            elif src[i] in ")]}":
                depth -= 1
            i += 1
        found.append(src[m.end():i])
    return found


def first_of_rows(init):
    """The first string in each { ... } row of a table's initialiser (a GameTile's name)."""
    body = init.strip()[1:-1]
    rows, depth, start = [], 0, 0
    for i, c in enumerate(body):
        if c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                rows.append(body[start:i + 1])
    return [found[0] for found in (literals(r) for r in rows) if found]


def code_texts(src_dir):
    """Every English text the code translates, as {text: [where...]}, and the calls whose text comes from elsewhere."""
    texts, elsewhere = {}, []
    for path in sorted(list(src_dir.glob("*.cpp")) + list(src_dir.glob("*.hpp"))):
        if path.name in ENGINE:
            continue
        src = without_comments(path.read_text(encoding="utf-8"))
        for m in CALL.finditer(src):
            arg = first_argument(src, m.end() - 1).strip()
            where = f"{path.name}:{src.count(chr(10), 0, m.start()) + 1}"
            found = literals(arg)
            if not found:
                # a constant: kRowLabel, kChoices[0], kHubTileNames[k], kTiles[t].name, a local such as `line`
                ident = re.match(r"([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*(\.\s*name)?$", arg)
                for init in initializers(src, ident.group(1)) if ident else []:
                    found += first_of_rows(init) if ident.group(3) else literals(init)
            if not found:
                elsewhere.append(f"{where} {m.group(1)}({arg})")
            for t in found:
                texts.setdefault(t, []).append(where)
    for t in ALSO:
        texts.setdefault(t, []).append("settings.cpp")
    return texts, elsewhere


def main():
    sys.stdout.reconfigure(encoding="utf-8")   # the Windows console's code page can't show every language
    if len(sys.argv) > 2:
        sys.exit("usage: check_fonts.py [<Fonts.zip>]")

    root = Path(__file__).resolve().parent.parent
    src = (root / "src" / "lang.cpp").read_text(encoding="utf-8")
    codes = re.findall(r'"([A-Z]+)"', re.search(r"kCodes\[\] = \{([^}]*)\}", src).group(1))
    table = src[src.index("const Text kTexts[] = {"):src.index("// clang-format on")]
    literal = r'"((?:[^"\\]|\\.)*)"'
    rows = re.findall(r"\{" + literal + r",\s*\{((?:\s*" + literal + r",?)+)\s*\}\}", table)
    keys = {c_string(row[0]) for row in rows}

    problems = 0
    texts, elsewhere = code_texts(root / "src")
    for text, where in sorted(texts.items()):
        if text not in keys:
            print(f"not in lang.cpp's table, so it stays in English: {text[:70]!r} ({', '.join(where)})")
            problems += 1
    print(f"{len(texts)} texts the code translates, looked up in the table ({len(elsewhere)} calls pass a mod's text "
          "or other text from elsewhere)")

    glyphs = None
    if len(sys.argv) == 2:
        fonts = zipfile.ZipFile(sys.argv[1])
        glyphs = {Path(n).stem: code_points(fonts.read(n)) for n in fonts.namelist() if n.endswith(".vfont")}
    for row in rows:
        english = c_string(row[0])
        translations = [c_string(t) for t in re.findall(literal, row[1])]
        if len(translations) != len(codes):
            print(f"{english[:50]!r}: {len(translations)} translations for {len(codes)} languages")
            problems += 1
            continue
        for code, text in [("EN", english)] + list(zip(codes, translations)):
            if placeholders(text) != placeholders(english):
                print(f"{code}: {' '.join(placeholders(text))} for {' '.join(placeholders(english))}, in {text[:60]!r}")
                problems += 1
            if glyphs is None:
                continue
            have = glyphs["Anthem_A"] | glyphs.get(FALLBACK.get(code, ""), set())
            missing = sorted({c for c in text if ord(c) not in have})
            if missing:
                print(f"{code}: {''.join(missing)} not in its font, in {text[:60]!r}")
                problems += 1
    fonts_note = "" if glyphs else " (fonts not checked: no Fonts.zip given)"
    print(f"{len(rows)} texts, {len(codes)} languages{fonts_note}: {problems} problem(s)")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
