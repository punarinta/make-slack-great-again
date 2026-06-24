# Slack setup

msga does not ship with Slack credentials. You must create your own Slack app and configure it before the client will connect to Slack. This is a one-time setup.

When you're done, you'll have three values to drop into `credentials.cmake` (see [Step 6](#step-6-add-credentials-to-the-build)). After that, follow the build steps in the [README](../README.md).

## Step 1. Create a Slack app

Go to [https://api.slack.com/apps](https://api.slack.com/apps) and click **Create New App → From scratch**.

Give it a name (e.g. "msga") and pick any development workspace to associate it with. The app will work across all your workspaces once installed.

## Step 2. Configure OAuth & permissions

In your app's settings, go to **OAuth & Permissions**.

Under **Redirect URLs**, add exactly:

```
http://localhost:17437/cb
```

Then scroll down to **User Token Scopes** and add the following scopes:

<details>
<summary>Full list of required OAuth scopes</summary>

| Scope | Purpose |
|---|---|
| `channels:history` | Read messages in public channels |
| `channels:read` | List public channels |
| `groups:history` | Read messages in private channels |
| `groups:read` | List private channels |
| `im:history` | Read direct messages |
| `im:read` | List direct message conversations |
| `mpim:history` | Read group direct messages |
| `mpim:read` | List group direct message conversations |
| `users:read` | Look up user info |
| `team:read` | Get workspace info |
| `emoji:read` | Load custom emoji |
| `reactions:read` | Read emoji reactions |
| `files:read` | Access shared files |
| `users.profile:read` | Read user profile fields |
| `search:read` | Search messages and files |
| `chat:write` | Send messages |
| `reactions:write` | Add/remove reactions |
| `files:write` | Upload files |
| `stars:write` | Star/unstar channels and conversations |
| `stars:read` | List starred channels and conversations |
| `channels:write` | Create public channels |
| `groups:write` | Create private channels |
| `mpim:write` | Create group DM conversations |
| `im:write` | Create direct message conversations |
| `users:write` | Set your presence — away/active (`/away`, `/active`) |
| `users.profile:write` | Set or clear your status (`/status`) |
| `dnd:write` | Pause/resume notifications (`/dnd`) |
| `canvases:read` | Look up canvas sections |
| `canvases:write` | Create, edit and delete canvases |

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
