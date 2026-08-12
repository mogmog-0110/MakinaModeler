# CsgNode 型の確定（D-02 / Task #2）

**調査日**: 2026-08-10
**前提**: 右手系・Y-up・度・メートル（[COORDINATES.md](COORDINATES.md)）

**結論**: `params[8]` では**足りない**。`params[12]` に拡張する。
`SuperEllipsoidBox` は**スコープ外に落とす**（R-06 クローズ）。

---

## 1. Grasp3D 側の実測パラメータ

`src/*.java` の `KEY_*` を全数え上げ。

| プリミティブ | オーサリング側パラメータ | 個数 |
|---|---|---|
| `Box` | X1,Y1,Z1,X2,Y2,Z2（対角 2 点で AABB） | 6 |
| `Sphere` | Radius | 1 |
| `Cylinder` | Cap point, Base point, Radius（Y 軸沿い） | 3 |
| **`Cone`** | X1,Y1,Z1,Radius1, X2,Y2,Z2,Radius2, Open | **9** |
| `Torus` | Major radius, Minor radius | 2 |
| `Plane` | Y（半空間 y ≤ Y） | 1 |
| `Disc` | Radius, Hole radius, Thickness | 3 |
| **`Triangle`** | X1,Y1,Z1,X2,Y2,Z2,X3,Y3,Z3, Thickness | **10** |
| `SuperEllipsoidBox` | NURBS 制御点（`Controll point interval` ほか、842 行） | — |

| 変換ノード | パラメータ | 備考 |
|---|---|---|
| `Translate` | X, Y, Z | 3 |
| **`Rotate`** | **axis（X/Y/Z のいずれか 1 つ）+ Degree** | **Euler ではなく単軸** |
| **`Scale`** | X, Y, Z | **非一様** |

### 1.1 これで `params[8]` は否決

`Triangle` が 10、`Cone` が 9。**当初の D-02 案は Grasp3D の 2 プリミティブを表現できない。**

`Cone` は `Open` をフラグに逃がせば 8 に収まるが、`Triangle` は逃がせない。

---

## 2. 確定する型

```cpp
struct CsgNode {
    std::uint32_t id;             // 不変 ID。添字と違い、追加削除でずれない（§6）
    std::uint8_t  op;             // プリミティブ / 変換 / ブーリアン
    std::uint8_t  materialId;
    std::uint16_t flags;          // Rotate の軸、Cone の Open、Disc/Triangle の patch 判定など
    std::uint16_t firstChild;     // ポインタではなく添字
    std::uint16_t childCount;     // n 項。Difference は [0] が本体、[1..] が刃（§7）
    std::uint16_t nameId;         // 名前テーブルへの添字（設計意図の保存）
    std::uint16_t _pad;
    float         params[12];     // op ごとに意味が変わる。最大は Triangle の 10
};                                // ヘッダ 16 B + params 48 B = 64 バイト
```

`op` に 1 バイト割いた理由：Grasp3D の要素は 9 プリミティブ + 3 変換 + 3 ブーリアン + Label 等で
すでに 16 を超える。将来 `SmoothUnion` などを足す余地も要る。

`flags` を新設した理由：`Rotate` の軸（3 値）と `Cone.Open`（bool）を `params` に float として
持たせるのは、**float に離散値を入れて後で `== 1.0f` で比較する**という典型的な事故のもとになる。

### 2.1 GameMemory サイズ再計算

| 項目 | 計算 | 結果 |
|---|---|---|
| `CsgNode` | ヘッダ 16 + params 48 | **64 B** |
| `FixedVec<CsgNode, 256>` | 64 × 256 + ヘッダ | **16 KB** |
| 名前 `FixedVec<FixedString<32>, 256>` | 32 × 256 | 8 KB |
| マテリアル `FixedVec<Material, 64>` | 仮 64 B × 64 | 4 KB |
| **GameMemory 合計** | | **約 28 KB** |
| 巻き戻し 10 秒 @60fps | 28 KB × 600 | **約 17 MB** |

**上限 32 MB に対して余裕あり。**

ヘッダをちょうど 16 B にしたので `CsgNode` は 64 B ぴったりに落ちる。
**不変 ID（§6）はこのアラインメントの隙間に無償で収まった** — `params[12]` にした時点で
どのみち 60 B → 64 B にパディングされるため、ID を足してもサイズは変わらない。

> トランスフォームを各ノードに 4x3 行列で inline する案は依然として否決。
> それをやると 1 ノード 100 B を超え、巻き戻しが 30 MB に迫る。

---

## 3. 評価プログラム側は別物（D-01）

GPU が歩く側は**正規化**する。オーサリング側の 10 パラメータは、変換を焼き込めば 4 以下になる。

| プリミティブ | 正規化後 | 個数 |
|---|---|---|
| Box | half-extents | 3 |
| Sphere | radius | 1 |
| Cylinder | radius, halfHeight | 2 |
| Cone | r1, r2, halfHeight（+ open フラグ） | 3 |
| Torus | major, minor | 2 |
| Plane | —（半空間、位置は変換へ） | 0 |
| Disc | radius, hole, thickness | 3 |
| Triangle | 正準配置（v0 を原点、v1 を +X 上、v2 を XY 平面）→ b, cx, cy, thickness | 4 |

スパイクの `NodePayload`（`params[4]` + 逆変換 3 行）は**このまま使える**。

---

## 4. 判明した設計上の落とし穴

### 4.1 Scale が非一様 →「距離を戻す」ことができない【重要】

スパイクは一様スケールを前提に `d * scale` で世界距離へ戻していた。
Grasp3D の `Scale` は **X/Y/Z 独立**なので、この方法は使えない。

Java 版 `SceneSdf.scaleFactor()` は **最小の軸倍率**を使う：

```java
return Math.min(sx, Math.min(sy, sz));   // 絶対値
```

これは真の距離の**保守的下界**（クラス doc にも「magnitude may be a conservative lower bound」と明記）。
スフィアトレースには**安全**（過大評価しないので貫通しない）だが、**歩幅が縮んでステップ数が増える**。

**決定**: Java 版と同じ最小軸方式を採用する。理由は 2 つ。
1. Phase 1 の完了条件が「Java 版と C++ 版で SDF 評価が一致」なので、**式を変えたら検証できない**
2. 安全側に倒れているので絵は壊れない

⚠ **非一様スケールを多用したモデルは遅くなる**ことを既知のコストとして記録する。
Phase 2 の性能測定は、一様スケールのシーンだけで測ると楽観的な数字が出る。

### 4.2 Rotate は単軸。スパイクは過剰実装だった

`SceneSdf.invApply` の実装：

```java
String axis = SceneBounds.axisOf(props);        // "X" | "Y" | "Z"
double a = -Math.toRadians(SceneBounds.num(props,"Degree",0));
```

**軸 1 つ + 角度 1 つ。** スパイクの `csg_scene.hpp` は Euler XYZ（`rotationDeg` が Vec3）で
実装したが、**移植元にその概念がない**。オーサリング側は単軸で持つ。

（複数軸の回転が要るなら `Rotate` ノードを重ねる。Grasp3D がそうしている。）

### 4.3 Disc と Triangle は厚みゼロ → CSG に入れると壊れる

`PatchSolid.java` のルール：

- Disc / Triangle は**面**であって内外を持たない（POV-Ray でいう patch object）
- POV-Ray は intersection / difference で拒否する（"Patch objects not allowed in intersection."）
- BSP プレビューも閉じた立体を前提にしており受け付けない
- **対策**: CSG の中に置かれた面には**サイズの 2%（下限 1e-4）の厚み**を与えて立体として扱う。
  面はそのスラブの中央（±thickness/2）に置くので、見た目は動かない。
  単独で置かれた面は厚みゼロのまま従来どおり描く

**決定**: このルールをそのまま移植する。**移植しないと CSG が壊れる。**
`flags` に「CSG 配下か否か」を持たせるのではなく、**平坦化時に判定して厚みを焼き込む**
（オーサリングツリーは厚み 0 のまま保つ＝ユーザーが見ている値と一致する）。

### 4.4 SuperEllipsoidBox は落とす【R-06 クローズ】

- 842 行、NURBS 制御点ベース（`jogl/glu/Nurbs.java` に依存）
- **閉形式の SDF が書けない**
- **Java 版 `SceneSdf.supported()` も対象外**にしている：

```java
return p instanceof Box || p instanceof Sphere || p instanceof Cylinder || ...
// SuperEllipsoidBox は含まれない
```

つまり **Grasp3D 自身が gap/overlap/floating/symmetry の数値検証から除外している**。
移植で落としても、既存機能に対する後退にはならない。

**決定**: スコープ外。`.gsf` に含まれていたら読み飛ばし、**警告を出して件数を報告する**
（Java 版の `countUnsupported` と同じ思想）。黙って消さないこと。

---

## 6. ノード ID の不変性（R-07 / Task #3）

**決定**: **`std::uint32_t id` を持たせる。単調増加、再利用しない。**

### 6.1 なぜ必要か

配列添字はノードの追加削除でずれる。一方、以下はすべて**ずれない参照**を要求する：

- **PR 差分でのモデルレビュー**（`[[メッシュを持たないモデラー]]` の訴求の一つ）
- **AI からの編集**（「さっき言及したあのノード」を指し続ける必要がある）
- 将来のアニメーション参照・マテリアル割り当て

### 6.2 コストはゼロだった

`params[12]` にした時点で構造体は 60 B → 64 B にパディングされる。
**その隙間に `uint32` がちょうど収まる。** サイズは 64 B のまま変わらない。

### 6.3 割り当て規則

- `GameMemory` に `std::uint32_t nextId` を持ち、単調増加で払い出す
- **削除された ID は再利用しない。** 再利用すると、古い参照が別のノードを指してしまい、
  「壊れた参照」より質の悪い「静かに間違った参照」になる

### 6.4 巻き戻しとの相互作用【既知の穴】

`nextId` も `GameMemory` の一部なので、**巻き戻すと `nextId` も巻き戻る**。
その後に新しいノードを作ると、破棄された未来で使われた ID が再び払い出される。

- **内部的には問題ない。** 破棄された未来は存在しなかったことになるので、整合している
- ⚠ **外部に持ち出された参照は壊れうる。** 巻き戻し前に AI が握った ID、
  あるいは書き出し済みの参照が、別のノードを指す可能性がある
- **対策は今はしない。** 実際に問題になってから、セッション ID との組で
  修飾するなどの手を打つ。ここで先回りすると複雑さだけが残る

---

## 7. n 項ノードとグルーピング（Task #4）

**決定**: **Grasp3D の n 項構造をそのまま保つ。二分化は平坦化時にのみ行い、必ず平衡化する。**

### 7.1 グルーピングは既に存在した

Phase S の教訓として「左寄せ退化木になるとサブツリー単位の最適化が不可能になる」と書いたが、
**それはスパイクのシーン生成器が二分チェーンを作っていたからで、Grasp3D の構造ではなかった。**

`SceneSdf.evalDifference`：

```java
// Difference: the first real child is the body, the rest are blades. d = max(d0, -min(blades))
```

`Difference` も `Intersection` も **n 項**。子を順に走査して畳み込む。

つまり**「この 8 個の穴をまとめて差分する」は、既に 1 ノード 9 子で表現されている。**
グルーピングを新たに発明する必要はなく、**壊さないようにするだけでよい。**

### 7.2 オーサリング側は n 項のまま

`childCount` は n を取る。`Difference` は `children[0]` が本体、`children[1..]` が刃。
Grasp3D と同じ意味論なので、`.gsf` からの変換が素直になり、
UI のツリー表示も Grasp3D と同じ形になる。

### 7.3 二分化は平坦化時のみ、かつ平衡に

GPU のスタックは二項なので、RPN に落とす時点で二分化が要る。ここで**やり方が効く**：

| 二分化 | 刃 n 個のスタック深さ | サブツリー AABB |
|---|---|---|
| 左寄せチェーン | **n** | 意味を持たない（毎回「今までの全部」） |
| **平衡木** | **⌈log₂ n⌉ + 1** | **刃のまとまりごとに意味を持つ** |

**平衡二分化を採用する。** 得られるもの：

1. **スタック深さが対数になる。** `MAX_STACK 16` で子 32768 個まで耐える。
   Phase S で懸念した `alloca` のサイズ問題も、これで実質的に消える
2. **サブツリー AABB が意味を持つ。** 刃 8 個のまとまりに対して 1 つの箱が定まる。
   Phase S でノード単位カリングが失敗したのは「1 テストで 1 ノードしか飛ばせない」からだった。
   **平衡木ならこの前提が変わる**（ただし再検証が必要。効くと決めつけない）

`Difference` の刃は `min` で畳むので、**刃どうしは平衡 min 木にできる**：

```
max(body, -min(min(b1,b2), min(b3,b4)))
```

### 7.4 やらないこと

- **オーサリングツリーを自動で平衡化しない。** ユーザーが並べた順序と構造は設計意図であり、
  勝手に組み替えると `[[設計意図がモデルに残る]]` という主張が崩れる。
  平衡化は**評価プログラム側の内部都合**に留める

---

## 8. PLAN.md への反映

- **D-02**: `params[8]` → **`params[12]`**、`flags` と不変 `id` を新設、ノード 40 B → **64 B**、
  GameMemory 約 22 KB → **約 28 KB**、巻き戻し 10 秒 13 MB → **17 MB**
- **R-06（SuperEllipsoidBox）**: ✅ **クローズ。落とす。**
- **R-07（ノード ID）**: ✅ **クローズ。`uint32 id` を持たせる。コストはパディングに吸収されてゼロ**（§6）
- **新規リスク R-13**: 非一様 Scale による歩幅低下（§4.1）
- **Phase 1 の移植項目に `PatchSolid` を明記**（§4.3。落とすと CSG が壊れる）
- **Phase 0 の「グルーピングを保つ」要件**: ✅ **達成済み。Grasp3D が既に n 項なので、
  保つ＝壊さない、で足りる**（§7）。平衡二分化は評価プログラム側の内部都合として実装する
- **Phase S の「サブツリー単位スキップは平衡化とセットでないと無意味」という保留**:
  平衡二分化により前提が変わる。**Phase 2 で再検証の価値あり**（ただし効くと決めつけない。
  ノード単位カリングは実測でゼロだった）
