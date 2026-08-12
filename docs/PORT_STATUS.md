# 移植の進捗と検証状況

**最終更新**: 2026-08-10

---

## 1. Java 参照との一致検証【Phase 1 の中核】

`makina-core/tests/sdf_compare.cpp` が、Grasp3D の `SceneSdf` と C++ 版 `Eval.hpp` を突き合わせる。

| シーン | 由来 | サンプル | 結果 |
|---|---|---|---|
| `test.gsf` | Grasp3D 同梱 | 8,636 | ✅ 全一致 |
| `cutaway_bug.gsf` | 〃 | 4,994 | ✅ 全一致 |
| `penrose.gsf` | 〃 | 7,205 | ✅ 全一致 |
| `pettobotoru.gsf` | 〃 | 8,063 | ✅ 全一致 |
| `verify_faces.gsf` | **生成**（`tools/makescenes`） | 7,529 | ✅ 全一致 |
| `verify_plane.gsf` | **生成** | 8,459 | ✅ 全一致 |
| `verify_transforms.gsf` | **生成** | 6,251 | ✅ 全一致 |
| **合計** | | **51,137** | **相対許容 1e-5 で全一致** |

### 1.1 検証方法

**サンプル座標は Java 側が出力し、C++ はその座標で評価する。**
両言語で同じ格子を独立生成すると末尾ビットがずれ、検出した不一致が
「本当の相違」なのか「格子のずれ」なのか議論になるため。

格子は粗（±3.0 / 11 分割）＋密（±1.0 / 9 分割）の二段。
Grasp3D の既定プリミティブが原点付近の単位スケールに集まるので、密側が表面近傍を拾う。

### 1.2 許容誤差が必要な理由

**`CsgNode` はパラメータを `float` で持つが、Grasp3D は `double`。**
これは意図的な選択で（`GameMemory` が半分になり、GPU 側はどのみち float）、
有効数字が約 7 桁に落ちる。よって比較は相対誤差 1e-5。

CPU 評価器自体は `double`（`MK_FLOAT`）なので、誤差源はパラメータの格納精度のみ。

### 1.3 検証シーンは GUI ではなくプログラムで生成する

既存コーパスは `Plane` / `Disc` / `Triangle` に一切触れておらず、
`Rotate` も `Scale` も各 2 個しかなかった（[SCENE_FORMAT.md](SCENE_FORMAT.md) §5.1）。

`tools/makescenes/MakeVerifyScenes.java` が不足分を**プログラムで**組み立てる。
`.gsf` は `DefaultTreeModel` のシリアライズなので、GUI で作ったものと区別がつかない。
**手で作ったバイナリは差分が取れず、後から「どのケースを狙ったのか」が分からなくなる。**

| ファイル | 網羅する経路 |
|---|---|
| `verify_faces` | `Disc`（穴なし／穴あり）、`Triangle`（3 軸のどの平面にも乗らない傾き） |
| `verify_plane` | `Plane` 単体、`Difference(Box, Plane)`、`Intersection(Sphere, Plane)` |
| `verify_transforms` | `Rotate` X/Y/Z（各 37°）、**非一様 `Scale`(2, 0.5, 1.5)**、**負の `Scale`**、**負の高さ `Cone`**、入れ子変換 |

角度に 37° / 40° を選んでいるのは、0/45/90 だと**対称性の陰に誤りが隠れる**ため。

**これで全 op が検証済みになった。**

⚠ **2 つの実装が一致しても、シーンが意味のある形をしている保証にはならない。**
退化したシーン（線分に潰れた三角形、実質ゼロの回転）なら、両者が「同じ間違った値」で
一致してしまう。**生成したシーンは Grasp3D で開いて目視確認する**
（`verify_transforms` は確認済み）。

### 1.5 3 系統クロスチェック（Phase 5）

同じ形状を**互いにコードを共有しない 3 通り**で表現し、突き合わせる。

| 系統 | 何を答えるか | 実装 | 検証 |
|---|---|---|---|
| SDF | 表面までの距離 | `Eval.hpp`（式の min/max 合成） | `sdf_compare` — Java と 51,137 サンプル一致 |
| B-rep | 内か外か | `Bsp.hpp` + `Tessellate*.hpp`（BSP でポリゴンを切る） | `bsp_compare` — SDF と 157,642 サンプル一致 |
| レイトレ | 画素の色 | `Pov.hpp`（POV-Ray へのテキスト出力） | `pov_compare` — Java とトークン列 7,812 件一致<br>`silhouette-check` — **実レンダリングのシルエットが SDF と IoU 0.9949〜0.9996** |

**壊れ方が違う**のが要点である。SDF はブーリアンで「もはや距離でない数」を合成して間違え、
B-rep は同一平面上の面でポリゴンを落として間違える。**同じ間違いをしない 2 者が一致するなら、
それは偶然ではない。**

`bsp_compare` が数えない 3 種類（いずれも既知の近似を誤報にしないため）:

- **表面帯** — 分割は多面体なので真の面より内側に最大 1 サグ分入る（`tessellationError`）
- **かすめ光線** — ポリゴンの辺をかすめる光線は交差数の偶奇で判定できない
- **立体にならない木** — `Plane` を含むシーンには境界表現が存在しない。**黙って通さず skip と報告する**

さらに「**内部サンプルが 0 なら FAIL**」を入れてある。何も内側に落ちていなければ
「空っぽの空間は空っぽ」に両者が同意しただけで、比較が何も証明していない。
例外は `Disc` / `Triangle` / `Plane` だけのシーンで、これは SDF の契約上そもそも内部を持たない
（面に厚みは無い）ので skip と報告する。

#### シルエット一致（`spike/silhouette-check.bat`）

**同じシーン・同じカメラで、SDF レイマーチと POV-Ray に白黒のマスクを描かせて重ねる。**

| シーン | IoU | 差分 | 周長 |
|---|---|---|---|
| `test` | 0.99956 | 120 px | 1839 |
| `pettobotoru` | 0.99926 | 90 px | 1474 |
| `cutaway_bug` | 0.99919 | 99 px | 1793 |
| `hero_flange` / `hero_sweep_0..4` | 0.99885 | 110 px | 1100 |
| `penrose` | 0.99851 | 103 px | 1676 |
| `verify_transforms` | 0.99487 | 97 px | 1251 |

**色ではなくシルエットを比べる理由**：シルエットが依存するのは
ジオメトリ・変換・カメラ・**利き手**だけである。色はシェーディング・トーンマップ・光の単位にも
依存し、それらは 2 つのレンダラが一致するはずのないもので、比べても「調整の差」しか測れない。

逆に言えば、**この 1 つの数字が形状・変換・カメラ・利き手を同時に検査している**。
右手系のシーンを左手系のレンダラに渡していれば IoU はほぼ 0 になる。

**差分は必ず「マーチ側だけ」に出る**（POV 側だけの画素は全シーンで 0）。
レイマーチは `d < hitEps` で止まるので真の面より手前で当たったことにする。
シルエットが 1 画素弱ふくらむ。**系統的で説明が付く**ので、許容量は面積比ではなく
**周長の倍数**で指定する（既定 2.0 = 「一周ぐるりと 2 画素まで」）。実測は全シーン 0.1 画素相当。

**除外するもの**：

- `verify_plane` — POV に far 平面は無く、レイマーチには有る。無限平面は POV では画面を埋め、
  こちらでは far 距離の円で切れる。差分画像を見ると**境界がまさに far の円**だった。
  `--mask` は Plane を含むシーンの `.pov` を書かず、理由を出力する
- `verify_faces` — 厚み 0 の面だけなので平坦化後に描くものが無い

#### CI は「別のマシンでコンパイルできるか」だけを見る

`.github/workflows/ci.yml`。linux-gcc / windows-msvc / macos-clang の 3 本。

走らせるのは **Java も GPU も要らない検査だけ** — round trip、SDF↔B-rep、編集、コマンド層。
Grasp3D との一致も POV-Ray も GPU も runner には無く、
用意できないものを用意したふりをすると**見かけより意味の薄い緑のチェック**になる。
ワークフロー自身のログに「ここで見ていないもの」を書き出してある。

**それでも CI を置く価値がある理由**：`makina-core` は
「ヘッダーオンリー・標準ライブラリ以外に依存しない」と主張している。
これを正直に保つ唯一の方法は、**このマシンではない場所でコンパイルすること**である。

初回の実行で 1 件見つかった。`writeNode` が `ordered_json` で組んだノードを
`json` で返していて、**戻す途中でキー順が消えていた**（全ノードがアルファベット順になっていた）。
GCC では `push_back` があいまいになって落ちたが、MSVC は片方を選んで通していたので、
書き出しを実装して以来ずっとソートされていた。

### 1.4 サンプルは格子だけでなく表面帯からも取る

**格子はほぼ全ての点を表面から遠い場所に費やす。** そこでは 2 つの実装が
「どちらも大きな正の数を返している」だけで一致し、**間違った理由で検証が通る**。

意味のある食い違いは**符号が反転する場所**にある — ブーリアンの継ぎ目、一致した面、
トーラスの軸。よって 3 段構成にした：

| 段 | 内容 |
|---|---|
| 粗格子 | シーン境界 + 50% マージン、11 分割 |
| 密格子 | 中央半分、9 分割 |
| **表面帯** | 各種点を**勾配に沿って表面へ射影**し、着地点と**その内側・外側の対**を出力 |

表面帯の実装（`SdfDump.surfaceBand`）：

- 表面から遠すぎる種点は**歩かせない**。遠くから射影すると流れて任意の場所に着地し、
  「誰も選んでいないサンプル」になる
- 射影は勾配方向のニュートン反復（12 回上限）。収束しなければ捨てる
- 着地点 ±（対角の 0.1%）の 2 点も出すので、**両符号が必ず検査される**

これでサンプル数は 14,420 → **51,137**。増えた分は全て符号境界上にある。

---

## 2. 移植状況

| Java | C++ | 状態 |
|---|---|---|
| `SceneSdf.primSdf` | `makina/Sdf.hpp` | ✅ 完了・検証済み |
| `SceneSdf.eval` / `evalNode` / `invApply` | `makina/Eval.hpp` | ✅ 完了・検証済み |
| `.gsf` の読み込み | `tools/gsf2json` (Java) → `makina/SceneJson.hpp` | ✅ 完了 |
| `SceneBounds` | `makina/Bounds.hpp` | ✅ 完了・検証済み（**理想化あり** → §4.1） |
| `SceneSdf.evalNode` の `Label` 扱い | `makina/Fidelity.hpp` | ✅ **理想化あり** → §4.3 |
| — | `makina/Flatten.hpp` | ✅ 平坦化（RPN・変換焼き込み・平衡二分化）。木と全点一致を検証 |
| — | `app/reflect_bridge.hpp` | ✅ **`Scene` が MitiruEngine の `GameMemory` 契約を満たすことを検証**（§5） |
| `CsgBsp` | `makina/Bsp.hpp` | ✅ 完了・検証済み |
| `CsgTess` | `makina/Tessellate.hpp` + `makina/TessellateScene.hpp` | ✅ 完了・検証済み |
| `SceneSdf.localSurface` / `surfaceSamplesBudget` | `makina/Surface.hpp` | ✅ 完了・検証済み |
| `SceneMeasure` | `makina/Measure.hpp` | ✅ 完了・検証済み |
| POV-Ray 出力（各要素の `povray()` + `PovExporter.traverse`） | `makina/Pov.hpp` + `makina/PovShape.hpp` | ✅ 完了・検証済み（トークン列一致 7,554 件） |
| `PatchSolid` | — | 未着手（⚠ 落とすと CSG が壊れる） |
| `SuperEllipsoidBox` | — | **スコープ外**（R-06） |

---

## 5. `GameMemory` 契約の検証（Task #6）

`app/reflect_check` が、`makina::Scene` を MitiruEngine の `GameMemory` として使えることを確認する。
**エンジン本体はビルドしない** — CEF・vcpkg・submodule が要るうえ、
**実際に壊れうるのは型の契約であり、それはヘッダだけで決まる**ため。

```
field        type     elem             offset  elemSize  capacity count?
nextId       u32      -                     0         4         1     no
nodes        vec      makina::CsgNode       16        64       256    yes
materials    vec      makina::Material    16404        40        64    yes
names        vec      makina::NameSlot    18968        32       256    yes

hero_flange.makina.json   nodes=25  materials=1  (host reads 25)
pettobotoru.makina.json   nodes=87  materials=6  (host reads 87)
```

### 5.1 `FixedArray` と継ぎ目の設計

`makina-core` はエンジンに依存できない（PLAN.md §3.1）ので、エンジンの `FixedVec` は使えず、
自前の `FixedArray` を持つ。両者は型として無関係。

**継ぎ目はアプリ側に置く。** `app/reflect_bridge.hpp` がエンジンの `IsFixedVec` トレイトを
`makina::FixedArray` に特殊化する。これだけでよい理由は、
**`makeFieldDescriptor` が `offsetof(M, count)` を makina 自身の型に対して計算する**から。

→ **レイアウトの一致は不要で、`count` という名前のメンバがあればよい。**
しかもそれはコンパイル時に検査される。

### 5.2 なぜライブ長が要るのか

生配列のままだと、**23 ノードのモデルが 256 要素として報告され、233 個はゼロ埋め**になる。
シーンを読む側（インスペクタ、AI エージェント）はどれが実体かを推測させられ、
読み取り予算も無駄になる。

同じ理由で `ReflectName` を宣言している。これがないと要素型名が空になり、
**87 個の「名前のないバイト塊」**として見える。名前があって初めて
「`ケーブル穴` という名前の `Difference`」が読み手の作れる文になる。

---

## 4. 意図的な理想化（PLAN.md D-11）

Grasp3D の不合理はそのまま持ち込まない。ただし**出力が変わるかどうかで扱いを分ける**。

### 4.1 (B) 出力が変わる — `Difference` / `Intersection` の境界を厳密化

Grasp3D の `SceneBounds` は `Difference` と `Intersection` を**単なるコンテナ**として扱い、
境界を子の和で見積もる。自身のコメントも "estimated conservatively" と認めている。

**Makina は演算子を尊重する：**

| 演算 | Grasp3D | Makina | 根拠 |
|---|---|---|---|
| `Difference` | 全子の和 | **第 1 子のみ** | `A - B ⊆ A` |
| `Intersection` | 全子の和 | **子の箱の積** | `A ∩ B ⊆ box(A) ∩ box(B)` |
| `Merge` | 全子の和 | 同じ | — |

**どちらも厳密に狭く、かつ常に真の結果を含む**ので、緩い箱で正しかったものが狭い箱で
誤りになることはない。後で効く：この箱はサブツリーのカリングと計測コマンドが使う基準であり、
**何も寄与しない刃で膨らんだ箱は決してカリングしない**。

**検証**（`sdf_compare` が毎回実行）:

1. 狭い箱 ⊆ 緩い箱（理想化が箱を広げていないこと）
2. **評価器が「内部」と判定した全サンプルが狭い箱に入っていること**（4 シーンとも合格）

⚠ **`BoundsResult::primitiveCount` の意味が変わっている。** 狭い側では
刃を訪問しないため、シーンが持つプリミティブ数より少なくなる（例: `pettobotoru` は
87 ノード中 7 個）。モデルの規模を知りたいなら `Scene::nodeCount` を見ること。

#### 二つの理想化を切り替える口 — `Fidelity`

§4.1 と §4.3 はどちらも「サブツリーに何が含まれるか」の判断で、**表面サンプルの本数に届く**。
§4.1 は境界箱を変え、境界箱は `filterEps`（対角長 × 1e-3）を決め、`filterEps` は
どこまで表面に近い点を残すかを決める。§4.3 は `eval` の答えそのものを変える。
つまり**理想化を入れたまま Java と突き合わせると、全シーンでサンプル数が食い違い、
比較が計測コードについて何も言わなくなる**。

そこで両方を `makina/Fidelity.hpp` の 2 フィールドにまとめ、`makina::kGrasp3D` で参照実装の
答えを選べるようにした。既定は Makina の答えで、`kGrasp3D` を渡すのは `measure_compare` /
`sdf_compare` だけである。**散らばった `if` として置かない**のが要点で、そうすると誰も
逸脱の一覧を作れなくなる。

**理想化を迂回して終わりにはしない**：`measure_compare` は毎回「Makina の箱 ⊆ 参照の箱」を
全直下子で検査する（3 軸 × 子数）。

### 4.3 (B) 出力が変わる — `Label` の配下は立体である

Grasp3D は**自分自身と食い違っている**。

| 消費側 | `Label` 配下を | 
|---|---|
| GL 描画（`MyGLEventListener`） | **描く**（`Label.render` は空だが子には降りる） |
| POV 出力（`PovExporter.traverse`） | **書き出す**（コメント `/* label: ... */` を出したうえで子に降りる） |
| `SceneBounds.accum` | **数える**（`Label` の分岐がそもそも無い） |
| `SceneSdf.evalNode` | **無視する**（`+∞` を返して降りない） |

3 対 1 で、しかも「無視する」側は後から足された評価器だけである。
決め手は `pettobotoru.gsf` で、**ボトルの溝の 1 本が `Label → Translate → Torus` の下にある**。
エディタ上でその溝は見えているので、作者は隠したつもりではない。

**Makina は多数側に合わせる。** `Label` は注釈付きのコンテナで、配下は立体の一部。

ただし **`Difference` / `Intersection` の被演算子としては読み飛ばす**。これは Grasp3D の
評価器の規則をそのまま採る — コメントは本体でも刃でもない。`Label` を演算子の子に置いたときに
黙って刃になる方が明らかに危険である。

**検証**（`labelcheck` で確認、`sdf_compare` が毎回実行）:

半径 0.97 の直線上で `y ∈ [0.68, 1.17]` の 8 点が両者で符号ごと食い違う。
Grasp3D の評価器は `-0.03`（壁の内側＝溝が無い）、Makina は正（溝が彫れている）。
GL と POV はこの溝を描くので、Makina が正しい。

### 4.2 (A) 出力が変わらない整理

一致検証が緑のまま通ることが「出力を変えていない」証明になっている（8,240 サンプル）。

| 対象 | 変更 | 根拠 |
|---|---|---|
| `Cone` | 9 パラメータ → **`radius` / `height`** | `Cone.render()` が読むのはこの 2 つだけ（§3.1） |
| `Disc` / `Triangle` | `thickness` を**削除** | どこからも読まれない。`PatchSolid` は厚みをサイズから算出する |
| マテリアル | `Shinness` → **`shininess`** | Grasp3D 側の綴り誤り |
| キー全般 | 末尾空白除去、`LIGHT_0` の命名正規化 | — |

> **安全網は残す。** `Cone` の死にプロパティは、デフォルトコンストラクタでは**書き込まれる**
> （`Cone(radius, height)` では書き込まれない）。よって `.gsf` によっては存在しうる。
> `Gsf2Json.DEAD_KEYS` が拾って件数を報告する。**黙って落とさない。**

---

## 3. 移植中に判明した Grasp3D の実装事実

移植の忠実さに直結するので記録する。**いずれも「バグ」ではなく、
コードを読めば確認できる事実**。

### 3.1 `Cone` の 9 パラメータのうち 7 個は死にデータ

`Cone.render()` が読むのは **`Radius1` と `Z2` だけ**。他は全てコメントアウト済み：

```java
double r1, z2;
//x1 = ((Double)properties.getValue(KEY_X1)).doubleValue();
r1 = ((Double)properties.getValue(KEY_RADIUS1)).doubleValue();
//r2 = ((Double)properties.getValue(KEY_RADIUS2)).doubleValue();
z2 = ((Double)properties.getValue(KEY_Z2)).doubleValue();

gl.glRotated(-90,1.0,0.0,0.0);
glut.glutSolidCone(r1, z2, 24, 24);
```

つまり Cone は**底面 y=0・頂点 y=Z2 の Y 軸円錐**であり、
`X1/Y1/Z1/X2/Y2/Radius2/Open` は第 2 端点ではない。
`SceneSdf` はこれに正確に一致している。**読んでしまうと描画と食い違う。**

### 3.2 `sign()` はゼロを正として扱う

```java
private static double sign(double v){ return v<0 ? -1.0 : 1.0; }
```

三値の符号関数（0 のとき 0）に置き換えると、`udTriangle` の
`s < 2.0` 判定で辺の平面上ちょうどの点の扱いが変わる。**二値のまま移植する。**

### 3.3 プロパティキーの照合は両側 trim

```java
if(k.equals(key) || k.trim().equals(key.trim())){
```

Grasp3D の内部キーは `"Radius "` のように末尾に空白があるが、
参照側は `"Radius"` で引く。JSON では末尾空白を除いた lowerCamelCase を使う。

### 3.4 変換ノードは子を union するコンテナ

`Rotate` / `Translate` / `Scale` はジオメトリを持たないが、
**逆変換を適用したうえで子を union する**。

### 3.5 プリミティブも子を持てる

```java
double d = POSITIVE_INFINITY;
if(uo instanceof Primitive){ ... d = pd * scale; }
double dc = unionChildren(nd, p, scale);
return Math.min(d, dc);
```

Box の下に子がぶら下がっていれば、**自身の表面と子の union の最小値**になる。

### 3.6 `Intersection` は空の子を読み飛ばす

```java
if(e == Double.POSITIVE_INFINITY) continue; // skip empty children
```

空の子を通すと積が全消滅するため。`Difference` の刃も同様に `Label` を除外する。
