#!/usr/bin/env python3
"""shell_audit.py -- app/ui/shell.html が本当に束縛されているかを見る。

なぜ要るのか。エンジンの束縛器は、知らない属性も、解決できない path も、**黙って**受け流す。
綴りを一つ間違えた行は、エラーも警告も出さずにただ何も起きない。BINDING.md はその危険を
認めたうえで `mitiru lint` が静的に照合すると書いているが、その道具はエンジンに実装が無い。

実際、最初に書いたシェルは木もプロパティも一つも束縛できていなかった。repeat の中で
`item.name` と書いていたが resolve() は path を item から辿るので `item` という名の
フィールドを探して undefined になる。`data-m-attr="src:item.icon"` は {} が無いため
文字列 "item.icon" をそのまま src に入れていた。プロパティ側は <template> が無く、
repeat が展開されないまま空の行が一つ残っていた。三つとも画面は出るし、何も言わない。

なので照合する相手は文書ではなく**束縛器そのもの**にしてある。対応属性の集合は
mitiru_bind.js を読んで取り出すので、エンジンが増やせばこの検査も勝手に追随し、
逆にエンジンが落とせばこちらが落ちる。

検査項目:
  A  data-m-* が束縛器の読む属性であること           (綴り違い・発明した属性)
  B  data-m-repeat に <template> があること           (展開されず一行残る)
  C  repeat の中の path が裸であること                (item. / view. 接頭辞)
  D  値を差し込む属性に {} があること                 (attr / style / tpl)
  E  edit. select. view. の行動名が Keymap.hpp にあること
  F  data-m-class の付けるクラスが CSS にあること
  G  束縛器の二つの script が読まれていること
  H  <img src> のファイルが実在すること
  I  data-m-input の送信名が送信時に読まれること     (下の「順序への依存」)
  J  読んでいる鍵を C++ が publish すること         (ViewState.hpp と突き合わせ)

J が BINDING.md の言う「静的な照合」の当のものである。属性の綴りが正しく、repeat の
書き方も正しくても、C++ が push しない名前は同じように黙って何も起こさない。ViewState.hpp
の publishedKeys() / publishedItemFields() が語彙で、あちらは viewstate_check.cpp が
「宣言どおりのものを本当に出しているか」を見ているので、宣言が願望になることはない。

12 項目すべて、tools/shell_audit_control.py が傷を入れて落ちることを毎回確かめている。

順序への依存について。プロパティ欄は行ごとに違うパラメータを編集するが、data-m-input が
送るのは値だけで、どの欄かを運ばない。そこで data-m-attr で行ごとに data-m-input を
書き換えている。これが成り立つのは sendInput が送信名を **送信時に** dataset から読むから
であって、配線時に控えていたら全部の行が同じ名前を送る。BINDING.md にこの保証は無いので、
I で束縛器の実装を直接見ている。ここが変わったら、プロパティ欄は黙って別の欄を編集する。
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHELL = os.path.join(ROOT, 'app', 'ui', 'shell.html')
KEYMAP = os.path.join(ROOT, 'makina-core', 'include', 'makina', 'Keymap.hpp')
VIEWSTATE = os.path.join(ROOT, 'makina-core', 'include', 'makina', 'ViewState.hpp')
MITIRU = os.environ.get('MITIRU_DIR', 'D:/sandbox/MitiruEngineDev')
BINDER = os.path.join(MITIRU, 'web', 'mitiru_runtime', 'mitiru_bind.js')

# 束縛器が読まない接頭辞。add. file. set. は Makina 側の語彙で、Keymap には無くてよい
# (ツールバーのボタンであってキー割り当てではない)。edit. select. view. だけが両方に居る。
SHARED_PREFIXES = ('edit.', 'select.', 'view.')


def fail(msg):
    print('    FAIL  ' + msg)
    return 1


def supported_attributes(js):
    """束縛器が実際に読む data-m-* の集合。文書ではなく実装から取る。"""
    names = set()
    for m in re.finditer(r'dataset\.m([A-Z][A-Za-z]*)', js):
        # dataset.mFoo -> data-m-foo, dataset.mFooBar -> data-m-foo-bar
        name = re.sub(r'(?<!^)([A-Z])', r'-\1', m.group(1)).lower()
        names.add('data-m-' + name)
    for m in re.finditer(r"data-m-([a-z-]+)", js):
        names.add('data-m-' + m.group(1))
    return names


def known_actions(hpp):
    """Keymap.hpp の knownActions() が並べている行動名。"""
    m = re.search(r'knownActions\(\)\s*\{(.*?)\n\}', hpp, re.S)
    if not m:
        return None
    return set(re.findall(r'"([a-z]+\.[A-Za-z]+)"', m.group(1)))


def repeat_blocks(html):
    """data-m-repeat の (リスト名, 開始, 終了)。終端は対応する閉じタグを素朴に数えて求める。"""
    out = []
    for m in re.finditer(r'<(\w+)([^>]*\bdata-m-repeat="([^"]*)")', html):
        tag = m.group(1)
        depth, end = 0, len(html)
        for t in re.finditer(r'<(/?)%s\b' % tag, html[m.start():]):
            depth += -1 if t.group(1) else 1
            if depth == 0:
                end = m.start() + t.end()
                break
        out.append((m.group(3), m.start(), end))
    return out


# 値として path を取る属性。data-m-input は送信名であって状態の path ではないので入らない。
# attr / style / tpl は文字列の中に {path} を書くので、取り出し方が違う。
PLAIN_PATH = 'text|show|hide|tween|flash|toast|value|repeat'
BRACED_PATH = 'attr|style|tpl'


def bound_paths(html, start, end):
    """[start, end) の範囲で束縛されている state path を (属性, path) で返す。"""
    chunk = html[start:end]
    for attr, value in re.findall(r'\b(data-m-(?:%s))="([^"]*)"' % PLAIN_PATH, chunk):
        for one in ([value] if attr != 'data-m-class' else []):
            token = one.strip().lstrip('!')
            token = re.split(r'[<>=!\s]', token)[0]
            if token and not token[0].isdigit():
                yield attr, token
    for spec in re.findall(r'\bdata-m-class="([^"]*)"', chunk):
        for pair in spec.split(';'):
            if ':' in pair:
                token = pair.split(':', 1)[1].strip().lstrip('!')
                token = re.split(r'[<>=!\s]', token)[0]
                if token and not token[0].isdigit():
                    yield 'data-m-class', token
    for attr, value in re.findall(r'\b(data-m-(?:%s))="([^"]*)"' % BRACED_PATH, chunk):
        for token in re.findall(r'\{([^}:]+)', value):
            yield attr, token.strip()
    for value in re.findall(r'\bdata-m-arg="([^"]*)"', chunk):
        token = value.strip()
        if token and token[0] not in '\'"' and not token[0].isdigit():
            yield 'data-m-arg', token


def published(hpp):
    """ViewState.hpp が publish すると宣言している鍵と、リスト項目のフィールド。"""
    keys = set()
    m = re.search(r'publishedKeys\(\)\s*\{(.*?)\n\}', hpp, re.S)
    if m:
        keys = set(re.findall(r'"([\w.]+)"', m.group(1)))
    fields = {}
    m = re.search(r'publishedItemFields\(\)\s*\{(.*?)\n\}', hpp, re.S)
    if m:
        for entry in re.finditer(r'\{"([\w.]+)",\s*\{([^}]*)\}\}', m.group(1)):
            fields[entry.group(1)] = set(re.findall(r'"(\w+)"', entry.group(2)))
    return keys, fields


def main():
    bad = 0
    # 引数で別の HTML を見られるのは、否定対照のためである。傷を入れた写しを渡せるので、
    # 本物の shell.html を書き換えてから戻す必要がなくなる -- 途中で落ちたときに壊れた
    # シェルが残る対照は、確かめている当のものより危ない。
    # アイコンの実在だけは本物の置き場を基準に見る (写しはどこに置かれてもよい)。
    shell = sys.argv[1] if len(sys.argv) > 1 else SHELL
    for path in (shell, KEYMAP, VIEWSTATE):
        if not os.path.exists(path):
            return fail("could not open '%s'" % os.path.basename(path))
    html = open(shell, encoding='utf-8').read()

    # A -- 束縛器が読む属性かどうか。エンジンが無ければ飛ばすが、黙っては飛ばさない。
    if os.path.exists(BINDER):
        supported = supported_attributes(open(BINDER, encoding='utf-8').read())
        used = set(re.findall(r'\b(data-m-[a-z-]+)=', html))
        for a in sorted(used - supported):
            bad += fail("'%s' is not an attribute the binder reads -- it will do nothing" % a)
        print('    %d binder attributes, all of them ones mitiru_bind.js reads' % len(used))

        # I -- 上の docstring が説明している順序依存。壊れると欄が入れ替わる。
        js = open(BINDER, encoding='utf-8').read()
        if not re.search(r"dispatch\(\s*'input:'\s*\+\s*el\.dataset\.mInput", js):
            bad += fail('the binder no longer reads data-m-input at dispatch time; the property '
                        'rows rewrite that attribute per row and now all send the same name')
    else:
        print('    SKIP  binder not found under MITIRU_DIR -- attribute set unchecked')

    # B / C / D / J -- repeat の中と外。
    keys, itemFields = published(open(VIEWSTATE, encoding='utf-8').read())
    if not keys:
        bad += fail('could not read publishedKeys() out of ViewState.hpp')

    spans = repeat_blocks(html)
    for listpath, start, end in spans:
        if '<template' not in html[start:end]:
            bad += fail("repeat '%s' has no <template>; it will not expand" % listpath)
        if listpath not in itemFields:
            bad += fail("repeat '%s' has no entry in publishedItemFields()" % listpath)
        if listpath not in keys:
            bad += fail("repeat '%s' names a list ViewState.hpp does not publish" % listpath)
        for attr, path in bound_paths(html, start, end):
            # 自分自身のリスト名は項目のフィールドではなく、外側の鍵である。
            if attr == 'data-m-repeat':
                continue
            if path.startswith(('item.', 'view.')):
                bad += fail("'%s' inside repeat '%s': fields are bare here, so this resolves "
                            'against the item and finds nothing' % (path, listpath))
            elif path not in itemFields.get(listpath, set()):
                bad += fail("repeat '%s' binds '%s', which its items do not carry" %
                            (listpath, path))

    # J -- repeat の外。C++ が push しない鍵は、束縛器が黙って捨てる。BINDING.md が
    # `mitiru lint` でやると書いて実装されていない照合が、これである。
    inside = lambda i: any(a <= i < b for _, a, b in spans)
    for m in re.finditer(r'\b(data-m-(?:%s|%s|arg|class))="' % (PLAIN_PATH, BRACED_PATH), html):
        if inside(m.start()):
            continue
        for attr, path in bound_paths(html, m.start(), html.index('"', m.end()) + 1):
            if path not in keys:
                bad += fail("the shell reads '%s', which ViewState.hpp does not publish" % path)
    print('    %d published keys, and the shell reads none the C++ side withholds' % len(keys))

    for attr, value in re.findall(r'\b(data-m-(?:attr|style|tpl))="([^"]*)"', html):
        if '{' not in value:
            bad += fail("'%s=\"%s\"' has no {path}; the literal text is written as-is" %
                        (attr, value))

    # E -- 行動名。ビューポートとコマンド層で揃えた語彙から、シェルだけ外れないように。
    actions = known_actions(open(KEYMAP, encoding='utf-8').read())
    if actions is None:
        bad += fail('could not read knownActions() out of Keymap.hpp')
    else:
        # <select> は一つの行動名しか持てないので、カメラのように選択肢ごとに違う行動を呼ぶ
        # ものは shell. 接頭辞の封筒を送り、中身の value が行動名になる。封筒は Keymap には
        # 無いが、value は在る必要がある — ここを見ないと、ビューポートに分岐の無いカメラを
        # 選択肢に並べられてしまう。
        named = set(re.findall(r'data-m-action="([^"]*)"', html))
        named |= set(re.findall(r'<option value="([^"]*)"', html))
        for name in sorted(named):
            if name.startswith(SHARED_PREFIXES) and name not in actions:
                bad += fail("the shell offers '%s', which is not a known action" % name)
        print('    %d actions, and every shared one is in the keymap vocabulary' % len(actions))

    # F -- クラス名。CSS 側と綴りが一文字違うだけで、選択行がどこにも光らなくなる。
    styled = set(re.findall(r'\.([a-zA-Z][\w-]*)\s*[,{:]', html.split('</style>')[0]))
    for spec in re.findall(r'data-m-class="([^"]*)"', html):
        for pair in spec.split(';'):
            if ':' in pair:
                cls = pair.split(':')[0].strip()
                if cls not in styled:
                    bad += fail("data-m-class adds '%s', which the stylesheet never styles" % cls)

    # G -- 束縛器そのもの。無ければ data-m-* は一つも動かない。
    for script in ('mitiru_cef_state.js', 'mitiru_bind.js'):
        if script not in html:
            bad += fail("'%s' is not loaded; no binding happens at all" % script)

    # H -- アイコン。壊れた src は空のボタンになるだけで、やはり何も言わない。
    for src in re.findall(r'<img[^>]*\bsrc="([^"{]+)"', html):
        if not os.path.exists(os.path.join(os.path.dirname(SHELL), src)):
            bad += fail("<img src=\"%s\"> is not there" % src)

    print()
    if bad:
        print('    THE SHELL LOOKS FINE AND BINDS NOTHING - %d problem(s)' % bad)
        return 1
    print('    the shell binds what it claims to bind')
    return 0


if __name__ == '__main__':
    sys.exit(main())
