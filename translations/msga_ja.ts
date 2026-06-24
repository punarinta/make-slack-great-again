<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="ja_JP" sourcelanguage="en_US">
<context>
    <name>slack::OAuthFlow</name>
    <message>
        <location filename="../src/backend/slack/oauth_flow.cpp" line="40"/>
        <source>App credentials are not configured.

Copy credentials.cmake.example to credentials.cmake, fill in your Slack app credentials, and rebuild.</source>
        <translation>アプリの認証情報が設定されていません。

credentials.cmake.example を credentials.cmake にコピーし、Slackアプリの認証情報を記入して再ビルドしてください。</translation>
    </message>
</context>
<context>
    <name>teams::OAuthFlow</name>
    <message>
        <location filename="../src/backend/teams/oauth_flow.cpp" line="51"/>
        <source>Microsoft Teams app credentials are not configured.

Set MSGA_TEAMS_CLIENT_ID in credentials.cmake and rebuild.</source>
        <translation>Microsoft Teams のアプリ認証情報が設定されていません。

credentials.cmake に MSGA_TEAMS_CLIENT_ID を設定して再ビルドしてください。</translation>
    </message>
</context>
<context>
    <name>LlmProviderBase</name>
    <message>
        <location filename="../src/llm/llm_provider_base.cpp" line="43"/>
        <source>%1 does not offer sign-in for third-party apps — use an API key</source>
        <translation>%1はサードパーティアプリのサインインに対応していません — APIキーを使用してください</translation>
    </message>
    <message>
        <location filename="../src/llm/llm_provider_base.cpp" line="74"/>
        <source>API key is empty</source>
        <translation>APIキーが空です</translation>
    </message>
    <message>
        <location filename="../src/llm/llm_provider_base.cpp" line="118"/>
        <source>Connected</source>
        <translation>接続済み</translation>
    </message>
    <message>
        <location filename="../src/llm/llm_provider_base.cpp" line="127"/>
        <source>%1 is not connected</source>
        <translation>%1は接続されていません</translation>
    </message>
    <message>
        <location filename="../src/llm/llm_provider_base.cpp" line="155"/>
        <source>Token refresh failed: %1</source>
        <translation>トークンの更新に失敗しました: %1</translation>
    </message>
</context>
<context>
    <name>LlmService</name>
    <message>
        <location filename="../src/llm/llm_service.cpp" line="51"/>
        <source>No AI provider connected — connect one in Settings → AI assistance</source>
        <translation>AIプロバイダーが接続されていません — 設定→「AIアシスタンス」で接続してください</translation>
    </message>
</context>
<context>
    <name>OAuthLoopbackFlow</name>
    <message>
        <location filename="../src/llm/oauth_loopback.cpp" line="43"/>
        <source>Could not listen on port %1: %2</source>
        <translation>ポート%1で待ち受けできませんでした: %2</translation>
    </message>
    <message>
        <location filename="../src/llm/oauth_loopback.cpp" line="90"/>
        <source>You can close this window and return to msga.</source>
        <translation>このウィンドウを閉じてmsgaに戻ってください。</translation>
    </message>
</context>
<context>
    <name>Session</name>
    <message>
        <location filename="../src/session/session.cpp" line="44"/>
        <source> — sign in to this workspace again to grant the new permission</source>
        <translation> — 新しい権限を付与するには、このワークスペースに再度サインインしてください</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="54"/>
        <source>You can't reply to this message.</source>
        <translation>このメッセージには返信できません。</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="56"/>
        <source>You're not a member of this channel.</source>
        <translation>このチャンネルのメンバーではありません。</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="58"/>
        <source>This conversation is archived.</source>
        <translation>この会話はアーカイブされています。</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="60"/>
        <source>The message is too long.</source>
        <translation>メッセージが長すぎます。</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="62"/>
        <source>This conversation no longer exists.</source>
        <translation>この会話は存在しません。</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="64"/>
        <source>You don't have permission to post here.</source>
        <translation>ここに投稿する権限がありません。</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="342"/>
        <source>Couldn't send message: %1</source>
        <translation>メッセージを送信できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="723"/>
        <source>Unknown user</source>
        <translation>不明なユーザー</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="887"/>
        <source>No such user: %1</source>
        <translation>ユーザーが見つかりません: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="938"/>
        <source>Usage: /dnd [duration, e.g. 30m or 2h] — or /dnd off to resume</source>
        <translation>使い方: /dnd [時間。例: 30m、2h] — 解除は /dnd off</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="954"/>
        <source>Command /%1 failed: %2</source>
        <translation>コマンド /%1 が失敗しました: %2</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="976"/>
        <source>Could not change presence: %1</source>
        <translation>プレゼンスを変更できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="994"/>
        <source>Could not set status: %1</source>
        <translation>ステータスを設定できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1015"/>
        <source>Could not update notifications: %1</source>
        <translation>通知設定を更新できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1037"/>
        <source>Could not update profile: %1</source>
        <translation>プロフィールを更新できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1058"/>
        <source>Could not update avatar: %1</source>
        <translation>アバターを更新できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1122"/>
        <source>Upload failed: %1</source>
        <translation>アップロードに失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1163"/>
        <source>Could not load canvas: %1</source>
        <translation>canvasを読み込めませんでした: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1194"/>
        <source>Could not create canvas: %1</source>
        <translation>canvasを作成できませんでした: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1210"/>
        <source>Canvas edit failed: %1</source>
        <translation>canvasの編集に失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1228"/>
        <source>Canvas deletion failed: %1</source>
        <translation>canvasの削除に失敗しました: %1</translation>
    </message>
</context>
<context>
    <name>MrkdwnParser</name>
    <message>
        <location filename="../src/text/mrkdwn_parser.cpp" line="91"/>
        <source>Today</source>
        <translation>今日</translation>
    </message>
    <message>
        <location filename="../src/text/mrkdwn_parser.cpp" line="93"/>
        <source>Yesterday</source>
        <translation>昨日</translation>
    </message>
    <message>
        <location filename="../src/text/mrkdwn_parser.cpp" line="95"/>
        <source>Tomorrow</source>
        <translation>明日</translation>
    </message>
</context>
<context>
    <name>BrowseChannelsDialog</name>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="49"/>
        <source>Search for channels</source>
        <translation>チャンネルを検索</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="55"/>
        <source>Create Channel</source>
        <translation>チャンネルを作成</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="72"/>
        <source>Channels</source>
        <translation>チャンネル</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="79"/>
        <source>People</source>
        <translation>メンバー</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="154"/>
        <source>%1 %2</source>
        <translation>%1%2</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="156"/>
        <source>member</source>
        <translation>人のメンバー</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="156"/>
        <source>members</source>
        <translation>人のメンバー</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="216"/>
        <source>Search for channels</source>
        <translation>チャンネルを検索</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="216"/>
        <source>Search for people</source>
        <translation>メンバーを検索</translation>
    </message>
</context>
<context>
    <name>BrowseListView</name>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_list_view.cpp" line="200"/>
        <source>Joined</source>
        <translation>参加済み</translation>
    </message>
</context>
<context>
    <name>CanvasPage</name>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="268"/>
        <source>Your canvas title</source>
        <translation>canvasのタイトル</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="272"/>
        <source>Go ahead, start writing!</source>
        <translation>さあ、書き始めましょう！</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="657"/>
        <source>This canvas was created with Slack's built-in editor and is not editable through the Slack API — it is read-only here.</source>
        <translation>このキャンバスは Slack の組み込みエディタで作成されており、Slack API 経由では編集できません。ここでは読み取り専用です。</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="659"/>
        <source>You don't have access to this canvas.</source>
        <translation>このキャンバスにアクセスする権限がありません。</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="676"/>
        <source>Copy link</source>
        <translation>リンクをコピー</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="685"/>
        <source>Delete canvas</source>
        <translation>canvasを削除</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="207"/>
        <source>Delete canvas</source>
        <translation>canvasを削除</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="211"/>
        <source>The canvas will be deleted for everyone in the conversation.
This action cannot be undone.</source>
        <translation>canvasはこの会話の全員に対して削除されます。
この操作は元に戻せません。</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="226"/>
        <source>Cancel</source>
        <translation>キャンセル</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="229"/>
        <source>Delete canvas</source>
        <translation>canvasを削除</translation>
    </message>
</context>
<context>
    <name>AttachmentStrip</name>
    <message>
        <location filename="../src/ui/composer/attachment_strip.cpp" line="297"/>
        <source>Remove attachment</source>
        <translation>添付ファイルを削除</translation>
    </message>
</context>
<context>
    <name>ComposerWidget</name>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="390"/>
        <source>Subject</source>
        <translation>件名</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="455"/>
        <source>Message #channel</source>
        <translation>#channel へのメッセージ</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="497"/>
        <source>Attach file</source>
        <translation>ファイルを添付</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="512"/>
        <source>Emoji</source>
        <translation>絵文字</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="513"/>
        <source>Mention</source>
        <translation>メンション</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="525"/>
        <source>Send message</source>
        <translation>メッセージを送信</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="534"/>
        <source>Schedule send</source>
        <translation>送信を予約</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1466"/>
        <source>Send at</source>
        <translation>送信日時</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1466"/>
        <source>Cancel</source>
        <translation>キャンセル</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1466"/>
        <source>Schedule</source>
        <translation>予約</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1592"/>
        <source>Attach File</source>
        <translation>ファイルを添付</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1624"/>
        <source>URL</source>
        <translation>URL</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1624"/>
        <source>Display text</source>
        <translation>表示テキスト</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1624"/>
        <source>Insert</source>
        <translation>挿入</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1624"/>
        <source>Cancel</source>
        <translation>キャンセル</translation>
    </message>
</context>
<context>
    <name>EditModeBanner</name>
    <message>
        <location filename="../src/ui/composer/edit_mode_banner.cpp" line="21"/>
        <source>Editing message</source>
        <translation>メッセージを編集中</translation>
    </message>
</context>
<context>
    <name>FormattingToolbar</name>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="66"/>
        <source>Bold</source>
        <translation>太字</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="67"/>
        <source>Italic</source>
        <translation>斜体</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="68"/>
        <source>Underline</source>
        <translation>下線</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="69"/>
        <source>Strikethrough</source>
        <translation>取り消し線</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="70"/>
        <source>Link</source>
        <translation>リンク</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="71"/>
        <source>Ordered list</source>
        <translation>番号付きリスト</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="72"/>
        <source>Bullet list</source>
        <translation>箇条書きリスト</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="73"/>
        <source>Blockquote</source>
        <translation>引用</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="74"/>
        <source>Inline code</source>
        <translation>インラインコード</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="75"/>
        <source>Code block</source>
        <translation>コードブロック</translation>
    </message>
</context>
<context>
    <name>MentionCompleter</name>
    <message>
        <location filename="../src/ui/composer/mention_completer.cpp" line="112"/>
        <source>Enter</source>
        <translation>Enter</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/mention_completer.cpp" line="161"/>
        <source>App</source>
        <translation>アプリ</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/mention_completer.cpp" line="306"/>
        <source>Enter</source>
        <translation>Enter</translation>
    </message>
</context>
<context>
    <name>ConvFooterWidget</name>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="135"/>
        <source>%n background task(s) running</source>
        <translation>%n 件のバックグラウンドタスクを実行中</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="215"/>
        <source>Hidden — you appear away to everyone. Click to use automatic presence.</source>
        <translation>非表示 — 全員に離席中と表示されます。クリックすると自動プレゼンスを使用します。</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="217"/>
        <source>Visible — but you appear away if no official Slack client is connected. Click to hide.</source>
        <translation>表示中 — ただし公式Slackクライアントが接続されていない場合は離席中と表示されます。クリックすると非表示にします。</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="221"/>
        <source>Visible — using automatic presence. Click to appear hidden.</source>
        <translation>表示中 — 自動プレゼンスを使用しています。クリックすると非表示にします。</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="291"/>
        <source>Profile &amp; status</source>
        <translation>プロフィールとステータス</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="335"/>
        <source>Manage profile</source>
        <translation>プロフィールを管理</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="341"/>
        <source>Manage status</source>
        <translation>ステータスを管理</translation>
    </message>
</context>
<context>
    <name>ConvListWidget</name>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="521"/>
        <source>Open a direct message</source>
        <translation>ダイレクトメッセージを開く</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="531"/>
        <source>Notify you about…</source>
        <translation>通知対象…</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="533"/>
        <source>All new posts</source>
        <translation>すべての新着投稿</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="540"/>
        <source>Just mentions</source>
        <translation>メンションのみ</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="547"/>
        <source>Mute and hide</source>
        <translation>ミュートして非表示</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="561"/>
        <source>Unstar channel</source>
        <translation>チャンネルのスターを外す</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="561"/>
        <source>Star channel</source>
        <translation>チャンネルにスターを付ける</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="568"/>
        <source>Leave channel</source>
        <translation>チャンネルから退出</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="581"/>
        <source>Unstar conversation</source>
        <translation>会話のスターを外す</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="581"/>
        <source>Star conversation</source>
        <translation>会話にスターを付ける</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="588"/>
        <source>Leave conversation</source>
        <translation>会話から退出</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="654"/>
        <source>Find a channel</source>
        <translation>チャンネルを探す</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="655"/>
        <source>Create a channel</source>
        <translation>チャンネルを作成</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="808"/>
        <source>Channels</source>
        <translation>チャンネル</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="809"/>
        <source>Direct messages</source>
        <translation>ダイレクトメッセージ</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="810"/>
        <source>Agents &amp; apps</source>
        <translation>エージェントとアプリ</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="844"/>
        <source>Add channels</source>
        <translation>チャンネルを追加</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="864"/>
        <source>%1 more %2</source>
        <translation>他%1件の%2</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="864"/>
        <source>channel</source>
        <translation>チャンネル</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="864"/>
        <source>channels</source>
        <translation>チャンネル</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1020"/>
        <source>EXT</source>
        <translation>外部</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1041"/>
        <source>you</source>
        <translation>自分</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1086"/>
        <source>you</source>
        <translation>自分</translation>
    </message>
</context>
<context>
    <name>ConvSelectorWidget</name>
    <message>
        <location filename="../src/ui/conv_selector/conv_selector_widget.cpp" line="61"/>
        <source>Search channels and people…</source>
        <translation>チャンネルやメンバーを検索…</translation>
    </message>
</context>
<context>
    <name>ConvTabsWidget</name>
    <message>
        <location filename="../src/ui/conv_tabs/conv_tabs_widget.cpp" line="62"/>
        <source>Messages</source>
        <translation>メッセージ</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_tabs/conv_tabs_widget.cpp" line="67"/>
        <source>Untitled</source>
        <translation>無題</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_tabs/conv_tabs_widget.cpp" line="67"/>
        <source>Add canvas</source>
        <translation>canvasを追加</translation>
    </message>
</context>
<context>
    <name>CreateChannelDialog</name>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="19"/>
        <source>Create a channel</source>
        <translation>チャンネルを作成</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="33"/>
        <source>Name</source>
        <translation>名前</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="42"/>
        <source>e.g. plan-budget</source>
        <translation>例: plan-budget</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="46"/>
        <source>Channels are where conversations happen around a topic. Use a name that is easy to find and understand.</source>
        <translation>チャンネルはトピックごとに会話が行われる場所です。見つけやすく、わかりやすい名前を付けましょう。</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="57"/>
        <source>Next</source>
        <translation>次へ</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="77"/>
        <source>Visibility</source>
        <translation>公開範囲</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="87"/>
        <source>Private — only specific people</source>
        <translation>プライベート — 特定のメンバーのみ</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="90"/>
        <source>Can only be viewed or joined by invitation</source>
        <translation>招待されたメンバーのみ閲覧・参加できます</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="97"/>
        <source>Step 2 of 2</source>
        <translation>ステップ 2/2</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="98"/>
        <source>Back</source>
        <translation>戻る</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="99"/>
        <source>Create</source>
        <translation>作成</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="139"/>
        <source>this workspace</source>
        <translation>このワークスペース</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="140"/>
        <source>Public — anyone in %1</source>
        <translation>パブリック — %1の全員</translation>
    </message>
</context>
<context>
    <name>DeleteMessageDialog</name>
    <message>
        <location filename="../src/ui/delete_message_dialog/delete_message_dialog.cpp" line="17"/>
        <source>Delete message</source>
        <translation>メッセージを削除</translation>
    </message>
    <message>
        <location filename="../src/ui/delete_message_dialog/delete_message_dialog.cpp" line="21"/>
        <source>This action cannot be undone.</source>
        <translation>この操作は元に戻せません。</translation>
    </message>
    <message>
        <location filename="../src/ui/delete_message_dialog/delete_message_dialog.cpp" line="68"/>
        <source>Cancel</source>
        <translation>キャンセル</translation>
    </message>
    <message>
        <location filename="../src/ui/delete_message_dialog/delete_message_dialog.cpp" line="69"/>
        <source>Delete</source>
        <translation>削除</translation>
    </message>
</context>
<context>
    <name>EmojiGrid</name>
    <message>
        <location filename="../src/ui/emoji_picker/emoji_picker_popup.cpp" line="430"/>
        <source>Search all emoji</source>
        <translation>すべての絵文字を検索</translation>
    </message>
    <message>
        <location filename="../src/ui/emoji_picker/emoji_picker_popup.cpp" line="451"/>
        <source>Skin Tone</source>
        <translation>肌の色</translation>
    </message>
    <message>
        <location filename="../src/ui/emoji_picker/emoji_picker_popup.cpp" line="593"/>
        <source>Search Results</source>
        <translation>検索結果</translation>
    </message>
    <message>
        <location filename="../src/ui/emoji_picker/emoji_picker_popup.cpp" line="623"/>
        <source>Frequently Used</source>
        <translation>よく使う絵文字</translation>
    </message>
    <message>
        <location filename="../src/ui/emoji_picker/emoji_picker_popup.cpp" line="661"/>
        <source>Custom</source>
        <translation>カスタム</translation>
    </message>
</context>
<context>
    <name>ForwardDialog</name>
    <message>
        <location filename="../src/ui/forward_dialog/forward_dialog.cpp" line="24"/>
        <source>Forward this message</source>
        <translation>このメッセージを転送</translation>
    </message>
    <message>
        <location filename="../src/ui/forward_dialog/forward_dialog.cpp" line="34"/>
        <source>Add a message, if you'd like.</source>
        <translation>必要に応じてメッセージを追加できます。</translation>
    </message>
    <message>
        <location filename="../src/ui/forward_dialog/forward_dialog.cpp" line="159"/>
        <source>Copy Link</source>
        <translation>リンクをコピー</translation>
    </message>
    <message>
        <location filename="../src/ui/forward_dialog/forward_dialog.cpp" line="160"/>
        <source>Cancel</source>
        <translation>キャンセル</translation>
    </message>
    <message>
        <location filename="../src/ui/forward_dialog/forward_dialog.cpp" line="161"/>
        <source>Forward</source>
        <translation>転送</translation>
    </message>
</context>
<context>
    <name>HuddleBanner</name>
    <message>
        <location filename="../src/ui/huddle_banner/huddle_banner.cpp" line="30"/>
        <source>A huddle is happening</source>
        <translation>ハドルが開催中です</translation>
    </message>
    <message>
        <location filename="../src/ui/huddle_banner/huddle_banner.cpp" line="33"/>
        <source>Join</source>
        <translation>参加</translation>
    </message>
    <message>
        <location filename="../src/ui/huddle_banner/huddle_banner.cpp" line="63"/>
        <source>Opens the huddle in Slack for web</source>
        <translation>Slack（ブラウザ版）でハドルを開きます</translation>
    </message>
</context>
<context>
    <name>ImageViewerOverlay</name>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="45"/>
        <source>Download</source>
        <translation>ダウンロード</translation>
    </message>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="46"/>
        <source>Forward</source>
        <translation>転送</translation>
    </message>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="47"/>
        <source>Open in browser</source>
        <translation>ブラウザで開く</translation>
    </message>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="48"/>
        <source>More actions</source>
        <translation>その他の操作</translation>
    </message>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="49"/>
        <source>Close</source>
        <translation>閉じる</translation>
    </message>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="146"/>
        <source>Loading image…</source>
        <translation>画像を読み込み中…</translation>
    </message>
</context>
<context>
    <name>ImapAddAccountDialog</name>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="21"/>
        <source>Add email account</source>
        <translation>メールアカウントを追加</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="35"/>
        <source>IMAP server</source>
        <translation>IMAP サーバー</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="37"/>
        <source>Detect</source>
        <translation>自動検出</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="47"/>
        <source>Sign in with Google</source>
        <translation>Google でサインイン</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="53"/>
        <source>Password or app password</source>
        <translation>パスワードまたはアプリパスワード</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="70"/>
        <source>Continue</source>
        <translation>続行</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="71"/>
        <source>Cancel</source>
        <translation>キャンセル</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="93"/>
        <source>Enter a valid email address.</source>
        <translation>有効なメールアドレスを入力してください。</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="107"/>
        <source>Add</source>
        <translation>追加</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="126"/>
        <source>Add</source>
        <translation>追加</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="132"/>
        <source>Sign in with %1</source>
        <translation>%1 でサインイン</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="150"/>
        <source>%1 needs an app password to sign in</source>
        <translation>%1 でサインインするにはアプリパスワードが必要です</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="152"/>
        <source>%1 needs an app password</source>
        <translation>%1 にはアプリパスワードが必要です</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="167"/>
        <source>Server: %1</source>
        <translation>サーバー: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="174"/>
        <source>Couldn't detect the server — enter it manually.</source>
        <translation>サーバーを検出できませんでした — 手動で入力してください。</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="194"/>
        <source>Detecting…</source>
        <translation>検出中…</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="202"/>
        <source>Enter the IMAP server or click Detect.</source>
        <translation>IMAP サーバーを入力するか、自動検出をクリックしてください。</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="206"/>
        <source>Enter your password.</source>
        <translation>パスワードを入力してください。</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="216"/>
        <source>Checking…</source>
        <translation>確認中…</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="224"/>
        <source>Sign-in failed: %1</source>
        <translation>サインインに失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="248"/>
        <source>Opening your browser to sign in…</source>
        <translation>サインインのためブラウザを開いています…</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="256"/>
        <source>Sign-in failed: no access token returned.</source>
        <translation>サインインに失敗しました: アクセストークンが返されませんでした。</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="271"/>
        <source>Checking…</source>
        <translation>確認中…</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="275"/>
        <source>Sign-in failed: %1</source>
        <translation>サインインに失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="286"/>
        <source>Sign-in failed: %1</source>
        <translation>サインインに失敗しました: %1</translation>
    </message>
</context>
<context>
    <name>MainWindow</name>
    <message>
        <location filename="../src/ui/main_window.cpp" line="398"/>
        <source>Log in to workspace</source>
        <translation>ワークスペースにログイン</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="847"/>
        <source>Couldn't apply the label.</source>
        <translation>ラベルを適用できませんでした。</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1243"/>
        <source>Login failed</source>
        <translation>ログインに失敗しました</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1243"/>
        <source>This service is not supported.</source>
        <translation>このサービスはサポートされていません。</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1264"/>
        <source>Login failed</source>
        <translation>ログインに失敗しました</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1485"/>
        <source>This is the beginning of your direct message history with %1.</source>
        <translation>ここから%1とのダイレクトメッセージの履歴が始まります。</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1491"/>
        <source>Message</source>
        <translation>メッセージ</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1491"/>
        <source>Message %1</source>
        <translation>%1へのメッセージ</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1706"/>
        <source>Someone</source>
        <translation>誰か</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1826"/>
        <source>Someone</source>
        <translation>誰か</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1832"/>
        <source>Started a huddle</source>
        <translation>ハドルを開始しました</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1835"/>
        <source>%1 started a huddle</source>
        <translation>%1さんがハドルを開始しました</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1865"/>
        <source>Join</source>
        <translation>参加</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1891"/>
        <source>Sample User</source>
        <translation>サンプルユーザー</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1899"/>
        <source>Hey — do you have a minute?</source>
        <translation>ちょっといいですか？</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1903"/>
        <source>%1: Heads up, the deploy is going out at 3pm</source>
        <translation>%1: お知らせ、デプロイは15時に行われます</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1907"/>
        <source>%1 started a huddle</source>
        <translation>%1さんがハドルを開始しました</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1912"/>
        <source>Join</source>
        <translation>参加</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2057"/>
        <source>Workspace admin</source>
        <translation>ワークスペース管理</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2064"/>
        <source>Log out</source>
        <translation>ログアウト</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2064"/>
        <source>Log out from %1</source>
        <translation>%1からログアウト</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2123"/>
        <source>Settings</source>
        <translation>設定</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2128"/>
        <source>Quit</source>
        <translation>終了</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2348"/>
        <source>Opens the huddle in Slack for web</source>
        <translation>Slack（ブラウザ版）でハドルを開きます</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2358"/>
        <source>Unstar conversation</source>
        <translation>会話のスターを外す</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2358"/>
        <source>Star conversation</source>
        <translation>会話にスターを付ける</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2369"/>
        <source>Search messages</source>
        <translation>メッセージを検索</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2504"/>
        <source>This is the beginning of your direct message history with %1.</source>
        <translation>ここから%1とのダイレクトメッセージの履歴が始まります。</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2589"/>
        <source>Message</source>
        <translation>メッセージ</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2589"/>
        <source>Message %1</source>
        <translation>%1へのメッセージ</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1278"/>
        <source>You appear away to others — no official Slack client is connected</source>
        <translation>公式Slackクライアントが接続されていないため、他のメンバーには離席中と表示されます</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1282"/>
        <source>Active</source>
        <translation>アクティブ</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1284"/>
        <source>Away</source>
        <translation>離席中</translation>
    </message>
</context>
<context>
    <name>MentionPopup</name>
    <message>
        <location filename="../src/ui/mention_popup/mention_popup.cpp" line="442"/>
        <source>(you)</source>
        <translation>(あなた)</translation>
    </message>
    <message>
        <location filename="../src/ui/mention_popup/mention_popup.cpp" line="125"/>
        <source>Enter</source>
        <translation>Enter</translation>
    </message>
    <message>
        <location filename="../src/ui/mention_popup/mention_popup.cpp" line="154"/>
        <source>APP</source>
        <translation>アプリ</translation>
    </message>
</context>
<context>
    <name>MessageListWidget</name>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="1660"/>
        <source>Reply in thread</source>
        <translation>スレッドで返信</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="1674"/>
        <source>Edit message</source>
        <translation>メッセージを編集</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="1688"/>
        <source>Copy link</source>
        <translation>リンクをコピー</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="1698"/>
        <source>Copy message</source>
        <translation>メッセージをコピー</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="1710"/>
        <source>Unpin from channel</source>
        <translation>チャンネルへのピン留めを解除</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="1727"/>
        <source>Pin to channel</source>
        <translation>チャンネルにピン留め</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="1748"/>
        <source>Forward message</source>
        <translation>メッセージを転送</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="1759"/>
        <source>Delete message…</source>
        <translation>メッセージを削除…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="1793"/>
        <source>You</source>
        <translation>あなた</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2008"/>
        <source>Slack doesn't let third-party apps press bot buttons, we are working on a workaround</source>
        <translation>Slack はサードパーティ製アプリからのボットボタン操作を許可していません。回避策を検討中です。</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2023"/>
        <source>No email app — address copied</source>
        <translation>メールアプリがないため、アドレスをコピーしました</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2059"/>
        <source>file</source>
        <translation>ファイル</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2061"/>
        <source>Save file</source>
        <translation>ファイルを保存</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2067"/>
        <source>Downloading %1</source>
        <translation>%1をダウンロード中</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2109"/>
        <source>image</source>
        <translation>画像</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2110"/>
        <source>Copying %1</source>
        <translation>%1をコピー中</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2160"/>
        <source>Copy link to image</source>
        <translation>画像へのリンクをコピー</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2160"/>
        <source>Copy link to file</source>
        <translation>ファイルへのリンクをコピー</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2170"/>
        <source>Copy full image</source>
        <translation>画像全体をコピー</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2184"/>
        <source>Delete image…</source>
        <translation>画像を削除…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2184"/>
        <source>Delete file…</source>
        <translation>ファイルを削除…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2636"/>
        <source>Add reaction</source>
        <translation>リアクションを追加</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2636"/>
        <source>Forward message</source>
        <translation>メッセージを転送</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2636"/>
        <source>More actions</source>
        <translation>その他の操作</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2645"/>
        <source>Download</source>
        <translation>ダウンロード</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2645"/>
        <source>Share</source>
        <translation>共有</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2645"/>
        <source>More actions</source>
        <translation>その他の操作</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2677"/>
        <source>Remove preview</source>
        <translation>プレビューを削除</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="65"/>
        <source>Oh my gosh, I really apologize, but your company is a reaaaly active Slack user. Still loading...</source>
        <translation>申し訳ありません。御社は本っ当にSlackをよく使っていますね。まだ読み込み中です...</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="68"/>
        <source>Oh, you must have a lot of co-workers and messages! Still loading...</source>
        <translation>同僚もメッセージもとても多いようですね！まだ読み込み中です...</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="70"/>
        <source>Loading your stuff...</source>
        <translation>データを読み込み中...</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="182"/>
        <source>Pinned by %1</source>
        <translation>%1がピン留めしました</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="182"/>
        <source>Pinned</source>
        <translation>ピン留め済み</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="479"/>
        <source>APP</source>
        <translation>アプリ</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="505"/>
        <source>EXT</source>
        <translation>外部</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="533"/>
        <source>(edited)</source>
        <translation>（編集済み）</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="754"/>
        <source>Loading image…</source>
        <translation>画像を読み込み中…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1214"/>
        <source>1 reply</source>
        <translation>1件の返信</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1214"/>
        <source>%1 replies</source>
        <translation>%1件の返信</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1237"/>
        <source>Close thread</source>
        <translation>スレッドを閉じる</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1254"/>
        <source>View thread</source>
        <translation>スレッドを表示</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1268"/>
        <source>Last reply</source>
        <translation>最後の返信</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1268"/>
        <source>Last reply %1</source>
        <translation>最後の返信: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1524"/>
        <source>Loading replies…</source>
        <translation>返信を読み込み中…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1548"/>
        <source>Reply to thread</source>
        <translation>スレッドに返信</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1557"/>
        <source>Reply to thread</source>
        <translation>スレッドに返信</translation>
    </message>
</context>
<context>
    <name>MsgRender</name>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="174"/>
        <source>Today</source>
        <translation>今日</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="176"/>
        <source>Yesterday</source>
        <translation>昨日</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="192"/>
        <source>today at %1</source>
        <translation>今日 %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="194"/>
        <source>yesterday at %1</source>
        <translation>昨日 %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="195"/>
        <source>%1 at %2</source>
        <translation>%1 %2</translation>
    </message>
</context>
<context>
    <name>ProfileAvatarWidget</name>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="98"/>
        <source>Change photo</source>
        <translation>写真を変更</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="118"/>
        <source>Profile</source>
        <translation>プロフィール</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="142"/>
        <source>Name</source>
        <translation>名前</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="144"/>
        <source>Your display name</source>
        <translation>表示名</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="145"/>
        <source>Email</source>
        <translation>メールアドレス</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="146"/>
        <source>name@example.com</source>
        <translation>name@example.com</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="147"/>
        <source>Phone</source>
        <translation>電話番号</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="148"/>
        <source>Optional</source>
        <translation>任意</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="159"/>
        <source>Cancel</source>
        <translation>キャンセル</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="160"/>
        <source>Save Changes</source>
        <translation>変更を保存</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="250"/>
        <source>Choose a profile photo</source>
        <translation>プロフィール写真を選択</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="250"/>
        <source>Images (*.png *.jpg *.jpeg *.gif)</source>
        <translation>画像 (*.png *.jpg *.jpeg *.gif)</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="255"/>
        <source>Uploading photo…</source>
        <translation>写真をアップロード中…</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="260"/>
        <source>Could not upload photo: %1</source>
        <translation>写真をアップロードできませんでした: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="263"/>
        <source>Photo updated.</source>
        <translation>写真を更新しました。</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="292"/>
        <source>Saving…</source>
        <translation>保存中…</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="296"/>
        <source>Could not save: %1</source>
        <translation>保存できませんでした: %1</translation>
    </message>
</context>
<context>
    <name>SearchWidget</name>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="77"/>
        <source>Search messages…</source>
        <translation>メッセージを検索…</translation>
    </message>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="234"/>
        <source>Search messages</source>
        <translation>メッセージを検索</translation>
    </message>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="245"/>
        <source>Close search</source>
        <translation>検索を閉じる</translation>
    </message>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="313"/>
        <source>Searching…</source>
        <translation>検索中…</translation>
    </message>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="386"/>
        <source>No results found.</source>
        <translation>結果が見つかりませんでした。</translation>
    </message>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="400"/>
        <source>Unknown channel</source>
        <translation>不明なチャンネル</translation>
    </message>
</context>
<context>
    <name>SettingsDialog</name>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="106"/>
        <source>RAM used: %1</source>
        <translation>RAM使用量: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="138"/>
        <source>Settings</source>
        <translation>設定</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="158"/>
        <source>Appearance</source>
        <translation>外観</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="159"/>
        <source>Notifications</source>
        <translation>通知</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="160"/>
        <source>AI assistance</source>
        <translation>AIアシスタンス</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="161"/>
        <source>Storage</source>
        <translation>ストレージ</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="162"/>
        <source>System</source>
        <translation>システム</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="163"/>
        <source>About</source>
        <translation>情報</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="644"/>
        <source>License</source>
        <translation>ライセンス</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="649"/>
        <source>MSGA — Make Slack Great Again
Copyright © 2026 Vladimir Osipov

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version (GPL-3.0-or-later).</source>
        <translation>MSGA — Make Slack Great Again
Copyright © 2026 Vladimir Osipov

本プログラムはフリーソフトウェアです。フリーソフトウェア財団が公開する GNU General Public License（バージョン 3、または任意でそれ以降のバージョン）の条項に基づき、再配布および改変を行うことができます（GPL-3.0-or-later）。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="662"/>
        <source>View full license</source>
        <translation>ライセンス全文を表示</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="674"/>
        <source>Contact</source>
        <translation>連絡先</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="680"/>
        <source>Questions or feedback: %1</source>
        <translation>ご質問・ご意見: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="688"/>
        <source>Found a bug?</source>
        <translation>バグを見つけましたか？</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="693"/>
        <source>Report it on GitHub so it can be tracked and fixed.</source>
        <translation>GitHub で報告すると、追跡・修正できます。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="699"/>
        <source>Report a bug</source>
        <translation>バグを報告</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="178"/>
        <source>Color theme</source>
        <translation>カラーテーマ</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="197"/>
        <source>Purple</source>
        <translation>紫</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="198"/>
        <source>Blue</source>
        <translation>青</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="199"/>
        <source>Green</source>
        <translation>グリーン</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="217"/>
        <source>Language</source>
        <translation>言語</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="228"/>
        <source>App language</source>
        <translation>アプリの言語</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="236"/>
        <source>System default</source>
        <translation>システムのデフォルト</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="245"/>
        <source>The new language will be applied the next time MSGA starts.
Time and date formats update immediately.</source>
        <translation>新しい言語は次回MSGAの起動時に適用されます。
時刻と日付の形式はすぐに更新されます。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="261"/>
        <source>Date/Time</source>
        <translation>日付と時刻</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="271"/>
        <source>12-hour clock (2:34 PM)</source>
        <translation>12時間表示（午後2:34）</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="272"/>
        <source>24-hour clock (14:34)</source>
        <translation>24時間表示（14:34）</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="283"/>
        <source>Threads</source>
        <translation>スレッド</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="294"/>
        <source>Standalone (open replies in a side panel)</source>
        <translation>スタンドアロン（サイドパネルで返信を開く）</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="295"/>
        <source>Inline (expand replies under the message)</source>
        <translation>インライン（メッセージの下に返信を展開）</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="306"/>
        <source>Conversations</source>
        <translation>会話</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="317"/>
        <source>Show conversations active in the last</source>
        <translation>次の期間内に活動のあった会話を表示:</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="323"/>
        <source> days</source>
        <translation> 日</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="330"/>
        <source>Conversations with no activity in this period are hidden
under an &quot;N more...&quot; row at the bottom of each section.</source>
        <translation>この期間に活動がない会話は、各セクション下部の
「他N件...」の行に隠れます。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="343"/>
        <source>Save</source>
        <translation>保存</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="360"/>
        <source>Enable desktop notifications</source>
        <translation>デスクトップ通知を有効にする</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="369"/>
        <source>All new messages</source>
        <translation>すべての新着メッセージ</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="370"/>
        <source>Direct messages and mentions only</source>
        <translation>ダイレクトメッセージとメンションのみ</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="380"/>
        <source>Notify me when a huddle starts</source>
        <translation>ハドルが開始したら通知する</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="383"/>
        <source>Play a sound for notifications</source>
        <translation>通知音を鳴らす</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="392"/>
        <source>Sound:</source>
        <translation>サウンド:</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="397"/>
        <source>Test</source>
        <translation>テスト</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="412"/>
        <source>Sample notifications</source>
        <translation>通知のサンプル</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="420"/>
        <source>New DM</source>
        <translation>新しいダイレクトメッセージ</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="421"/>
        <source>New channel message</source>
        <translation>新しいチャンネルメッセージ</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="422"/>
        <source>New huddle</source>
        <translation>新しいハドル</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="425"/>
        <source>Test</source>
        <translation>テスト</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="455"/>
        <source>Save</source>
        <translation>保存</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="475"/>
        <source>Cache</source>
        <translation>キャッシュ</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="480"/>
        <source>Cache size:</source>
        <translation>キャッシュサイズ:</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="490"/>
        <source>Conversations, user names, message history, and image thumbnails
stored locally to speed up startup.</source>
        <translation>起動を高速化するため、会話、ユーザー名、メッセージ履歴、
画像サムネイルをローカルに保存しています。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="499"/>
        <source>Limit cache to</source>
        <translation>キャッシュ上限:</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="505"/>
        <source> MB</source>
        <translation> MB</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="512"/>
        <source>When the cache grows past this limit, the least recently
viewed images are deleted first.</source>
        <translation>キャッシュがこの上限を超えると、最も長く表示されていない
画像から順に削除されます。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="531"/>
        <source>Clear cache</source>
        <translation>キャッシュをクリア</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="541"/>
        <source>State</source>
        <translation>状態</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="546"/>
        <source>Sidebar visit history used to decide which conversations are shown.
Clear this to let the app re-analyse activity from scratch on next load.</source>
        <translation>どの会話を表示するかの判断に使う、サイドバーの閲覧履歴です。
クリアすると、次回読み込み時に活動を最初から分析し直します。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="556"/>
        <source>Clear state</source>
        <translation>状態をクリア</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="575"/>
        <source>Version</source>
        <translation>バージョン</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="582"/>
        <source>Version %1, built %2</source>
        <translation>バージョン %1（ビルド: %2）</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="593"/>
        <source>Check for updates</source>
        <translation>アップデートを確認</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="609"/>
        <source>Memory</source>
        <translation>メモリ</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="630"/>
        <source>RAM used: %1</source>
        <translation>RAM使用量: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="654"/>
        <source>Connect an AI provider to enable assistant features.
Create an API key in your own provider account and paste it below —
it is stored on this computer and sent only to that provider.</source>
        <translation>アシスタント機能を使うにはAIプロバイダーを接続してください。
プロバイダーのアカウントでAPIキーを作成し、下に貼り付けてください。
キーはこのコンピューターに保存され、そのプロバイダーにのみ送信されます。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="662"/>
        <source>AI provider</source>
        <translation>AIプロバイダー</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="670"/>
        <source>Default:</source>
        <translation>デフォルト:</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="707"/>
        <source>Connect (OAuth)</source>
        <translation>接続（OAuth）</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="711"/>
        <source>Disconnect</source>
        <translation>切断</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="720"/>
        <source>Paste your API key</source>
        <translation>APIキーを貼り付け</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="724"/>
        <source>Save key</source>
        <translation>キーを保存</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="730"/>
        <source>Get an API key from %1…</source>
        <translation>%1でAPIキーを取得…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="759"/>
        <source>%1: %2</source>
        <translation>%1: %2</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="785"/>
        <source>Connected as %1</source>
        <translation>%1として接続済み</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="786"/>
        <source>Connected with API key (%1)</source>
        <translation>APIキーで接続済み（%1）</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="789"/>
        <source>Waiting for browser sign-in…</source>
        <translation>ブラウザでのサインインを待っています…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="791"/>
        <source>Not connected</source>
        <translation>未接続</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1199"/>
        <source>Last checked: %1</source>
        <translation>最終確認: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1207"/>
        <source>Update checks not available.</source>
        <translation>アップデートの確認は利用できません。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1212"/>
        <source>Checking for updates…</source>
        <translation>アップデートを確認中…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1215"/>
        <source>Update downloaded — restart the app to apply.</source>
        <translation>アップデートをダウンロードしました — 適用するにはアプリを再起動してください。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1229"/>
        <source>Checking for updates…</source>
        <translation>アップデートを確認中…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1233"/>
        <source>msga is up to date.</source>
        <translation>msgaは最新の状態です。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1237"/>
        <source>Version %1 available — downloading…</source>
        <translation>バージョン%1が利用可能です — ダウンロード中…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1240"/>
        <source>Downloading update… %1%</source>
        <translation>アップデートをダウンロード中… %1%</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1244"/>
        <source>Update downloaded — restart the app to apply.</source>
        <translation>アップデートをダウンロードしました — 適用するにはアプリを再起動してください。</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1249"/>
        <source>Check failed: %1</source>
        <translation>確認に失敗しました: %1</translation>
    </message>
</context>
<context>
    <name>StatusDialog</name>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="105"/>
        <source>Set a status</source>
        <translation>ステータスを設定</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="150"/>
        <source>What's your status?</source>
        <translation>ステータスは？</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="161"/>
        <source>In a meeting</source>
        <translation>会議中</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="162"/>
        <source>Commuting</source>
        <translation>通勤中</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="163"/>
        <source>Out sick</source>
        <translation>体調不良</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="164"/>
        <source>Vacationing</source>
        <translation>休暇中</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="165"/>
        <source>Working remotely</source>
        <translation>リモート勤務</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="168"/>
        <source>Don't clear</source>
        <translation>クリアしない</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="169"/>
        <source>30 minutes</source>
        <translation>30分</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="170"/>
        <source>1 hour</source>
        <translation>1時間</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="171"/>
        <source>4 hours</source>
        <translation>4時間</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="172"/>
        <source>Today</source>
        <translation>今日</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="173"/>
        <source>This week</source>
        <translation>今週</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="179"/>
        <source>Suggestions</source>
        <translation>候補</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="179"/>
        <source>For %1</source>
        <translation>%1向け</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="203"/>
        <source>Clear after</source>
        <translation>クリアするタイミング</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="210"/>
        <source>Don't clear</source>
        <translation>クリアしない</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="211"/>
        <source>30 minutes</source>
        <translation>30分</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="212"/>
        <source>1 hour</source>
        <translation>1時間</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="213"/>
        <source>4 hours</source>
        <translation>4時間</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="214"/>
        <source>Today</source>
        <translation>今日</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="215"/>
        <source>This week</source>
        <translation>今週</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="226"/>
        <source>Clear status</source>
        <translation>ステータスをクリア</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="231"/>
        <source>Cancel</source>
        <translation>キャンセル</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="232"/>
        <source>Save</source>
        <translation>保存</translation>
    </message>
</context>
<context>
    <name>ThreadPanel</name>
    <message>
        <location filename="../src/ui/thread_panel/thread_panel.cpp" line="66"/>
        <source>Thread</source>
        <translation>スレッド</translation>
    </message>
    <message>
        <location filename="../src/ui/thread_panel/thread_panel.cpp" line="135"/>
        <source>Reply in thread…</source>
        <translation>スレッドに返信…</translation>
    </message>
</context>
<context>
    <name>TitleBar</name>
    <message>
        <location filename="../src/ui/title_bar/title_bar.cpp" line="216"/>
        <source>Unpin window</source>
        <translation>ウィンドウの固定を解除</translation>
    </message>
    <message>
        <location filename="../src/ui/title_bar/title_bar.cpp" line="216"/>
        <source>Pin window on top</source>
        <translation>ウィンドウを最前面に固定</translation>
    </message>
</context>
<context>
    <name>TypingIndicatorWidget</name>
    <message>
        <location filename="../src/ui/typing_indicator/typing_indicator.cpp" line="94"/>
        <source>&lt;b&gt;You&lt;/b&gt; are typing on another device…</source>
        <translation>&lt;b&gt;あなた&lt;/b&gt;が別のデバイスで入力中…</translation>
    </message>
    <message>
        <location filename="../src/ui/typing_indicator/typing_indicator.cpp" line="102"/>
        <source>You</source>
        <translation>あなた</translation>
    </message>
    <message>
        <location filename="../src/ui/typing_indicator/typing_indicator.cpp" line="106"/>
        <source>%1 is typing…</source>
        <translation>%1が入力中…</translation>
    </message>
    <message>
        <location filename="../src/ui/typing_indicator/typing_indicator.cpp" line="106"/>
        <source>%1 are typing…</source>
        <translation>%1が入力中…</translation>
    </message>
</context>
<context>
    <name>UpdateBar</name>
    <message>
        <location filename="../src/ui/update_bar/update_bar.cpp" line="66"/>
        <source>A new version of msga has been downloaded. Restart to apply.</source>
        <translation>msgaの新しいバージョンをダウンロードしました。適用するには再起動してください。</translation>
    </message>
    <message>
        <location filename="../src/ui/update_bar/update_bar.cpp" line="67"/>
        <source>Restart now</source>
        <translation>今すぐ再起動</translation>
    </message>
    <message>
        <location filename="../src/ui/update_bar/update_bar.cpp" line="69"/>
        <source>A new version of msga is ready to install.</source>
        <translation>msgaの新しいバージョンをインストールする準備ができました。</translation>
    </message>
    <message>
        <location filename="../src/ui/update_bar/update_bar.cpp" line="70"/>
        <source>Open installer</source>
        <translation>インストーラーを開く</translation>
    </message>
</context>
<context>
    <name>UpdateChecker</name>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="83"/>
        <source>Automatic updates are not supported on this platform.</source>
        <translation>このプラットフォームでは自動アップデートはサポートされていません。</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="105"/>
        <source>Could not parse version manifest.</source>
        <translation>バージョンマニフェストを解析できませんでした。</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="125"/>
        <source>Cannot write update to %1</source>
        <translation>%1にアップデートを書き込めません</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="152"/>
        <source>Download failed: %1</source>
        <translation>ダウンロードに失敗しました: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="169"/>
        <source>Could not replace binary: %1</source>
        <translation>バイナリを置き換えられませんでした: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="185"/>
        <source>Could not move current binary — check file permissions on %1</source>
        <translation>現在のバイナリを移動できませんでした — %1のファイル権限を確認してください</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="191"/>
        <source>Could not place new binary at %1</source>
        <translation>新しいバイナリを%1に配置できませんでした</translation>
    </message>
</context>
<context>
    <name>UserProfileCard</name>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="65"/>
        <source>Deactivated account</source>
        <translation>無効化されたアカウント</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="67"/>
        <source>Workspace Owner</source>
        <translation>ワークスペースのオーナー</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="69"/>
        <source>Workspace Admin</source>
        <translation>ワークスペースの管理者</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="71"/>
        <source>App</source>
        <translation>アプリ</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="79"/>
        <source>%1 local time</source>
        <translation>現地時間 %1</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="116"/>
        <source>Message</source>
        <translation>メッセージ</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="381"/>
        <source>Message</source>
        <translation>メッセージ</translation>
    </message>
</context>
<context>
    <name>WelcomeWidget</name>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="38"/>
        <source>Keyboard shortcuts</source>
        <translation>キーボードショートカット</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="93"/>
        <source>Send message</source>
        <translation>メッセージを送信</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="94"/>
        <source>New line in message</source>
        <translation>メッセージ内で改行</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="95"/>
        <source>Edit last message</source>
        <translation>最後のメッセージを編集</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="96"/>
        <source>Bold</source>
        <translation>太字</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="97"/>
        <source>Italic</source>
        <translation>斜体</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="98"/>
        <source>Strikethrough</source>
        <translation>取り消し線</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="99"/>
        <source>Inline code</source>
        <translation>インラインコード</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="100"/>
        <source>Attach file</source>
        <translation>ファイルを添付</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="101"/>
        <source>Emoji picker</source>
        <translation>絵文字ピッカー</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="102"/>
        <source>Cancel / exit edit</source>
        <translation>キャンセル / 編集を終了</translation>
    </message>
</context>
<context>
    <name>WorkspaceSwitcher</name>
    <message>
        <location filename="../src/ui/workspace_switcher/workspace_switcher.cpp" line="448"/>
        <source>Add workspace</source>
        <translation>ワークスペースを追加</translation>
    </message>
    <message>
        <location filename="../src/ui/workspace_switcher/workspace_switcher.cpp" line="451"/>
        <source>Settings</source>
        <translation>設定</translation>
    </message>
</context>
<context>
    <name>relativeTime</name>
    <message>
        <location filename="../src/util/relative_time.cpp" line="10"/>
        <source>just now</source>
        <translation>たった今</translation>
    </message>
    <message>
        <location filename="../src/util/relative_time.cpp" line="13"/>
        <source>%n minute ago</source>
        <translation>%n 分前</translation>
    </message>
    <message>
        <location filename="../src/util/relative_time.cpp" line="17"/>
        <source>%n hour ago</source>
        <translation>%n 時間前</translation>
    </message>
    <message>
        <location filename="../src/util/relative_time.cpp" line="21"/>
        <source>%n day ago</source>
        <translation>%n 日前</translation>
    </message>
    <message>
        <location filename="../src/util/relative_time.cpp" line="25"/>
        <source>%n month ago</source>
        <translation>%n か月前</translation>
    </message>
    <message>
        <location filename="../src/util/relative_time.cpp" line="28"/>
        <source>%n year ago</source>
        <translation>%n 年前</translation>
    </message>
</context>
<context>
    <name>Sound</name>
    <message>
        <location filename="../src/util/sound_player.cpp" line="53"/>
        <source>msga chime</source>
        <translation>msga チャイム</translation>
    </message>
</context>
</TS>
