# 設定ファイルを SPIFFS で運用する方法

SD カードを使わずに、本体内蔵の SPIFFS に置いた YAML 設定ファイルで起動する方法です。Core2 / CoreS3 / AtomS3R 共通です。

起動時の設定の読込順は次のとおりです。SD カードに設定ファイル一式が無ければ SPIFFS の設定が使われます。

1. SD カードの `/app/AiStackChanEx/SC_ExConfig.yaml`、`/yaml/SC_SecConfig.yaml`、`/yaml/SC_BasicConfig.yaml`（AtomS3R は対象外）
2. SPIFFS の `/SC_ExConfig.yaml`、`/SC_SecConfig.yaml`、`/SC_BasicConfig.yaml`
3. どちらも揃っていない場合は Web UI（Config ページ）での設定を促す

## 1. SPIFFS へ書き込むファイルを用意する

`AI_StackChan_Ex/firmware/data/` フォルダ（無ければ作成）の**直下**に、次の3ファイルを置きます。テンプレートはリポジトリ直下の `Copy-to-SD/` にあります。

```
firmware/data/
├── SC_BasicConfig.yaml   (Copy-to-SD/yaml/SC_BasicConfig.yaml)
├── SC_SecConfig.yaml     (Copy-to-SD/yaml/SC_SecConfig.yaml)
└── SC_ExConfig.yaml      (Copy-to-SD/app/AiStackChanEx/SC_ExConfig.yaml)
```

> Note:
> - SD カードと同じフォルダ構成（`app/`、`yaml/`）では読み込まれません。必ず `data/` 直下に置いてください。SPIFFS はファイル名（パス含む）が31文字までのため、SD と同じパスは使えません。
> - `firmware/data/` は `.gitignore` で除外されています。API キーや Wi-Fi パスワードを含むため、コミットしないでください。

## 2. SPIFFS イメージを書き込む

PlatformIO で対象の env を選び、「Upload Filesystem Image」を実行します。コマンドラインの場合は次のとおりです。

```bash
cd firmware
pio run -e m5stack-cores3-realtime -t uploadfs
```

AtomS3R の書き込み待機状態への入れ方は [AtomS3R の手順](atoms3r.md) を参照してください。

> Note:
> Upload Filesystem Image は SPIFFS 全体を上書きします。Web UI で保存した設定、ロール（システムプロンプト）、Wake Word の登録データなど、SPIFFS 上の他のファイルも消えます。

## 3. SD カードから設定ファイルを削除する

SD カードに設定ファイル一式があると、そちらが優先されます。SPIFFS の設定を使うには、SD カードから次のいずれかを削除してください（SD カード自体は、アラーム音や PhotoFrame など他の機能のために挿したままで構いません）。

- `/app/AiStackChanEx/SC_ExConfig.yaml`
- `/yaml/SC_SecConfig.yaml`
- `/yaml/SC_BasicConfig.yaml`

起動時のシリアルログに `Loading config from SPIFFS.` と表示されれば、SPIFFS の設定が使われています。

## 書き込み後の設定変更

- **Web UI**：ブラウザで `http://<ｽﾀｯｸﾁｬﾝの IP>/config.html` を開き、Wi-Fi、API キー、サーボ、AI サービス、MCP サーバーを変更できます。保存時は既存ファイルに変更した項目だけを反映し、Web UI に無い項目（TTS、STT、Wake Word、StackChan-API 連携など）は保持します。ただし YAML のコメントと書式は失われます。
- **FTP**：Wi-Fi 接続後は FTP で SPIFFS のファイルを直接読み書きできます（ユーザ名：stackchan、パスワード：stackchan）。
