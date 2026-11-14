#!/usr/bin/env python3
"""Builds a mod's translations into its DLL: PO files -> a generated .inc for include/fh2mod_l10n.h.

    python3 platform/tools/l10n/po2inc.py <out.inc> <array name> l10n/pl.po l10n/pt_BR.po ...
    python3 platform/tools/l10n/po2inc.py --check <out.inc> <array name> l10n/*.po

Each file's language comes from its name (pl.po, pt_BR.po or pt-BR.po; pl_PL.po is read as pl and
zh_TW.po as zh-Hant, as the platform reads them). The .inc holds an array of
FH2Translation, one per file, with a copy of the PO file cut down to what the platform reads: the
entries that are translated and not fuzzy, without comments. An entry whose {placeholders} don't match
its English, or whose text has a control character other than \\n and \\t or an escape that isn't UTF-8
(\\xE9), is left out, with a warning naming its file and line (the platform would refuse it too). So is a
file for a language none of the game's languages looks for (es_AR.po), with the name to use, and of two
files for one language (pl.po and pl_PL.po) the one not named for the language itself. A header whose
nplurals isn't the platform's for the language gets a warning, and a file that isn't UTF-8 stops it.
Then, in the mod:

    #include "fh2mod_l10n.h"
    #include "mymod_l10n.inc"
    ...
    g_mod = api->register_mod(&info);
    FH2_ADD_TRANSLATIONS(api, g_mod, mymod_translations);

--check writes nothing, and exits 1 when <out.inc> isn't what it would write (for CI). Python 3,
standard library only.
"""
import os
import re
import sys

sys.dont_write_bytecode = True   # no __pycache__ in the platform's folder (a mod's submodule)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pofile  # noqa: E402


def tag_of(path, as_named=False):
    stem = os.path.splitext(os.path.basename(path))[0]
    parts = re.split(r'[-_]', stem)
    if not parts or not re.fullmatch(r'[A-Za-z]{2,3}', parts[0]):
        raise SystemExit("%s: the file's name isn't a language (pl.po, pt_BR.po...)" % path)
    out = [parts[0].lower()]
    for p in parts[1:]:
        if re.fullmatch(r'[A-Za-z]{4}', p):
            out.append(p.title())
        elif re.fullmatch(r'[A-Za-z]{2}', p):
            out.append(p.upper())
        elif re.fullmatch(r'\d{3}', p):
            out.append(p)
        else:
            raise SystemExit("%s: the file's name isn't a language (pl.po, pt_BR.po...)" % path)
    tag = '-'.join(out)
    return tag if as_named else catalogue_tag(tag)


# The game's languages' tags, and the catalogues each looks in (as the platform's fallbacks()).
GAME_TAGS = ['en', 'en-GB', 'pl', 'pt-BR', 'de', 'fr', 'es-ES', 'es-MX', 'it', 'ja', 'ko', 'zh-Hant', 'cs', 'hu', 'ru', 'nl']
ONE_FORM = {'pl': 'Polish', 'de': 'German', 'fr': 'French', 'it': 'Italian', 'ja': 'Japanese', 'ko': 'Korean', 'cs': 'Czech',
            'hu': 'Hungarian', 'ru': 'Russian', 'nl': 'Dutch'}


def language_name(tag):
    names = dict(ONE_FORM, **{'en': 'English', 'en-GB': 'British English', 'pt-BR': 'Brazilian Portuguese', 'pt': 'Portuguese',
                              'es-ES': 'Spanish (Spain)', 'es-MX': 'Spanish (Mexico)', 'es': 'Spanish',
                              'zh-Hant': 'Traditional Chinese', 'zh': 'Chinese'})
    return names.get(tag, tag)


def fallbacks(tag):
    out = [tag]
    if tag == 'es-MX':
        out.append('es-ES')
    if '-' in tag:
        out.append(tag.split('-')[0])
    return out


def catalogue_tag(tag):
    """As the platform keeps it: pl-PL, de-DE, ja-JP, en-US and the like as the language alone, and zh-TW, zh-HK,
    zh-MO and zh-Hant-* as zh-Hant, the game's Traditional Chinese. Others as they are (pt-BR, de-AT)."""
    if tag in ('zh-TW', 'zh-HK', 'zh-MO') or tag.startswith('zh-Hant-'):
        return 'zh-Hant'
    if tag in ('pl-PL', 'de-DE', 'fr-FR', 'it-IT', 'ja-JP', 'ko-KR', 'cs-CZ', 'hu-HU', 'ru-RU', 'nl-NL', 'en-US'):
        return tag.split('-')[0]
    return tag


def never_shown(tag):
    """'' when one of the game's languages looks for `tag`'s text; else why not, and the name to use (as the platform says)."""
    if any(tag in fallbacks(t) for t in GAME_TAGS):
        return ''
    lang = tag.split('-')[0]
    head = "none of the game's languages looks for %s text" % tag
    if lang == 'en':
        return head + ' (its English is en, American, or en-GB, British), so name the file en.po or en_GB.po'
    if lang == 'pt':
        return head + ' (its Portuguese is pt-BR, Brazilian), so name the file pt_BR.po or pt.po'
    if lang == 'es':
        return head + ' (its Spanish is es-ES, for Spain, or es-MX, for Latin America), so name the file es_ES.po, es_MX.po or es.po'
    if lang == 'zh':
        return head + ' (its Chinese is zh-Hant, Traditional; it has no Simplified Chinese), so name a Traditional Chinese file zh_Hant.po'
    if lang in ONE_FORM:
        return head + ' (its %s is %s), so name the file %s.po' % (ONE_FORM[lang], lang, lang)
    return head + ', as the game has no such language'


def read_po(path):
    """The file's text, as UTF-8; a file in another encoding stops the build with one sentence, as the platform reads
    none of it."""
    with open(path, 'rb') as f:
        raw = f.read()
    if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
        raise SystemExit('%s is saved as UTF-16 (Notepad\'s "Unicode"): save it as UTF-8' % path)
    try:
        text = raw.decode('utf-8-sig')
    except UnicodeDecodeError as e:
        raise SystemExit("%s:%d isn't UTF-8: save it as UTF-8" % (path, raw[:e.start].count(b'\n') + 1))
    return text.replace('\r\n', '\n').replace('\r', '\n')   # as open() in text mode reads it


def compact(path, tag, warnings):
    text = read_po(path)
    entries = pofile.parse(text, os.path.basename(path))
    # pofile reads an escape that makes bytes that aren't UTF-8 (\xE9) as U+FFFD, where the platform leaves the entry
    # out; in a file with no U+FFFD of its own, that's where one comes from
    own_fffd = '\ufffd' in text
    out = []
    kept = 0
    forms = max(pofile.plural_form(tag, n) for n in range(200)) + 1
    for e in entries:
        if e.is_header():
            m = re.search(r'nplurals\s*=\s*(\d+)', e.msgstr.get(0, ''))
            if m and int(m.group(1)) != forms:
                warnings.append('%s: its header says nplurals=%s, but the platform reads %d plural %s for %s (%s, in gettext\'s usual '
                                'order for it), so a count may show the wrong form: check its plural texts' %
                                (path, m.group(1), forms, 'form' if forms == 1 else 'forms', language_name(tag),
                                 'msgstr[0] alone' if forms == 1 else 'msgstr[0] to msgstr[%d]' % (forms - 1)))
            continue
        if 'fuzzy' in e.flags or not e.translated():
            continue
        control = [c for t in e.forms() for c in t if ord(c) < 0x20 and c not in '\n\t']
        if control:
            warnings.append('%s:%d: "%s": its text has a control character (U+%04X), which the game can\'t show; left out' %
                            (path, e.line, e.ctxt if e.ctxt is not None else e.msgid, ord(control[0])))
            continue
        if not own_fffd and any('\ufffd' in t for t in e.forms()):
            warnings.append('%s:%d: "%s": its text has an escape that isn\'t UTF-8 (such as \\xE9); left out' %
                            (path, e.line, e.ctxt if e.ctxt is not None else e.msgid))
            continue
        bad = [t for i, t in enumerate(e.forms())
               if t and not pofile.placeholders_fit(e, t, e.plural is not None and pofile.form_only_for_one(tag, i))]
        if bad:
            warnings.append('%s:%d: "%s": its placeholders (%s) don\'t match the English\'s (%s); left out' %
                            (path, e.line, e.ctxt if e.ctxt is not None else e.msgid, ', '.join(pofile.placeholders(bad[0])) or 'none',
                             ', '.join(sorted(set(pofile.placeholders(e.msgid)) | set(pofile.placeholders(e.plural or '')))) or 'none'))
            continue
        pofile.write_entry(e, out, comments=False)
        kept += 1
    return '\n'.join(out) + ('\n' if out else ''), kept


def c_string(data, indent):
    """`data` (bytes) as C string literals, a line of the PO file each; bytes beyond ASCII as octal escapes."""
    lines = data.split(b'\n')
    pieces = [l + b'\n' for l in lines[:-1]] + ([lines[-1]] if lines[-1] else [])
    out = []
    for p in pieces:
        s = ''
        for b in p:
            c = chr(b)
            if c == '\\':
                s += '\\\\'
            elif c == '"':
                s += '\\"'
            elif c == '\n':
                s += '\\n'
            elif c == '\t':
                s += '\\t'
            elif 0x20 <= b < 0x7F and c != '?':   # no '?': trigraphs
                s += c
            else:
                s += '\\%03o' % b
        out.append('%s"%s"' % (indent, s))
    return '\n'.join(out) if out else indent + '""'


def build(name, files):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
        raise SystemExit('%s: the array name has to be a C name' % name)
    warnings = []
    blocks = []
    rel = [f.replace('\\', '/') for f in files]
    tags = [tag_of(f) for f in files]
    # Of two files for one language, the one named for the language itself is used (pl.po, not pl_PL.po), else the
    # first, as the platform reads its override folder
    used = {}
    for path, tag in zip(files, tags):
        if tag not in used or (tag_of(path, True) == tag and tag_of(used[tag], True) != tag):
            used[tag] = path
    for path, shown, tag in zip(files, rel, tags):
        why = never_shown(tag)
        if why:
            warnings.append('%s: %s; left out' % (path, why))
            continue
        if used[tag] != path:
            warnings.append('%s is %s (%s) too, and %s is the one used; left out' %
                            (path, language_name(tag), tag, os.path.basename(used[tag])))
            continue
        text, kept = compact(path, tag, warnings)
        data = text.encode('utf-8')
        blocks.append('    /* %s: %d %s */\n    {"%s",\n%s,\n     %d},' %
                      (os.path.basename(shown), kept, 'string' if kept == 1 else 'strings', tag, c_string(data, '     '), len(data)))
    head = ('/* Generated by tools/l10n/po2inc.py from %s: don\'t edit; run it again after changing them. */\n'
            % ', '.join(os.path.basename(f) for f in rel))
    body = 'static const FH2Translation %s[] = {\n%s\n};\n' % (name, '\n'.join(blocks)) if blocks else \
        'static const FH2Translation %s[] = {{NULL, NULL, 0}};   /* no PO files yet */\n' % name
    return head + body, warnings


def main(argv):
    check = '--check' in argv
    args = [a for a in argv if a != '--check']
    if len(args) < 2:
        print(__doc__)
        return 2
    out, name, files = args[0], args[1], args[2:]
    text, warnings = build(name, sorted(files, key=lambda f: os.path.basename(f)))
    for w in warnings:
        print('warning: ' + w, file=sys.stderr)
    if check:
        try:
            with open(out, encoding='utf-8', newline='') as f:
                now = f.read().replace('\r\n', '\n')
        except OSError:
            now = None
        if now != text:
            print('%s is out of date: run tools/l10n/po2inc.py again' % out, file=sys.stderr)
            return 1
        return 0
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
