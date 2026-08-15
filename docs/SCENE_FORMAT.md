# シーン記述形式（Task #5）

**版**: 1
**拡張子**: `.makina.json`

Makina / MitiruEngine / Grasp3D(Java) の 3 者が食う共通形式。

---

## 1. 設計方針

### 1.1 これはメモリダンプではない

`CsgNode`（[CSG_NODE.md](CSG_NODE.md)）は `params[12]` の平坦配列だが、**JSON は名前付きキーで書く。**

```jsonc
// これ
{ "op": "Cylinder", "radius": 0.085, "capPoint": 0.55, "basePoint": -0.55 }

// ではなく、これではない
{ "op": 2, "params": [0.085, 0.55, -0.55, 0, 0, 0, 0, 0, 0, 0, 0, 0] }
```

**理由**: この形式の用途の一つが「モデルを PR でレビューする」こと。
`radius: 0.085 → 0.095` は差分として読めるが、`params[0]` の変化は読めない。
平坦配列は**メモリ上の都合**であって、人と AI が読む面ではない。

### 1.2 木は入れ子で書く

添字参照ではなく `children` の入れ子にする。読みやすく、差分が構造単位で出る。
平坦配列（`firstChild` / `childCount`）は**読み込み時に構築する**。

### 1.3 一方向で構わない

Grasp3D の `.gsf` へ書き戻す経路は**作らない**。
`.gsf` は Grasp3D が引き続き権威を持ち、Makina は JSON を権威とする。
よって型情報を JSON に冗長に持たせる必要はない（型は `op` とキー名から一意に決まる）。

---

## 2. 構造

```jsonc
{
  "format": "makina-scene",
  "version": 1,
  "units": "meter",                       // 1.0 = 1 m
  "coordinates": "right-handed-y-up",     // COORDINATES.md
  "angles": "degrees",

  "source": {                             // 由来。人間向けの手がかりで、意味は持たない
    "tool": "gsf2json",
    "file": "penrose.gsf"
  },

  "nextId": 128,                          // 次に払い出す不変 ID

  "root": {
    "id": 1,
    "op": "Difference",
    "name": "ケーブル穴",                  // 設計意図。空でもよいが、消さないこと
    "material": 0,                        // materials への添字。省略時は継承
    "radius": 0.085,                      // op ごとのパラメータ（§3）
    "children": [ /* ... */ ]
  },

  "materials": [
    {
      "id": 0,
      "diffuse": [255, 255, 0],           // 0-255 整数。Grasp3D の Color をそのまま
      "alpha": 1.0,
      "ambient": 0.1,
      "specular": 0.0,
      "shininess": 1.0,
      "emission": 0.0,
      "texture": null                     // テクスチャ名 or null
      // 以下は POV 由来の任意項。既定値のときは書かない（古いファイルがそのまま往復するため）:
      //   "reflection": 0.3            POV finish{reflection}。既定 0
      //   "ior": 1.5                   POV interior{ior}。既定 1
      //   "brilliance": 0.9            POV finish{brilliance}: 拡散項の N·L の指数。既定 1
      //                                （POV に吐かせて cos^b と 4 桁一致、spike/pov_brilliance_probe.py）
      //   "finishDiffuse": 0.78        POV finish{diffuse}: 拡散の係数。既定 0.6。"diffuse" は色なので別名
      //   "pigment": {...}             pattern と color_map。2 stop (0/1) は "colorA"/"colorB"、
      //                                それ以外は "stops": [[pos,[r,g,b]],...] (最大 8、昇順)。
      //                                補間は区間線形・外側は端色クランプ (POV 実測、
      //                                spike/pov_colormap_probe.py 11/11 点)
    }
  ]
}
```

### 2.1 `id` の規則

- `uint32`、単調増加、**再利用しない**（[CSG_NODE.md](CSG_NODE.md) §6）
- `.gsf` には ID が存在しないので、**エクスポータが前順走査で 1 から採番する**
- 同じ `.gsf` を再変換すれば同じ ID になる（決定的）
- ⚠ ただし **Grasp3D 側で編集してから再変換すると ID はずれる。**
  一度 Makina に取り込んだら、**ID の権威は Makina 側に移る**

---

## 3. `op` とパラメータ

キー名は Grasp3D のプロパティ名から**末尾空白を除き、lowerCamelCase 化**したもの。
（Grasp3D の内部キーは `"Radius "` のように末尾に空白がある）

### 3.1 プリミティブ

| `op` | パラメータ |
|---|---|
| `Box` | `x1`,`y1`,`z1`,`x2`,`y2`,`z2`（対角 2 点） |
| `Sphere` | `radius` |
| `Cylinder` | `capPoint`, `basePoint`, `radius`（Y 軸沿い） |
| `Cone` | `x1`,`y1`,`z1`,`radius1`, `x2`,`y2`,`z2`,`radius2`, `open` |
| `Torus` | `majorRadius`, `minorRadius` |
| `Plane` | `y`（半空間 y ≤ y） |
| `Disc` | `radius`, `holeRadius`, `thickness` |
| `Triangle` | `x1`..`z3`（9 個）, `thickness` |

### 3.2 変換

| `op` | パラメータ |
|---|---|
| `Translate` | `x`, `y`, `z` |
| `Rotate` | `axis`（`"X"`\|`"Y"`\|`"Z"`）, `degree` — **単軸**。Euler ではない |
| `Scale` | `x`, `y`, `z` — **非一様可** |

### 3.3 ブーリアン（**n 項**）

| `op` | 意味 |
|---|---|
| `Merge` | `children` 全部の和 |
| `Difference` | `children[0]` から `children[1..]` の**和**を引く |
| `Intersection` | `children` 全部の積 |

### 3.3b フィールド（POV の blob 由来）

| `op` | パラメータ | 意味 |
|---|---|---|
| `Blob` | `threshold` | `children` は**成分**であり和集合の要素ではない。各成分の密度の合計が threshold を横切る所が表面 |
| `BlobSphere` | `x`,`y`,`z`,`radius`,`strength` | 密度 `strength·(1−(r/R)²)²`（r ≥ R で 0） |
| `BlobCylinder` | `x1`..`z2`, `radius`, `strength` | 距離は線分まで（サポートはカプセル）。falloff は球成分と同じ |

成分は変換ノードの下に置ける（成分ごとの `scale` は POV がそう書くため）。
`Blob` の外に置かれた成分は評価に寄与しない（Eval.hpp が空を返す）。

### 3.3c 回転体と掃引（POV の sor / sphere_sweep 由来）

| `op` | パラメータ | 意味 |
|---|---|---|
| `Sor` | — | `children` は `SorPoint` をファイル順に。h に対する r² の 3 次（各区間は囲む 4 点を通る、実測で確定）を局所 Y 軸まわりに回転。両端の点は端の傾きだけを決め、表面上にない |
| `SorPoint` | `radius`, `height` | 制御点 1 つ |
| `SphereSweep` | `spline`（`"linear"`\|`"bspline"`、flags 経由） | `children` は `SweepPoint` をファイル順に。スプラインに沿って動く球の包絡。半径も中心と同じスプラインで補間 |
| `SweepPoint` | `x`,`y`,`z`,`radius` | 制御点 1 つ |

3.3b と同じく、`.gsf` からは決して生成されない語彙。制御点を外に置いても寄与しない。
定義の単一の出どころは `SorProfile.hpp` / `SweepProfile.hpp` で、
評価（Eval）・境界（Bounds）・GPU 焼き込み（Flatten / codegen）が同じ折れ線を読む。

### 3.4 綴りの訂正

Grasp3D のキー `"Shinness "` は**綴り誤り**。JSON では **`shininess`** に訂正する。

新しい形式に誤字を持ち込むと永久に残る（読み手は毎回「これは typo か仕様か」を判断させられる）。
変換は一方向（§1.3）なので、`.gsf` 側へ戻す必要もない。
訂正表は `Gsf2Json.SPELLING_FIXES` の 1 箇所にまとめてある。

キー名の機械変換規則：末尾空白を除去 → 空白区切りで lowerCamelCase 化。
ただし**全大文字トークンは丸ごと小文字化**する（`LIGHT_0` → `light_0`。
先頭 1 文字だけ小文字にすると `lIGHT_0` になる）。

### 3.5 対象外

| `op` | 扱い |
|---|---|
| `SuperEllipsoidBox` | **落とす**（[CSG_NODE.md](CSG_NODE.md) §4.4）。`"op": "Unsupported"` として
`"originalOp"` と共に残し、**件数を警告で報告する**。黙って消さない |
| `Label` | ツリー上の注記。ジオメトリを持たない。`op: "Label"` として保持し、評価時に読み飛ばす |
| `Light` / `Camera` | シーン記述の別セクションへ（版 1 では未対応。`Unsupported` 扱い） |

---

## 4. 読み込み時の構築

JSON（入れ子）→ `CsgNode` 平坦配列（[CSG_NODE.md](CSG_NODE.md) §2）への変換は読み込み側の責務。

1. 前順走査で `children` を平坦化し、`firstChild` / `childCount` を埋める
2. `nextId` を復元する
3. `op` 名を `uint8` に解決する
4. パラメータ名を `params[]` の位置に解決する

**ラウンドトリップの定義**: JSON → `CsgNode[]` → JSON が**意味的に一致**すること。
キー順序と浮動小数の表記揺れは許容する（`0.1` と `1.0e-01` を区別しない）。

---

## 5. 既存 `.gsf` での実測（Task #7）

`tools/gsf2json/` で Grasp3D の全 `.gsf` を変換した結果。**4 件とも成功、未対応要素ゼロ。**

| ファイル | ノード数 | マテリアル数 |
|---|---|---|
| `test.gsf` | 2 | 1 |
| `cutaway_bug.gsf` | 20 | 4 |
| `penrose.gsf` | 27 | 1 |
| **`pettobotoru.gsf`** | **87** | 6 |

### 5.1 op の出現頻度（全 4 ファイル合計）

| 回数 | op |
|---|---|
| 40 | `Translate` |
| 27 | `Torus` |
| 22 | `Box` |
| 12 | `Difference` |
| 9 | `Cylinder` |
| 8 | `Label` |
| 4 | `Merge` / `SceneRoot` |
| 3 | `Sphere` |
| 2 | `Cone` / `Rotate` / `Scale` |
| 1 | `Intersection` |

### 5.2 n 項グルーピングは実データに存在する

`pettobotoru.gsf` のブーリアン構造：

```
Merge 'Merge' children=2
    Difference children=2
      Difference children=2
        Merge children=9      ← n 項グルーピング
        Difference children=2
          Merge children=9    ← n 項グルーピング
  Intersection children=2
      Difference children=2
        Merge children=9      ← n 項グルーピング
```

**9 子の `Merge` が 3 箇所。** [CSG_NODE.md](CSG_NODE.md) §7 で設計した n 項グルーピングは、
理論上の可能性ではなく**実際のユーザーデータに存在する**。

平坦化で左寄せに二分化すればスタック深さ 9、平衡化すれば 4。**平衡化の効果が実データで確認できる。**

なお `Difference` は本モデルでは**全て二項**だった。n 項を許す設計は維持するが、
「実際には二項が主で、まとまりは `Merge` で表現される」というのが実際の使われ方らしい。

### 5.3 ⚠ 既存コーパスが**触れていない**要素

**`Plane` / `Disc` / `Triangle` / `SuperEllipsoidBox` は 4 ファイルのどれにも出現しない。**

帰結：

- **`SuperEllipsoidBox` を落とす判断（R-06）は、既存資産に対して損失ゼロ**であることが実測で確認できた
- ⚠ **`PatchSolid` の厚みゼロ面ルール（[CSG_NODE.md](CSG_NODE.md) §4.3）は、既存コーパスでは検証できない。**
  Phase 1 の Java 版との一致検証には、**Disc / Triangle / Plane を含む合成シーンを別途作る必要がある**

## 6. 版管理

`version` は破壊的変更でのみ上げる。キーの追加は上げない（読み手は未知キーを無視する）。
`version` が読み手の対応上限を超えていたら、**推測で読まずにエラーにする。**
