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
    """data-m-repeat の要素と、その中身 (対応する閉じタグまでを素朴に数えて切り出す)。"""
    out = []
    for m in re.finditer(r'<(\w+)([^>]*\bdata-m-repeat=)', html):
        tag = m.group(1)
        depth, i = 0, m.start()
        for t in re.finditer(r'<(/?)%s\b' % tag, html[m.start():]):
            depth += -1 if t.group(1) else 1
            if depth == 0:
                i = m.start() + t.end()
                break
        out.append(html[m.start():i])
    return out


def main():
    bad = 0
    for path in (SHELL, KEYMAP):
        if not os.path.exists(path):
            return fail("could not open '%s'" % os.path.basename(path))
    html = open(SHELL, encoding='utf-8').read()

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

    # B / C / D -- repeat の中。
    for block in repeat_blocks(html):
        listpath = re.search(r'data-m-repeat="([^"]*)"', block).group(1)
        if '<template' not in block:
            bad += fail("repeat '%s' has no <template>; it will not expand" % listpath)
        for attr, value in re.findall(r'\b(data-m-(?:text|arg|value|class|show|hide))="([^"]*)"',
                                      block):
            for path in re.findall(r'[A-Za-z_][\w.]*', value.split(':')[-1]):
                if path.startswith(('item.', 'view.')):
                    bad += fail("'%s=\"%s\"' inside repeat '%s': fields are bare here, so this "
                                'resolves against the item and finds nothing' % (attr, value,
                                                                                listpath))
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
