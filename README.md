# Make Slack Great Again

A fast native Slack client built in C++ with Qt6 with performance optimizations.

## 1. Use a prebuilt version

Download the latest prebuilt binary from [msga.app](https://msga.app/):

- [Linux x86-64](https://msga.app/download/msga-linux-x86_64) (any distribution)
- [macOS Apple Silicon](https://msga.app/download/msga-macos-arm64.dmg)
- Windows — coming soon

A Windows build is planned once the client is stable enough for daily use.

## 2. Build your own version

msga app does not ship with Slack credentials. You must create your own Slack app and configure it before the client will connect. This is a one-time setup.

### 1. Create a Slack app

Go to [https://api.slack.com/apps](https://api.slack.com/apps) and click **Create New App → From scratch**.

Give it a name (e.g. "msga") and pick any development workspace to associate it with. The app will work across all your workspaces once installed.

### 2. Configure OAuth & permissions

In your app's settings, go to **OAuth & Permissions**.

Under **Redirect URLs**, add exactly:

```
http://localhost:17437/cb
```

Then scroll down to **User Token Scopes** and add the following scopes:

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

### 3. Enable socket mode

Go to **Socket Mode** in the sidebar and toggle **Enable Socket Mode** on.

Then go to **Basic Information → App-Level Tokens** and click **Generate Token and Scopes**. Name the token anything (e.g. "socket"), add the scope `connections:write`, and click **Generate**. Copy the token — it starts with `xapp-1-`.

### 4. Subscribe to events

Go to **Event Subscriptions** in the sidebar and toggle **Enable Events** on.

Under **Subscribe to events on behalf of users**, add:

| Event | Purpose |
|---|---|
| `message.channels` | Live messages in public channels |
| `message.groups` | Live messages in private channels |
| `message.im` | Live direct messages |
| `message.mpim` | Live group direct messages |

Click **Save Changes**. Slack will prompt you to reinstall the app — do so via **Install App → Reinstall to Workspace**.

### 5. Install the app

Go to **Install App** and click **Install to Workspace**. Authorize the requested permissions.

### 6. Add credentials to the build

From **Basic Information**, copy:

- **Client ID** — under *App Credentials*
- **Client Secret** — under *App Credentials* (click *Show*)

You also need the **App Token** (`xapp-1-…`) from step 3.

Copy `credentials.cmake.example` to `credentials.cmake` in the project root and fill in the three values:

```cmake
set(MSGA_CLIENT_ID     "your-client-id")
set(MSGA_CLIENT_SECRET "your-client-secret")
set(MSGA_XAPP          "xapp-1-...")
```

`credentials.cmake` is gitignored and never committed. Rebuild after editing it — the values are compiled in.

### 7. Build and run

```sh
./scripts/build.sh
./build/msga
```
