# AtomS3RのSPIFFSにYAMLファイルを書き込む方法

> Core2 / CoreS3 を含む共通の手順と、Web UI で保存したときの動作は [設定ファイルを SPIFFS で運用する方法](spiffs_config.md) を参照してください。

1. 下図のように、AI_StackChan_Ex/firmware/dataに書き込むYAMLファイルを置く。(dataフォルダがない場合は作成する。)
![](../images/spiffs_data_folder.png)

2. PCとAtomS3RをUSBケーブルで接続する。

3. 緑色のLEDが点灯するまでリセットボタンを長押し（約2秒）してから離すことで、AtomS3Rを書き込み待機状態にする。

4. Platformioの画面(下図)で、① envとして「m5stack-atoms3r」または「m5stack-atoms3r-realtime」を選択し、②「Upload Filesystem Image」を実行する。

![](../images/atoms3r_upload_filesystem.png)

> Note:  
> WiFi接続できるようになった後は、FTPでSPIFFSのファイルの読み書きができます（ユーザ名：stackchan、パスワード：stackchan）。FFFTP等のフリーソフトで接続できます。
