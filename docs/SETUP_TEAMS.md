# Microsoft Teams setup

msga can host Microsoft Teams workspaces alongside Slack. Teams talks to **Microsoft Graph** over OAuth 2.0, so — like Slack — you must register your own app and configure it before the client will connect. This is a one-time setup.

When you're done, you'll have one value to drop into `credentials.cmake` (see [Step 3](#step-3-add-credentials-to-the-build)). After that, follow the build steps in the [README](../README.md).

> **Heads up — admin consent.** Since late October 2025, Microsoft's managed default consent policy requires **tenant-admin consent** for the delegated scopes that expose Teams content (`Chat.Read`, `Chat.ReadWrite`, and siblings). On a typical managed tenant an individual user **cannot self-consent** — your IT admin must approve the app registration first. If you're just testing, a [Microsoft 365 Developer Program](https://developer.microsoft.com/microsoft-365/dev-program) tenant makes you global admin of a throwaway org so you can grant consent yourself.

## Step 1. Register an Entra (Azure AD) app

1. Go to **[Azure Portal](https://portal.azure.com) → Microsoft Entra ID → App registrations → New registration**.
2. **Name:** `msga`.
3. **Supported account types:** *Accounts in any organizational directory (multitenant)*. Teams has no personal-account API, so org accounts only.
4. Click **Register**, then copy the **Application (client) ID** — you'll need it in Step 3.

## Step 2. Configure authentication & permissions

### Redirect URI

Under **Authentication → Add a platform → Mobile and desktop applications**, add a **Custom redirect URI**:

```
msga://oauth/callback
```

Enter it lowercase, in the custom field. Leave the canned MSAL/LiveSDK redirects unticked — msga runs its own Authorization Code + PKCE flow. Save.

### Allow public client flows

Under **Authentication → Advanced settings**, set **Allow public client flows = Yes**. There is no client secret — desktop apps use Authorization Code + PKCE.

### API permissions

Under **API permissions → Add a permission → Microsoft Graph → Delegated permissions**, add the following scopes:

| Scope | Purpose |
|---|---|
| `openid` | Sign-in |
| `profile` | Basic profile |
| `offline_access` | Refresh tokens (stay signed in) |
| `User.Read` | Read your own profile |
| `User.ReadBasic.All` | Resolve other users' names/avatars |
| `User.ReadWrite` | Update your own profile fields |
| `Team.ReadBasic.All` | List the teams you belong to |
| `Channel.ReadBasic.All` | List channels in those teams |
| `Chat.Read` | Read your chats (DMs / group chats) |
| `Chat.ReadWrite` | Send and edit in chats |
| `ChannelMessage.Read.All` | Read channel messages |
| `ChannelMessage.Send` | Post to channels |
| `Presence.Read.All` | Read presence (online/away) |
| `Presence.ReadWrite` | Set your own presence |
| `Files.ReadWrite.All` | Upload and download shared files |

Several of these are admin-walled (see the note at the top). Add the whole set up front — adding a scope later forces every user to re-consent.

### Grant consent

If you're an admin on the tenant (e.g. a Developer Program tenant), click **Grant admin consent for &lt;tenant&gt;** so the scopes are pre-approved. Otherwise, your tenant admin must approve the app — msga surfaces an "ask your admin" screen with the consent link when it hits the wall.

## Step 3. Add credentials to the build

Copy `credentials.cmake.example` to `credentials.cmake` in the project root (if you haven't already) and fill in the Teams client id from Step 1:

```cmake
set(MSGA_TEAMS_CLIENT_ID "your-entra-application-client-id")
```

No secret is needed (public client + PKCE). `credentials.cmake` is gitignored and never committed. Rebuild after editing it — the value is compiled in.

Now head back to the [README](../README.md) to configure your build environment and build the app.

## Notes & limitations

- **Realtime** is delivered by delta-style polling (~5 s) of conversations you've opened this session — Microsoft exposes no public client-direct websocket for Teams messages. Conversations never opened this session refresh on open / full reload, and others' live edits/deletes refresh on reopen.
- **No canvases, no huddles, no slash commands, no custom emoji** — these have no Graph equivalent and are hidden automatically.
- **Channel read state is partial** — Graph has no per-message read API for channels (chats are fine).
