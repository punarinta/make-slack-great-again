<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="en_US" sourcelanguage="en_US">
<context>
    <name>slack::OAuthFlow</name>
    <message>
        <location filename="../src/backend/slack/oauth_flow.cpp" line="45"/>
        <source>No Slack app keys are set up yet.

Open Settings → System, choose “Slack app keys”, and paste your client ID, client secret and app token. Building msga yourself? Put them in credentials.cmake instead and rebuild.</source>
        <translation>No Slack app keys are set up yet.

Open Settings → System, choose “Slack app keys”, and paste your client ID, client secret and app token. Building msga yourself? Put them in credentials.cmake instead and rebuild.</translation>
    </message>
</context>
<context>
    <name>BrowserLogin</name>
    <message>
        <location filename="../src/backend/slack/session_import/browser_login.cpp" line="396"/>
        <source>Opening %1…</source>
        <translation>Opening %1…</translation>
    </message>
    <message>
        <location filename="../src/backend/slack/session_import/browser_login.cpp" line="433"/>
        <source>Sign in to Slack in the browser window — msga picks it up automatically.</source>
        <translation>Sign in to Slack in the browser window — msga picks it up automatically.</translation>
    </message>
    <message>
        <location filename="../src/backend/slack/session_import/browser_login.cpp" line="605"/>
        <source>Signed in — finishing up in msga. You can close this.</source>
        <translation>Signed in — finishing up in msga. You can close this.</translation>
    </message>
    <message>
        <location filename="../src/backend/slack/session_import/browser_login.cpp" line="646"/>
        <source>Signed in — looking up your workspaces…</source>
        <translation>Signed in — looking up your workspaces…</translation>
    </message>
</context>
<context>
    <name>teams::OAuthFlow</name>
    <message>
        <location filename="../src/backend/teams/oauth_flow.cpp" line="53"/>
        <source>Microsoft Teams app credentials are not configured.

Set MSGA_TEAMS_CLIENT_ID in credentials.cmake and rebuild.</source>
        <translation>Microsoft Teams app credentials are not configured.

Set MSGA_TEAMS_CLIENT_ID in credentials.cmake and rebuild.</translation>
    </message>
</context>
<context>
    <name>LlmProviderBase</name>
    <message>
        <location filename="../src/llm/llm_provider_base.cpp" line="43"/>
        <source>%1 does not offer sign-in for third-party apps — use an API key</source>
        <translation>%1 does not offer sign-in for third-party apps — use an API key</translation>
    </message>
    <message>
        <location filename="../src/llm/llm_provider_base.cpp" line="74"/>
        <source>API key is empty</source>
        <translation>API key is empty</translation>
    </message>
    <message>
        <location filename="../src/llm/llm_provider_base.cpp" line="118"/>
        <source>Connected</source>
        <translation>Connected</translation>
    </message>
    <message>
        <location filename="../src/llm/llm_provider_base.cpp" line="127"/>
        <source>%1 is not connected</source>
        <translation>%1 is not connected</translation>
    </message>
    <message>
        <location filename="../src/llm/llm_provider_base.cpp" line="155"/>
        <source>Token refresh failed: %1</source>
        <translation>Token refresh failed: %1</translation>
    </message>
</context>
<context>
    <name>LlmService</name>
    <message>
        <location filename="../src/llm/llm_service.cpp" line="64"/>
        <source>No AI provider connected — connect one in Settings → AI assistance</source>
        <translation>No AI provider connected — connect one in Settings → AI assistance</translation>
    </message>
</context>
<context>
    <name>OAuthLoopbackFlow</name>
    <message>
        <location filename="../src/llm/oauth_loopback.cpp" line="45"/>
        <source>Could not listen on port %1: %2</source>
        <translation>Could not listen on port %1: %2</translation>
    </message>
    <message>
        <location filename="../src/llm/oauth_loopback.cpp" line="92"/>
        <source>You can close this window and return to msga.</source>
        <translation>You can close this window and return to msga.</translation>
    </message>
</context>
<context>
    <name>Session</name>
    <message>
        <location filename="../src/session/session.cpp" line="53"/>
        <source> — sign in to this workspace again to grant the new permission</source>
        <translation> — sign in to this workspace again to grant the new permission</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="63"/>
        <source>You can't reply to this message.</source>
        <translation>You can't reply to this message.</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="65"/>
        <source>You're not a member of this channel.</source>
        <translation>You're not a member of this channel.</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="67"/>
        <source>This conversation is archived.</source>
        <translation>This conversation is archived.</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="69"/>
        <source>The message is too long.</source>
        <translation>The message is too long.</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="71"/>
        <source>This conversation no longer exists.</source>
        <translation>This conversation no longer exists.</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="73"/>
        <source>You don't have permission to post here.</source>
        <translation>You don't have permission to post here.</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="380"/>
        <source>Couldn't send message: %1</source>
        <translation>Couldn't send message: %1</translation>
    </message>
    <message numerus="yes">
        <location filename="../src/session/session.cpp" line="468"/>
        <source>Slack is rate-limiting requests (%1) — retrying in %n second(s).</source>
        <translation>
            <numerusform>Slack is rate-limiting requests (%1) — retrying in %n second.</numerusform>
            <numerusform>Slack is rate-limiting requests (%1) — retrying in %n seconds.</numerusform>
        </translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1528"/>
        <source>Unknown user</source>
        <translation>Unknown user</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1692"/>
        <source>No such user: %1</source>
        <translation>No such user: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1743"/>
        <source>Usage: /dnd [duration, e.g. 30m or 2h] — or /dnd off to resume</source>
        <translation>Usage: /dnd [duration, e.g. 30m or 2h] — or /dnd off to resume</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1759"/>
        <source>Command /%1 failed: %2</source>
        <translation>Command /%1 failed: %2</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1794"/>
        <source>Could not change presence: %1</source>
        <translation>Could not change presence: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1812"/>
        <source>Could not set status: %1</source>
        <translation>Could not set status: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1833"/>
        <source>Could not update notifications: %1</source>
        <translation>Could not update notifications: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1855"/>
        <source>Could not update profile: %1</source>
        <translation>Could not update profile: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1876"/>
        <source>Could not update avatar: %1</source>
        <translation>Could not update avatar: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1947"/>
        <source>Upload failed: %1</source>
        <translation>Upload failed: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="1989"/>
        <source>Could not load canvas: %1</source>
        <translation>Could not load canvas: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="2020"/>
        <source>Could not create canvas: %1</source>
        <translation>Could not create canvas: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="2036"/>
        <source>Canvas edit failed: %1</source>
        <translation>Canvas edit failed: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="2054"/>
        <source>Canvas deletion failed: %1</source>
        <translation>Canvas deletion failed: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="2448"/>
        <source>Couldn't set the reminder: %1</source>
        <translation>Couldn't set the reminder: %1</translation>
    </message>
    <message>
        <location filename="../src/session/session.cpp" line="2472"/>
        <source>Couldn't remove the reminder: %1</source>
        <translation>Couldn't remove the reminder: %1</translation>
    </message>
</context>
<context>
    <name>MrkdwnParser</name>
    <message>
        <location filename="../src/text/mrkdwn_parser.cpp" line="92"/>
        <source>Today</source>
        <translation>Today</translation>
    </message>
    <message>
        <location filename="../src/text/mrkdwn_parser.cpp" line="94"/>
        <source>Yesterday</source>
        <translation>Yesterday</translation>
    </message>
    <message>
        <location filename="../src/text/mrkdwn_parser.cpp" line="96"/>
        <source>Tomorrow</source>
        <translation>Tomorrow</translation>
    </message>
</context>
<context>
    <name>BrowseChannelsDialog</name>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="49"/>
        <source>Search for channels</source>
        <translation>Search for channels</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="55"/>
        <source>Create Channel</source>
        <translation>Create Channel</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="72"/>
        <source>Channels</source>
        <translation>Channels</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="79"/>
        <source>People</source>
        <translation>People</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="154"/>
        <source>%1 %2</source>
        <translation>%1 %2</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="156"/>
        <source>member</source>
        <translation>member</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="156"/>
        <source>members</source>
        <translation>members</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="219"/>
        <source>Search for channels</source>
        <translation>Search for channels</translation>
    </message>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_channels_dialog.cpp" line="219"/>
        <source>Search for people</source>
        <translation>Search for people</translation>
    </message>
</context>
<context>
    <name>BrowseListView</name>
    <message>
        <location filename="../src/ui/browse_channels_dialog/browse_list_view.cpp" line="200"/>
        <source>Joined</source>
        <translation>Joined</translation>
    </message>
</context>
<context>
    <name>CanvasPage</name>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="268"/>
        <source>Your canvas title</source>
        <translation>Your canvas title</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="272"/>
        <source>Go ahead, start writing!</source>
        <translation>Go ahead, start writing!</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="657"/>
        <source>This canvas was created with Slack's built-in editor and is not editable through the Slack API — it is read-only here.</source>
        <translation>This canvas was created with Slack's built-in editor and is not editable through the Slack API — it is read-only here.</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="659"/>
        <source>You don't have access to this canvas.</source>
        <translation>You don't have access to this canvas.</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="676"/>
        <source>Copy link</source>
        <translation>Copy link</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="685"/>
        <source>Delete canvas</source>
        <translation>Delete canvas</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="207"/>
        <source>Delete canvas</source>
        <translation>Delete canvas</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="211"/>
        <source>The canvas will be deleted for everyone in the conversation.
This action cannot be undone.</source>
        <translation>The canvas will be deleted for everyone in the conversation.
This action cannot be undone.</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="226"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
    <message>
        <location filename="../src/ui/canvas_page/canvas_page.cpp" line="229"/>
        <source>Delete canvas</source>
        <translation>Delete canvas</translation>
    </message>
</context>
<context>
    <name>AttachmentStrip</name>
    <message>
        <location filename="../src/ui/composer/attachment_strip.cpp" line="297"/>
        <source>Remove attachment</source>
        <translation>Remove attachment</translation>
    </message>
</context>
<context>
    <name>ComposerWidget</name>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="408"/>
        <source>Subject</source>
        <translation>Subject</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="473"/>
        <source>Message #channel</source>
        <translation>Message #channel</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="515"/>
        <source>Attach file</source>
        <translation>Attach file</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="530"/>
        <source>Emoji</source>
        <translation>Emoji</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="531"/>
        <source>Mention</source>
        <translation>Mention</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="543"/>
        <source>Send message</source>
        <translation>Send message</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="553"/>
        <source>Schedule send</source>
        <translation>Schedule send</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1535"/>
        <source>Send at</source>
        <translation>Send at</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1535"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1535"/>
        <source>Schedule</source>
        <translation>Schedule</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1661"/>
        <source>Attach File</source>
        <translation>Attach File</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1696"/>
        <source>URL</source>
        <translation>URL</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1696"/>
        <source>Display text</source>
        <translation>Display text</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1696"/>
        <source>Insert</source>
        <translation>Insert</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/composer_widget.cpp" line="1696"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
</context>
<context>
    <name>EditModeBanner</name>
    <message>
        <location filename="../src/ui/composer/edit_mode_banner.cpp" line="21"/>
        <source>Editing message</source>
        <translation>Editing message</translation>
    </message>
</context>
<context>
    <name>FormattingToolbar</name>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="67"/>
        <source>Bold</source>
        <translation>Bold</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="68"/>
        <source>Italic</source>
        <translation>Italic</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="69"/>
        <source>Underline</source>
        <translation>Underline</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="70"/>
        <source>Strikethrough</source>
        <translation>Strikethrough</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="71"/>
        <source>Link</source>
        <translation>Link</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="72"/>
        <source>Ordered list</source>
        <translation>Ordered list</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="73"/>
        <source>Bullet list</source>
        <translation>Bullet list</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="74"/>
        <source>Blockquote</source>
        <translation>Blockquote</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="75"/>
        <source>Inline code</source>
        <translation>Inline code</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/formatting_toolbar.cpp" line="76"/>
        <source>Code block</source>
        <translation>Code block</translation>
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
        <translation>App</translation>
    </message>
    <message>
        <location filename="../src/ui/composer/mention_completer.cpp" line="306"/>
        <source>Enter</source>
        <translation>Enter</translation>
    </message>
</context>
<context>
    <name>ConvFooterWidget</name>
    <message numerus="yes">
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="138"/>
        <source>%n background task(s) running</source>
        <translation>
            <numerusform>%n background task running</numerusform>
            <numerusform>%n background tasks running</numerusform>
        </translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="232"/>
        <source>Hidden — you appear away to everyone. Click to use automatic presence.</source>
        <translation>Hidden — you appear away to everyone. Click to use automatic presence.</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="234"/>
        <source>Visible — but you appear away if no official Slack client is connected. Click to hide.</source>
        <translation>Visible — but you appear away if no official Slack client is connected. Click to hide.</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="238"/>
        <source>Visible — using automatic presence. Click to appear hidden.</source>
        <translation>Visible — using automatic presence. Click to appear hidden.</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="311"/>
        <source>Profile &amp; status</source>
        <translation>Profile &amp; status</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="355"/>
        <source>Manage profile</source>
        <translation>Manage profile</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_footer/conv_footer_widget.cpp" line="361"/>
        <source>Manage status</source>
        <translation>Manage status</translation>
    </message>
</context>
<context>
    <name>ConvListWidget</name>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="754"/>
        <source>Open a direct message</source>
        <translation>Open a direct message</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="783"/>
        <source>Notify you about…</source>
        <translation>Notify you about…</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="785"/>
        <source>All new posts</source>
        <translation>All new posts</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="792"/>
        <source>Just mentions</source>
        <translation>Just mentions</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="799"/>
        <source>Mute and hide</source>
        <translation>Mute and hide</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="813"/>
        <source>Unstar channel</source>
        <translation>Unstar channel</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="813"/>
        <source>Star channel</source>
        <translation>Star channel</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="820"/>
        <source>Leave channel</source>
        <translation>Leave channel</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="833"/>
        <source>Unstar conversation</source>
        <translation>Unstar conversation</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="833"/>
        <source>Star conversation</source>
        <translation>Star conversation</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="840"/>
        <source>Leave conversation</source>
        <translation>Leave conversation</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="851"/>
        <source>Unmute</source>
        <translation>Unmute</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="851"/>
        <source>Mute</source>
        <translation>Mute</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="924"/>
        <source>Find a channel</source>
        <translation>Find a channel</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="925"/>
        <source>Create a channel</source>
        <translation>Create a channel</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1075"/>
        <source>Channels</source>
        <translation>Channels</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1076"/>
        <source>Direct messages</source>
        <translation>Direct messages</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1077"/>
        <source>Agents &amp; apps</source>
        <translation>Agents &amp; apps</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1143"/>
        <source>Threads</source>
        <translation>Threads</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1152"/>
        <source>Saved messages</source>
        <translation>Saved messages</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1172"/>
        <source>Add channels</source>
        <translation>Add channels</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1192"/>
        <source>%1 more %2</source>
        <translation>%1 more %2</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1192"/>
        <source>channel</source>
        <translation>channel</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1192"/>
        <source>channels</source>
        <translation>channels</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1384"/>
        <source>EXT</source>
        <translation>EXT</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1406"/>
        <source>you</source>
        <translation>you</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_list/conv_list_widget.cpp" line="1448"/>
        <source>you</source>
        <translation>you</translation>
    </message>
</context>
<context>
    <name>ConvSelectorWidget</name>
    <message>
        <location filename="../src/ui/conv_selector/conv_selector_widget.cpp" line="61"/>
        <source>Search channels and people…</source>
        <translation>Search channels and people…</translation>
    </message>
</context>
<context>
    <name>ConvTabsWidget</name>
    <message>
        <location filename="../src/ui/conv_tabs/conv_tabs_widget.cpp" line="62"/>
        <source>Messages</source>
        <translation>Messages</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_tabs/conv_tabs_widget.cpp" line="67"/>
        <source>Untitled</source>
        <translation>Untitled</translation>
    </message>
    <message>
        <location filename="../src/ui/conv_tabs/conv_tabs_widget.cpp" line="67"/>
        <source>Add canvas</source>
        <translation>Add canvas</translation>
    </message>
</context>
<context>
    <name>CreateChannelDialog</name>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="19"/>
        <source>Create a channel</source>
        <translation>Create a channel</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="33"/>
        <source>Name</source>
        <translation>Name</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="42"/>
        <source>e.g. plan-budget</source>
        <translation>e.g. plan-budget</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="46"/>
        <source>Channels are where conversations happen around a topic. Use a name that is easy to find and understand.</source>
        <translation>Channels are where conversations happen around a topic. Use a name that is easy to find and understand.</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="57"/>
        <source>Next</source>
        <translation>Next</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="77"/>
        <source>Visibility</source>
        <translation>Visibility</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="87"/>
        <source>Private — only specific people</source>
        <translation>Private — only specific people</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="90"/>
        <source>Can only be viewed or joined by invitation</source>
        <translation>Can only be viewed or joined by invitation</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="97"/>
        <source>Step 2 of 2</source>
        <translation>Step 2 of 2</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="98"/>
        <source>Back</source>
        <translation>Back</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="99"/>
        <source>Create</source>
        <translation>Create</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="139"/>
        <source>this workspace</source>
        <translation>this workspace</translation>
    </message>
    <message>
        <location filename="../src/ui/create_channel_dialog/create_channel_dialog.cpp" line="140"/>
        <source>Public — anyone in %1</source>
        <translation>Public — anyone in %1</translation>
    </message>
</context>
<context>
    <name>DeleteMessageDialog</name>
    <message>
        <location filename="../src/ui/delete_message_dialog/delete_message_dialog.cpp" line="17"/>
        <source>Delete message</source>
        <translation>Delete message</translation>
    </message>
    <message>
        <location filename="../src/ui/delete_message_dialog/delete_message_dialog.cpp" line="21"/>
        <source>This action cannot be undone.</source>
        <translation>This action cannot be undone.</translation>
    </message>
    <message>
        <location filename="../src/ui/delete_message_dialog/delete_message_dialog.cpp" line="75"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
    <message>
        <location filename="../src/ui/delete_message_dialog/delete_message_dialog.cpp" line="76"/>
        <source>Delete</source>
        <translation>Delete</translation>
    </message>
</context>
<context>
    <name>EmojiGrid</name>
    <message>
        <location filename="../src/ui/emoji_picker/emoji_picker_popup.cpp" line="430"/>
        <source>Search all emoji</source>
        <translation>Search all emoji</translation>
    </message>
    <message>
        <location filename="../src/ui/emoji_picker/emoji_picker_popup.cpp" line="451"/>
        <source>Skin Tone</source>
        <translation>Skin Tone</translation>
    </message>
    <message>
        <location filename="../src/ui/emoji_picker/emoji_picker_popup.cpp" line="593"/>
        <source>Search Results</source>
        <translation>Search Results</translation>
    </message>
    <message>
        <location filename="../src/ui/emoji_picker/emoji_picker_popup.cpp" line="623"/>
        <source>Frequently Used</source>
        <translation>Frequently Used</translation>
    </message>
    <message>
        <location filename="../src/ui/emoji_picker/emoji_picker_popup.cpp" line="661"/>
        <source>Custom</source>
        <translation>Custom</translation>
    </message>
</context>
<context>
    <name>ForwardDialog</name>
    <message>
        <location filename="../src/ui/forward_dialog/forward_dialog.cpp" line="24"/>
        <source>Forward this message</source>
        <translation>Forward this message</translation>
    </message>
    <message>
        <location filename="../src/ui/forward_dialog/forward_dialog.cpp" line="34"/>
        <source>Add a message, if you'd like.</source>
        <translation>Add a message, if you'd like.</translation>
    </message>
    <message>
        <location filename="../src/ui/forward_dialog/forward_dialog.cpp" line="172"/>
        <source>Copy Link</source>
        <translation>Copy Link</translation>
    </message>
    <message>
        <location filename="../src/ui/forward_dialog/forward_dialog.cpp" line="173"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
    <message>
        <location filename="../src/ui/forward_dialog/forward_dialog.cpp" line="174"/>
        <source>Forward</source>
        <translation>Forward</translation>
    </message>
</context>
<context>
    <name>HuddleBanner</name>
    <message>
        <location filename="../src/ui/huddle_banner/huddle_banner.cpp" line="30"/>
        <source>A huddle is happening</source>
        <translation>A huddle is happening</translation>
    </message>
    <message>
        <location filename="../src/ui/huddle_banner/huddle_banner.cpp" line="33"/>
        <source>Join</source>
        <translation>Join</translation>
    </message>
    <message>
        <location filename="../src/ui/huddle_banner/huddle_banner.cpp" line="63"/>
        <source>Opens the huddle in Slack for web</source>
        <translation>Opens the huddle in Slack for web</translation>
    </message>
</context>
<context>
    <name>ImageViewerOverlay</name>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="45"/>
        <source>Download</source>
        <translation>Download</translation>
    </message>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="46"/>
        <source>Forward</source>
        <translation>Forward</translation>
    </message>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="47"/>
        <source>Open in browser</source>
        <translation>Open in browser</translation>
    </message>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="48"/>
        <source>More actions</source>
        <translation>More actions</translation>
    </message>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="49"/>
        <source>Close</source>
        <translation>Close</translation>
    </message>
    <message>
        <location filename="../src/ui/image_viewer/image_viewer.cpp" line="146"/>
        <source>Loading image…</source>
        <translation>Loading image…</translation>
    </message>
</context>
<context>
    <name>ImapAddAccountDialog</name>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="21"/>
        <source>Add email account</source>
        <translation>Add email account</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="35"/>
        <source>IMAP server</source>
        <translation>IMAP server</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="37"/>
        <source>Detect</source>
        <translation>Detect</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="47"/>
        <source>Sign in with Google</source>
        <translation>Sign in with Google</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="53"/>
        <source>Password or app password</source>
        <translation>Password or app password</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="70"/>
        <source>Continue</source>
        <translation>Continue</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="71"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="93"/>
        <source>Enter a valid email address.</source>
        <translation>Enter a valid email address.</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="107"/>
        <source>Add</source>
        <translation>Add</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="126"/>
        <source>Add</source>
        <translation>Add</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="132"/>
        <source>Sign in with %1</source>
        <translation>Sign in with %1</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="150"/>
        <source>%1 needs an app password to sign in</source>
        <translation>%1 needs an app password to sign in</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="152"/>
        <source>%1 needs an app password</source>
        <translation>%1 needs an app password</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="167"/>
        <source>Server: %1</source>
        <translation>Server: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="174"/>
        <source>Couldn't detect the server — enter it manually.</source>
        <translation>Couldn't detect the server — enter it manually.</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="194"/>
        <source>Detecting…</source>
        <translation>Detecting…</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="202"/>
        <source>Enter the IMAP server or click Detect.</source>
        <translation>Enter the IMAP server or click Detect.</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="206"/>
        <source>Enter your password.</source>
        <translation>Enter your password.</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="216"/>
        <source>Checking…</source>
        <translation>Checking…</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="224"/>
        <source>Sign-in failed: %1</source>
        <translation>Sign-in failed: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="248"/>
        <source>Opening your browser to sign in…</source>
        <translation>Opening your browser to sign in…</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="256"/>
        <source>Sign-in failed: no access token returned.</source>
        <translation>Sign-in failed: no access token returned.</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="271"/>
        <source>Checking…</source>
        <translation>Checking…</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="275"/>
        <source>Sign-in failed: %1</source>
        <translation>Sign-in failed: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/imap_add_account/imap_add_account_dialog.cpp" line="286"/>
        <source>Sign-in failed: %1</source>
        <translation>Sign-in failed: %1</translation>
    </message>
</context>
<context>
    <name>MainWindow</name>
    <message>
        <location filename="../src/ui/main_window.cpp" line="441"/>
        <source>Log in to workspace</source>
        <translation>Log in to workspace</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="978"/>
        <source>Couldn't apply the label.</source>
        <translation>Couldn't apply the label.</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1459"/>
        <source>Convert to session</source>
        <translation>Convert to session</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1460"/>
        <source>Add one workspace with your Slack session first — its cookie is reused for the rest.</source>
        <translation>Add one workspace with your Slack session first — its cookie is reused for the rest.</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1467"/>
        <source>Convert to session</source>
        <translation>Convert to session</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1467"/>
        <source>All Slack workspaces already use your session.</source>
        <translation>All Slack workspaces already use your session.</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1481"/>
        <source>Convert to session</source>
        <translation>Convert to session</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1482"/>
        <source>Couldn't convert your workspaces: %1</source>
        <translation>Couldn't convert your workspaces: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1563"/>
        <source>Login failed</source>
        <translation>Login failed</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1563"/>
        <source>This service is not supported.</source>
        <translation>This service is not supported.</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1592"/>
        <source>Login failed</source>
        <translation>Login failed</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1839"/>
        <source>Message</source>
        <translation>Message</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1839"/>
        <source>Message %1</source>
        <translation>Message %1</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2096"/>
        <source>Someone</source>
        <translation>Someone</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2235"/>
        <source>Someone</source>
        <translation>Someone</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2241"/>
        <source>Started a huddle</source>
        <translation>Started a huddle</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2244"/>
        <source>%1 started a huddle</source>
        <translation>%1 started a huddle</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2274"/>
        <source>Join</source>
        <translation>Join</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2342"/>
        <source>Reminder</source>
        <translation>Reminder</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2342"/>
        <source>Reminder — %1</source>
        <translation>Reminder — %1</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2361"/>
        <source>You asked to be reminded about a message.</source>
        <translation>You asked to be reminded about a message.</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2416"/>
        <source>Sample User</source>
        <translation>Sample User</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2424"/>
        <source>Hey — do you have a minute?</source>
        <translation>Hey — do you have a minute?</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2428"/>
        <source>%1: Heads up, the deploy is going out at 3pm</source>
        <translation>%1: Heads up, the deploy is going out at 3pm</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2432"/>
        <source>%1 started a huddle</source>
        <translation>%1 started a huddle</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2437"/>
        <source>Join</source>
        <translation>Join</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2625"/>
        <source>Workspace admin</source>
        <translation>Workspace admin</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2631"/>
        <source>Unmute</source>
        <translation>Unmute</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2631"/>
        <source>Mute</source>
        <translation>Mute</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2636"/>
        <source>Log out</source>
        <translation>Log out</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2636"/>
        <source>Log out from %1</source>
        <translation>Log out from %1</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2695"/>
        <source>Settings</source>
        <translation>Settings</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="2700"/>
        <source>Quit</source>
        <translation>Quit</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="3092"/>
        <source>Opens the huddle in Slack for web</source>
        <translation>Opens the huddle in Slack for web</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="3102"/>
        <source>Unstar conversation</source>
        <translation>Unstar conversation</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="3102"/>
        <source>Star conversation</source>
        <translation>Star conversation</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="3113"/>
        <source>Search messages</source>
        <translation>Search messages</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="3344"/>
        <source>Message</source>
        <translation>Message</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="3344"/>
        <source>Message %1</source>
        <translation>Message %1</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1606"/>
        <source>You appear away to others — no official Slack client is connected</source>
        <translation>You appear away to others — no official Slack client is connected</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1610"/>
        <source>Active</source>
        <translation>Active</translation>
    </message>
    <message>
        <location filename="../src/ui/main_window.cpp" line="1612"/>
        <source>Away</source>
        <translation>Away</translation>
    </message>
</context>
<context>
    <name>MentionPopup</name>
    <message>
        <location filename="../src/ui/mention_popup/mention_popup.cpp" line="442"/>
        <source>(you)</source>
        <translation>(you)</translation>
    </message>
    <message>
        <location filename="../src/ui/mention_popup/mention_popup.cpp" line="125"/>
        <source>Enter</source>
        <translation>Enter</translation>
    </message>
    <message>
        <location filename="../src/ui/mention_popup/mention_popup.cpp" line="154"/>
        <source>APP</source>
        <translation>APP</translation>
    </message>
</context>
<context>
    <name>MessageListWidget</name>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="1934"/>
        <source>Open full table</source>
        <translation>Open full table</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2186"/>
        <source>Reply in thread</source>
        <translation>Reply in thread</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2203"/>
        <source>Unmute thread</source>
        <translation>Unmute thread</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2203"/>
        <source>Mute thread</source>
        <translation>Mute thread</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2221"/>
        <source>Edit message</source>
        <translation>Edit message</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2235"/>
        <source>Copy link</source>
        <translation>Copy link</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2245"/>
        <source>Copy message</source>
        <translation>Copy message</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2257"/>
        <source>Unpin from channel</source>
        <translation>Unpin from channel</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2274"/>
        <source>Pin to channel</source>
        <translation>Pin to channel</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2298"/>
        <source>Remove reminder</source>
        <translation>Remove reminder</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2310"/>
        <source>Remind me</source>
        <translation>Remind me</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2323"/>
        <source>Forward message</source>
        <translation>Forward message</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2332"/>
        <source>Summarize down</source>
        <translation>Summarize down</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2343"/>
        <source>Delete message…</source>
        <translation>Delete message…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2380"/>
        <source>Remind me about this…</source>
        <translation>Remind me about this…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2381"/>
        <source>In 20 minutes</source>
        <translation>In 20 minutes</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2382"/>
        <source>In 1 hour</source>
        <translation>In 1 hour</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2383"/>
        <source>In 3 hours</source>
        <translation>In 3 hours</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2385"/>
        <source>Tomorrow</source>
        <translation>Tomorrow</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2388"/>
        <source>Next week</source>
        <translation>Next week</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2392"/>
        <source>Custom…</source>
        <translation>Custom…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2416"/>
        <source>Summaries need an AI provider. Connect one in Settings → AI assistance.</source>
        <translation>Summaries need an AI provider. Connect one in Settings → AI assistance.</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2459"/>
        <source>You</source>
        <translation>You</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2699"/>
        <source>Slack doesn't let third-party apps press bot buttons, we are working on a workaround</source>
        <translation>Slack doesn't let third-party apps press bot buttons, we are working on a workaround</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2714"/>
        <source>No email app — address copied</source>
        <translation>No email app — address copied</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2750"/>
        <source>file</source>
        <translation>file</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2752"/>
        <source>Save file</source>
        <translation>Save file</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2758"/>
        <source>Downloading %1</source>
        <translation>Downloading %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2800"/>
        <source>image</source>
        <translation>image</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2801"/>
        <source>Copying %1</source>
        <translation>Copying %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2851"/>
        <source>Preview</source>
        <translation>Preview</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2857"/>
        <source>Copy link to image</source>
        <translation>Copy link to image</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2857"/>
        <source>Copy link to file</source>
        <translation>Copy link to file</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2867"/>
        <source>Copy full image</source>
        <translation>Copy full image</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2881"/>
        <source>Delete image…</source>
        <translation>Delete image…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2881"/>
        <source>Delete file…</source>
        <translation>Delete file…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2899"/>
        <source>file</source>
        <translation>file</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2901"/>
        <source>Downloading %1</source>
        <translation>Downloading %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2908"/>
        <source>This file is empty</source>
        <translation>This file is empty</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="2913"/>
        <source>Showing the first %1 of %2 rows</source>
        <translation>Showing the first %1 of %2 rows</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="3374"/>
        <source>Add reaction</source>
        <translation>Add reaction</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="3374"/>
        <source>Forward message</source>
        <translation>Forward message</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="3374"/>
        <source>More actions</source>
        <translation>More actions</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="3383"/>
        <source>Download</source>
        <translation>Download</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="3383"/>
        <source>Share</source>
        <translation>Share</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="3383"/>
        <source>More actions</source>
        <translation>More actions</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list.cpp" line="3415"/>
        <source>Remove preview</source>
        <translation>Remove preview</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="81"/>
        <source>Oh my gosh, I really apologize, but your company is a reaaaly active Slack user. Still loading...</source>
        <translation>Oh my gosh, I really apologize, but your company is a reaaaly active Slack user. Still loading...</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="84"/>
        <source>Oh, you must have a lot of co-workers and messages! Still loading...</source>
        <translation>Oh, you must have a lot of co-workers and messages! Still loading...</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="86"/>
        <source>Loading your stuff...</source>
        <translation>Loading your stuff...</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="113"/>
        <source>No messages yet</source>
        <translation>No messages yet</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="184"/>
        <source>Open full table</source>
        <translation>Open full table</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="264"/>
        <source>Pinned by %1</source>
        <translation>Pinned by %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="264"/>
        <source>Pinned</source>
        <translation>Pinned</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="299"/>
        <source>Reminder — past due</source>
        <translation>Reminder — past due</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="300"/>
        <source>Reminder — %1</source>
        <translation>Reminder — %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="617"/>
        <source>APP</source>
        <translation>APP</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="618"/>
        <source>EXT</source>
        <translation>EXT</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="620"/>
        <source>(edited)</source>
        <translation>(edited)</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1112"/>
        <source>Loading image…</source>
        <translation>Loading image…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1594"/>
        <source>1 reply</source>
        <translation>1 reply</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1594"/>
        <source>%1 replies</source>
        <translation>%1 replies</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1617"/>
        <source>Close thread</source>
        <translation>Close thread</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1634"/>
        <source>View thread</source>
        <translation>View thread</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1648"/>
        <source>Last reply</source>
        <translation>Last reply</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1648"/>
        <source>Last reply %1</source>
        <translation>Last reply %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1883"/>
        <source>Loading replies…</source>
        <translation>Loading replies…</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1907"/>
        <source>Reply to thread</source>
        <translation>Reply to thread</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_list_paint.cpp" line="1916"/>
        <source>Reply to thread</source>
        <translation>Reply to thread</translation>
    </message>
</context>
<context>
    <name>MsgRender</name>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="179"/>
        <source>Today</source>
        <translation>Today</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="181"/>
        <source>Yesterday</source>
        <translation>Yesterday</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="197"/>
        <source>today at %1</source>
        <translation>today at %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="199"/>
        <source>yesterday at %1</source>
        <translation>yesterday at %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="200"/>
        <source>%1 at %2</source>
        <translation>%1 at %2</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="212"/>
        <source>yesterday at %1</source>
        <translation>yesterday at %1</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="215"/>
        <source>%1 at %2</source>
        <translation>%1 at %2</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="217"/>
        <source>%1 at %2</source>
        <translation>%1 at %2</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="262"/>
        <source>group message</source>
        <translation>group message</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="274"/>
        <source>%1 in %2</source>
        <translation>%1 in %2</translation>
    </message>
    <message>
        <location filename="../src/ui/message_list/message_render.cpp" line="280"/>
        <source>message</source>
        <translation>message</translation>
    </message>
</context>
<context>
    <name>ParallelUsageBanner</name>
    <message>
        <location filename="../src/ui/parallel_usage_banner/parallel_usage_banner.cpp" line="34"/>
        <source>The same app keys are running on another device and keep interrupting your Slack connection.</source>
        <translation>The same app keys are running on another device and keep interrupting your Slack connection.</translation>
    </message>
    <message>
        <location filename="../src/ui/parallel_usage_banner/parallel_usage_banner.cpp" line="37"/>
        <source>How to solve this?</source>
        <translation>How to solve this?</translation>
    </message>
</context>
<context>
    <name>ProfileAvatarWidget</name>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="98"/>
        <source>Change photo</source>
        <translation>Change photo</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="118"/>
        <source>Profile</source>
        <translation>Profile</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="142"/>
        <source>Name</source>
        <translation>Name</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="144"/>
        <source>Your display name</source>
        <translation>Your display name</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="145"/>
        <source>Email</source>
        <translation>Email</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="146"/>
        <source>name@example.com</source>
        <translation>name@example.com</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="147"/>
        <source>Phone</source>
        <translation>Phone</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="148"/>
        <source>Optional</source>
        <translation>Optional</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="159"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="160"/>
        <source>Save Changes</source>
        <translation>Save Changes</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="250"/>
        <source>Choose a profile photo</source>
        <translation>Choose a profile photo</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="250"/>
        <source>Images (*.png *.jpg *.jpeg *.gif)</source>
        <translation>Images (*.png *.jpg *.jpeg *.gif)</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="255"/>
        <source>Uploading photo…</source>
        <translation>Uploading photo…</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="260"/>
        <source>Could not upload photo: %1</source>
        <translation>Could not upload photo: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="263"/>
        <source>Photo updated.</source>
        <translation>Photo updated.</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="292"/>
        <source>Saving…</source>
        <translation>Saving…</translation>
    </message>
    <message>
        <location filename="../src/ui/profile_dialog/profile_dialog.cpp" line="296"/>
        <source>Could not save: %1</source>
        <translation>Could not save: %1</translation>
    </message>
</context>
<context>
    <name>ReminderDialog</name>
    <message>
        <location filename="../src/ui/reminder_dialog/reminder_dialog.cpp" line="15"/>
        <source>Reminder</source>
        <translation>Reminder</translation>
    </message>
    <message>
        <location filename="../src/ui/reminder_dialog/reminder_dialog.cpp" line="26"/>
        <source>When</source>
        <translation>When</translation>
    </message>
    <message>
        <location filename="../src/ui/reminder_dialog/reminder_dialog.cpp" line="36"/>
        <source>Time</source>
        <translation>Time</translation>
    </message>
    <message>
        <location filename="../src/ui/reminder_dialog/reminder_dialog.cpp" line="46"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
    <message>
        <location filename="../src/ui/reminder_dialog/reminder_dialog.cpp" line="47"/>
        <source>Save</source>
        <translation>Save</translation>
    </message>
</context>
<context>
    <name>SavedMessagesPage</name>
    <message>
        <location filename="../src/ui/saved_page/saved_messages_page.cpp" line="121"/>
        <source>No preview available</source>
        <translation>No preview available</translation>
    </message>
    <message>
        <location filename="../src/ui/saved_page/saved_messages_page.cpp" line="150"/>
        <source>Message</source>
        <translation>Message</translation>
    </message>
    <message>
        <location filename="../src/ui/saved_page/saved_messages_page.cpp" line="221"/>
        <source>Remove</source>
        <translation>Remove</translation>
    </message>
    <message>
        <location filename="../src/ui/saved_page/saved_messages_page.cpp" line="289"/>
        <source>Reminder set for %1</source>
        <translation>Reminder set for %1</translation>
    </message>
    <message>
        <location filename="../src/ui/saved_page/saved_messages_page.cpp" line="322"/>
        <source>Saved messages</source>
        <translation>Saved messages</translation>
    </message>
    <message>
        <location filename="../src/ui/saved_page/saved_messages_page.cpp" line="404"/>
        <source>Messages you set reminders on will appear here.</source>
        <translation>Messages you set reminders on will appear here.</translation>
    </message>
</context>
<context>
    <name>SearchWidget</name>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="77"/>
        <source>Search messages…</source>
        <translation>Search messages…</translation>
    </message>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="234"/>
        <source>Search messages</source>
        <translation>Search messages</translation>
    </message>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="245"/>
        <source>Close search</source>
        <translation>Close search</translation>
    </message>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="313"/>
        <source>Searching…</source>
        <translation>Searching…</translation>
    </message>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="386"/>
        <source>No results found.</source>
        <translation>No results found.</translation>
    </message>
    <message>
        <location filename="../src/ui/search/search_widget.cpp" line="400"/>
        <source>Unknown channel</source>
        <translation>Unknown channel</translation>
    </message>
</context>
<context>
    <name>SessionImportDialog</name>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="24"/>
        <source>The Slack desktop app wasn't found on this computer.</source>
        <translation>The Slack desktop app wasn't found on this computer.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="26"/>
        <source>Couldn't read Slack's data — try quitting the Slack app first.</source>
        <translation>Couldn't read Slack's data — try quitting the Slack app first.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="28"/>
        <source>Couldn't read Slack's saved session automatically.</source>
        <translation>Couldn't read Slack's saved session automatically.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="30"/>
        <source>Automatic import isn't available in this build.</source>
        <translation>Automatic import isn't available in this build.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="31"/>
        <source>Automatic import didn't work.</source>
        <translation>Automatic import didn't work.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="37"/>
        <source>No supported browser was found — browser sign-in needs Chrome, Chromium, Brave, Edge or Vivaldi.</source>
        <translation>No supported browser was found — browser sign-in needs Chrome, Chromium, Brave, Edge or Vivaldi.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="42"/>
        <source>Couldn't start your browser.</source>
        <translation>Couldn't start your browser.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="44"/>
        <source>Couldn't read the session back from the browser window.</source>
        <translation>Couldn't read the session back from the browser window.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="46"/>
        <source>Browser sign-in was cancelled.</source>
        <translation>Browser sign-in was cancelled.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="48"/>
        <source>Browser sign-in timed out.</source>
        <translation>Browser sign-in timed out.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="49"/>
        <source>Browser sign-in didn't work.</source>
        <translation>Browser sign-in didn't work.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="76"/>
        <source>Add Slack workspace with a session token</source>
        <translation>Add Slack workspace with a session token</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="86"/>
        <source>Log in to Slack in a browser window and msga uses that session instead of app keys — so it runs on your account's own rate limits and avoids the shared-key timeouts. New messages arrive by polling (there's no live push this way).</source>
        <translation>Log in to Slack in a browser window and msga uses that session instead of app keys — so it runs on your account's own rate limits and avoids the shared-key timeouts. New messages arrive by polling (there's no live push this way).</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="92"/>
        <source>Sign in with your existing Slack session instead of app keys — it uses your account's own rate limits, so it avoids the shared-key timeouts. New messages arrive by polling (there's no live push this way).</source>
        <translation>Sign in with your existing Slack session instead of app keys — it uses your account's own rate limits, so it avoids the shared-key timeouts. New messages arrive by polling (there's no live push this way).</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="103"/>
        <source>Sign in with %1</source>
        <translation>Sign in with %1</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="114"/>
        <source>Import from local Slack</source>
        <translation>Import from local Slack</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="123"/>
        <source>Paste a session cookie instead</source>
        <translation>Paste a session cookie instead</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="141"/>
        <source>1. Sign in to the workspace in your browser.
2. Open developer tools (F12) → Application → Cookies → https://app.slack.com, and copy the value of the cookie named “d” (it starts with xoxd-) into Cookie. Browsers hide this cookie from scripts, so it has to be copied by hand.
3. Enter your workspace address.</source>
        <translation>1. Sign in to the workspace in your browser.
2. Open developer tools (F12) → Application → Cookies → https://app.slack.com, and copy the value of the cookie named “d” (it starts with xoxd-) into Cookie. Browsers hide this cookie from scripts, so it has to be copied by hand.
3. Enter your workspace address.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="156"/>
        <source>Cookie</source>
        <translation>Cookie</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="164"/>
        <source>Workspace</source>
        <translation>Workspace</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="178"/>
        <source>Use app keys (OAuth) instead</source>
        <translation>Use app keys (OAuth) instead</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="188"/>
        <source>Add workspace</source>
        <translation>Add workspace</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="190"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="209"/>
        <source>Opening a browser window…</source>
        <translation>Opening a browser window…</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="237"/>
        <source>Signed in — enter your workspace address.</source>
        <translation>Signed in — enter your workspace address.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="248"/>
        <source>Importing from local Slack…</source>
        <translation>Importing from local Slack…</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="260"/>
        <source>Found your Slack session — enter your workspace address.</source>
        <translation>Found your Slack session — enter your workspace address.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="271"/>
        <source>Paste the “d” cookie value.</source>
        <translation>Paste the “d” cookie value.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="275"/>
        <source>Enter your workspace address (e.g. myteam.slack.com).</source>
        <translation>Enter your workspace address (e.g. myteam.slack.com).</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="287"/>
        <source>Verifying your session…</source>
        <translation>Verifying your session…</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="301"/>
        <source>That session was rejected — the cookie may have expired. Sign in to Slack again and copy a fresh cookie.</source>
        <translation>That session was rejected — the cookie may have expired. Sign in to Slack again and copy a fresh cookie.</translation>
    </message>
    <message>
        <location filename="../src/ui/session_import_dialog/session_import_dialog.cpp" line="306"/>
        <source>Couldn't verify that session. Check the cookie and workspace address and try again.</source>
        <translation>Couldn't verify that session. Check the cookie and workspace address and try again.</translation>
    </message>
</context>
<context>
    <name>SettingsDialog</name>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="117"/>
        <source>RAM used: %1</source>
        <translation>RAM used: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="149"/>
        <source>Settings</source>
        <translation>Settings</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="169"/>
        <source>Appearance</source>
        <translation>Appearance</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="170"/>
        <source>Notifications</source>
        <translation>Notifications</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="171"/>
        <source>AI assistance</source>
        <translation>AI assistance</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="172"/>
        <source>Storage</source>
        <translation>Storage</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="173"/>
        <source>System</source>
        <translation>System</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="174"/>
        <source>About</source>
        <translation>About</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="190"/>
        <source>Color theme</source>
        <translation>Color theme</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="209"/>
        <source>Purple</source>
        <translation>Purple</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="210"/>
        <source>Charcoal</source>
        <translation>Charcoal</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="211"/>
        <source>Blue</source>
        <translation>Blue</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="212"/>
        <source>Green</source>
        <translation>Green</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="246"/>
        <source>Font size</source>
        <translation>Font size</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="256"/>
        <source>Small</source>
        <translation>Small</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="257"/>
        <source>Medium (default)</source>
        <translation>Medium (default)</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="258"/>
        <source>Large</source>
        <translation>Large</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="273"/>
        <source>Language</source>
        <translation>Language</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="284"/>
        <source>App language</source>
        <translation>App language</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="292"/>
        <source>System default</source>
        <translation>System default</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="301"/>
        <source>The new language will be applied the next time MSGA starts.
Time and date formats update immediately.</source>
        <translation>The new language will be applied the next time MSGA starts.
Time and date formats update immediately.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="317"/>
        <source>Date/Time</source>
        <translation>Date/Time</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="327"/>
        <source>12-hour clock (2:34 PM)</source>
        <translation>12-hour clock (2:34 PM)</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="328"/>
        <source>24-hour clock (14:34)</source>
        <translation>24-hour clock (14:34)</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="339"/>
        <source>Threads</source>
        <translation>Threads</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="350"/>
        <source>Standalone (open replies in a side panel)</source>
        <translation>Standalone (open replies in a side panel)</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="351"/>
        <source>Inline (expand replies under the message)</source>
        <translation>Inline (expand replies under the message)</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="362"/>
        <source>Conversations</source>
        <translation>Conversations</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="373"/>
        <source>Show conversations active in the last</source>
        <translation>Show conversations active in the last</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="379"/>
        <source> days</source>
        <translation> days</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="386"/>
        <source>Conversations with no activity in this period are hidden
under an &quot;N more...&quot; row at the bottom of each section.</source>
        <translation>Conversations with no activity in this period are hidden
under an &quot;N more...&quot; row at the bottom of each section.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="394"/>
        <source>Show the Agents &amp;&amp; apps section</source>
        <translation>Show the Agents &amp;&amp; apps section</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="402"/>
        <source>Save</source>
        <translation>Save</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="419"/>
        <source>Enable desktop notifications</source>
        <translation>Enable desktop notifications</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="428"/>
        <source>All new messages</source>
        <translation>All new messages</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="429"/>
        <source>Direct messages and mentions only</source>
        <translation>Direct messages and mentions only</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="439"/>
        <source>Notify me when a huddle starts</source>
        <translation>Notify me when a huddle starts</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="442"/>
        <source>Play a sound for notifications</source>
        <translation>Play a sound for notifications</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="451"/>
        <source>Sound:</source>
        <translation>Sound:</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="456"/>
        <source>Test</source>
        <translation>Test</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="471"/>
        <source>Sample notifications</source>
        <translation>Sample notifications</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="479"/>
        <source>New DM</source>
        <translation>New DM</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="480"/>
        <source>New channel message</source>
        <translation>New channel message</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="481"/>
        <source>New huddle</source>
        <translation>New huddle</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="484"/>
        <source>Test</source>
        <translation>Test</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="514"/>
        <source>Save</source>
        <translation>Save</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="534"/>
        <source>Cache</source>
        <translation>Cache</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="539"/>
        <source>Cache size:</source>
        <translation>Cache size:</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="549"/>
        <source>Conversations, user names, message history, and image thumbnails
stored locally to speed up startup.</source>
        <translation>Conversations, user names, message history, and image thumbnails
stored locally to speed up startup.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="558"/>
        <source>Limit cache to</source>
        <translation>Limit cache to</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="564"/>
        <source> MB</source>
        <translation> MB</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="571"/>
        <source>When the cache grows past this limit, the least recently
viewed images are deleted first.</source>
        <translation>When the cache grows past this limit, the least recently
viewed images are deleted first.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="590"/>
        <source>Clear cache</source>
        <translation>Clear cache</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="600"/>
        <source>State</source>
        <translation>State</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="605"/>
        <source>Sidebar visit history used to decide which conversations are shown.
Clear this to let the app re-analyse activity from scratch on next load.</source>
        <translation>Sidebar visit history used to decide which conversations are shown.
Clear this to let the app re-analyse activity from scratch on next load.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="615"/>
        <source>Clear state</source>
        <translation>Clear state</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="634"/>
        <source>Version</source>
        <translation>Version</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="641"/>
        <source>Version %1, built %2</source>
        <translation>Version %1, built %2</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="652"/>
        <source>Check for updates automatically</source>
        <translation>Check for updates automatically</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="660"/>
        <source>When off, msga never contacts the update server on its own — use the
button below to look for a new version.</source>
        <translation>When off, msga never contacts the update server on its own — use the
button below to look for a new version.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="669"/>
        <source>Check for updates</source>
        <translation>Check for updates</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="689"/>
        <source>Slack connection</source>
        <translation>Slack connection</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="693"/>
        <source>Choose how msga connects to Slack.</source>
        <translation>Choose how msga connects to Slack.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="705"/>
        <source>Slack session — no app keys, uses your own account's limits</source>
        <translation>Slack session — no app keys, uses your own account's limits</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="708"/>
        <source>Slack app keys — OAuth sign-in with live message push</source>
        <translation>Slack app keys — OAuth sign-in with live message push</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="714"/>
        <source>Restart msga to apply this change.</source>
        <translation>Restart msga to apply this change.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="727"/>
        <source>Add a workspace using your existing Slack session. New messages arrive by polling — there's no live push in this mode.</source>
        <translation>Add a workspace using your existing Slack session. New messages arrive by polling — there's no live push in this mode.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="736"/>
        <source>Import Slack session…</source>
        <translation>Import Slack session…</translation>
    </message>
    <message numerus="yes">
        <location filename="../src/ui/settings/settings_dialog.cpp" line="755"/>
        <source>You still have %n Slack workspace(s) on app keys. Convert them to session so no workspace uses Socket Mode.</source>
        <translation>
            <numerusform>You still have %n Slack workspace on app keys. Convert them to session so no workspace uses Socket Mode.</numerusform>
            <numerusform>You still have %n Slack workspaces on app keys. Convert them to session so no workspace uses Socket Mode.</numerusform>
        </translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="766"/>
        <source>Convert them to session</source>
        <translation>Convert them to session</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="787"/>
        <source>Run your own Slack app so you don't share connection keys with other devices and users. Leave a field empty to use the built-in default.</source>
        <translation>Run your own Slack app so you don't share connection keys with other devices and users. Leave a field empty to use the built-in default.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="796"/>
        <source>How to create your Slack app…</source>
        <translation>How to create your Slack app…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="828"/>
        <source>Client ID</source>
        <translation>Client ID</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="828"/>
        <source>e.g. 1234567890.1234567890</source>
        <translation>e.g. 1234567890.1234567890</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="829"/>
        <source>Client secret</source>
        <translation>Client secret</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="829"/>
        <source>Paste your client secret</source>
        <translation>Paste your client secret</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="830"/>
        <source>App-level token</source>
        <translation>App-level token</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="830"/>
        <source>Paste your xapp- token</source>
        <translation>Paste your xapp- token</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="839"/>
        <source>Save and restart</source>
        <translation>Save and restart</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="869"/>
        <source>Memory</source>
        <translation>Memory</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="890"/>
        <source>RAM used: %1</source>
        <translation>RAM used: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="903"/>
        <source>License</source>
        <translation>License</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="908"/>
        <source>MSGA — Make Slack Great Again
Copyright © 2026 Vladimir Osipov

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version (GPL-3.0-or-later).</source>
        <translation>MSGA — Make Slack Great Again
Copyright © 2026 Vladimir Osipov

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version (GPL-3.0-or-later).</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="921"/>
        <source>View full license</source>
        <translation>View full license</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="933"/>
        <source>Contact</source>
        <translation>Contact</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="939"/>
        <source>Questions or feedback: %1</source>
        <translation>Questions or feedback: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="947"/>
        <source>Found a bug?</source>
        <translation>Found a bug?</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="952"/>
        <source>Report it on GitHub so it can be tracked and fixed.</source>
        <translation>Report it on GitHub so it can be tracked and fixed.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="958"/>
        <source>Report a bug</source>
        <translation>Report a bug</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="991"/>
        <source>Connect an AI provider to enable assistant features.
Create an API key in your own provider account and paste it below —
it is stored on this computer and sent only to that provider.</source>
        <translation>Connect an AI provider to enable assistant features.
Create an API key in your own provider account and paste it below —
it is stored on this computer and sent only to that provider.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="999"/>
        <source>AI provider</source>
        <translation>AI provider</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1007"/>
        <source>Default:</source>
        <translation>Default:</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1044"/>
        <source>Connect (OAuth)</source>
        <translation>Connect (OAuth)</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1048"/>
        <source>Disconnect</source>
        <translation>Disconnect</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1057"/>
        <source>Paste your API key</source>
        <translation>Paste your API key</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1061"/>
        <source>Save key</source>
        <translation>Save key</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1067"/>
        <source>Get an API key from %1…</source>
        <translation>Get an API key from %1…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1096"/>
        <source>%1: %2</source>
        <translation>%1: %2</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1104"/>
        <source>Your language</source>
        <translation>Your language</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1109"/>
        <source>AI features address you in this language.
It follows the app language until you pick one here.</source>
        <translation>AI features address you in this language.
It follows the app language until you pick one here.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1118"/>
        <source>Native language:</source>
        <translation>Native language:</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1178"/>
        <source>Connected as %1</source>
        <translation>Connected as %1</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1179"/>
        <source>Connected with API key (%1)</source>
        <translation>Connected with API key (%1)</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1182"/>
        <source>Waiting for browser sign-in…</source>
        <translation>Waiting for browser sign-in…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1184"/>
        <source>Not connected</source>
        <translation>Not connected</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1624"/>
        <source>No changes to save.</source>
        <translation>No changes to save.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1628"/>
        <source>Saved. Restarting…</source>
        <translation>Saved. Restarting…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1676"/>
        <source>Last checked: %1</source>
        <translation>Last checked: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1684"/>
        <source>Update checks not available.</source>
        <translation>Update checks not available.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1689"/>
        <source>Checking for updates…</source>
        <translation>Checking for updates…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1692"/>
        <source>Update downloaded — restart the app to apply.</source>
        <translation>Update downloaded — restart the app to apply.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1706"/>
        <source>Checking for updates…</source>
        <translation>Checking for updates…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1710"/>
        <source>msga is up to date.</source>
        <translation>msga is up to date.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1714"/>
        <source>Version %1 available — downloading…</source>
        <translation>Version %1 available — downloading…</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1717"/>
        <source>Downloading update… %1%</source>
        <translation>Downloading update… %1%</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1721"/>
        <source>Update downloaded — restart the app to apply.</source>
        <translation>Update downloaded — restart the app to apply.</translation>
    </message>
    <message>
        <location filename="../src/ui/settings/settings_dialog.cpp" line="1726"/>
        <source>Check failed: %1</source>
        <translation>Check failed: %1</translation>
    </message>
</context>
<context>
    <name>StatusDialog</name>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="105"/>
        <source>Set a status</source>
        <translation>Set a status</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="150"/>
        <source>What's your status?</source>
        <translation>What's your status?</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="161"/>
        <source>In a meeting</source>
        <translation>In a meeting</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="162"/>
        <source>Commuting</source>
        <translation>Commuting</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="163"/>
        <source>Out sick</source>
        <translation>Out sick</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="164"/>
        <source>Vacationing</source>
        <translation>Vacationing</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="165"/>
        <source>Working remotely</source>
        <translation>Working remotely</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="168"/>
        <source>Don't clear</source>
        <translation>Don't clear</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="169"/>
        <source>30 minutes</source>
        <translation>30 minutes</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="170"/>
        <source>1 hour</source>
        <translation>1 hour</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="171"/>
        <source>4 hours</source>
        <translation>4 hours</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="172"/>
        <source>Today</source>
        <translation>Today</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="173"/>
        <source>This week</source>
        <translation>This week</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="179"/>
        <source>Suggestions</source>
        <translation>Suggestions</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="179"/>
        <source>For %1</source>
        <translation>For %1</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="203"/>
        <source>Clear after</source>
        <translation>Clear after</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="210"/>
        <source>Don't clear</source>
        <translation>Don't clear</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="211"/>
        <source>30 minutes</source>
        <translation>30 minutes</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="212"/>
        <source>1 hour</source>
        <translation>1 hour</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="213"/>
        <source>4 hours</source>
        <translation>4 hours</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="214"/>
        <source>Today</source>
        <translation>Today</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="215"/>
        <source>This week</source>
        <translation>This week</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="226"/>
        <source>Clear status</source>
        <translation>Clear status</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="231"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
    <message>
        <location filename="../src/ui/status_dialog/status_dialog.cpp" line="232"/>
        <source>Save</source>
        <translation>Save</translation>
    </message>
</context>
<context>
    <name>SummarizeJob</name>
    <message>
        <location filename="../src/ui/summary_dialog/summarize_job.cpp" line="58"/>
        <source>Summarizing discussion…</source>
        <translation>Summarizing discussion…</translation>
    </message>
    <message>
        <location filename="../src/ui/summary_dialog/summarize_job.cpp" line="120"/>
        <source>Nothing to summarize — no text messages in the selected range.</source>
        <translation>Nothing to summarize — no text messages in the selected range.</translation>
    </message>
    <message>
        <location filename="../src/ui/summary_dialog/summarize_job.cpp" line="140"/>
        <source>Couldn't summarize: %1</source>
        <translation>Couldn't summarize: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/summary_dialog/summarize_job.cpp" line="181"/>
        <source>[shared a file]</source>
        <translation>[shared a file]</translation>
    </message>
    <message>
        <location filename="../src/ui/summary_dialog/summarize_job.cpp" line="182"/>
        <source>[shared a file: %1]</source>
        <translation>[shared a file: %1]</translation>
    </message>
</context>
<context>
    <name>SummaryDialog</name>
    <message>
        <location filename="../src/ui/summary_dialog/summary_dialog.cpp" line="50"/>
        <source>Discussion summary</source>
        <translation>Discussion summary</translation>
    </message>
    <message>
        <location filename="../src/ui/summary_dialog/summary_dialog.cpp" line="78"/>
        <source>Copy</source>
        <translation>Copy</translation>
    </message>
    <message>
        <location filename="../src/ui/summary_dialog/summary_dialog.cpp" line="81"/>
        <source>Copied</source>
        <translation>Copied</translation>
    </message>
    <message>
        <location filename="../src/ui/summary_dialog/summary_dialog.cpp" line="82"/>
        <source>Copy</source>
        <translation>Copy</translation>
    </message>
    <message>
        <location filename="../src/ui/summary_dialog/summary_dialog.cpp" line="89"/>
        <source>Open settings</source>
        <translation>Open settings</translation>
    </message>
</context>
<context>
    <name>ThreadExportJob</name>
    <message>
        <location filename="../src/ui/thread_panel/thread_export_job.cpp" line="51"/>
        <source>Downloading thread…</source>
        <translation>Downloading thread…</translation>
    </message>
    <message>
        <location filename="../src/ui/thread_panel/thread_export_job.cpp" line="117"/>
        <source>Thread in %1</source>
        <translation>Thread in %1</translation>
    </message>
    <message>
        <location filename="../src/ui/thread_panel/thread_export_job.cpp" line="118"/>
        <source>Messages: %1</source>
        <translation>Messages: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/thread_panel/thread_export_job.cpp" line="125"/>
        <source>(edited)</source>
        <translation>(edited)</translation>
    </message>
    <message>
        <location filename="../src/ui/thread_panel/thread_export_job.cpp" line="131"/>
        <source>[file: %1]</source>
        <translation>[file: %1]</translation>
    </message>
    <message>
        <location filename="../src/ui/thread_panel/thread_export_job.cpp" line="131"/>
        <source>untitled</source>
        <translation>untitled</translation>
    </message>
</context>
<context>
    <name>ThreadPanel</name>
    <message>
        <location filename="../src/ui/thread_panel/thread_panel.cpp" line="71"/>
        <source>Thread</source>
        <translation>Thread</translation>
    </message>
    <message>
        <location filename="../src/ui/thread_panel/thread_panel.cpp" line="166"/>
        <source>Reply in thread…</source>
        <translation>Reply in thread…</translation>
    </message>
    <message>
        <location filename="../src/ui/thread_panel/thread_panel.cpp" line="221"/>
        <source>Save thread</source>
        <translation>Save thread</translation>
    </message>
    <message>
        <location filename="../src/ui/thread_panel/thread_panel.cpp" line="231"/>
        <source>Download thread as text</source>
        <translation>Download thread as text</translation>
    </message>
</context>
<context>
    <name>ThreadsPage</name>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="179"/>
        <source>%1 files</source>
        <translation>%1 files</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="322"/>
        <source>New</source>
        <translation>New</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="354"/>
        <source>Show 1 more reply</source>
        <translation>Show 1 more reply</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="354"/>
        <source>Show %1 more replies</source>
        <translation>Show %1 more replies</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="370"/>
        <source>Reply in thread…</source>
        <translation>Reply in thread…</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="505"/>
        <source>%1 and %2 others</source>
        <translation>%1 and %2 others</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="508"/>
        <source>you</source>
        <translation>you</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="509"/>
        <source>%1 and you</source>
        <translation>%1 and you</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="547"/>
        <source>Reply in thread…</source>
        <translation>Reply in thread…</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="614"/>
        <source>Threads</source>
        <translation>Threads</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="637"/>
        <source>Show more threads</source>
        <translation>Show more threads</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="711"/>
        <source>Loading threads…</source>
        <translation>Loading threads…</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="741"/>
        <source>Threads you're following will appear here.</source>
        <translation>Threads you're following will appear here.</translation>
    </message>
    <message>
        <location filename="../src/ui/threads_page/threads_page.cpp" line="751"/>
        <source>Couldn't load threads. Try again later.</source>
        <translation>Couldn't load threads. Try again later.</translation>
    </message>
</context>
<context>
    <name>TitleBar</name>
    <message>
        <location filename="../src/ui/title_bar/title_bar.cpp" line="216"/>
        <source>Unpin window</source>
        <translation>Unpin window</translation>
    </message>
    <message>
        <location filename="../src/ui/title_bar/title_bar.cpp" line="216"/>
        <source>Pin window on top</source>
        <translation>Pin window on top</translation>
    </message>
</context>
<context>
    <name>TypingIndicatorWidget</name>
    <message>
        <location filename="../src/ui/typing_indicator/typing_indicator.cpp" line="94"/>
        <source>&lt;b&gt;You&lt;/b&gt; are typing on another device…</source>
        <translation>&lt;b&gt;You&lt;/b&gt; are typing on another device…</translation>
    </message>
    <message>
        <location filename="../src/ui/typing_indicator/typing_indicator.cpp" line="102"/>
        <source>You</source>
        <translation>You</translation>
    </message>
    <message>
        <location filename="../src/ui/typing_indicator/typing_indicator.cpp" line="106"/>
        <source>%1 is typing…</source>
        <translation>%1 is typing…</translation>
    </message>
    <message>
        <location filename="../src/ui/typing_indicator/typing_indicator.cpp" line="106"/>
        <source>%1 are typing…</source>
        <translation>%1 are typing…</translation>
    </message>
</context>
<context>
    <name>UpdateBar</name>
    <message>
        <location filename="../src/ui/update_bar/update_bar.cpp" line="66"/>
        <source>A new version of msga has been downloaded. Restart to apply.</source>
        <translation>A new version of msga has been downloaded. Restart to apply.</translation>
    </message>
    <message>
        <location filename="../src/ui/update_bar/update_bar.cpp" line="67"/>
        <source>Restart now</source>
        <translation>Restart now</translation>
    </message>
    <message>
        <location filename="../src/ui/update_bar/update_bar.cpp" line="69"/>
        <source>A new version of msga is ready to install.</source>
        <translation>A new version of msga is ready to install.</translation>
    </message>
    <message>
        <location filename="../src/ui/update_bar/update_bar.cpp" line="70"/>
        <source>Open installer</source>
        <translation>Open installer</translation>
    </message>
</context>
<context>
    <name>UpdateChecker</name>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="93"/>
        <source>Automatic updates are not supported on this platform.</source>
        <translation>Automatic updates are not supported on this platform.</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="115"/>
        <source>Could not parse version manifest.</source>
        <translation>Could not parse version manifest.</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="135"/>
        <source>Cannot write update to %1</source>
        <translation>Cannot write update to %1</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="162"/>
        <source>Download failed: %1</source>
        <translation>Download failed: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="179"/>
        <source>Could not replace binary: %1</source>
        <translation>Could not replace binary: %1</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="195"/>
        <source>Could not move current binary — check file permissions on %1</source>
        <translation>Could not move current binary — check file permissions on %1</translation>
    </message>
    <message>
        <location filename="../src/ui/update_checker/update_checker.cpp" line="201"/>
        <source>Could not place new binary at %1</source>
        <translation>Could not place new binary at %1</translation>
    </message>
</context>
<context>
    <name>UserProfileCard</name>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="70"/>
        <source>Deactivated account</source>
        <translation>Deactivated account</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="72"/>
        <source>Workspace Owner</source>
        <translation>Workspace Owner</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="74"/>
        <source>Workspace Admin</source>
        <translation>Workspace Admin</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="76"/>
        <source>App</source>
        <translation>App</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="84"/>
        <source>%1 local time</source>
        <translation>%1 local time</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="123"/>
        <source>Message</source>
        <translation>Message</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="395"/>
        <source>Copied</source>
        <translation>Copied</translation>
    </message>
    <message>
        <location filename="../src/ui/user_profile_card/user_profile_card.cpp" line="432"/>
        <source>Message</source>
        <translation>Message</translation>
    </message>
</context>
<context>
    <name>WelcomeWidget</name>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="38"/>
        <source>Keyboard shortcuts</source>
        <translation>Keyboard shortcuts</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="93"/>
        <source>Send message</source>
        <translation>Send message</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="94"/>
        <source>New line in message</source>
        <translation>New line in message</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="95"/>
        <source>Edit last message</source>
        <translation>Edit last message</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="96"/>
        <source>Bold</source>
        <translation>Bold</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="97"/>
        <source>Italic</source>
        <translation>Italic</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="98"/>
        <source>Strikethrough</source>
        <translation>Strikethrough</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="99"/>
        <source>Inline code</source>
        <translation>Inline code</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="100"/>
        <source>Attach file</source>
        <translation>Attach file</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="101"/>
        <source>Emoji picker</source>
        <translation>Emoji picker</translation>
    </message>
    <message>
        <location filename="../src/ui/welcome_tips/welcome_widget.cpp" line="102"/>
        <source>Cancel / exit edit</source>
        <translation>Cancel / exit edit</translation>
    </message>
</context>
<context>
    <name>WorkspaceSwitcher</name>
    <message>
        <location filename="../src/ui/workspace_switcher/workspace_switcher.cpp" line="448"/>
        <source>Add workspace</source>
        <translation>Add workspace</translation>
    </message>
    <message>
        <location filename="../src/ui/workspace_switcher/workspace_switcher.cpp" line="451"/>
        <source>Settings</source>
        <translation>Settings</translation>
    </message>
</context>
<context>
    <name>relativeTime</name>
    <message>
        <location filename="../src/util/relative_time.cpp" line="10"/>
        <source>just now</source>
        <translation>just now</translation>
    </message>
    <message numerus="yes">
        <location filename="../src/util/relative_time.cpp" line="13"/>
        <source>%n minute ago</source>
        <translation>
            <numerusform>%n minute ago</numerusform>
            <numerusform>%n minute ago</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <location filename="../src/util/relative_time.cpp" line="17"/>
        <source>%n hour ago</source>
        <translation>
            <numerusform>%n hour ago</numerusform>
            <numerusform>%n hour ago</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <location filename="../src/util/relative_time.cpp" line="21"/>
        <source>%n day ago</source>
        <translation>
            <numerusform>%n day ago</numerusform>
            <numerusform>%n day ago</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <location filename="../src/util/relative_time.cpp" line="25"/>
        <source>%n month ago</source>
        <translation>
            <numerusform>%n month ago</numerusform>
            <numerusform>%n month ago</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <location filename="../src/util/relative_time.cpp" line="28"/>
        <source>%n year ago</source>
        <translation>
            <numerusform>%n year ago</numerusform>
            <numerusform>%n year ago</numerusform>
        </translation>
    </message>
</context>
<context>
    <name>Sound</name>
    <message>
        <location filename="../src/util/sound_player.cpp" line="53"/>
        <source>msga chime</source>
        <translation>msga chime</translation>
    </message>
</context>
<context>
    <name>domain</name>
    <message>
        <location filename="../src/backend/domain.h" line="773"/>
        <source>Huddle happened</source>
        <translation>Huddle happened</translation>
    </message>
</context>
</TS>
