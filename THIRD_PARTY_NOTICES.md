# Third-Party Notices

本ソフトウェアは以下の第三者コンポーネントを含みます。各コンポーネントは
それぞれのライセンスに従います。

## nlohmann/json

`makina-core/external/nlohmann/json.hpp`。MIT License。

```
Copyright (c) 2013-2025 Niels Lohmann <http://nlohmann.me>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Lucide

`app/ui/assets/icons/lucide/` の SVG と、`app/ui/shell.html` に直接書いた
同じ図形。ISC License。

```
ISC License

Copyright (c) for portions of Lucide are held by Cole Bemis 2013-2022 as part of
Feather (MIT). All other copyright (c) for Lucide are held by Lucide Contributors
2022.

Permission to use, copy, modify, and/or distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright notice
and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.
```

## Grasp3D について

本ソフトウェアは、大学の授業で使われている Java 製モデラー Grasp3D を C++ へ
移す作業から始まっています。移植の正しさを確かめるため、Grasp3D の出力と数値を
突き合わせる検証を持ちます (`tools/sdfdump` ほか)。

**Grasp3D 自体のコードとアセットは、このリポジトリには含まれていません。**
`tools/` 配下の Java は、Grasp3D の保存ファイルを読んで参照値を書き出すために
こちらで書いたもので、Grasp3D の派生物ではありません。検証を走らせるには
Grasp3D を別途用意する必要があります。

ツールバーのプリミティブとブーリアンのアイコン (`app/ui/assets/icons/grasp3d/`) は
Grasp3D のものをそのまま使っています。Grasp3D 側にライセンスの明示が無いため、
再配布の可否は確認できていません。

## POV-Ray

検証で POV-Ray を外部プロセスとして呼びますが、同梱はしていません。
実行には別途インストールが必要です。
