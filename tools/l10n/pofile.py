"""Reading and writing gettext PO files, for po2inc.py and extract.py (Python 3, standard library only).

Reads what the platform reads (src/l10n.cpp): msgctxt, msgid, msgid_plural, msgstr and msgstr[n], text
over several lines, C escapes, "#, fuzzy" and the other comments, and old entries (#~), which are left out.
"""
import re

PLACEHOLDER = re.compile(r'\{[^{} \t\n]+\}')   # as the platform's: no brace, space, tab or line break inside


class Entry:
    def __init__(self):
        self.ctxt = None          # the key; None for an entry without one
        self.msgid = ''
        self.plural = None        # msgid_plural, for an entry with plural forms
        self.msgstr = {}          # form number -> text (0 alone without plural forms)
        self.flags = set()        # from "#, ..."
        self.comments = []        # "#." lines, without the "#. "
        self.refs = []            # "#:" lines' references
        self.line = 0             # where its msgid is

    def is_header(self):
        return self.msgid == '' and self.ctxt is None

    def forms(self):
        if self.plural is None:
            return [self.msgstr.get(0, '')]
        n = max(self.msgstr) + 1 if self.msgstr else 0
        return [self.msgstr.get(i, '') for i in range(n)]

    def translated(self):
        return any(self.forms())


def placeholders(text):
    return sorted(set(PLACEHOLDER.findall(text or '')))


def placeholders_fit(entry, text, only_for_one=False):
    """As the platform checks them: the same as the English's; for a plural form, none the English hasn't
    got in either form, and every one both English forms have, unless the form is only ever used for 1
    (then it may say "one" instead of {n})."""
    t = set(placeholders(text))
    one = set(placeholders(entry.msgid))
    if entry.plural is None:
        return t == one
    other = set(placeholders(entry.plural))
    return t <= (one | other) and (only_for_one or (one & other) <= t)


def plural_form(tag, n):
    """Which msgstr[] holds the form for n in a PO file for tag, as the platform picks it (src/l10n.cpp)."""
    lang = tag.split('-')[0]
    a = abs(n)
    d10, d100 = a % 10, a % 100
    few = 2 <= d10 <= 4 and (d100 < 10 or d100 >= 20)
    if lang == 'pl':
        return 0 if a == 1 else 1 if few else 2
    if lang in ('ru', 'uk', 'be'):
        return 0 if d10 == 1 and d100 != 11 else 1 if few else 2
    if lang in ('cs', 'sk'):
        return 0 if a == 1 else 1 if 2 <= a <= 4 else 2
    if lang in ('pt', 'fr'):
        return 1 if a > 1 else 0
    if lang in ('ja', 'ko', 'zh'):
        return 0
    return 0 if a == 1 else 1


def form_only_for_one(tag, form):
    """Whether msgstr[form] of a PO file for tag is only ever used for 1 (Polish's form 0, not Russian's)."""
    return plural_form(tag, 1) == form and all(n == 1 or plural_form(tag, n) != form for n in range(200))


_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v'}


def unquote(s, where):
    s = s.strip()
    if len(s) < 2 or s[0] != '"':
        raise ValueError('%s: expected a "string"' % where)
    out = bytearray()
    i = 1
    while i < len(s):
        c = s[i]
        if c == '"':
            if s[i + 1:].strip():
                raise ValueError('%s: text after the closing quote' % where)
            return out.decode('utf-8', errors='replace')
        if c != '\\':
            out += c.encode('utf-8')
            i += 1
            continue
        i += 1
        if i >= len(s):
            break
        e = s[i]
        if e in _ESCAPES:
            out += _ESCAPES[e].encode()
            i += 1
        elif e == 'x':
            m = re.match(r'[0-9A-Fa-f]{1,2}', s[i + 1:])
            if not m:
                raise ValueError('%s: \\x without hex digits' % where)
            out.append(int(m.group(0), 16))
            i += 1 + len(m.group(0))
        elif e in '01234567':
            m = re.match(r'[0-7]{1,3}', s[i:])
            out.append(int(m.group(0), 8) & 0xFF)
            i += len(m.group(0))
        else:
            out += e.encode('utf-8')
            i += 1
    raise ValueError('%s: no closing quote' % where)


def parse(text, name='<po>'):
    """Every entry of a PO file, the header and untranslated and fuzzy ones included (old #~ ones not)."""
    if text.startswith('\ufeff'):
        text = text[1:]
    entries = []
    cur = Entry()
    seen_id = seen_str = False
    last = None   # (attribute, form) the next "..." line adds to

    def flush():
        nonlocal cur, seen_id, seen_str, last
        if seen_id:
            entries.append(cur)
        cur, seen_id, seen_str, last = Entry(), False, False, None

    for n, raw in enumerate(text.split('\n'), 1):   # lines end at \n alone, as the platform reads them
        line = raw.strip(' \t\r')
        where = '%s:%d' % (name, n)
        if not line:
            flush()
            continue
        if line.startswith('#'):
            if seen_str:
                flush()
            if line.startswith('#~'):
                continue
            if line.startswith('#,'):
                cur.flags |= {f.strip() for f in line[2:].split(',') if f.strip()}
            elif line.startswith('#.'):
                cur.comments.append(line[2:].strip())
            elif line.startswith('#:'):
                cur.refs += line[2:].split()
            continue
        if line.startswith('"'):
            if last is None:
                raise ValueError('%s: a string with no keyword before it' % where)
            attr, form = last
            s = unquote(line, where)
            if attr == 'msgstr':
                cur.msgstr[form] += s
            else:
                setattr(cur, attr, getattr(cur, attr) + s)
            continue
        kw, rest = (re.split(r'[ \t]', line, maxsplit=1) + [''])[:2]   # a space or a tab after the keyword, as the platform reads it
        if kw == 'msgctxt':
            if seen_id or seen_str:
                flags = cur.flags if not seen_id else set()
                flush()
                cur.flags = flags
            cur.ctxt = unquote(rest, where)
            last = ('ctxt', 0)
        elif kw == 'msgid':
            if seen_id or seen_str:
                flush()
            seen_id = True
            cur.msgid = unquote(rest, where)
            cur.line = n
            last = ('msgid', 0)
        elif kw == 'msgid_plural' and seen_id:
            cur.plural = unquote(rest, where)
            last = ('plural', 0)
        elif kw == 'msgstr' and seen_id:
            seen_str = True
            cur.msgstr[0] = unquote(rest, where)
            last = ('msgstr', 0)
        elif re.fullmatch(r'msgstr\[\d\]', kw) and seen_id:
            seen_str = True
            form = int(kw[7])
            cur.msgstr[form] = unquote(rest, where)
            last = ('msgstr', form)
        else:
            raise ValueError("%s: this line isn't PO" % where)
    flush()
    return entries


def quote(text, indent=''):
    """A PO string: one line, or "" then a line per \\n when the text has line breaks."""
    def esc(s):
        return s.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\r', '\\r').replace('\n', '\\n')
    parts = text.split('\n')
    if len(parts) == 1:
        return '"%s"' % esc(text)
    pieces = [p + '\n' for p in parts[:-1]] + ([parts[-1]] if parts[-1] else [])
    return '""\n' + '\n'.join('%s"%s"' % (indent, esc(p)) for p in pieces)


def write_entry(e, out, comments=True):
    if comments:
        for c in e.comments:
            out.append('#. ' + c)
        if e.refs:
            out.append('#: ' + ' '.join(e.refs))
        if e.flags:
            out.append('#, ' + ', '.join(sorted(e.flags)))
    if e.ctxt is not None:
        out.append('msgctxt ' + quote(e.ctxt))
    out.append('msgid ' + quote(e.msgid))
    if e.plural is not None:
        out.append('msgid_plural ' + quote(e.plural))
        forms = e.forms() or ['', '']
        for i, f in enumerate(forms):
            out.append('msgstr[%d] %s' % (i, quote(f)))
    else:
        out.append('msgstr ' + quote(e.msgstr.get(0, '')))
    out.append('')
