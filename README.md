# Adjustment Layer – Third-party Plugin for OBS Studio

⚠ **This is a third-party, unofficial plugin for OBS Studio.**  
This project is **not affiliated with or endorsed by the OBS Project**.

An **Adjustment Layer** source allows effect filters applied to it to affect **all sources behind it**, similar to adjustment layers in video editing software.

OBS Studioに「調整レイヤー」ソースを追加するサードパーティ製プラグインです。  
本プラグインは OBS Studio の公式プロジェクトではありません。

---

## How to Use / 使い方

1. Add **"Adjustment Layer"** from the Sources panel.  
   ソース一覧で **「調整レイヤー」** を追加します。
2. Place it above the sources you want to affect.  
   エフェクトを適用したいソースよりも上に配置します。
3. Right-click the Adjustment Layer → **Filters**.  
   調整レイヤーを右クリックして **「フィルター」** を選択します。
4. Add any Effect Filters (Color Correction, Blur, etc.).  
   好きなエフェクトフィルター（色補正、ブラー等）を追加します。

---

## How to Build / ビルド方法

This plugin supports both **Out-of-tree builds (Recommended)** and **In-tree builds (Optional)**.

本プラグインは **Out-of-tree ビルド（推奨）** と **In-tree ビルド（任意）** の両方に対応しています。

---

### Out-of-tree Build (Recommended)

Build the plugin independently while referencing an existing OBS Studio build.  
This is the **recommended and most stable method**, especially on **macOS**.

OBS Studio のビルド成果物を参照して、プラグイン単体をビルドする方法です。  
**最も安定しており、macOS ではこちらを推奨します。**

#### Prepare OBS Studio source code

`obs-studio` is expected to be located in the parent directory of this repository
You can specify a custom location via the `OBS_ROOT` environment variable

OBS Studio のソースコードが本リポジトリの親ディレクトリにあることを想定しています。  
別の場所にある場合は `OBS_ROOT` 環境変数で指定できます。

#### Run the build script

**macOS**
```bash
./build.sh
```

**Windows**
```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The built plugin will be generated in the `dist` directory.  
ビルド成果物は `dist` ディレクトリに生成されます。

---

### In-tree Build (Optional)

Build the plugin as part of the OBS Studio source tree.  
This method is mainly intended for **development and debugging**.

OBS Studio のソースツリーに組み込んでビルドする方法です。  
**主に開発・デバッグ用途**を想定しています。

1. Place this repository into:
   ```
   obs-studio/plugins/adjustment-layer
   ```
2. Add the following line to `obs-studio/plugins/CMakeLists.txt`:
   ```cmake
   add_subdirectory(adjustment-layer)
   ```
3. Build OBS Studio as usual.

> ⚠ **macOS Note**  
> On macOS, in-tree builds may fail due to legacy OpenGL / AGL-related dependencies in some OBS plugins.  
> If you encounter build errors, please use the **Out-of-tree build** instead.
>
> ⚠ **macOS 注意事項**  
> macOS では、一部の OBS プラグインが依存している旧 OpenGL / AGL の影響により、  
> in-tree ビルドが失敗する場合があります。  
> その場合は **Out-of-tree ビルド** を利用してください。

---

## Notes / 注意事項

- Per-source show/hide transitions are currently not supported.  
  ソース単体の表示・非表示トランジション機能には現在対応していません。

---

## License / ライセンス

- BSD 2-Clause License

---

Developed by Horaiken ([@HoraiChan](https://x.com/HoraiChan))
