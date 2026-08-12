# CI 用のシーン一式

`tools/gsf2json` が Grasp3D の `.gsf` から作ったものの写し。**これだけがコミットされている。**

## なぜ入力だけコミットして、参照ダンプはしないのか

区別しているのは「入力」と「答え」である。

- **シーン（ここ）** は入力。安定していて小さく、これが無いと CI で走らせるものが何も無い
- **参照ダンプ（`tools/*/out`）** は Java 参照実装の答え。
  これをコミットすると、**リポジトリの中で古くなりうるもの**になる。
  毎回 `.gsf` から作り直すことだけが「今のコードを今の参照と比べている」を保証する

したがって CI が走らせるのは **Java を要らない検査だけ**である：

| 検査 | 何と何を比べるか | CI |
|---|---|---|
| `roundtrip` | JSON → Scene → JSON | ✅ |
| `bsp_compare` | SDF と B-rep（どちらも自分のコード） | ✅ |
| `edit_check` | 編集後のツリー不変条件 | ✅ |
| `command_check` | コマンド層の受理と拒否 | ✅ |
| `sdf_compare` | **Java の答え** | ❌ `verify-all.bat` で |
| `measure_compare` | **Java の答え** | ❌ 同上 |
| `pov_compare` | **Java の答え** | ❌ 同上 |
| シルエット | **POV-Ray の絵** | ❌ 同上 |

CI が「緑」と言っても、それは**移植の一致検証を通ったという意味ではない**。
そちらはローカルで `verify-all.bat` を走らせるまで分からない。

## 更新のしかた

`tools/makescenes` か Grasp3D の `.gsf` を触ったら：

```
tools\makescenes\build-and-run.bat
tools\gsf2json\build-and-run.bat
copy tools\gsf2json\out\*.makina.json makina-core\tests\scenes\
```

`hero_sweep_*` は入れていない。`hero_flange` と同じ形の溝半径違いで、
CI にとっては同じものを 5 回検査するだけになる。
