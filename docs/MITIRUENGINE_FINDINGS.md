# MitiruEngine への指摘まとめ

**調査日**: 2026-08-10
**調査対象**: `github.com/mogmog-0110/MitiruEngine` v0.30.0（`main`、`external/` は sgc のみ取得）
**調査の目的**: Makina（CSG ソリッドモデラー）を載せる前提で描画基盤を読んだ際の副産物

> **この文書は「バグ報告」ではなく「確認依頼」です。** 実際に破綻しているかまでは検証していない
> 項目には ⚠ を付けています。断定しているのは、コードを読めば確認できる事実のみです。

---

## ✅ 復旧済み：`MitiruMML` のリモートから DSP 実装が失われていた

> **2026-08-11 に復旧完了。** 以下は経緯の記録。
>
> - `MitiruMML` へ `4b272ee` を push（**追加のみ。force-push も履歴書き換えもしていない**）
> - `MitiruEngineDev` の submodule ポインタを `9c0b83ee` → `4b272ee` に更新（commit `aea019f3`）
> - **新規 clone + `git submodule update --init --recursive` が通ることを確認**
> - `mitiru_tests_runtime`: 830 cases / 3220 assertions すべて成功
>
> ⚠ **元の commit 履歴は復元できていない。** 失われた DSP 作業は 1 コミットにまとまっている。

<details>
<summary>発見時の経緯（クリックで展開）</summary>


**`MitiruEngineDev` は新規クローンからビルドできない。** submodule `external/mml` が
リモートに存在しないコミットを指しており、しかも**リモートの現在の内容では足りない**。

### 何が起きているか

```
MitiruEngineDev が記録している mml      : 9c0b83ee8c2c673c0bb8056622113dbcdac70ff6
MitiruMML リモートの ref はこれ 1 本だけ : 084382853eed38fe36813d06bede48eeaddeed23 (refs/heads/main)
9c0b83ee を直接 fetch                    : upload-pack: not our ref  → 存在しない
```

`git submodule update --init` は次で止まる：

```
fatal: remote error: upload-pack: not our ref 9c0b83ee...
fatal: Fetched in submodule path 'external/mml', but it did not contain 9c0b83ee...
```

### リモートの `main` に差し替えても直らない

`08438285` を使うとビルドが別の場所で落ちる：

```
tests/mitiru/TestAudioDsp.cpp(15): fatal error C1083: 'mitiru_mml/Biquad.hpp': No such file or directory
tests/mitiru/TestMixBus.cpp(12):   fatal error C1083: 'mitiru_mml/MixBus.hpp':  No such file or directory
```

| | ヘッダ数 | `Biquad` `MixBus` `Chorus` `Compressor` `Delay` `Equalizer` |
|---|---|---|
| MitiruMML リモート `main` (`08438285`) | 少ない | **無い** |
| **リリース版が vendor している mml** | **35** | **ある** |

つまり **MitiruMML は履歴を書き換えられ、DSP 一式が失われている。**
リモートの `main` は engine が要求するものより**古い**。

### 現存する唯一のコピー

`D:\sandbox\MitiruEngine`（リリース版）は `external/` を丸ごと vendor しているので、
**失われた DSP 実装はそこに残っている**。リリース版は submodule を使わないため無傷。

### 押し戻す前に確認したこと

上書きで既存の作業を潰さないよう、ファイル集合と内容を突き合わせた。

- **リモートにあってスナップショットに無いファイル：無し**（失われるものが無い）
- 共通ファイルの差分は 3 件のみで、**いずれも DSP 関連の純粋な追加**

| ファイル | 差分 |
|---|---|
| `MitiruMML.hpp` | +7 DSP ヘッダの include |
| `Synthesizer.hpp` | +19 レゾナンスフィルタ（Biquad）対応 |
| `WavWriter.hpp` | +17 ステレオ出力のオーバーロード |

つまりスナップショットは**リモートの現状の上に DSP 機能を載せたもの**であり、
復旧は追加操作のみで済む、と確認したうえで push した。

> なぜ気づかれていなかったか：一度クローンして submodule を取得済みの環境では再現しない。
> **新規クローンでのみ現れる**（今回それをやったので出た）。

</details>

---

## テストゲートの実行結果（2026-08-11）

`python tools/test_gate.py --build-dir build-check --config Debug`（`.claude/rules/definition-of-done.md` の関門）

```
total 2720  passed 2697  failed 21  timeout 0  exception 0  not-run 0  skipped 2
```

**21 件の失敗はすべて原因が特定でき、いずれも本作業の変更とは無関係。**

| 件数 | 原因 | 証拠 |
|---|---|---|
| **20** | `mitiru_host.exe` が存在しない | `MITIRU_DISABLE_CEF=ON` でビルドしたため host が作られない。失敗した e2e / replay_golden はすべてコマンドラインに `mitiru_host.exe` を含む |
| **1** | **この GPU がメッシュシェーダー未対応** | エンジン自身が出力: `[mitiru] clod: mesh shaders unavailable - drawModel disabled` → `TestClodRenderer.cpp:345` で `2.80859% <= 0.5%` が失敗 |

新規追加した `[gfx][compute]` は **4 テスト / 8 アサーション全通過**。
render スイート単体では 890 中 889 通過（残る 1 件が上のメッシュシェーダー由来）。

> ⚠ **「ゲート通過」とは言えない。** 上記 21 件は環境要因だが、
> クリーンなベースライン（変更前）でのゲート実行は行っていないので、
> 「変更前後で失敗集合が同一」を実測で示したわけではない。
> ただし 20 件は存在しない実行ファイルを起動しようとした失敗であり、
> 1 件はエンジン自身が理由を印字しているので、変更が原因になりようがない。

### 派生する観察：ゴールデン画像がメッシュシェーダー必須のハードで作られている

`TestClodRenderer` のゴールデンは `drawModel`（メッシュシェーダー経路）が有効な環境で
生成されている。**メッシュシェーダー非対応の GPU では、正しく動いていても必ず落ちる。**

CI やチームで環境が揃わない場合、この手のテストは
「capability を検出して skip」か「capability 別のゴールデン」が要る。
現状は**ハードウェアの違いがテスト失敗として現れる**。

---

## 優先度サマリ

| # | 内容 | 優先 | Makina を止めるか |
|---|---|---|---|
| **MML** | ~~`MitiruMML` から DSP 実装が失われ、Dev が新規クローンからビルドできない~~ | ✅ **復旧済み** | — |
| B-1 | **Vulkan バックエンド 3,064 行に消費者がいない** | **高** | 止めない（Phase 8 で効く） |
| B-3 | ~~`gfx` 抽象にコンピュートがない~~ → **実装済み**（`feat/gfx-compute`） | 高 | 解消 |
| UI-1 | `UiThemeGenerator.hpp:379` が新しい MSVC でコンパイルできない（死んだ行） | 中 | **修正済み** |
| A-1 | ~~投影の深度レンジが 2 系統併存~~ → **検証のうえ修正済み**（`e4f975e1`） | 高 | 解消 |
| B-2 | 上位レンダラが `gfx` を経由せず `<d3d12.h>` 直 include | 中 | 止めない |
| A-2 | ~~`Shadow.hpp` のコメントが実装とずれ~~ → **修正済み** | 低 | 解消 |
| B-4 | `BackendInit` が具象型へダウンキャスト | 低 | 止めない（B-1 と同時に解く） |
| B-5 | ウィンドウ型がバックエンドで分岐 | 低 | 止めない（B-1 と同時に解く） |
| CLOD | ゴールデンがメッシュシェーダー必須ハード前提 | 中 | **修正済み**（`b3548fbe`） |
| — | （指摘ではない）makina-core をエンジンから使えるようにした | — | `d6421891` |
| — | （指摘ではない）焼いた DXIL を読む `CsgBake` | — | `12c369e1` |
| SCRUB | **スクラブコマンドが同着 mtime で落ちる** | 中 | **修正済み**（`ac79b597`） |
| C-1 | `render/sdf/` の名前がジオメトリ SDF と衝突する | 低 | **Makina 側で回避済み** |

---

## A. 描画経路の一貫性

### A-1 投影の深度レンジが 2 系統併存している【検証済み・修正済み `e4f975e1`】

**事実**：エンジン内に投影行列の生成が 2 系統ある。

| 経路 | 実体 | 深度レンジ |
|---|---|---|
| **A** | `GlmBridge::perspective` = `glm::perspectiveRH_ZO` | **[0,1]**（D3D 正） |
| **B** | `Camera3D::projectionMatrix()` = `sgc::Mat4f::perspective` | **[-1,1]**（GL 規約） |

#### 何が起きるかを式で詰めた

両者は `z_ndc = A + B/d` の形で、同じ near/far を端点に取る。したがって

```
z_ndc(GL) = 2 * z_ndc(D3D) - 1
```

D3D のクリップ空間は `0 <= z <= w` 固定なので、B を D3D に渡すと：

1. **`z_ndc(D3D) < 0.5` の領域が消える。** 境界は `d = 2*near*far/(near+far)`。
   `near=0.1, far=100` なら `d < 0.1998` — 実害は小さい
2. **残った部分も他パスと違う深度を書く。** `z_ndc < 1` の全域で
   `2z-1 < z` なので、**同じ距離のメッシュより必ず小さい z**。
   深度テストが LESS なら**常に手前に来る**

**2 が本体である。** 「近くが消える」ではなく「ソートが壊れる」という壊れ方で、
しかも症状がソートのバグに見える。

#### 実際に渡していたのは 3 箇所、いずれも消費者なし

| 場所 | バックエンド | 判定 |
|---|---|---|
| `GpuParticleDx12.hpp:214` | D3D12 | ❌ 誤り → 修正 |
| `GpuParticleDx11.hpp:316` | D3D11 | ❌ 誤り → 修正 |
| `Skybox::drawDx11(ctx, camera)` | D3D11 | ❌ 誤り → 修正 |
| `Sprite3DRenderer.hpp:142` | **WebGL2**（`#ifdef __EMSCRIPTEN__`） | ✅ **正しい** — 前回の報告が誤り |
| `DeferredPipeline.hpp:122` / `Pipeline3D.hpp:104` | — | 消費者が 1 つも無い |

> **前回の報告の訂正。** `Sprite3DRenderer` を DX12 消費者として挙げていたが、
> あれは `__EMSCRIPTEN__` 配下の WebGL2 レンダラで、[-1,1] が正しい。
> ヘッダの `@brief` を読まずに `viewProjectionMatrix()` の grep だけで判断していた。

**したがって現時点で壊れている描画は無い。**
3 箇所とも呼び出し元がツリーに存在しないため、症状として出ていなかった。
次にそれらを使う人のために仕掛けられた罠、という状態だった。

#### 直した内容

- `Camera3D::projectionMatrixZO()` / `viewProjectionMatrixZO()` を追加（D3D 規約）
- `projectionMatrix()` / `viewProjectionMatrix()` に **`@warning` で規約と症状を明記**
- D3D の 3 箇所を ZO 版へ
- `Camera3D` に glm 依存を入れたくないので ZO 行列は手組み。
  **`GlmBridge::perspective` と要素まで一致することをテストで担保**
  （`TestCamera3D` の "projection depth range"、near/far の写像・クリップ境界・
  深度の大小関係も同時に検査）

### A-2 `Shadow.hpp` のコメントが実装とずれている【修正済み】

`include/mitiru/render/Shadow.hpp:81-82`：

> 左手系 (glm::lookAtLH と同規約) で組む。**メインカメラは glm::lookAtLH + perspectiveLH_ZO**、…

しかし `GlmBridge::lookAt` は `glm::lookAtRH`、`GlmBridge::perspective` は `glm::perspectiveRH_ZO`。
**LH ではなく RH** です。同ファイル 84 行では `sgc::Mat4f::lookAt` を正しく「右手系」と呼んでおり、
実装との整合はそちらが取れています。

シャドウの `lightViewMatrix` は左手系で**手組み**されており（110-120 行）、
その理由が「メインカメラが LH だから」と書かれています。実際にはメインカメラは RH なので、
**コメントの理由付けが誤っている**（動いているなら実装が正しく、説明が古い）。

**対応済み**：本当の理由（`lightProjectionMatrix` が正の view z を前提としている）だけを残し、
「メインカメラが LH だから」を削除、メインカメラが RH であることを明記した。

---

## B. RHI 抽象の実効性

### B-1 Vulkan バックエンドに消費者がいない【優先度 高】

**事実**：`include/mitiru/gfx/vulkan/` に **3,064 行**（DX12 は 3,808 行）の実装がある。
しかし `gfx/vulkan` を include しているのは：

```
include/mitiru/gfx/GfxFactory.hpp
include/mitiru/gfx/vulkan/…（自分自身の内部ファイル）
```

**それだけです。** `GfxFactory::createDevice(Backend::Vulkan, …)` は `VulkanDevice` を返せますが、
デバイスを実際の描画パイプラインに繋ぐ `BackendInit.hpp` には **Vulkan の分岐がありません**：

```cpp
// include/mitiru/render/BackendInit.hpp
if (backend == gfx::Backend::Dx11) { … }
if (backend == gfx::Backend::Dx12) { auto* dx12 = static_cast<gfx::Dx12Device*>(device); … }
// Vulkan の分岐なし
```

つまり **Vulkan デバイスとスワップチェインは作れるが、その上で 3D シーンを描く経路が存在しない。**

**なぜ重要か**：ポートフォリオとして語りたいのは「両方書けます」ではなく
**「同じパスが抽象越しに DX12 と Vulkan の両方で同じ絵を出す」**のはずです。
現状はその主張ができません。3,064 行が資産として死んでいるのはもったいない。

**推奨**：一気に全経路を移すのではなく、**1 パスだけ**（例えば Skybox かポストプロセス 1 枚）を
`gfx` 抽象越しに書き直し、`BackendInit` に Vulkan 分岐を足して、
**同じ絵が両バックエンドで出ることを 1 回証明する**。そこから広げる。

### B-2 上位レンダラが `gfx` を経由していない【優先度 中】

```cpp
// include/mitiru/render/Renderer3D_DX12.hpp:32
#include <d3d12.h>
#include <mitiru/gfx/dx12/Dx12Device.hpp>   // 抽象ではなく具象
```

`gfx/` に `IDevice` / `ICommandList` / `IPipeline` … と抽象が揃っているのに、
**主要レンダラはそれを飛ばして DX12 を直接叩いています。**
抽象が「作られたが使われていない」状態で、B-1 の直接の原因でもあります。

**推奨**：全面移行は高コストなので、**新規に書くパスから `gfx` 越しにする**方針を決める。
既存を書き換えるのは B-1 の証明が済んでから。

### B-3 コンピュートが `gfx` 抽象の外にある【優先度 高】

> ⚠ **訂正（2026-08-11）**：初版で「型だけが用意されて中身がない」と書いたが、**誤り**。
> release 版だけを読んで `gfx` の下だけを見ていた。開発版には
> **`include/mitiru/render/dx12/Dx12ComputeContext.hpp`（406 行）** があり、
> コンピュートは**動く実装として存在する**（HLSL のインラインコンパイル、
> ルートシグネチャ管理、`dispatch` まで揃っている）。専用テスト
> `tests/mitiru/TestDx12ComputeContext.cpp` もある。

**正しい問題は「無い」ではなく「抽象の外にある」。**

| | 状態 |
|---|---|
| `render/dx12/Dx12ComputeContext` | **動作する。ただし DX12 直書きで `gfx` を経由しない** |
| `gfx` 抽象 | `IShader.hpp:15` に `ShaderStage::Compute`、`GfxTypes.hpp` に `UnorderedAccess` の enum 値はあるが、**`ICommandList` に `Dispatch` が無く、`IPipeline` にコンピュートパイプラインの口も無い** |

つまり B-1（Vulkan に消費者がいない）・B-2（上位レンダラが `gfx` 非経由）と**同じ根**です。
機能はあるが抽象の外にあるので、**Vulkan へは持っていけない**。

**Makina への影響**：SDF レイマーチのフィールド計算（AO / カーバチャ / 厚み）は
本来コンピュートで書きたい。今回は DX12 直書きで回避しましたが、
Phase 8（Vulkan への引き上げ）でここが必要になります。

**実施済み**（ブランチ `feat/gfx-compute`）:

| 追加 | 内容 |
|---|---|
| `GfxTypes.hpp` | `RootParamType` / `RootParam` / `ComputePipelineDesc` — **バックエンド非依存の語彙**。D3D12 のルートパラメータと Vulkan の descriptor set layout binding で意味が一致するものだけを表す |
| `IPipeline` | `isCompute()`（既定 `false` なので既存パイプラインは無変更で通る） |
| `ICommandList` | `dispatch` / `setComputeRootSignature` / `setComputeRoot{DescriptorTable,CBV,SRV,UAV}` / `uavBarrier` |
| `Dx12ComputePipeline`（新規・220 行） | 記述子からルートシグネチャを**組み立てる**。graphics 側の `Dx12Pipeline` は固定だが、コンピュートは用途ごとに要求リソースが違うので固定できない |
| `Dx12CommandList` | 上記の DX12 実装。`setPipeline` がコンピュートを分岐 |
| `tests/mitiru/TestGfxCompute.cpp` | 契約テスト |

**設計判断を 1 つ**：`dispatch` の既定実装は**黙って何もしない代わりに一度だけ警告する**。
graphics 側の未実装（`setRootSignature` 等）が無害なのは DX11 に概念自体が無いからだが、
**`dispatch` を捨てると計算が実行されず、後段は未計算の結果を正しい結果として扱う**。
既存の「静かな no-op」に揃えるより、ここは揃えないほうが正しい。

あわせて `setPipeline` が `dynamic_cast` 失敗時に黙って戻る箇所にも警告を入れた。
黙って戻ると**直前の PSO のまま描画が続き**、「なぜか前のシェーダーで描かれる」という
追いにくい症状になる。

⚠ **`Dx12ComputeContext` と役割が重なる。** 長期的には後者を `gfx` の上に載せ替えるか
退役させるのが筋だが、それは別の判断なので手を付けていない。

### B-4 `BackendInit` が具象型へダウンキャストしている【優先度 低】

```cpp
auto* dx12 = static_cast<gfx::Dx12Device*>(device);
```

ヘッダ冒頭に「`dynamic_cast` を並べるのを避けるため device が報告する `Backend` enum で分岐する」
という設計意図が書かれており、**意図的な選択**であることは分かります。
ただ結果として、バックエンドを増やすたびにここへ分岐が増える構造になっています。
B-1 に着手するとき、この形のままでよいか一度考える価値があります。

### B-5 ウィンドウ型がバックエンドで分岐している【優先度 低】

`GfxFactory` を読むと、DX12 は `Win32Window`、Vulkan は `GlfwWindow` を要求します
（`"Vulkan backend requires a GlfwWindow"`）。
バックエンドを切り替えるとウィンドウ生成側も変わるため、B-1 に着手する際は
ここも同時に解く必要があります。**先に把握しておくべき依存**として記録します。

---

## C. 命名

### C-1 `render/sdf/` は SDF フォント【優先度 低・Makina 側で回避済み】

`include/mitiru/render/sdf/` の中身は `SdfFontAtlas` / `SdfTextRendererGpu` など
**フォントレンダリング**です。ジオメトリの符号付き距離場とは別物。

Makina 側は `render/csg/` を使うことで衝突を回避しますが、
将来 `render/sdf/` を見た人が確実に混乱します。

**推奨**：`render/text_sdf/` あるいは `render/font/` へのリネーム。急ぎではありません。

---

## SCRUB スクラブコマンドが同じ mtime で落ちる【修正済み `ac79b597`】

**見つかり方が本題である。** テストゲート 2,729 件のうち
`ScrubControlChannel: monotonic seq lets the host skip stale commands` だけが落ちた。
単体で走らせると **6/6 通る**。

最初、これは私の変更のせいではないと判断して先に進みかけたが、
それだけでは「たまたま落ちた」で終わってしまうので、ゲートを**もう一度**走らせて再現させ、
`LastTest.log` から実際の assertion を読んだ：

```
CHECK( applied == 20 )   with expansion:  10 == 20
Test time = 0.03 sec
```

2 回目の `poll()` が何も返していない。テスト全体が 0.03 秒で終わっており、
**2 つの write が同じ mtime に入った**。

### 中身

`ScrubControlReader::poll()` は「mtime が前回と同じなら読まない」で新着を判定していた。
NTFS の記録粒度では連続した 2 つの write が同じ mtime を持ちうる。
そうなると 2 通目は**痕跡なく落ちる**。
スクラバーをドラッグしている最中はまさに連続した write が飛ぶので、
実機では「たまにスクラブが効かない」として出る。

`poll()` は中身を比べるようにした。ファイルは数十バイトで、
この読み取りを節約してコマンドを落とす方がはるかに高い。

### テストの作り方

**速さで狙うテストは書かない。** バグ自体が速い時だけ出るので、
速く書き込むテストはバグと同じくらい当てにならない。
2 通目を書いたあと `last_write_time` を 1 通目の値に**書き戻して**同着を作る。

両方向で確認した — 旧ルールで **3/3 失敗**、新ルールで **5/5 成功**。

---

## 残りは 1 つの塊 — B-1 / B-2 / B-4 / B-5

この 4 件は別々の指摘のように並んでいるが、**同じ 1 つの作業**である。

- **B-2**（上位レンダラが `<d3d12.h>` を直 include）が原因で、
- **B-1**（Vulkan 3,064 行に消費者がいない）という結果が出ており、
- その状態で backend を増やすと **B-4**（`BackendInit` の分岐）と
  **B-5**（backend ごとにウィンドウ型が変わる）が同時に効いてくる

したがって B-4 / B-5 を単独で直しても意味がない。B-1 に着手するときに一緒に解く。

**最初の一歩として推奨する形**（前回から変えていない）：

> **1 パスだけ `gfx` 抽象を経由させて、Vulkan で動くことを示す。**
> 全部を移すのではなく、いちばん小さいパス（たとえばポストプロセスの 1 枚）を選ぶ。
> 抽象に足りないものがあれば、そこで初めて分かる。
> `feat/gfx-compute` でコンピュートを足したときがまさにそれで、
> `RootParamType` / `ComputePipelineDesc` / `uavBarrier` が足りないと分かったのは
> **実際に 1 本通そうとしたから**だった。

これはアーキテクチャの決定なので、**エンジンの持ち主が決めるべき**と考えて手を出していない。

---

## 補足：良かった点

指摘だけ並べると偏るので、読んでいて良いと思った点も書いておきます。

- **`docs/SCOPE.md` が canonical 宣言つきで、他の doc と食い違ったらこれが勝つと明記されている。**
  個人プロジェクトでここまで規律を明文化して守れている例はまず見ません。
  Makina を「エンジンの機能」にせず別製品として外に立てる判断は、この SCOPE を守るためです。
- **FLAT_POD の制約設計**。「状態は 1 個の flat POD」という制約 1 つから、巻き戻し・録画再生・
  AI 構造化観測・反実仮想の 4 機能が同じ `memcpy` で生えている。制約が機能を生んでいる好例です。
- `BackendInit.hpp` の冒頭コメントのように、**「なぜこの形を選んだか」がヘッダに残っている**箇所が多い。
  B-4 を「低優先」に置いたのは、意図が読めたからです。
