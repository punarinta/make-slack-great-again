# Slack setup

msga is shipped with shared Slack credentials. In this mode you share the app quota with other people. This is basically a demo mode, as it limits you A LOT. You must create your own Slack app and configure it before the client will connect to Slack. This is a relatively simple one-time setup and is completely free.

When you're done, you'll have three values. If you build your own app, drop them into `credentials.cmake` (see [Step 6](#step-6-add-credentials-to-the-build)). After that, follow the build steps in the [README](../README.md). If you use our prebuilt app and don't want to bother with building, simply go to Settins->System in the app, paste those credentials there and click "Save".

## Step 1. Create a Slack app

Go to [https://api.slack.com/apps](https://api.slack.com/apps) and click **Create New App → From scratch**.

Give it a name (e.g. "msga") and pick any development workspace to associate it with. The app will work across all your workspaces once installed.

## Step 2. Configure OAuth & permissions

In your app's settings, go to **OAuth & Permissions**.

Under **Redirect URLs**, add exactly:

```
http://localhost:17437/cb
```

The **OAuth & Permissions** page has two scope lists: **Bot Token Scopes** and **User Token Scopes**. msga signs you in as *yourself* — it uses a **user token** (`xoxp-…`) only and never a bot token. Set the two lists accordingly.

### Bot Token Scopes

**Leave this list empty.** msga does not use a bot token, and the live-message events (Step 4) are subscribed *on behalf of users*, which keys off the User Token Scopes below — not bot scopes. Do not add `admin.*` scopes anywhere: they require an Enterprise Grid org-level install by an org owner/admin and will prevent ordinary members from authorizing the app.

### User Token Scopes

Scroll down to **User Token Scopes** and add the following:

<details>
<summary>Full list of required OAuth scopes</summary>

| Scope | Purpose |
|---|---|
| `canvases:read` | Read canvas files (`files.info` gates canvas files behind this) |
| `canvases:write` | Create, edit and delete canvases |
| `channels:history` | Read messages in public channels |
| `channels:read` | List public channels |
| `channels:write` | Create public channels |
| `chat:write` | Send messages |
| `dnd:read` | See other users' Do Not Disturb state (live `dnd_updated_user` events) |
| `dnd:write` | Pause/resume notifications (`/dnd`) |
| `emoji:read` | Load custom emoji |
| `files:read` | Access shared files |
| `files:write` | Upload files |
| `groups:history` | Read messages in private channels |
| `groups:read` | List private channels |
| `groups:write` | Create private channels |
| `im:history` | Read direct messages |
| `im:read` | List direct message conversations |
| `im:write` | Create direct message conversations |
| `mpim:history` | Read group direct messages |
| `mpim:read` | List group direct message conversations |
| `mpim:write` | Create group DM conversations |
| `pins:write` | Pin/unpin messages |
| `reactions:read` | Read emoji reactions |
| `reactions:write` | Add/remove reactions |
| `search:read` | Search messages and files |
| `stars:write` | Star/unstar channels and conversations |
| `team:read` | Get workspace info |
| `users.profile:read` | Read user profile fields |
| `users.profile:write` | Set or clear your status (`/status`) |
| `users:read` | Look up user info |
| `users:write` | Set your presence — away/active (`/away`, `/active`) |

</details>

## Step 3. Enable socket mode

Go to **Socket Mode** in the sidebar and toggle **Enable Socket Mode** on.

Then go to **Basic Information → App-Level Tokens** and click **Generate Token and Scopes**. Name the token anything (e.g. "socket"), add the scope `connections:write`, and click **Generate**. Copy the token — it starts with `xapp-1-`.

## Step 4. Subscribe to events

Go to **Event Subscriptions** in the sidebar and toggle **Enable Events** on.

Under **Subscribe to events on behalf of users**, add:

| Event | Purpose |
|---|---|
| `message.channels` | Live messages in public channels |
| `message.groups` | Live messages in private channels |
| `message.im` | Live direct messages |
| `message.mpim` | Live group direct messages |
| `reaction_added` | Live reactions as they're added |
| `reaction_removed` | Live reactions as they're removed |
| `channel_created` | New channels appear without a refresh |
| `member_joined_channel` | Live channel membership updates |
| `user_change` | Live profile, name and avatar updates |
| `dnd_updated_user` | Live Do Not Disturb changes |

Click **Save Changes**. Slack will prompt you to reinstall the app — do so via **Install App → Reinstall to Workspace**.

## Step 5. Install the app

Go to **Install App** and click **Install to Workspace**. Authorize the requested permissions.

## Step 6. Add credentials to the build

From **Basic Information**, copy:

- **Client ID** — under *App Credentials*
- **Client Secret** — under *App Credentials* (click *Show*)

You also need the **App Token** (`xapp-1-…`) from step 3.

Copy `credentials.cmake.example` to `credentials.cmake` in the project root and fill in the three Slack values:

```cmake
set(MSGA_CLIENT_ID     "your-client-id")
set(MSGA_CLIENT_SECRET "your-client-secret")
set(MSGA_XAPP          "xapp-1-...")
```

`credentials.cmake` is gitignored and never committed. Rebuild after editing it — the values are compiled in.

Now head back to the [README](../README.md) to configure your build environment and build the app.
