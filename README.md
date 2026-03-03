# BprToBat

Borland C++ Builder 3.0 (BCB3) の `.bpr` プロジェクトファイルを解析し、パラレルビルド用 `.bat` ファイルを自動生成するツール。

## 使用方法

```
BprToBat.exe <bprファイル> [ワーカー数]
```

| 引数 | 説明 |
|------|------|
| `bprファイル` | `.bpr` ファイルのパス（必須） |
| `ワーカー数` | 並列コンパイルのワーカー数（省略時: 4、範囲: 1〜16） |

### 出力ファイル

`{プロジェクト名}_build_parallel.bat` が `.bpr` と同じディレクトリに生成される。

### 実行例

```bat
REM Gats2120.bpr からパラレルビルドBAT生成（ワーカー4）
BprToBat.exe Gats2120.bpr

REM ワーカー数を8に指定
BprToBat.exe Gats2120.bpr 8

REM サブディレクトリのBPRファイル
BprToBat.exe NGPinCount\NGPinCount.bpr 2
```

## 生成されるBATの特徴

- **パラレルコンパイル**: ソースファイルをラウンドロビンでワーカーに振り分け、CPUアフィニティ付きで並列実行
- **ワーカー数の実行時変更**: 生成されたBATファイル内の `NUM_WORKERS` 変数を書き換えるだけでワーカー数を変更可能（再生成不要）
- **バージョン情報**: BPR内の IncludeVerInfo が有効な場合、VERSIONINFO リソース (.rc) を自動生成しリンク
- **リソースコンパイル**: RESFILES に含まれる追加リソース (.res) も自動コンパイル
- **アイコン保持**: 既存の .rc ファイルがある場合、ICON宣言等の非VERSIONINFO行を引き継ぐ

## 対応するBPRファイル形式

BCB3 が生成する `.bpr` ファイル形式に対応。以下の情報を解析する:

- Makefile変数（PROJECT, OBJFILES, CFLAG1-3, PFLAGS, RFLAGS, LFLAGS, LIBFILES, LIBRARIES, PATHCPP, PATHPAS, ALLOBJ, ALLLIB 等）
- バックスラッシュ継続行（`\` による複数行にまたがる変数定義）
- `[Version Info]` セクション（MajorVer, MinorVer, Release, Build 等）
- `[Version Info Keys]` セクション（CompanyName, FileDescription 等）
- `$(BCB)` マクロの `%BCB%` 環境変数への変換

## 前提条件

- **Borland C++ Builder 3.0** がインストールされていること（`C:\Program Files\Borland\CBuilder3` またはBCB環境変数で指定）
- 生成されたBATを実行するには BCB3 のコンパイラ (`bcc32`) とリンカ (`ilink32`) が必要

## ビルド方法

VS2022 Build Tools (MSVC) を使用してコンパイルする。

```bat
cd BprToBat
build.bat
```

または手動で:

```bat
cl /EHsc /O2 /Fe:BprToBat.exe BprToBat.cpp
```

## ファイル構成

```
BprToBat/
    BprToBat.cpp    -- ソースコード
    BprToBat.exe    -- 実行ファイル
    build.bat       -- ビルドスクリプト
    README.md       -- このファイル
```
