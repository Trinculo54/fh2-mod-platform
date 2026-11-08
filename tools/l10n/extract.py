#!/usr/bin/env python3
"""Lists a mod's translatable text in l10n/<domain>.pot, for translators (docs/translating.md).

    python3 platform/tools/l10n/extract.py --domain MyMod src
    python3 platform/tools/l10n/extract.py --domain MyMod --check src      (for CI: exits 1 if out of date)

Scans the C and C++ files under the paths given for
    FH2_TR(api, mod, "key", "English")              api->tr(mod, "key", "English")
    FH2_TR_PLURAL(api, mod, "key", "one", "other", n)   api->tr_plural(mod, "key", "one", "other", n)
    FH2_TR_DESCRIPTION("English")                   (FH2ModInfo.description, key "mod.description")
    api->set_string_tr(mod, "Table.str", 0x1234, "key", L"English")
The key and the English have to be plain string literals (adjacent ones are joined); the key may be
NULL or nullptr. Anything else in the FH2_TR macros stops it with the file and line, since a
translation can only be found for text it can see. A ->tr(), ->tr_plural() or ->set_string_tr() call
it can't read (another object's method of that name, say) gets a warning instead, and is passed over.

Each entry gets its file:line, and as a translator's note the "// L10N:" comment on the line above the
call (or at the end of its line). The note can start with a slot kind in brackets, which sets the
length limit, "#, max-length:N": "// L10N(row-label): ..." or "// L10N(max=24): ...". Without one, the
limit comes from the key: *.label 30, *.description and mod.description 450, *.choice.* 12, title.* 24,
notice* 40, tile.* 18, tab.* 12. The kinds:
    row-label 30, slider-label 26, description 450, tile-heading 18, tab-name 12, choice 12,
    title-entry 24, notice-line 40, hud-title 28, hud-text 90, section-title 22, card-name 40,
    card-description 110, popup-title 28, popup-subtitle 45
A key used twice with different English stops it too.

The .pot goes to l10n/<domain>.pot under the folder above the first path (-o to choose), and its
references are relative to the folder above the .pot's (--root to choose). Python 3, standard
library only.
"""
import argparse
import os
import re
import sys

sys.dont_write_bytecode = True   # no __pycache__ in the platform's folder (a mod's submodule)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pofile  # noqa: E402

KINDS = {
    'row-label': 30, 'slider-label': 26, 'description': 450, 'tile-heading': 18, 'tab-name': 12, 'choice': 12,
    'title-entry': 24, 'notice-line': 40, 'hud-title': 28, 'hud-text': 90, 'section-title': 22, 'card-name': 40,
    'card-description': 110, 'popup-title': 28, 'popup-subtitle': 45,
}
EXTENSIONS = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inc')
SKIP_FILES = ('fh2mod_l10n.h', 'fh2mod.h')
# name -> (argument index of the key, of the English, of the plural English or None); key None: mod.description
CALLS = {
    'FH2_TR': (2, 3, None),
    'FH2_TR_PLURAL': (2, 3, 4),
    'FH2_TR_DESCRIPTION': (None, 0, None),
    '->tr': (1, 2, None),
    '->tr_plural': (1, 2, 3),
    '->set_string_tr': (3, 4, None),
}
CALL_RE = re.compile(r'(\bFH2_TR_PLURAL\b|\bFH2_TR_DESCRIPTION\b|\bFH2_TR\b|->\s*tr_plural\b|->\s*tr\b|->\s*set_string_tr\b)\s*\(')


class Stop(Exception):
    pass


def blank_comments(src):
    """The source with comments turned to spaces (line breaks kept), and each line's comment text."""
    out = []
    comments = {}
    i, n, line = 0, len(src), 1
    while i < n:
        c = src[i]
        if c == '\n':
            line += 1
            out.append(c)
            i += 1
        elif src.startswith('//', i):
            j = src.find('\n', i)
            j = n if j < 0 else j
            comments.setdefault(line, []).append(src[i + 2:j].strip())
            out.append(' ' * (j - i))
            i = j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            chunk = src[i:j]
            out.append(''.join('\n' if ch == '\n' else ' ' for ch in chunk))
            line += chunk.count('\n')
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and src[j] != c and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out), comments


def split_args(code, start):
    """The arguments of the call whose '(' is at code[start - 1], as (text, offset) pairs."""
    args, depth, i, cur = [], 0, start, start
    while i < len(code):
        c = code[i]
        if c in '"\'':
            j = i + 1
            while j < len(code) and code[j] != c:
                j += 2 if code[j] == '\\' else 1
            i = j + 1
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            if depth == 0:
                args.append((code[cur:i], cur))
                return args
            depth -= 1
        elif c == ',' and depth == 0:
            args.append((code[cur:i], cur))
            cur = i + 1
        i += 1
    return None


C_ESC = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'", '?': '?', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v'}


def literal(arg):
    """The text of an argument made of string literals (u8"..." and L"..." too), None for NULL/nullptr, else raise."""
    s = arg.strip()
    if s in ('NULL', 'nullptr', '0'):
        return None
    out = bytearray()
    i = 0
    found = False
    while i < len(s):
        if s[i].isspace():
            i += 1
            continue
        if s.startswith('u8"', i):
            i += 2
        elif s.startswith('L"', i):
            i += 1
        if s[i] != '"':
            raise Stop('not a plain string literal: %s' % s[:60])
        found = True
        i += 1
        while i < len(s) and s[i] != '"':
            if s[i] == '\\':
                e = s[i + 1]
                if e in C_ESC:
                    out += C_ESC[e].encode()
                    i += 2
                elif e == 'x':
                    m = re.match(r'[0-9A-Fa-f]+', s[i + 2:])
                    out.append(int(m.group(0), 16) & 0xFF)
                    i += 2 + len(m.group(0))
                elif e in '01234567':
                    m = re.match(r'[0-7]{1,3}', s[i + 1:])
                    out.append(int(m.group(0), 8) & 0xFF)
                    i += 1 + len(m.group(0))
                else:
                    raise Stop('an escape the tools don\'t read: \\%s' % e)
            else:
                out += s[i].encode('utf-8')
                i += 1
        i += 1
    if not found:
        raise Stop('not a plain string literal: %s' % s[:60])
    return out.decode('utf-8')


def limit_for(key, note):
    m = re.match(r'\((?:max=(\d+)|([a-z-]+))\)\s*:?\s*(.*)', note or '')
    if m:
        if m.group(1):
            return int(m.group(1)), m.group(3)
        if m.group(2) in KINDS:
            return KINDS[m.group(2)], m.group(3)
        raise Stop('unknown slot kind "%s" (one of %s)' % (m.group(2), ', '.join(sorted(KINDS))))
    note = (note or '').lstrip(':').strip()
    if key is None:
        return None, note
    if key == 'mod.description' or key.endswith('.description'):
        return 450, note
    if key.startswith('title.'):
        return 24, note
    if '.choice.' in key or key.endswith('.choice'):
        return 12, note
    if key.startswith('notice'):
        return 40, note
    if key.startswith('tile.'):
        return 18, note
    if key.startswith('tab.'):
        return 12, note
    if key.endswith('.label'):
        return 30, note
    return None, note


def scan(path, shown, entries, order):
    with open(path, encoding='utf-8-sig', errors='replace') as f:
        src = f.read()
    code, comments = blank_comments(src)
    for m in CALL_RE.finditer(code):
        name = re.sub(r'\s+', '', m.group(1))
        line = code.count('\n', 0, m.start()) + 1
        args = split_args(code, m.end())
        ki, ei, pi = CALLS[name]
        try:
            if args is None or len(args) <= max(i for i in (ki, ei, pi) if i is not None):
                raise Stop('a call it can\'t read')
            key = 'mod.description' if ki is None else literal(args[ki][0])
            english = literal(args[ei][0])
            plural = literal(args[pi][0]) if pi is not None else None
            if english is None:
                raise Stop('the English is NULL')
        except Stop as e:
            if name.startswith('->'):
                # perhaps another object's tr(), or the platform's with text it can't see: said, and passed over
                print('warning: %s:%d: %s() isn\'t read: %s' % (shown, line, name[2:], e), file=sys.stderr)
                continue
            raise Stop('%s:%d: %s' % (shown, line, e))
        note = ''
        for l in (line, line - 1):
            for c in comments.get(l, []):
                if c.startswith('L10N'):
                    note = c[4:]
            if note:
                break
        try:
            limit, note = limit_for(key, note)
        except Stop as e:
            raise Stop('%s:%d: %s' % (shown, line, e))
        ident = (key, english if key is None else None)
        e = entries.get(ident)
        if e is None:
            e = pofile.Entry()
            e.ctxt, e.msgid, e.plural = key, english, plural
            entries[ident] = e
            order.append(ident)
        elif e.msgid != english or e.plural != plural:
            raise Stop('%s:%d: the key "%s" is already used for other English ("%s")' % (shown, line, key, e.msgid))
        e.refs.append('%s:%d' % (shown, line))
        if note and note not in e.comments:
            e.comments.append(note)
        if limit:
            e.flags.add('max-length:%d' % limit)


def main(argv):
    ap = argparse.ArgumentParser(description='Lists translatable text in l10n/<domain>.pot.')
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--domain', required=True, help="the .pot's name: your mod's")
    ap.add_argument('-o', '--output', help='the .pot (default: l10n/<domain>.pot beside the first path)')
    ap.add_argument('--root', help='references are relative to this folder (default: the one above the .pot\'s)')
    ap.add_argument('--check', action='store_true', help='write nothing; exit 1 if the .pot is out of date')
    a = ap.parse_args(argv)
    out = a.output or os.path.join(os.path.dirname(os.path.abspath(a.paths[0])), 'l10n', a.domain + '.pot')
    root = os.path.abspath(a.root or os.path.dirname(os.path.dirname(os.path.abspath(out))))
    files = []
    for p in a.paths:
        if os.path.isdir(p):
            for d, _, names in os.walk(p):
                files += [os.path.join(d, n) for n in names if n.lower().endswith(EXTENSIONS) and n not in SKIP_FILES]
        else:
            files.append(p)
    entries, order = {}, []
    try:
        for f in sorted(files, key=lambda f: os.path.relpath(f, root).replace('\\', '/')):
            scan(f, os.path.relpath(os.path.abspath(f), root).replace('\\', '/'), entries, order)
    except Stop as e:
        print('extract.py: %s' % e, file=sys.stderr)
        return 1
    lines = ['# %s: the English text to translate, from tools/l10n/extract.py. Don\'t edit this file: run it again.' % a.domain,
             '# Translate it into l10n/<language>.po (pl.po, pt_BR.po): docs/translating.md in the FH2 Mod Platform.',
             'msgid ""', 'msgstr ""', '"Content-Type: text/plain; charset=UTF-8\\n"', '"Content-Transfer-Encoding: 8bit\\n"',
             '"X-Domain: %s\\n"' % a.domain, '']
    for ident in order:
        pofile.write_entry(entries[ident], lines)
    text = '\n'.join(lines)
    if a.check:
        try:
            with open(out, encoding='utf-8', newline='') as f:
                now = f.read().replace('\r\n', '\n')
        except OSError:
            now = None
        if now != text:
            print('%s is out of date: run tools/l10n/extract.py again' % out, file=sys.stderr)
            return 1
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('%s: %d %s' % (out, len(order), 'string' if len(order) == 1 else 'strings'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
