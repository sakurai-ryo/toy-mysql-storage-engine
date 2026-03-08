### エラーハンドリング

- Storage Engineのエラーハンドリングはどうなっているか
  - my_error関数
  - InnoDBは以下を行なっている
    - mysql-server/include/my_base.hに定義されている`HA_ERR_`で始まるエラーコードを返すのが一般

```
my_error を呼ぶ場合
ユーザーに具体的なエラーメッセージを伝えたいとき。 サーバー層がそのエラーコードから適切なメッセージを生成できない場合に、ストレージエンジン側でメッセージをセットします。


// 具体的な情報をユーザーに伝える
my_error(ER_FK_DEPTH_EXCEEDED, MYF(0), FK_MAX_CASCADE_DEL);
return HA_ERR_FK_DEPTH_EXCEEDED;
my_error を呼ばない場合
サーバー層が HA_ERR_* コードから自動的に適切なメッセージを生成できるとき。 サーバー層には HA_ERR_* をユーザー向けメッセージに変換する仕組み（handler::print_error() など）があります。


// サーバー層が HA_ERR_UNSUPPORTED を適切に処理してくれる
return HA_ERR_UNSUPPORTED;
まとめ
パターン	用途
my_error() + return HA_ERR_*	エンジン固有の詳細なエラー情報を伝えたい
return HA_ERR_* のみ	汎用的なエラーでサーバー層のデフォルトメッセージで十分
my_error は MySQL のエラーメッセージキュー（THD に紐づく）にメッセージを積む関数です。サーバー層は HA_ERR_* を受け取った際、既にメッセージがキューにあればそれを使い、なければ print_error() でデフォルトメッセージを生成します。
```

### MySQLのラッパー関数

- システムコールなどは直接実行しない
  - ポータブルにするため
- MySQLが提供するラッパー関数を使用する
  - 例: `my_store_ptr`、`my_get_ptr`など
