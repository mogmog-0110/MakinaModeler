# 手で書かれた POV-Ray シーン（インポータ用の入力）

`scene.pov` と `pingu.pov` は**このプロジェクトの外で書かれた**シーンである。
検証の入力として価値があるのはその一点で、
ここにある `.makina.json` のフィクスチャはどれも私が「通るように」作ったものなので、
自分の思い込みは自分では突けない。

`scene.pov` は `#include "pingu.inc"` を書いているが、渡されたのは `pingu.pov` である。
インポータを書くときは、この不一致をどう扱うか（拡張子を読み替えるのか、
`.inc` を要求するのか）を先に決めること — 黙って読み替えると、
別のファイルを読んでいることに気付けなくなる。

## 何が使われているか（実測）

| 使える見込み | | 未対応 | |
|---|---|---|---|
| sphere / box / cylinder / torus / plane | 14/9/11/1/15 | `#macro` | 4 |
| union / difference / intersection / merge | 15/3/7/2 | `normal { bumps }` | 13 |
| translate / rotate / scale | 46/24/49 | `radiosity` | 2 |
| texture / pigment / finish | 45/17/18 | `area_light` | 3 |
| camera / light_source | 2/6 | `blob` / `sphere_sweep` | 1/2 |
| `#declare` / `object` | 53/57 | | |

**幾何と CSG と変換はほぼ全部こちらにある。** 重いのは言語の側で、
`#declare` が 53 回、`object { 名前 }` が 57 回 — つまり
**シンボル表とインスタンス化**が要る。`#macro` は 4 回で、ここが一番厄介。

> `blob` と `sphere_sweep` は「未対応」に置いてあるが、
> **距離場にとってはむしろ簡単な部類**である。blob は smooth-min そのもので、
> sphere_sweep はカプセルの連なり。POV 側が苦労する機能で、こちらが楽をできる。
> インポータを書くなら、この 2 つは早い段階で入れる価値がある。

## 読めないものは近似せず断ること

`radiosity` を無視して読み込むと、**読めたように見えて絵が違う**ものができる。
POV との照合を持っている以上、それは最悪の失敗の形である
（`CsgBake.hpp` が古い .cso を拒む理由と同じ）。
対応していない指定に当たったら、名前を挙げて断ること。
