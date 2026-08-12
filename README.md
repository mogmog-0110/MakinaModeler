# Makina

CSG ソリッドモデラー。距離場（SDF）を直接レイマーチする DX12 レンダラと、
**形状そのものから経年変化を導出する**マテリアルを持つ。

教育用ツール Grasp3D（Java / JOGL / POV-Ray）の C++ 移植から始まり、
移植の一致検証をそのまま「3 通りの独立実装が同じ形を描く」証明に使っている。

自作ゲームエンジン **MitiruEngine** の姉妹プロジェクト。
ここで作ったモデルはエンジン側で描画にも衝突判定にも使える（同じ距離場から出る）。

---

## 1. 何が新しいのか

### 1.1 経年変化がテクスチャではない

摩耗も汚れも埃も、**距離場から計算される**。UV も、ベイクも、テクスチャも無い。

| マスク | 何から出るか |
|---|---|
| 摩耗（磨かれた縁） | 正のカーバチャ × 露出（AO） |
| 汚れ（溜まる） | 低い AO × 非凸 |
| 汚れ（流れ落ちる） | 上向きへのソフトシャドウ（空遮蔽）× 非水平 |
| 埃 | 上向き × 開けている × 摩耗していない |
| 透過 | 薄さ（内向きマーチ） |

**形を変えると、同じフレームで摩耗が付いて回る。**
溝の半径を動かすと汚れの溜まる場所が動き、溝がボルト穴と交わればその交点に汚れが溜まる。
ベイクしたテクスチャには言えない主張である。

詳細: [docs/WEATHERING.md](docs/WEATHERING.md)

### 1.2 AI が自分の仕事を数値で確かめられる

```
makina_edit describe <scene.json>     ツリー（id と名前付きパラメータ）
makina_edit apply    <scene.json> <commands.json> -o <out.json>
makina_edit measure  <scene.json>     隙間・干渉・浮き・対称性
```

絵しか見られないエージェントは「動かしたボスがボアを避けているか」を推測するしかない。
`measure` を叩けるエージェントは数字を得る。

巻き戻しは**厳密**。シーンは 1 個の trivially-copyable な struct なので、
スナップショットはコピーそのもので、コマンドに逆操作を持たせる必要が無い。

詳細: [docs/AGENT_API.md](docs/AGENT_API.md)

---

## 2. 正しさをどう示しているか

同じ形状を**互いにコードを共有しない 3 通り**で表現して突き合わせる。

| 系統 | 何を答えるか | 検証 |
|---|---|---|
| SDF | 表面までの距離 | Java 参照実装と 51,137 サンプル一致 |
| B-rep（BSP） | 内か外か | SDF と **156,932 サンプル一致** |
| レイトレ（POV-Ray） | 画素 | SDF と**シルエット IoU 0.9949〜0.9996** |

**壊れ方が違う**のが要点である。
SDF はブーリアンで「もはや距離でない数」を合成して間違え、
B-rep は同一平面上の面でポリゴンを落として間違える。同じ間違いはしない。

シルエットを比べるのは、それが**ジオメトリ・変換・カメラ・利き手だけ**に依存するから。
1 つの数字で 4 つを同時に検査できる。右手系のシーンを左手系で描いていれば IoU はほぼ 0 になる。

```bash
verify-all.bat
```

参照ダンプの生成から GPU の突き合わせまで一括で走る。最後に `everything agrees` が出る。

詳細: [docs/PORT_STATUS.md](docs/PORT_STATUS.md)

---

## 3. 構成

```
makina-core/     ヘッドレス。GPU もエンジンも知らない。ヘッダーオンリー
  include/makina/
    Sdf.hpp        C++ と HLSL が共有するプリミティブ距離関数（1 つの定義）
    Eval.hpp       CPU 評価器
    Flatten.hpp    ツリー → RPN 評価プログラム（変換の焼き込み・平衡二分化）
    Bounds.hpp     ブーリアンを尊重した AABB
    Measure.hpp    隙間・干渉・浮き・対称性
    Bsp.hpp        BSP ブーリアン（第 2 実装）
    Pov.hpp        POV-Ray 出力（第 3 実装）
    Edit.hpp       id 指定のツリー編集
    History.hpp    スナップショット履歴
    Command.hpp    JSON コマンド
    Fidelity.hpp   Grasp3D と意図的に食い違う点、2 つだけ
  tools/makina_edit.cpp

spike/           DX12 レンダラ（レイマーチ・ジオメトリフィールド・経年変化）
tools/           Grasp3D から参照ダンプを吐く Java ツール群
docs/
```

**依存はこの向きにしかない**: `makina-core ← MitiruEngine`、`makina-core ← Makina`。
`makina-core` は標準ライブラリ以外に依存しないので、テストにデバイスもウィンドウも要らない。

---

## 4. 進捗

| Phase | 内容 | 状態 |
|---|---|---|
| S | 技術検証スパイク | ✅ コード生成で 4.9 倍、インタプリタ案を棄却 |
| 0 | 型設計・シーン記述 | ✅ |
| 1 | makina-core | ✅ |
| 2 | SDF レイマーチ（DX12） | ✅ |
| 3 | アプリ（CEF）と操作性 | 未着手 |
| 4 | 経年変化 | ✅ |
| 5 | 3 系統クロスチェック | ✅ CI 化を除く |
| 6 | エンジン統合 | CPU 側 ✅ / GPU パス未着手 |
| 7 | AI 編集 | コア側 ✅ / ブリッジは Phase 3 待ち |
| 8 | Vulkan | 未着手 |

計画と決定記録: [PLAN.md](PLAN.md)

---

## 5. ビルド

Windows / MSVC 14.51 / CMake + Ninja / DXC。

```bash
makina-core\build-and-test.bat
```

```bash
spike\build.bat
```

```bash
spike\build\bin\render_scene.exe --weathered tools\gsf2json\out\hero_flange.makina.json
```

参照ダンプの再生成には JDK 22 と Grasp3D のビルド済みクラス（`D:\sandbox\Grasp3D\bin`）が要る。
シルエット比較には POV-Ray（`Grasp3D\povray\bin\povray.exe`）が要る。
