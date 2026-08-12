# 座標系規約（R-01 / Task #1）

**調査日**: 2026-08-10
**結論**: **右手系・Y-up。Grasp3D と MitiruEngine の主経路は既に一致しており、座標変換は不要。**
ただし**投影行列の深度レンジだけは選択を誤ると壊れる**。

---

## 1. 実測結果

| 系 | 手系 | 上方向 | 深度レンジ | 根拠 |
|---|---|---|---|---|
| Grasp3D GL ビュー | **右手系** | +Y | GL [-1,1] | `SceneMatrix` / `SceneBounds` / `SceneSdf` に「right-handed, matching glRotate」と明記 |
| Grasp3D POV 出力 | **右手系** | +Y | — | `PovCamera.writeCamera` が `right<-imageAspect,0,0>` と `sky<0,1,0>` を書く |
| MitiruEngine 主経路（`GlmBridge`） | **右手系** | +Y | **[0,1]** | `glm::lookAtRH` + `glm::perspectiveRH_ZO`（ADR 0029 参照） |
| MitiruEngine `sgc` 経路 | 右手系 | +Y | **[-1,1]** ⚠ | `sgc::Mat4f::perspective` の doc に「右手座標系、Zを[-1,1]にマッピング」 |
| DX12 ハードウェア NDC | — | — | **[0,1] 固定** | D3D 仕様 |

### 1.1 POV-Ray の反転について

POV-Ray の既定は**左手系**（`right<1.33,0,0>`）。Grasp3D は `right` を**負**で書くことで
右手系に反転させている。GL ビューと絵を一致させるための意図的な処置であり、
**Makina でも同じことをしなければならない**。`PovCamera.java` のコメントがこの意図を残している。

### 1.2 `sgc::Mat4f::lookAt` は右手系

```cpp
const Vec3<T> forward = (eye - target).normalized();   // 視点から離れる向き = 右手系
```

`Shadow.hpp` のコメントもこれを「右手系」と呼んでおり、実装と一致している。

---

## 2. Makina の規約【確定】

1. **右手系・Y-up** を全経路で使う。
   Grasp3D（移植元）と MitiruEngine の主経路が既に一致しているため、**変換は恒等**。
   → R-01 で想定していた「4 者の食い違いを 1 箇所に閉じ込める」作業は**発生しない**。
2. **投影は必ず深度レンジ [0,1]** を使う。DX12 では `GlmBridge::perspective`
   （= `glm::perspectiveRH_ZO`）系を使い、**`sgc::Mat4f::perspective` は使わない**。
3. **単位系は 1.0 = 1 m。**
4. **POV-Ray 出力では `right` を負で書く**（§1.1）。忘れると鏡像になる。
5. 角度は度（degree）で保持する。Grasp3D の `Rotate` ノードが度なので、
   移植時に単位を変えると既存 `.gsf` の解釈が変わる。

> スパイクのレイマーチャは eye / forward / right / up から直接レイを組んでおり投影行列を
> 使わないため、この規約に非依存だった。**GBuffer に深度を書き始める Phase 2 から効いてくる。**

---

## 3. MitiruEngine 側で見つかった要確認事項

**エンジン内に投影規約が 2 系統併存している。**

| 経路 | 使っているもの | 深度レンジ | 使用箇所 |
|---|---|---|---|
| A | `GlmBridge::perspective` = `perspectiveRH_ZO` | **[0,1]**（DX12 正） | `Renderer3D_DX12` / `Renderer3D` / `GeometryPass3D` / `MotionVectorPass` / `Skybox` |
| B | `Camera3D::projectionMatrix()` = `sgc::Mat4f::perspective` | **[-1,1]**（GL 規約） | `DeferredPipeline`（122 行）/ `Pipeline3D` / `Sprite3DRenderer` / `GpuParticleDx11` / `GpuParticleDx12` |

DX12 の NDC は [0,1] 固定なので、経路 B は**理屈の上では錐台の手前半分が NDC z<0 に落ちて
クリップされる**。

⚠ **これは「バグである」と断定していない。** 実際に描画が破綻しているかは未確認で、
下流に補正が入っている可能性、あるいは経路 B が現在の DX12 描画で使われていない可能性がある。
**MitiruEngine の作者（＝本人）による確認が要る項目**として記録する。

### 3.1 `Shadow.hpp` のコメントが実装とずれている

`Shadow.hpp` 81–82 行：

> メインカメラは glm::lookAtLH + perspectiveLH_ZO

しかし `GlmBridge::lookAt` は `glm::lookAtRH`、`GlmBridge::perspective` は
`glm::perspectiveRH_ZO` であり、**LH ではなく RH**。
同じファイルの 84 行では `sgc::Mat4f::lookAt` を正しく「右手系」と呼んでいる。

**コメントが古いだけと思われるが、シャドウの左手系 view はこの前提の上に組まれている**
（`lightViewMatrix` は `forward = target - eye` の左手系で手組みされており、
「light projection が正の view z を前提とするため」と説明されている）。
シャドウが現に動いているなら実装が正しくコメントが古い。**要確認。**

---

## 4. Phase 0 への影響

- **座標変換レイヤ（`Coord.hpp`）は作らない。** 変換が恒等であるものに抽象を挟むと、
  読む人に「何か変換されている」と誤解させるだけで害になる。
  代わりに**本文書を規約の唯一の出典とする**。
- `CsgNode`（Task #2）は右手系・Y-up・度・メートルを前提に設計してよい。
- Phase 2 で GBuffer に書き込む際、**投影は経路 A（`GlmBridge`）を使う**こと。
