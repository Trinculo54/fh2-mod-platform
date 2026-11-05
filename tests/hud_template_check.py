"""The HUD layer preview's page check, offline and read only: the A_Hud.xaml the platform wrote with a HUD
layer (its copy is fh2-mods\\platform-hudlayer-A_Hud.xaml), against the props table it wrote beside it
(fh2-mods\\platform-hudlayer-props.txt) and the game's UI.zip. tests/run_hud_layer_test.sh runs it on the
pages tests/hud_layer_test.cpp writes.

    python -I hud_template_check.py <A_Hud.xaml> <props.txt> <UI.zip> [--parts BAR,CHIPS,...]
                                    [--textures <dir>] [--notice]

--parts: the parts the layers asked for; without it they're read from the props table. --textures: a
folder with copies of the game's ui\\textures\\Anthem.zip and data_bound\\<name>.zip; without it check 7
is skipped. --notice: the page also carries the platform's notice about mods that didn't load.

Checks:
  1. the page parses as XML (so its tags balance);
  2. every {StaticResource K} and {DynamicResource K} the layer uses is a key the page can use: a child
     of AnthemApp.xaml's own ResourceDictionary, of one of its merged dictionaries' root, or of the page's
     Page.Resources ResourceDictionary (never a key nested in a Style's or a template's own resources);
  3. each FH2HL_ key is defined once, as a child of Page.Resources' ResourceDictionary;
  4. no {StaticResource FH2HL_x} before FH2HL_x's definition has ended;
  5. a Style's BasedOn names a Style of the same target type;
  7. the texture of every ImageSource key the layer uses (the controller pictures) is in the texture zips;
  8. bindings, from real attribute values only: each FH2Mod_Hud_ name is a property of the props table,
     bound where its type goes (Angle, EndAngle, X, ScaleX: float; Text: String; Visibility through the
     boolean converter and True/False triggers: bool; numeric triggers: int, on one of its values); every
     int value has its trigger; every property is bound;
  9. no literal Text= in the layer but the platform's "+", "<" and ">";
 10. FH2HL_Root is the last child of the Grid inside LayoutRoot; the layer's resources are in
     Page.Resources' dictionary;
 11. FH2HL_Root's Style is exactly Visibility Collapsed plus one trigger, FH2Mod_Hud_Shown True -> Visible,
     and the root has no Visibility of its own;
 12. the layer block holds exactly one top element, the FH2HL_Root Grid;
 13. the page is UI.zip's Scenes/Anthem/A_Hud.xaml plus only the resources block, the layer block and, with
     a ring, the xmlns:ed line (and with --notice the platform's notice, where it puts it);
 14. ed:Arc exactly when BAR or COUNTDOWN is a part, with xmlns:ed; no ControllerButtonControl; and
     ExitActions only to stop the countdown pop's or the shutter's named storyboard, which the page has;
 15. every Style goes on an element of its target type: a Style="{StaticResource K}" names a Style whose
     TargetType is the element's own type, and an inline Style sits in an <X.Style> inside an X and
     targets X; the layer's keyed Styles are the only others;
 16. at most 640 bindings, counted as the writer counts them (every {Binding FH2Mod_Hud_...} attribute);
 17. Effect="{x:Null}" only on the selected tabs' dark letters;
 18. ClipToBounds="True" on exactly the bar's middle Grid and the tip's inner Grid.
Exit 0 when every check that ran passes."""
import argparse
import difflib
import os
import re
import sys
import zipfile
import xml.etree.ElementTree as ET

NS_P = "http://schemas.microsoft.com/winfx/2006/xaml/presentation"
NS_X = "http://schemas.microsoft.com/winfx/2006/xaml"
NS_L = "clr-namespace:ForzaUI;assembly="
NS_ED = "http://schemas.microsoft.com/expression/2010/drawing"
ED_NS = 'xmlns:ed="http://schemas.microsoft.com/expression/2010/drawing"'
MARKS = [("<!-- FH2 Mod Platform: HUD layer resources -->",
          "<!-- FH2 Mod Platform: end of the HUD layer resources -->"),
         ("<!-- FH2 Mod Platform: HUD layer -->",
          "<!-- FH2 Mod Platform: end of the HUD layer -->")]
FLOAT_ATTRS = {"ScaleX", "ScaleY", "X", "Y", "EndAngle", "Angle"}
KINDS = {"bool": "b", "int": "i", "float": "f", "String": "s"}
WORD = {"b": "bool", "i": "int", "f": "float", "s": "String", "?": "?"}
PART_PROP = {"BAR": "Bar", "CHIPS": "Chips", "TIP": "Tip", "PANEL": "Panel", "COUNTDOWN": "Count"}
q = lambda ns, tag: "{%s}%s" % (ns, tag)
XKEY, XNAME = q(NS_X, "Key"), q(NS_X, "Name")
PREFIX = {NS_P: "", NS_X: "x:", NS_L: "local:", NS_ED: "ed:"}

ap = argparse.ArgumentParser()
ap.add_argument("page")
ap.add_argument("props")
ap.add_argument("uizip")
ap.add_argument("--parts")
ap.add_argument("--textures")
ap.add_argument("--notice", action="store_true", help="the page also carries the platform's notice about mods that didn't load")
args = ap.parse_args()

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print("%s %s%s" % ("ok  " if ok else "FAIL", name, (": " + detail) if detail and not ok else ""))


def short(tag):
    if tag.startswith("{"):
        ns, local = tag[1:].split("}", 1)
        return PREFIX.get(ns, "?:") + local
    return tag


def type_name(t):
    t = (t or "").strip()
    m = re.match(r"^\{x:Type\s+([^}]+)\}$", t)
    if m:
        t = m.group(1).strip()
    return t.split(":")[-1]


raw = open(args.page, "rb").read()
page = raw.decode("utf-8")
z = zipfile.ZipFile(args.uizip)
names = {n.replace("\\", "/").lower(): n for n in z.namelist()}


def ui(name):
    return z.read(names[name.lower()]).decode("utf-8-sig", "replace")


prop_text = open(args.props, encoding="utf-8").read()
if args.parts:
    parts = [p.strip().upper() for p in args.parts.split(",") if p.strip()]
else:
    parts = [p for p, n in PART_PROP.items() if ("FH2Mod_Hud_%s\t" % n) in prop_text]
    print("(no --parts: the props table's parts are taken as asked for)")
ring_wanted = "BAR" in parts or "COUNTDOWN" in parts
print("page %s: %d bytes, %d lines, parts %s" % (args.page, len(raw), page.count("\n") + 1, ", ".join(parts) or "none"))

# ---- 1. XML
try:
    root = ET.fromstring(raw)
    check("1. the page parses as XML (tags balance)", True)
except ET.ParseError as e:
    check("1. the page parses as XML (tags balance)", False, str(e))
    print("RESULT: %d checks, %d failed" % (len(results), results.count(False)))
    sys.exit(1)


# ---- the keys pages can use
class Key:
    def __init__(self, el, where):
        self.element = short(el.tag)
        self.target = type_name(el.get("TargetType"))
        self.text = (el.text or "").strip()
        self.uri = el.get("UriSource") or ""
        self.where = where


known = {}
app_root = ET.fromstring(ui("AnthemApp.xaml").encode("utf-8"))
app_rd = app_root.find(q(NS_P, "Application.Resources") + "/" + q(NS_P, "ResourceDictionary"))
for el in (list(app_rd) if app_rd is not None else []):
    if el.get(XKEY):
        known[el.get(XKEY)] = Key(el, "AnthemApp.xaml")
dicts = re.findall(r'Source="(Resources/Anthem/[^"]+\.xaml)"', ui("AnthemApp.xaml"))
for d in dicts:
    rd = ET.fromstring(ui(d).encode("utf-8"))
    for el in list(rd):
        if el.get(XKEY):
            known.setdefault(el.get(XKEY), Key(el, d))
res = root.find(q(NS_P, "Page.Resources"))
page_rd = res.find(q(NS_P, "ResourceDictionary")) if res is not None else None
own = {el.get(XKEY): Key(el, "the page") for el in (list(page_rd) if page_rd is not None else []) if el.get(XKEY)}

# the layer's elements: its resources (the FH2HL_ children of the page's dictionary) and the layer's root
layer_res = [el for el in (list(page_rd) if page_rd is not None else []) if (el.get(XKEY) or "").startswith("FH2HL_")]
zp = [e for e in root.iter(q(NS_L, "ZPanel")) if e.get(XNAME) == "LayoutRoot"]
grid = zp[0].find(q(NS_P, "Grid")) if zp else None
roots = [e for e in grid if e.get(XNAME) == "FH2HL_Root"] if grid is not None else []
layer_els = [e for top in layer_res + roots for e in top.iter()]


def resource_uses(v):
    return re.findall(r"\{(Static|Dynamic)Resource ([^},\s]+)", v)


# ---- 2. every key the layer uses is one the page can use (the game's own elements are as UI.zip has them)
used = {}
for el in layer_els:
    for a, v in el.attrib.items():
        for kind, k in resource_uses(v):
            used.setdefault(k, kind)
missing = sorted(k for k in used if k not in known and k not in own)
check("2. every resource key is one the page can use (%d used, %d dictionaries, %d application keys, %d the page's own)"
      % (len(used), len(dicts), len(known), len(own)), used and not missing, ", ".join(missing))

# ---- 3. each FH2HL_ key once, at the page dictionary's level
counts = {}
for m in re.finditer(r'x:Key="(FH2HL_[^"]+)"', page):
    counts[m.group(1)] = counts.get(m.group(1), 0) + 1
nested = sorted(k for k in counts if k not in own)
twice = sorted(k for k, n in counts.items() if n != 1)
check("3. each of the layer's %d FH2HL_ keys is defined once, in Page.Resources' dictionary" % len(counts),
      counts and not nested and not twice, "twice: %s; nested: %s" % (", ".join(twice), ", ".join(nested)))


# ---- 4. no forward StaticResource to the layer's own keys (text offsets, comments taken out)
def blank_comments(t):
    return re.sub(r"<!--.*?-->", lambda m: " " * len(m.group(0)), t, flags=re.S)


plain = blank_comments(page)


def element_end(t, start):
    """One past the end of the element whose start tag begins at `start` (comments already blanked)."""
    depth = 0
    for m in re.finditer(r"<(/?)([A-Za-z_][\w:.]*)([^>]*?)(/?)>", t[start:]):
        if m.group(1):
            depth -= 1
        elif not m.group(4):
            depth += 1
        if depth == 0:
            return start + m.end()
    return len(t)


def_end = {}
for m in re.finditer(r'<[\w:.]+[^>]*?x:Key="(FH2HL_[^"]+)"', plain):
    def_end[m.group(1)] = element_end(plain, m.start())
forward = []
for m in re.finditer(r"\{StaticResource (FH2HL_[^},\s]+)", plain):
    k = m.group(1)
    if k in def_end and m.start() < def_end[k]:
        forward.append("%s at offset %d (defined until %d)" % (k, m.start(), def_end[k]))
check("4. no {StaticResource FH2HL_...} before its definition has ended", not forward, "; ".join(forward[:5]))

# ---- 5. BasedOn: a Style of the same target type
bad_base = []
for el in root.iter(q(NS_P, "Style")):
    b = el.get("BasedOn")
    if not b:
        continue
    m = re.match(r"^\{StaticResource ([^}]+)\}$", b)
    if not m:
        bad_base.append("BasedOn %r isn't a StaticResource" % b)
        continue
    base = own.get(m.group(1)) or known.get(m.group(1))
    mine = type_name(el.get("TargetType"))
    if base is None:
        continue   # (check 2)
    if base.element != "Style":
        bad_base.append("%s is a %s, not a Style" % (m.group(1), base.element))
    elif mine and base.target and mine != base.target:
        bad_base.append("a Style for %s is BasedOn %s, a Style for %s" % (mine, m.group(1), base.target))
check("5. every BasedOn names a Style of the same target type", not bad_base, "; ".join(bad_base[:5]))

# ---- 7. textures: every ImageSource key the layer uses (the controller pictures), in the texture zips
tex_paths = {}
for el in layer_els:
    for a, v in el.attrib.items():
        for kind, k in resource_uses(v):
            kd = own.get(k) or known.get(k)
            if kd and kd.element in ("ImageSource", "BitmapImage"):
                p = kd.text or kd.uri
                m = re.search(r"Textures[\\/][^\"<>]+", p)
                tex_paths[k] = m.group(0) if m else "(key %s: %r)" % (k, p)
if not args.textures:
    print("skip 7. the textures of the %d pictures the layer uses (no --textures folder given)" % len(tex_paths))
else:
    tex = {}

    def add_zip(path, prefix):
        for n in zipfile.ZipFile(path).namelist():
            tex.setdefault((prefix + n.replace("\\", "/")).rsplit(".", 1)[0].lower(), n)

    zips_seen = []
    if os.path.exists(os.path.join(args.textures, "Anthem.zip")):
        add_zip(os.path.join(args.textures, "Anthem.zip"), "anthem/")
        zips_seen.append("Anthem.zip")
    db = os.path.join(args.textures, "data_bound")
    for f in sorted(os.listdir(db)) if os.path.isdir(db) else []:
        if f.lower().endswith(".zip"):
            add_zip(os.path.join(db, f), "data_bound/" + f[:-4] + "/")
            zips_seen.append("data_bound/" + f)
    absent = []
    for k, p in sorted(tex_paths.items()):
        t = p.replace("\\", "/")
        t = t[len("Textures/"):] if t.lower().startswith("textures/") else t
        if t.rsplit(".", 1)[0].lower() not in tex:
            absent.append("%s (%s)" % (k, p))
    # the 24 glyphs' keys (the tabs' and chords' LB and RB are glyphs 5 and 6, the same keys)
    want_pictures = 24 if "BAR" in parts or "TIP" in parts else 0
    check("7. the textures of the %d pictures the layer uses are in the texture zips (%s)" % (len(tex_paths), ", ".join(zips_seen) or "none found"),
          zips_seen and not absent and len(tex_paths) == want_pictures,
          ", ".join(absent[:5]) if absent else "%d pictures, %d expected for these parts" % (len(tex_paths), want_pictures))

# ---- 8. bindings
props = {}
for line in open(args.props, encoding="utf-8"):
    f = line.rstrip("\r\n").split("\t")
    if f and f[0].startswith("FH2Mod_Hud_"):
        vals = [int(x) for x in f[3].split(",") if x] if len(f) > 3 else []
        props[f[0][len("FH2Mod_Hud_"):]] = (KINDS.get(f[1], "?"), vals)
bound, problems, trig_vals = set(), [], {}
n_bindings = n_triggers = 0
for el in layer_els:
    for a, v in el.attrib.items():
        if "FH2Mod_Hud_" not in v:
            continue
        m = re.match(r"^\{Binding FH2Mod_Hud_([A-Za-z0-9_]+)([},])", v)
        if not m:
            problems.append("%s=%r names a layer property without binding it plainly" % (a, v))
            continue
        n = m.group(1)
        if n not in props:
            problems.append("FH2Mod_Hud_%s isn't one of the layer's properties" % n)
            continue
        tag = short(el.tag)
        if a == "Visibility":
            kind = "b" if re.search(r", Converter=\{StaticResource A_FZBooleanToVisibilityConverter(_Inverse)?\}\}$", v) else None
        elif a == "Binding" and tag == "DataTrigger":
            val = el.get("Value")
            kind = "b" if val in ("True", "False") else "i" if (val or "").isdigit() else None
        elif "Converter=" in v:
            kind = None
        elif a == "Text":
            kind = "s"
        elif a in FLOAT_ATTRS:
            kind = "f"
        else:
            kind = None
        if kind is None:
            problems.append("FH2Mod_Hud_%s is bound through %s on %s in a way the check doesn't know (%r)" % (n, a, tag, v))
            continue
        if kind != props[n][0]:
            problems.append("FH2Mod_Hud_%s is a %s, bound where a %s goes (%s on %s)" % (n, WORD[props[n][0]], WORD[kind], a, tag))
            continue
        if a == "Binding":
            n_triggers += 1
            if kind == "i":
                val = int(el.get("Value"))
                if val not in props[n][1]:
                    problems.append("a trigger shows FH2Mod_Hud_%s = %d, which isn't one of its values" % (n, val))
                trig_vals.setdefault(n, set()).add(val)
        bound.add(n)
        n_bindings += 1
# every binding in the page is in the layer's elements (none elsewhere)
outside = len(re.findall(r"\{Binding FH2Mod_Hud_", plain)) - n_bindings
for n, (kind, vals) in props.items():
    if n not in bound:
        problems.append("FH2Mod_Hud_%s isn't bound anywhere" % n)
        continue
    for v in vals:
        if v not in trig_vals.get(n, set()):
            problems.append("no trigger shows FH2Mod_Hud_%s = %d, one of its values" % (n, v))
if outside:
    problems.append("%d binding(s) to layer properties outside the layer's elements" % outside)
check("8. bindings: names, kinds, trigger values and coverage (%d properties in the table, %d bound, %d bindings, %d DataTriggers)"
      % (len(props), len(bound), n_bindings, n_triggers), props and not problems, "; ".join(problems[:6]))

# ---- 9. no literal Text but the platform's +, < and >
literal = ["%s Text=%r" % (short(e.tag), e.get("Text")) for e in layer_els
           if e.get("Text") is not None and not e.get("Text").startswith("{Binding FH2Mod_Hud_") and e.get("Text") not in ("+", "<", ">")]
check("9. no literal Text= in the layer's elements but the platform's +, < and >", not literal, "; ".join(literal[:5]))

# ---- 10. placement
check("10. FH2HL_Root is the last child of the Grid inside LayoutRoot; the %d resources are in Page.Resources' dictionary"
      % len(layer_res), len(roots) == 1 and list(grid)[-1] is roots[0] and layer_res and len(layer_res) == len(counts))

# ---- 11. the root collapsed unless the layer is on
ok11, why11 = False, "no FH2HL_Root style"
rst = [el for el in layer_res if el.get(XKEY) == "FH2HL_Root"]
if rst:
    s = rst[0]
    kids = list(s)
    ok11 = (short(s.tag) == "Style" and type_name(s.get("TargetType")) == "Grid" and not s.get("BasedOn") and len(kids) == 2
            and short(kids[0].tag) == "Setter" and kids[0].get("Property") == "Visibility" and kids[0].get("Value") == "Collapsed"
            and short(kids[1].tag) == "Style.Triggers" and len(kids[1]) == 1)
    if ok11:
        t = kids[1][0]
        ok11 = (short(t.tag) == "DataTrigger" and t.get("Binding") == "{Binding FH2Mod_Hud_Shown}" and t.get("Value") == "True"
                and len(t) == 1 and short(t[0].tag) == "Setter" and t[0].get("Property") == "Visibility"
                and t[0].get("Value") == "Visible" and len(t[0]) == 0)
    why11 = "the style isn't Setter Visibility Collapsed plus the one trigger FH2Mod_Hud_Shown True -> Visible"
if ok11 and roots:
    ok11 = roots[0].get("Style") == "{StaticResource FH2HL_Root}" and roots[0].get("Visibility") is None
    why11 = "the root's Style isn't FH2HL_Root, or it has a Visibility of its own"
check("11. FH2HL_Root is collapsed unless FH2Mod_Hud_Shown is true", ok11, why11)

# ---- 12. the layer block: one top element, the root
ok12, why12 = False, "the layer's markers aren't there once each"
a, b = MARKS[1]
if page.count(a) == 1 and page.count(b) == 1 and page.find(a) < page.find(b):
    inner = page[page.find(a) + len(a):page.find(b)]
    wrapped = '<W xmlns="%s" xmlns:x="%s" xmlns:local="%s" xmlns:ed="%s">%s</W>' % (NS_P, NS_X, NS_L, NS_ED, inner)
    try:
        w = ET.fromstring(wrapped.encode("utf-8"))
        tops = list(w)
        ok12 = len(tops) == 1 and short(tops[0].tag) == "Grid" and tops[0].get(XNAME) == "FH2HL_Root" and not (w.text or "").strip() \
            and not (tops[0].tail or "").strip()
        why12 = "%d top element(s): %s" % (len(tops), ", ".join("%s %s" % (short(t.tag), t.get(XNAME) or "") for t in tops[:4]))
    except ET.ParseError as e:
        why12 = "the layer block doesn't parse on its own: %s" % e
check("12. the layer block holds exactly one top element, FH2HL_Root", ok12, why12)

# ---- 13. the diff against the game's page
orig = ui("Scenes/Anthem/A_Hud.xaml")
rest = page
ok_marks = True
for a, b in MARKS:
    i, j = rest.find(a), rest.find(b)
    if i < 0 or j < i or rest.count(a) != 1 or rest.count(b) != 1:
        ok_marks = False
        continue
    start = rest.rfind("\n", 0, i) + 1
    end = rest.find("\n", j) + 1
    rest = rest[:start] + rest[end:]
if ED_NS in rest:
    i = rest.find(ED_NS)
    rest = rest[:rest.rfind("\n", 0, i) + 1] + rest[rest.find("\n", i) + 1:]
if args.notice:
    # write_hud_notice's two insertions, as it makes them: its style just before </ResourceDictionary>,
    # its block just before the last </Grid> (each ends with the indent of the line it went into)
    i = rest.find('<Style x:Key="FH2Mod_Style_ModNotice"')
    j = rest.find("</ResourceDictionary>", i)
    if i >= 12 and j > i:
        rest = rest[:i - 12] + rest[j:]
    i = rest.find(chr(13) + chr(10) + "            <!-- FH2 Mod Platform: mods that didn't load -->")
    j = rest.find("</Grid>", i)
    if i >= 0 and j > i:
        rest = rest[:i] + rest[j:]
same = ok_marks and rest == orig
detail = "" if ok_marks else "the blocks' markers aren't there once each"
if ok_marks and not same:
    d = list(difflib.unified_diff(orig.splitlines(), rest.splitlines(), "UI.zip A_Hud.xaml", "page minus insertions", lineterm="", n=1))
    detail = " | ".join(d[:12])
check("13. the page is UI.zip's A_Hud.xaml plus only the resources and the layer%s%s"
      % (" and xmlns:ed" if ED_NS in page else "", " and the platform's notice" if args.notice else ""), same, detail)

# ---- 14. the rings: ed:Arc exactly when BAR or COUNTDOWN is a part, with xmlns:ed; the only ExitActions stop
# the countdown pop's or the shutter's storyboard (the game's HUD pages' pattern, AR_BucketListActivation.xaml)
arc = "<ed:Arc " in page
STOPPABLE = ("FH2HL_CountPopBegin", "FH2HL_ShutterBegin")
exits = re.findall(r"<DataTrigger\.ExitActions>(.*?)</DataTrigger\.ExitActions>", page, re.S)
bad_exits = []
for body in exits:
    m = re.fullmatch(r'\s*<StopStoryboard BeginStoryboardName="([^"]+)" />\s*', body)
    if not m or m.group(1) not in STOPPABLE or ('<BeginStoryboard x:Name="%s">' % m.group(1)) not in page:
        bad_exits.append(body.strip()[:80])
check("14. ed:Arc and xmlns:ed exactly when BAR or COUNTDOWN is a part (%s); %d ExitActions, each a stop of a named storyboard"
      % (", ".join(parts) or "none", len(exits)),
      arc == ring_wanted and (ED_NS in page) == ring_wanted and "<local:ControllerButtonControl " not in page
      and page.count("<DataTrigger.ExitActions>") == len(exits) and not bad_exits,
      "in the page: ed:Arc %d, xmlns:ed %d, other ExitActions: %s" % (arc, ED_NS in page, bad_exits))

# ---- 15. every Style on an element of its target type
parent = {c: p for p in root.iter() for c in p}
local = lambda tag: short(tag).split(":")[-1]
bad_style = []
for el in layer_els:
    v = el.get("Style")
    if v is not None:
        m = re.match(r"^\{(?:Static|Dynamic)Resource ([^}\s]+)\}$", v)
        kd = (own.get(m.group(1)) or known.get(m.group(1))) if m else None
        if not m:
            bad_style.append("a %s's Style %r isn't a resource" % (short(el.tag), v))
        elif kd is None:
            pass   # (check 2)
        elif kd.element != "Style":
            bad_style.append("a %s's Style is %s, a %s" % (short(el.tag), m.group(1), kd.element))
        elif kd.target != local(el.tag):
            bad_style.append("the Style %s (for %s) is on a %s" % (m.group(1), kd.target or "no type", short(el.tag)))
    if local(el.tag) != "Style" or el in layer_res:
        continue
    holder = parent.get(el)
    hname = short(holder.tag) if holder is not None else ""
    owner = hname[:-len(".Style")] if hname.endswith(".Style") else ""
    host = parent.get(holder) if holder is not None else None
    if not owner or host is None or short(host.tag) != owner:
        bad_style.append("a Style sits in a %s%s, not in an element's own X.Style"
                         % (hname or "(nothing)", " inside a %s" % short(host.tag) if host is not None else ""))
    elif type_name(el.get("TargetType")) != type_name(owner):
        bad_style.append("the Style in a %s targets %s" % (hname, type_name(el.get("TargetType")) or "nothing"))
n_inline = sum(1 for e in layer_els if local(e.tag) == "Style" and e not in layer_res)
n_keyed = sum(1 for e in layer_els if e.get("Style") is not None)
check("15. every Style is on an element of its target type (%d Style= attributes, %d inline Styles)" % (n_keyed, n_inline),
      not bad_style, "; ".join(bad_style[:5]))

# ---- 16. the binding budget, counted as the writer counts (every {Binding FH2Mod_Hud_...} attribute value)
check("16. %d bindings, at most 640" % n_bindings, n_bindings <= 640)

# ---- 17. Effect="{x:Null}" only on the selected tabs' dark letters
nulls = [e for e in layer_els if e.get("Effect") == "{x:Null}"]
bad_null = [short(e.tag) for e in nulls if not (short(e.tag) == "local:FZTextBlock" and e.get("Color") == "#FF2F2F2F"
                                                and re.match(r"^\{Binding FH2Mod_Hud_Tab[0-7]Sel,", e.get("Visibility") or ""))]
check("17. Effect={x:Null} only on the selected tabs' dark letters (%d)" % len(nulls),
      not bad_null and len(nulls) == (8 if "BAR" in parts else 0), ", ".join(bad_null[:5]))

# ---- 18. ClipToBounds on exactly the bar's middle Grid and the tip's inner Grid
clips = [e for e in layer_els if e.get("ClipToBounds") == "True"]
want_clips = ("BAR" in parts) + ("TIP" in parts)
mid = [e for e in clips if e.get("Grid.Column") == "1" and short(e.tag) == "Grid"]
tip = [e for e in clips if e.get("Margin") == "16,0,12,0" and short(e.tag) == "Grid"]
check("18. ClipToBounds on exactly the bar's middle Grid and the tip's inner Grid (%d)" % len(clips),
      len(clips) == want_clips and all(short(e.tag) == "Grid" for e in clips) and len(mid) == ("BAR" in parts)
      and len(tip) == ("TIP" in parts))

print("RESULT: %d checks, %d failed" % (len(results), results.count(False)))
sys.exit(0 if all(results) else 1)
