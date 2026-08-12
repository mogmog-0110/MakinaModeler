# Grasp3D 側に入れた変更

移植の過程で Grasp3D 本体に手を入れた分の記録。**参照実装としての出力を変える変更は入れていない**
— 入れると、移植の一致検証が「同じ人が同じ日に両側を書き換えた結果」になってしまう。

---

## 1. `PovExporter` を切り出した（新規ファイル）

**前**: POV 出力の木走査は `GRASP_MAIN.ExportAction`（Swing の `AbstractAction`）の
`private` メソッド群だった。呼ぶにはアプリ全体を起動するしかない。

**後**: `PovExporter.java` に移した。`GRASP_MAIN.ExportAction.traverse` は 1 行の委譲になった。

移したもの: `traverse` (4 引数 / 5 引数)・`countPovObjects`・`firstPrimitive`・`cutterMaterial`。
どれも GUI の状態に触っていなかったので、そのまま `static` にできた。

**なぜ**: 出力自体は GUI 無しで走らせたいものだから（バッチ変換、他レンダラとの突き合わせ）。
実際に `makina/tools/povdump` がこれを呼んでいて、Makina の POV 出力の参照になっている。

**出力は 1 バイトも変わらない。** ロジックはそのまま移しただけ。

---

## 2. `RenderableSceneElement.CR` / `TB` をクラスロード時に確定させた

```java
// 前
protected static byte CR[] = null;
protected static byte TB[] = null;
public RenderableSceneElement(String s){ super(s); CR = "\n".getBytes(); TB = "\t".getBytes(); }

// 後
protected static final byte CR[] = "\n".getBytes(StandardCharsets.UTF_8);
protected static final byte TB[] = "\t".getBytes(StandardCharsets.UTF_8);
public RenderableSceneElement(String s){ super(s); }
```

**なぜ**: `.gsf` から読み戻したシーンは**逆シリアル化**で作られ、コンストラクタは走らない。
GUI 経由なら他の要素がどこかで構築されるので偶然埋まっていたが、
「ファイルを読んで書き出すだけ」のツールでは `null` のままで、最初の `povray()` の中で
`NullPointerException` になった（`SceneRoot.povray:98`）。

これは静的状態をコンストラクタで初期化していたことの帰結で、初期化のタイミングが
インスタンス生成に依存していた。定数なのだからクラスロード時に決めてよい。

---

## 3. 見つけたが**直していない**もの

### 3.1 `SceneSdf.evalNode` の `Label` 扱い（Grasp3D 内部の不整合）

`SceneSdf.evalNode` は `Label` に当たると `+∞` を返して配下に降りない。
一方 GL 描画も POV 出力も `SceneBounds` も配下を見る（PORT_STATUS.md §4.3）。
`pettobotoru.gsf` ではボトルの溝が 1 本 `Label` の下にあり、
**評価器だけがその溝を知らない**。

Makina 側は多数側に合わせた（`Fidelity::labelsAreGeometry`）。
Grasp3D 側は**あえて直していない**：参照実装を治療と同時に動かすと、
一致検証が何も測らなくなる。直すなら移植の検証が終わってから、単独の変更として。

### 3.2 `CsgTess` / `CsgPreview` は未調査

CSG プレビューのメッシュ化。Makina は SDF を直接レイマーチするので移植対象ではないが、
`CsgBsp` を第 2 実装として使うときに関係してくる可能性がある。
