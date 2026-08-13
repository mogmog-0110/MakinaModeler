#!/usr/bin/env python3
"""shell_audit_control.py -- shell_audit.py の否定対照。

検査を書いたことと、検査が効いていることは別である。このプロジェクトはそれを 3 度学んで
いて、いちばん高くついたのは「6 つの軸すべてが通るのに、消費側が揃って旗を無視していた」
回だった。**一致は、両方が正しいことではなく、両方が同じことをしている証拠にしかならない。**

なのでここでは shell.html に傷を 1 つ入れた写しを作り、shell_audit がそれを落とすことを
確かめる。落とさなかった項目は、書いてあるだけで働いていない。

写しに対して走らせるので本物は触らない。以前は本物を書き換えて戻していたが、途中で
落ちれば壊れたシェルが残る -- 確かめている当のものより危ない対照になっていた。
"""

import io
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHELL = os.path.join(ROOT, 'app', 'ui', 'shell.html')
AUDIT = os.path.join(HERE, 'shell_audit.py')

# (何を確かめるか, 元の断片, 傷を入れた断片)
CASES = [
    ('A  attribute the binder never reads', 'data-m-text="name"', 'data-m-txet="name"'),
    ('B  repeat without <template>', '      <template>\n        <div class="node"',
     '      <div class="node"'),
    ('C  repeat inside uses item.', 'data-m-text="name"', 'data-m-text="item.name"'),
    ('D  attr with no braces', 'data-m-attr="src: assets/icons/{icon}"',
     'data-m-attr="src: assets/icons/icon"'),
    ('E  action not in the keymap', 'data-m-action="edit.delete"',
     'data-m-action="edit.remove"'),
    ('E  camera the viewport cannot do', '<option value="view.top">',
     '<option value="view.diagonal">'),
    ('F  class the stylesheet lacks', 'is-selected: selected', 'is-sel: selected'),
    ('G  binder not loaded', '<script src="mitiru_runtime/mitiru_bind.js"></script>', ''),
    ('H  icon that is not there', 'grasp3d/box16.gif', 'grasp3d/cube16.gif'),
    ('J  key C++ never publishes', 'data-m-text="view.status.frame"',
     'data-m-text="view.status.frameRate"'),
    ('J  item field the list lacks', 'data-m-text="name"', 'data-m-text="nmae"'),
    ('J  repeat over a list nobody publishes', 'data-m-repeat="view.tree"',
     'data-m-repeat="view.outliner"'),
]


def main():
    good = io.open(SHELL, encoding='utf-8', newline='').read()
    bad = 0
    tmp = os.path.join(tempfile.gettempdir(), 'makina_shell_control.html')

    for label, old, new in CASES:
        if good.count(old) < 1:
            # 錨が消えたということは、対照が何も傷つけていないということである。
            print('    FAIL  %-38s the control anchor is gone; it wounds nothing' % label)
            bad += 1
            continue
        io.open(tmp, 'w', encoding='utf-8', newline='').write(good.replace(old, new, 1))
        r = subprocess.run([sys.executable, AUDIT, tmp], capture_output=True, text=True)
        if r.returncode == 0:
            print('    FAIL  %-38s not caught' % label)
            bad += 1

    if os.path.exists(tmp):
        os.remove(tmp)

    print()
    if bad:
        print('    %d CHECK(S) IN shell_audit DO NOTHING' % bad)
        return 1
    print('    all %d of shell_audit\'s checks fail when they should' % len(CASES))
    return 0


if __name__ == '__main__':
    sys.exit(main())
