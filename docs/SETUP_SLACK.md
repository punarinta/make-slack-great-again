# Slack setup

There are **two ways** to connect a Slack workspace to msga. You don't need both — pick whichever suits you:

| | **Session sign-in** (recommended) | **Your own Slack app** |
|---|---|---|
| Setup effort | None — just log in through a browser | ~10 min one-time app registration |
| Cost | Free | Free |
| Rate limits | Your own personal limits | Your app's own limits |
| Real-time messages | Near-instant (checked every ~5 s) | Instant (live push) |
| Best for | Almost everyone | Power users who want instant push |

> msga also ships with **shared** built-in credentials as a demo. That's fine for a quick look, but everyone using the prebuilt app shares one small quota, so it throttles heavily. Either option below gives you your own quota instead.

---

## Option A — Session sign-in (recommended)

You log in to Slack the normal way — in a browser — and msga uses that session. There's **nothing to register** and no app to build, and all your requests count against *your* personal quota instead of a shared one.

The only trade-off: Slack's instant-push channel isn't available to a browser session, so msga fetches new messages by checking every few seconds instead. In practice new messages appear within a few seconds — you won't usually notice the difference.

### The easy way: sign in through a browser

1. In msga, add a Slack workspace (or go to **Settings → System**).
2. Choose **Slack session** — it's the default.
3. Click **Sign in with &lt;your browser&gt;**.

A browser window opens on Slack's sign-in page. Log in the way you normally would — password,
email code or SSO. When you're in, msga picks the session up by itself and the window closes; you
never see a token or a cookie.

That window uses a **private, temporary browser profile**: it doesn't touch your normal browsing
profile or your saved logins, and it's deleted the moment sign-in finishes. Slack's "open the
desktop app" page can't hijack it either — msga tells the temporary profile to ignore
`slack://` links.

This needs Chrome, Chromium, Brave, Edge or Vivaldi installed (Firefox can't be driven this way).
If none is found, msga skips straight to the manual steps below.

> **On Linux**, if you have the Slack desktop app installed, **Import from local Slack** is even
> quicker — one click, no browser window at all.

### The manual way: getting your session cookie yourself

You only need this if browser sign-in isn't available (no supported browser, or you'd rather not have msga open one). msga needs one value from your logged-in Slack: the **`d` cookie**. It's a secret (treat it like a password), so your browser hides it from ordinary copy — you grab it from the developer tools:

1. Open **[https://app.slack.com](https://app.slack.com)** in your web browser and make sure you're signed in to your workspace.
2. Open your browser's developer tools — press **F12** (or right-click the page → **Inspect**).
3. Go to the **Application** tab (in Firefox it's called **Storage**).
4. In the left sidebar, expand **Cookies** and click **`https://app.slack.com`**.
5. Find the cookie named **`d`** and copy its **Value** — it starts with `xoxd-`.
6. Back in msga, paste that value and type your workspace address (e.g. `myteam.slack.com`). msga works out everything else automatically and signs you in.

### Good to know

- **One cookie covers all your workspaces.** The `d` cookie belongs to your Slack account, so importing it once lets msga connect every workspace you're signed in to.
- **Logging out rotates it.** If you sign out of Slack in your browser (or change your password), the cookie changes and msga will ask you to import a fresh one.
- **It's stored locally**, the same way msga stores any other sign-in token — nothing is sent anywhere except Slack.
- You can switch between session and app-keys mode anytime in **Settings → System**.

If session sign-in works for you, you're done — you can ignore the rest of this guide.

---

## Option B — Your own Slack app (app keys)

Create your own free Slack app if you want Slack's **instant real-time push** (Socket Mode) rather than polling. It's a one-time setup and completely free.

When you're done you'll have three values. If you build your own msga, drop them into `credentials.cmake` (see [Step 6](#step-6-add-credentials-to-the-build)) and follow the build steps in the [README](../README.md). If you use the prebuilt app and don't want to build, just go to **Settings → System**, pick **Slack app keys**, paste the three values there and click **Save**.

### Step 1. Create a Slack app

Go to [https://api.slack.com/apps](https://api.slack.com/apps) and click **Create New App → From scratch**.

Give it a name (e.g. "msga") and pick any development workspace to associate it with. The app will work across all your workspaces once installed.

### Step 2. Configure OAuth & permissions

In your app's settings, go to **OAuth & Permissions**.

Under **Redirect URLs**, add exactly:

```
http://localhost:17437/cb
```

The **OAuth & Permissions** page has two scope lists: **Bot Token Scopes** and **User Token Scopes**. msga signs you in as *yourself* — it uses a **user token** (`xoxp-…`) only and never a bot token. Set the two lists accordingly.

#### Bot Token Scopes

**Leave this list empty.** msga does not use a bot token, and the live-message events (Step 4) are subscribed *on behalf of users*, which keys off the User Token Scopes below — not bot scopes. Do not add `admin.*` scopes anywhere: they require an Enterprise Grid org-level install by an org owner/admin and will prevent ordinary members from authorizing the app.

#### User Token Scopes

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

### Step 3. Enable socket mode

Go to **Socket Mode** in the sidebar and toggle **Enable Socket Mode** on.

Then go to **Basic Information → App-Level Tokens** and click **Generate Token and Scopes**. Name the token anything (e.g. "socket"), add the scope `connections:write`, and click **Generate**. Copy the token — it starts with `xapp-1-`.

### Step 4. Subscribe to events

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

### Step 5. Install the app

Go to **Install App** and click **Install to Workspace**. Authorize the requested permissions.

### Step 6. Add credentials to the build

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
