// Twitch connection for stream song requests: the streamer's own account, logged in once with the
// OAuth device code flow (public client, no secret), reads chat through EventSub over WebSocket
// (channel.chat.message) and answers through the Send Chat Message API.
//
// Scopes: user:read:chat and user:write:chat, nothing else. The tokens are stored in
// %LOCALAPPDATA%\Pick\twitch-token.json and never written to the console or a log.
using System.Net;
using System.Net.Http.Headers;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading.Channels;

sealed record ChatMessage(string MessageId, string UserId, string Login, string DisplayName, string Text,
                          bool IsBroadcaster, bool IsModerator);

sealed class TwitchToken
{
    public string ClientId { get; set; } = "";
    public string AccessToken { get; set; } = "";
    public string RefreshToken { get; set; } = "";
    public string UserId { get; set; } = "";
    public string Login { get; set; } = "";
    public DateTime ExpiresUtc { get; set; }
}

sealed class TwitchAuth
{
    public const string Scopes = "user:read:chat user:write:chat";
    // Client ID of a Twitch application registered as "Public" (no secret). A public client ID is
    // not a secret. Empty: the streamer enters the client ID of their own registration in the
    // dashboard (see README, "Twitch song requests").
    public const string DefaultClientId = "";

    static string TokenPath => Path.Combine(AppData.Dir, "twitch-token.json");
    static readonly JsonSerializerOptions FileJson = new() { WriteIndented = true };

    readonly HttpClient _http;
    readonly object _gate = new();
    readonly SemaphoreSlim _refreshLock = new(1, 1);
    TwitchToken? _token;
    CancellationTokenSource? _deviceFlow;

    // Dashboard state: disconnected, awaiting_user (code shown), connected; plus a message.
    public string State { get; private set; } = "disconnected";
    public string? Message { get; private set; }
    public string? UserCode { get; private set; }
    public string? VerificationUri { get; private set; }
    public DateTime? CodeExpiresUtc { get; private set; }
    public event Action? Changed;

    public TwitchAuth(HttpClient http)
    {
        _http = http;
        try
        {
            if (File.Exists(TokenPath))
            {
                _token = JsonSerializer.Deserialize<TwitchToken>(File.ReadAllText(TokenPath));
                if (_token != null && _token.RefreshToken.Length > 0 && _token.UserId.Length > 0)
                    State = "connected";
                else
                    _token = null;
            }
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
            _token = null;
        }
    }

    public string? Login { get { lock (_gate) return _token?.Login; } }
    public string? UserId { get { lock (_gate) return _token?.UserId; } }
    public string? ClientId { get { lock (_gate) return _token?.ClientId; } }
    public bool Connected { get { lock (_gate) return _token != null; } }

    // Starts the device code flow; the dashboard shows the code and the link. Polling runs in the
    // background until the streamer has approved, the code expired or it was cancelled.
    public async Task<string?> StartLogin(string clientId)
    {
        _deviceFlow?.Cancel();
        var cts = _deviceFlow = new CancellationTokenSource();
        HttpResponseMessage resp;
        try
        {
            resp = await _http.PostAsync("https://id.twitch.tv/oauth2/device",
                new FormUrlEncodedContent(new Dictionary<string, string> { ["client_id"] = clientId, ["scopes"] = Scopes }), cts.Token);
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException)
        {
            return "Twitch didn't answer. Check the internet connection.";
        }
        using (resp)
        {
            if (!resp.IsSuccessStatusCode)
                return resp.StatusCode == HttpStatusCode.BadRequest
                    ? "Twitch refused the client ID. Check it, and that the application's client type is \"Public\"."
                    : $"Twitch answered {(int)resp.StatusCode}.";
            using var doc = JsonDocument.Parse(await resp.Content.ReadAsStringAsync());
            var r = doc.RootElement;
            string deviceCode = r.GetProperty("device_code").GetString()!;
            int expiresIn = r.GetProperty("expires_in").GetInt32();
            int interval = Math.Max(1, r.GetProperty("interval").GetInt32());
            UserCode = r.GetProperty("user_code").GetString();
            VerificationUri = r.GetProperty("verification_uri").GetString();
            CodeExpiresUtc = DateTime.UtcNow.AddSeconds(expiresIn);
            State = "awaiting_user";
            Message = null;
            Changed?.Invoke();
            _ = Task.Run(() => PollDeviceCode(clientId, deviceCode, interval, cts.Token));
            return null;
        }
    }

    async Task PollDeviceCode(string clientId, string deviceCode, int interval, CancellationToken ct)
    {
        try
        {
            while (DateTime.UtcNow < CodeExpiresUtc)
            {
                await Task.Delay(TimeSpan.FromSeconds(interval), ct);
                using var resp = await _http.PostAsync("https://id.twitch.tv/oauth2/token", new FormUrlEncodedContent(new Dictionary<string, string>
                {
                    ["client_id"] = clientId, ["scopes"] = Scopes, ["device_code"] = deviceCode,
                    ["grant_type"] = "urn:ietf:params:oauth:grant-type:device_code",
                }), ct);
                string body = await resp.Content.ReadAsStringAsync(ct);
                if (resp.IsSuccessStatusCode)
                {
                    var token = ParseTokenResponse(clientId, body);
                    string? problem = await FillIdentity(token);
                    if (problem != null)
                        throw new InvalidOperationException(problem);
                    lock (_gate)
                        _token = token;
                    Save(token);
                    State = "connected";
                    Message = null;
                    UserCode = VerificationUri = null;
                    Console.WriteLine($"[twitch] logged in as {token.Login}");
                    Changed?.Invoke();
                    return;
                }
                if (body.Contains("authorization_pending", StringComparison.Ordinal))
                    continue;
                if (body.Contains("slow_down", StringComparison.Ordinal))
                {
                    interval += 5;
                    continue;
                }
                throw new InvalidOperationException(body.Contains("denied", StringComparison.OrdinalIgnoreCase)
                    ? "The login was declined on Twitch." : "The login code expired or was refused. Try again.");
            }
            throw new InvalidOperationException("The login code expired. Try again.");
        }
        catch (OperationCanceledException) when (ct.IsCancellationRequested)
        {
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or JsonException or InvalidOperationException or KeyNotFoundException)
        {
            State = "disconnected";
            Message = e is InvalidOperationException ? e.Message : "Login failed: Twitch didn't answer properly.";
            UserCode = VerificationUri = null;
            Changed?.Invoke();
        }
    }

    public void CancelLogin()
    {
        _deviceFlow?.Cancel();
        if (State == "awaiting_user")
        {
            State = Connected ? "connected" : "disconnected";
            UserCode = VerificationUri = null;
            Changed?.Invoke();
        }
    }

    static TwitchToken ParseTokenResponse(string clientId, string body)
    {
        using var doc = JsonDocument.Parse(body);
        var r = doc.RootElement;
        return new TwitchToken
        {
            ClientId = clientId,
            AccessToken = r.GetProperty("access_token").GetString()!,
            RefreshToken = r.GetProperty("refresh_token").GetString()!,
            ExpiresUtc = DateTime.UtcNow.AddSeconds(r.GetProperty("expires_in").GetInt32()),
        };
    }

    // GET /oauth2/validate: fills in user id and login and checks the scopes. Null when valid.
    async Task<string?> FillIdentity(TwitchToken token)
    {
        using var msg = new HttpRequestMessage(HttpMethod.Get, "https://id.twitch.tv/oauth2/validate");
        msg.Headers.Authorization = new AuthenticationHeaderValue("OAuth", token.AccessToken);
        using var resp = await _http.SendAsync(msg);
        if (resp.StatusCode == HttpStatusCode.Unauthorized)
            return "invalid";
        resp.EnsureSuccessStatusCode();
        using var doc = JsonDocument.Parse(await resp.Content.ReadAsStringAsync());
        var r = doc.RootElement;
        var scopes = r.TryGetProperty("scopes", out var s) && s.ValueKind == JsonValueKind.Array
            ? s.EnumerateArray().Select(x => x.GetString()).ToHashSet() : new HashSet<string?>();
        if (!Scopes.Split(' ').All(scopes.Contains))
            return "Twitch granted fewer permissions than Pick needs. Log in again.";
        token.UserId = r.GetProperty("user_id").GetString() ?? "";
        token.Login = r.GetProperty("login").GetString() ?? "";
        return null;
    }

    // A valid access token, refreshed when it is about to expire; null when logged out.
    public async Task<TwitchToken?> GetToken(bool forceRefresh = false)
    {
        TwitchToken? t;
        lock (_gate)
            t = _token;
        if (t == null)
            return null;
        if (!forceRefresh && t.ExpiresUtc > DateTime.UtcNow.AddMinutes(5))
            return t;
        await _refreshLock.WaitAsync();
        try
        {
            lock (_gate)
                t = _token;
            if (t == null)
                return null;
            if (!forceRefresh && t.ExpiresUtc > DateTime.UtcNow.AddMinutes(5))
                return t; // refreshed by another caller meanwhile
            // Public clients refresh without a secret; the refresh token is single use, so the new
            // one must be saved before anything else can fail.
            using var resp = await _http.PostAsync("https://id.twitch.tv/oauth2/token", new FormUrlEncodedContent(new Dictionary<string, string>
            {
                ["client_id"] = t.ClientId, ["grant_type"] = "refresh_token", ["refresh_token"] = t.RefreshToken,
            }));
            if (resp.StatusCode is HttpStatusCode.BadRequest or HttpStatusCode.Unauthorized)
            {
                Forget("The Twitch login expired or was revoked. Log in again.");
                return null;
            }
            resp.EnsureSuccessStatusCode();
            var fresh = ParseTokenResponse(t.ClientId, await resp.Content.ReadAsStringAsync());
            fresh.UserId = t.UserId;
            fresh.Login = t.Login;
            lock (_gate)
                _token = fresh;
            Save(fresh);
            return fresh;
        }
        finally
        {
            _refreshLock.Release();
        }
    }

    // Twitch requires apps to validate their token at startup and every hour.
    public async Task ValidateLoop(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                var t = await GetToken();
                if (t != null)
                {
                    var probe = new TwitchToken { AccessToken = t.AccessToken };
                    string? problem = await FillIdentity(probe);
                    if (problem == "invalid")
                        await GetToken(forceRefresh: true);
                    else if (problem != null)
                        Forget(problem);
                }
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException or JsonException or KeyNotFoundException)
            {
                // Offline: try again next round.
            }
            try
            {
                await Task.Delay(TimeSpan.FromHours(1), ct);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    // Logs out: revokes the token at Twitch and deletes the file.
    public async Task Logout()
    {
        CancelLogin();
        TwitchToken? t;
        lock (_gate)
            t = _token;
        if (t != null)
        {
            try
            {
                using var _ = await _http.PostAsync("https://id.twitch.tv/oauth2/revoke", new FormUrlEncodedContent(
                    new Dictionary<string, string> { ["client_id"] = t.ClientId, ["token"] = t.AccessToken }));
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException)
            {
            }
        }
        Forget(null);
    }

    void Forget(string? message)
    {
        lock (_gate)
            _token = null;
        try
        {
            File.Delete(TokenPath);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
        State = "disconnected";
        Message = message;
        if (message != null)
            Console.WriteLine($"[twitch] {message}");
        Changed?.Invoke();
    }

    static void Save(TwitchToken t)
    {
        Directory.CreateDirectory(AppData.Dir);
        string tmp = TokenPath + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(t, FileJson));
        File.Move(tmp, TokenPath, overwrite: true);
    }
}

// Reads chat through EventSub WebSocket and sends rate-limited replies.
sealed class TwitchChat(TwitchAuth auth, HttpClient http)
{
    const string EventSubUrl = "wss://eventsub.wss.twitch.tv/ws";
    // Twitch allows the broadcaster 100 messages per 30 s; Pick stays far below that.
    static readonly TimeSpan SendInterval = TimeSpan.FromSeconds(1.5);
    static readonly TimeSpan MaxReplyAge = TimeSpan.FromSeconds(20);

    readonly Channel<(string text, string? replyTo, DateTime queued)> _outbox =
        Channel.CreateBounded<(string, string?, DateTime)>(new BoundedChannelOptions(8) { FullMode = BoundedChannelFullMode.DropWrite });

    public string State { get; private set; } = "offline"; // offline, connecting, listening
    public string? Message { get; private set; }
    public event Func<ChatMessage, Task>? Received;
    CancellationTokenSource? _session;

    // Restart the chat connection, e.g. after a login or logout.
    public void Restart() => _session?.Cancel();

    public void Reply(string text, string? replyToMessageId)
    {
        text = text.Trim();
        if (text.Length > 450)
            text = text[..447] + "…";
        _outbox.Writer.TryWrite((text, replyToMessageId, DateTime.UtcNow)); // dropped when 8 are waiting
    }

    public async Task Run(CancellationToken stop)
    {
        _ = Task.Run(() => SendLoop(stop));
        int backoff = 2;
        while (!stop.IsCancellationRequested)
        {
            if (!auth.Connected)
            {
                State = "offline";
                Message = null;
                await Wait(TimeSpan.FromSeconds(2), stop);
                continue;
            }
            _session = CancellationTokenSource.CreateLinkedTokenSource(stop);
            try
            {
                State = "connecting";
                await Listen(_session.Token);
                backoff = 2;
            }
            catch (Exception e) when (e is WebSocketException or HttpRequestException or JsonException or KeyNotFoundException or
                                        InvalidOperationException or IOException or OperationCanceledException)
            {
                if (stop.IsCancellationRequested)
                    return;
                if (e is not OperationCanceledException || !_session.IsCancellationRequested)
                {
                    Message = e is InvalidOperationException ? e.Message : "Chat connection lost, reconnecting…";
                    Console.WriteLine($"[twitch] chat: {Message}");
                    State = "offline";
                    await Wait(TimeSpan.FromSeconds(backoff), stop);
                    backoff = Math.Min(backoff * 2, 60);
                }
            }
        }
    }

    static async Task Wait(TimeSpan t, CancellationToken ct)
    {
        try
        {
            await Task.Delay(t, ct);
        }
        catch (OperationCanceledException)
        {
        }
    }

    async Task Listen(CancellationToken ct)
    {
        var ws = new ClientWebSocket();
        try
        {
            await ws.ConnectAsync(new Uri(EventSubUrl), ct);
            var (welcome, keepalive) = await ReadWelcome(ws, ct);
            await Subscribe(welcome, ct);
            State = "listening";
            Message = null;
            Console.WriteLine($"[twitch] listening to #{auth.Login}'s chat");
            while (true)
            {
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
                timeout.CancelAfter(TimeSpan.FromSeconds(keepalive + 10));
                string? text;
                try
                {
                    text = await ReadText(ws, timeout.Token);
                }
                catch (OperationCanceledException) when (!ct.IsCancellationRequested)
                {
                    throw new IOException("keepalive timeout");
                }
                if (text == null)
                    throw new IOException($"closed by Twitch ({ws.CloseStatus})");
                using var doc = JsonDocument.Parse(text);
                var root = doc.RootElement;
                string type = root.GetProperty("metadata").GetProperty("message_type").GetString() ?? "";
                switch (type)
                {
                    case "notification":
                        if (ParseChat(root.GetProperty("payload").GetProperty("event")) is { } msg && Received != null)
                        {
                            try
                            {
                                await Received(msg);
                            }
                            catch (Exception e)
                            {
                                Console.WriteLine($"[twitch] chat handler failed: {e.Message}");
                            }
                        }
                        break;
                    case "session_reconnect":
                    {
                        // Connect to the new URL, wait for its welcome, then drop the old socket.
                        // Subscriptions carry over.
                        string url = root.GetProperty("payload").GetProperty("session").GetProperty("reconnect_url").GetString()!;
                        if (!url.StartsWith("wss://", StringComparison.Ordinal) || !new Uri(url).Host.EndsWith(".twitch.tv", StringComparison.Ordinal))
                            throw new IOException("unexpected reconnect URL");
                        var next = new ClientWebSocket();
                        await next.ConnectAsync(new Uri(url), ct);
                        (_, keepalive) = await ReadWelcome(next, ct);
                        ws.Abort();
                        ws.Dispose();
                        ws = next;
                        break;
                    }
                    case "revocation":
                        throw new InvalidOperationException("Twitch ended the chat subscription (login revoked?). Log in again.");
                }
            }
        }
        finally
        {
            ws.Abort();
            ws.Dispose();
        }
    }

    static async Task<(string sessionId, int keepalive)> ReadWelcome(ClientWebSocket ws, CancellationToken ct)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(TimeSpan.FromSeconds(15));
        string text = await ReadText(ws, timeout.Token) ?? throw new IOException("closed before welcome");
        using var doc = JsonDocument.Parse(text);
        var root = doc.RootElement;
        if (root.GetProperty("metadata").GetProperty("message_type").GetString() != "session_welcome")
            throw new IOException("no welcome message");
        var session = root.GetProperty("payload").GetProperty("session");
        int keepalive = session.TryGetProperty("keepalive_timeout_seconds", out var k) && k.ValueKind == JsonValueKind.Number ? k.GetInt32() : 10;
        return (session.GetProperty("id").GetString()!, keepalive);
    }

    static async Task<string?> ReadText(ClientWebSocket ws, CancellationToken ct)
    {
        var buffer = new byte[16 * 1024];
        using var ms = new MemoryStream();
        while (true)
        {
            var r = await ws.ReceiveAsync(buffer, ct);
            if (r.MessageType == WebSocketMessageType.Close)
                return null;
            ms.Write(buffer, 0, r.Count);
            if (ms.Length > 1024 * 1024)
                throw new IOException("message too large");
            if (r.EndOfMessage)
                return Encoding.UTF8.GetString(ms.GetBuffer(), 0, (int)ms.Length);
        }
    }

    async Task Subscribe(string sessionId, CancellationToken ct)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            var token = await auth.GetToken(forceRefresh: attempt > 0) ?? throw new InvalidOperationException("Not logged in to Twitch.");
            using var msg = new HttpRequestMessage(HttpMethod.Post, "https://api.twitch.tv/helix/eventsub/subscriptions");
            msg.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token.AccessToken);
            msg.Headers.Add("Client-Id", token.ClientId);
            msg.Content = JsonContent(new
            {
                type = "channel.chat.message", version = "1",
                condition = new { broadcaster_user_id = token.UserId, user_id = token.UserId },
                transport = new { method = "websocket", session_id = sessionId },
            });
            using var resp = await http.SendAsync(msg, ct);
            if (resp.IsSuccessStatusCode)
                return;
            if (resp.StatusCode == HttpStatusCode.Unauthorized && attempt == 0)
                continue;
            throw new InvalidOperationException(resp.StatusCode == HttpStatusCode.Forbidden
                ? "Twitch refused the chat subscription (missing permission). Log in again."
                : $"Twitch refused the chat subscription ({(int)resp.StatusCode}).");
        }
    }

    static ChatMessage? ParseChat(JsonElement e)
    {
        string text = e.GetProperty("message").GetProperty("text").GetString() ?? "";
        if (!text.StartsWith('!'))
            return null; // only commands are of interest; nothing else is kept
        // Shared chat: only messages sent in this channel, not ones mirrored from a partner channel.
        if (e.TryGetProperty("source_broadcaster_user_id", out var src) && src.ValueKind == JsonValueKind.String &&
            src.GetString() != e.GetProperty("broadcaster_user_id").GetString())
            return null;
        bool broadcaster = false, moderator = false;
        if (e.TryGetProperty("badges", out var badges) && badges.ValueKind == JsonValueKind.Array)
            foreach (var b in badges.EnumerateArray())
            {
                string set = b.GetProperty("set_id").GetString() ?? "";
                broadcaster |= set == "broadcaster";
                moderator |= set == "moderator";
            }
        string userId = e.GetProperty("chatter_user_id").GetString() ?? "";
        broadcaster |= userId == e.GetProperty("broadcaster_user_id").GetString();
        return new ChatMessage(e.GetProperty("message_id").GetString() ?? "", userId,
                               e.GetProperty("chatter_user_login").GetString() ?? "", e.GetProperty("chatter_user_name").GetString() ?? "",
                               text, broadcaster, moderator);
    }

    async Task SendLoop(CancellationToken stop)
    {
        try
        {
            await foreach (var (text, replyTo, queued) in _outbox.Reader.ReadAllAsync(stop))
            {
                if (DateTime.UtcNow - queued > MaxReplyAge)
                    continue;
                try
                {
                    await Send(text, replyTo, stop);
                }
                catch (Exception e) when (e is HttpRequestException or TaskCanceledException)
                {
                    Console.WriteLine($"[twitch] chat reply not sent: {e.Message}");
                }
                await Task.Delay(SendInterval, stop);
            }
        }
        catch (OperationCanceledException)
        {
        }
    }

    async Task Send(string text, string? replyTo, CancellationToken ct)
    {
        var token = await auth.GetToken();
        if (token == null)
            return;
        using var msg = new HttpRequestMessage(HttpMethod.Post, "https://api.twitch.tv/helix/chat/messages");
        msg.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token.AccessToken);
        msg.Headers.Add("Client-Id", token.ClientId);
        msg.Content = JsonContent(replyTo != null
            ? new { broadcaster_id = token.UserId, sender_id = token.UserId, message = text, reply_parent_message_id = replyTo }
            : (object)new { broadcaster_id = token.UserId, sender_id = token.UserId, message = text });
        using var resp = await http.SendAsync(msg, ct);
        if (!resp.IsSuccessStatusCode)
            Console.WriteLine($"[twitch] chat reply refused ({(int)resp.StatusCode})");
    }

    static StringContent JsonContent(object o) => new(JsonSerializer.Serialize(o), Encoding.UTF8, "application/json");
}
