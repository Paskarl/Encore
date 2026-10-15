// Stream song requests: chat commands (!sr, !queue, !np, !next, !cancel), the moderation rules,
// and the feeder that moves approved requests into the game's setlist.
//
// Requests go through these states:
//   pending  -> waiting for the streamer (skipped with auto-approve)
//   approved -> waiting to be sent to the game, in round-robin order
//   queued   -> in the game's setlist (POST /queue), at Position
//   playing  -> the setlist is at that song
//   played, rejected, cancelled, failed -> finished
//   missing  -> not in the library; the streamer may search enchor.us for it (never chat)
//   downloading -> the streamer started an enchor.us download for it
//
// The feeder keeps at most FeedAhead approved songs ahead of the current one in the setlist, so
// the order stays editable in the dashboard and the setlist doesn't run out while requests wait.
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

sealed class RequestSettings
{
    public bool Open { get; set; } = true;            // chat may request
    public bool AutoApprove { get; set; }
    public int MaxOpenPerViewer { get; set; } = 1;    // pending/approved/queued requests per viewer
    public int CooldownSeconds { get; set; } = 120;   // between two requests of one viewer
    public int MaxLengthSeconds { get; set; }         // 0 = no limit; songs without a length pass
    public bool RoundRobin { get; set; } = true;      // viewers with fewer played requests first
    public int FeedAhead { get; set; } = 2;           // approved songs kept ahead in the game's setlist
    public bool ModsBypassLimits { get; set; } = true; // broadcaster and moderators skip limit and cooldown
    public List<string> Blocklist { get; set; } = new(); // checksums, or words matched in title/artist
    public string? ClientId { get; set; }             // Twitch application (public client)

    public RequestSettings Clamped()
    {
        MaxOpenPerViewer = Math.Clamp(MaxOpenPerViewer, 1, 10);
        CooldownSeconds = Math.Clamp(CooldownSeconds, 0, 3600);
        MaxLengthSeconds = Math.Clamp(MaxLengthSeconds, 0, 3600);
        FeedAhead = Math.Clamp(FeedAhead, 1, 5);
        Blocklist = Blocklist.Select(b => StreamRequests.CleanText(b, 100)).Where(b => b.Length > 0).Distinct(StringComparer.OrdinalIgnoreCase).Take(500).ToList();
        if (ClientId != null && !Regex.IsMatch(ClientId, "^[a-z0-9]{1,64}$"))
            ClientId = null;
        return this;
    }
}

sealed class SongRequest
{
    public int Id { get; set; }
    public string UserId { get; set; } = "";
    public string Login { get; set; } = "";
    public string Display { get; set; } = "";
    public string Query { get; set; } = "";
    public string? Checksum { get; set; }
    public string Title { get; set; } = "";
    public string Artist { get; set; } = "";
    public int? Length { get; set; }
    public string State { get; set; } = "pending";
    public string? Note { get; set; }
    public int? Position { get; set; }                // 1-based place in the game's setlist
    public string? JobId { get; set; }                // enchor.us download started for it
    public bool Pinned { get; set; }                  // moved to the top by the streamer
    public DateTime Created { get; set; } = DateTime.UtcNow;
    public DateTime Updated { get; set; } = DateTime.UtcNow;

    public static readonly string[] OpenStates = { "pending", "approved", "queued", "missing", "downloading" };
    public bool IsOpen => OpenStates.Contains(State);
    public string Label => Artist.Length > 0 ? $"{Title} – {Artist}" : Title;
}

sealed class StreamRequests
{
    static string FilePath => Path.Combine(AppData.Dir, "twitch-requests.json");
    static readonly TimeSpan ChoiceLifetime = TimeSpan.FromMinutes(2);
    static readonly TimeSpan CommandSpacing = TimeSpan.FromSeconds(3);

    readonly IHttpClientFactory _pick;
    readonly DownloadManager _downloads;
    readonly object _gate = new();
    readonly List<SongRequest> _requests = new();
    readonly Dictionary<string, int> _playedPerUser = new();          // this session, for round robin
    readonly Dictionary<string, DateTime> _lastRequest = new();       // cooldown
    readonly Dictionary<string, DateTime> _lastCommand = new();       // spam guard
    readonly Dictionary<string, (DateTime at, List<Pick.LibrarySong> songs)> _choices = new();
    int _nextId = 1;
    int _mismatchStreak;
    bool _dirty;

    public RequestSettings Settings { get; private set; } = new();
    public SetlistSnapshot? Setlist { get; private set; }
    public string? FeedProblem { get; private set; }

    public sealed record SetlistSong(string Title, string Artist, string Checksum);
    public sealed record SetlistSnapshot(bool Playing, int Index, List<SetlistSong> Songs);

    public StreamRequests(IHttpClientFactory pick, DownloadManager downloads)
    {
        _pick = pick;
        _downloads = downloads;
        try
        {
            if (File.Exists(FilePath))
            {
                var saved = JsonSerializer.Deserialize<SavedState>(File.ReadAllText(FilePath));
                if (saved != null)
                {
                    Settings = (saved.Settings ?? new()).Clamped();
                    _requests.AddRange(saved.Requests ?? new());
                    _nextId = _requests.Count > 0 ? _requests.Max(r => r.Id) + 1 : 1;
                }
            }
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
        }
    }

    sealed class SavedState
    {
        public RequestSettings? Settings { get; set; }
        public List<SongRequest>? Requests { get; set; }
    }

    // --- chat ---------------------------------------------------------------------------------

    // Chat text as Pick uses it: no control or format characters, single spaces, limited length.
    public static string CleanText(string? s, int max)
    {
        var sb = new StringBuilder();
        foreach (char c in s ?? "")
        {
            var cat = char.GetUnicodeCategory(c);
            if (char.IsControl(c) || cat is System.Globalization.UnicodeCategory.Format or System.Globalization.UnicodeCategory.OtherNotAssigned)
                sb.Append(' ');
            else
                sb.Append(c);
        }
        string t = Regex.Replace(sb.ToString(), @"\s+", " ").Trim();
        return t.Length > max ? t[..max].Trim() : t;
    }

    static string Norm(string s) => Regex.Replace(EnchorClient.StripTags(s), @"\s+", " ").Trim();
    static string Short(string s, int max = 60) { s = Norm(s); return s.Length > max ? s[..(max - 1)] + "…" : s; }
    static string SongText(string title, string artist) => artist.Length > 0 ? $"{Short(title)} – {Short(artist, 40)}" : Short(title);

    // Handles one chat message; returns the reply, or null for none.
    public async Task<string?> OnChat(ChatMessage m)
    {
        string text = CleanText(m.Text, 200);
        int space = text.IndexOf(' ');
        string cmd = (space < 0 ? text : text[..space]).ToLowerInvariant();
        string arg = space < 0 ? "" : text[(space + 1)..].Trim();
        if (cmd is not ("!sr" or "!songrequest" or "!queue" or "!songlist" or "!np" or "!song" or "!next" or "!cancel"))
            return null;
        lock (_gate)
        {
            // One command per viewer every few seconds; extra ones are ignored without a reply.
            if (_lastCommand.TryGetValue(m.UserId, out var last) && DateTime.UtcNow - last < CommandSpacing)
                return null;
            _lastCommand[m.UserId] = DateTime.UtcNow;
            if (_lastCommand.Count > 5000)
                _lastCommand.Clear();
        }
        return cmd switch
        {
            "!sr" or "!songrequest" => await SongRequestCommand(m, CleanText(arg, 100)),
            "!queue" or "!songlist" => QueueText(),
            "!np" or "!song" => NowPlayingText(),
            "!next" => NextText(),
            "!cancel" => Cancel(m),
            _ => null,
        };
    }

    async Task<string> SongRequestCommand(ChatMessage m, string q)
    {
        if (!Settings.Open)
            return "Song requests are closed right now.";
        if (q.Length == 0)
            return "Request a song with !sr <title> or !sr <title> <artist>.";

        // "!sr 2": pick from the last list of matches this viewer was shown.
        if (q is "1" or "2" or "3")
        {
            Pick.LibrarySong? chosen = null;
            lock (_gate)
                if (_choices.TryGetValue(m.UserId, out var c) && DateTime.UtcNow - c.at < ChoiceLifetime && int.Parse(q) <= c.songs.Count)
                {
                    chosen = c.songs[int.Parse(q) - 1];
                    _choices.Remove(m.UserId);
                }
            if (chosen == null)
                return "Nothing to choose from. Request with !sr <title>.";
            return Add(m, q, chosen);
        }

        var songs = await Pick.Search(_pick, q, 10);
        if (songs == null)
            return "Clone Hero isn't running right now, try again later.";
        if (songs.Count == 0)
            return AddMissing(m, q);

        string nq = Norm(q).ToLowerInvariant();
        var exact = songs.Where(s =>
        {
            string t = Norm(s.Title).ToLowerInvariant(), a = Norm(s.Artist).ToLowerInvariant();
            return nq == t || nq == $"{t} {a}" || nq == $"{a} {t}" || nq == $"{t} - {a}" || nq == $"{a} - {t}";
        }).ToList();
        if (songs.Count == 1)
            return Add(m, q, songs[0]);
        if (exact.Count == 1)
            return Add(m, q, exact[0]);

        var top = (exact.Count > 1 ? exact : songs).Take(3).ToList();
        lock (_gate)
        {
            _choices[m.UserId] = (DateTime.UtcNow, top);
            if (_choices.Count > 1000)
                _choices.Clear();
        }
        var sb = new StringBuilder("Which one? ");
        for (int i = 0; i < top.Count; ++i)
            sb.Append(i + 1).Append(") ").Append(SongText(top[i].Title, top[i].Artist)).Append(' ');
        sb.Append("– answer with !sr 1").Append(top.Count > 1 ? $"-{top.Count}" : "");
        return sb.ToString();
    }

    // Per-viewer checks shared by library and missing requests. Null when the viewer may request.
    string? ViewerRefusal(ChatMessage m)
    {
        if (m.IsBroadcaster || (m.IsModerator && Settings.ModsBypassLimits))
            return null;
        var open = _requests.Where(r => r.UserId == m.UserId && r.IsOpen).ToList();
        if (open.Count >= Settings.MaxOpenPerViewer)
            return open.Count == 1
                ? $"You already have a request open ({Short(open[0].Label)}). !cancel replaces it."
                : $"You already have {open.Count} requests open.";
        if (_lastRequest.TryGetValue(m.UserId, out var last))
        {
            int wait = Settings.CooldownSeconds - (int)(DateTime.UtcNow - last).TotalSeconds;
            if (wait > 0)
                return $"Please wait {(wait >= 90 ? $"{(wait + 59) / 60} min" : $"{wait} s")} before your next request.";
        }
        return null;
    }

    bool Blocked(string? checksum, string title, string artist)
    {
        string hay = $"{Norm(title)} {Norm(artist)}";
        return Settings.Blocklist.Any(b =>
            (checksum != null && b.Equals(checksum, StringComparison.OrdinalIgnoreCase)) ||
            (!Regex.IsMatch(b, "^[0-9A-Fa-f]{32}$") && hay.Contains(b, StringComparison.OrdinalIgnoreCase)));
    }

    string Add(ChatMessage m, string query, Pick.LibrarySong song)
    {
        lock (_gate)
        {
            if (ViewerRefusal(m) is { } refusal)
                return refusal;
            if (Blocked(song.Checksum, song.Title, song.Artist))
                return $"Sorry, {Short(song.Title)} can't be requested.";
            if (Settings.MaxLengthSeconds > 0 && song.Length > Settings.MaxLengthSeconds)
                return $"{Short(song.Title)} is too long (max {Settings.MaxLengthSeconds / 60}:{Settings.MaxLengthSeconds % 60:D2}).";
            if (_requests.Any(r => r.IsOpen && string.Equals(r.Checksum, song.Checksum, StringComparison.OrdinalIgnoreCase)))
                return $"{Short(song.Title)} is already requested.";
            var req = NewRequest(m, query);
            req.Checksum = song.Checksum.ToUpperInvariant();
            req.Title = Norm(song.Title);
            req.Artist = Norm(song.Artist);
            req.Length = song.Length;
            req.State = Settings.AutoApprove ? "approved" : "pending";
            Save();
            if (req.State == "approved")
            {
                int place = UpcomingLocked().FindIndex(x => x.Id == req.Id) + 1;
                return $"Added {SongText(req.Title, req.Artist)} (#{place} in the queue).";
            }
            return $"Requested {SongText(req.Title, req.Artist)}. The streamer will have a look.";
        }
    }

    string AddMissing(ChatMessage m, string query)
    {
        lock (_gate)
        {
            if (ViewerRefusal(m) is { } refusal)
                return refusal;
            if (Blocked(null, query, ""))
                return "Sorry, that can't be requested.";
            if (_requests.Any(r => r.State == "missing" && r.Query.Equals(query, StringComparison.OrdinalIgnoreCase)))
                return $"“{Short(query)}” is already on the streamer's list.";
            var req = NewRequest(m, query);
            req.Title = query;
            req.State = "missing";
            Save();
            return $"“{Short(query)}” isn't in the library. I passed it on to the streamer.";
        }
    }

    SongRequest NewRequest(ChatMessage m, string query)
    {
        var req = new SongRequest
        {
            Id = _nextId++, UserId = m.UserId, Login = m.Login, Display = CleanText(m.DisplayName, 40), Query = query,
        };
        _requests.Add(req);
        _lastRequest[m.UserId] = DateTime.UtcNow;
        return req;
    }

    string Cancel(ChatMessage m)
    {
        lock (_gate)
        {
            var mine = _requests.Where(r => r.UserId == m.UserId && r.IsOpen).OrderByDescending(r => r.Created).ToList();
            var cancellable = mine.FirstOrDefault(r => r.State is "pending" or "approved" or "missing");
            if (cancellable != null)
            {
                SetState(cancellable, "cancelled", "cancelled in chat");
                _lastRequest.Remove(m.UserId); // a replacement may follow right away
                Save();
                return $"Cancelled your request for {Short(cancellable.Label)}.";
            }
            if (mine.Count > 0)
                return $"{Short(mine[0].Label)} is already in the game's setlist; ask the streamer.";
            return "You have no open request.";
        }
    }

    // --- queue order and texts ---------------------------------------------------------------

    // Upcoming requests: those already in the game's setlist (by position), then approved ones in
    // the order the feeder will send them.
    List<SongRequest> UpcomingLocked()
    {
        var inSetlist = _requests.Where(r => r.State == "queued").OrderBy(r => r.Position ?? int.MaxValue);
        var approved = _requests.Where(r => r.State == "approved")
            .OrderByDescending(r => r.Pinned)
            .ThenBy(r => Settings.RoundRobin ? _playedPerUser.GetValueOrDefault(r.UserId) + CountAhead(r) : 0)
            .ThenBy(r => r.Created);
        return inSetlist.Concat(approved).ToList();
    }

    // Round robin: a viewer's earlier approved or queued requests count as "already had a turn".
    int CountAhead(SongRequest r) =>
        _requests.Count(o => o.UserId == r.UserId && o.Id != r.Id && o.Created < r.Created && o.State is "approved" or "queued");

    SongRequest? PlayingLocked() => _requests.FirstOrDefault(r => r.State == "playing");

    string QueueText()
    {
        lock (_gate)
        {
            var up = UpcomingLocked();
            int pending = _requests.Count(r => r.State == "pending");
            if (up.Count == 0)
                return pending > 0 ? $"Nothing approved yet · {pending} waiting for approval."
                                   : "The request queue is empty. Request with !sr <title>.";
            var sb = new StringBuilder("Up next: ");
            foreach (var (r, i) in up.Take(3).Select((r, i) => (r, i)))
                sb.Append(i + 1).Append(") ").Append(Short(r.Title, 40)).Append(" (").Append(r.Display).Append(") ");
            if (up.Count > 3)
                sb.Append($"+{up.Count - 3} more");
            if (pending > 0)
                sb.Append($" · {pending} waiting for approval");
            return sb.ToString().Trim();
        }
    }

    string NowPlayingText()
    {
        lock (_gate)
        {
            var s = Setlist;
            if (s == null || !s.Playing || s.Index < 0 || s.Index >= s.Songs.Count)
                return "No setlist is playing right now.";
            var song = s.Songs[s.Index];
            var req = PlayingLocked();
            return $"Now playing: {SongText(song.Title, song.Artist)}" + (req != null ? $" (requested by {req.Display})" : "");
        }
    }

    string NextText()
    {
        lock (_gate)
        {
            var s = Setlist;
            if (s != null && s.Playing && s.Index + 1 < s.Songs.Count && s.Index >= 0)
            {
                var song = s.Songs[s.Index + 1];
                var req = _requests.FirstOrDefault(r => r.State == "queued" && r.Position == s.Index + 2);
                return $"Next: {SongText(song.Title, song.Artist)}" + (req != null ? $" (requested by {req.Display})" : "");
            }
            var up = UpcomingLocked().FirstOrDefault();
            return up != null ? $"Next request: {SongText(up.Title, up.Artist)} (requested by {up.Display})" : "Nothing queued yet.";
        }
    }

    // --- dashboard -----------------------------------------------------------------------------

    public object Snapshot()
    {
        lock (_gate)
        {
            var upcoming = UpcomingLocked();
            static object View(SongRequest r) => new
            {
                id = r.Id, user = r.Display, login = r.Login, query = r.Query, checksum = r.Checksum, title = r.Title,
                artist = r.Artist, length = r.Length, state = r.State, note = r.Note, position = r.Position, pinned = r.Pinned,
                jobId = r.JobId, ageSeconds = (int)(DateTime.UtcNow - r.Created).TotalSeconds,
            };
            return new
            {
                settings = Settings,
                setlist = Setlist,
                feedProblem = FeedProblem,
                pending = _requests.Where(r => r.State == "pending").OrderBy(r => r.Created).Select(View).ToList(),
                missing = _requests.Where(r => r.State is "missing" or "downloading").OrderBy(r => r.Created).Select(View).ToList(),
                playing = PlayingLocked() is { } p ? View(p) : null,
                upcoming = upcoming.Select(View).ToList(),
                history = _requests.Where(r => !r.IsOpen && r.State != "playing").OrderByDescending(r => r.Updated).Take(30).Select(View).ToList(),
            };
        }
    }

    // Overlay data: what is on screen for viewers. No ids or checksums.
    public object OverlaySnapshot()
    {
        lock (_gate)
        {
            var s = Setlist;
            object? now = null;
            var next = new List<object>();
            if (s != null && s.Playing && s.Index >= 0 && s.Index < s.Songs.Count)
            {
                var song = s.Songs[s.Index];
                now = new { title = Norm(song.Title), artist = Norm(song.Artist), requestedBy = PlayingLocked()?.Display };
                for (int i = s.Index + 1; i < s.Songs.Count; ++i)
                {
                    var req = _requests.FirstOrDefault(r => r.State == "queued" && r.Position == i + 1);
                    next.Add(new { title = Norm(s.Songs[i].Title), artist = Norm(s.Songs[i].Artist), requestedBy = req?.Display });
                }
            }
            else
            {
                foreach (var r in _requests.Where(r => r.State == "queued").OrderBy(r => r.Position))
                    next.Add(new { title = r.Title, artist = r.Artist, requestedBy = (string?)r.Display });
            }
            foreach (var r in UpcomingLocked().Where(r => r.State == "approved"))
                next.Add(new { title = r.Title, artist = r.Artist, requestedBy = (string?)r.Display });
            return new
            {
                playing = s?.Playing ?? false,
                setlistPosition = s != null && s.Playing ? s.Index + 1 : (int?)null,
                setlistCount = s?.Songs.Count,
                now,
                next,
                pending = _requests.Count(r => r.State == "pending"),
                open = Settings.Open,
            };
        }
    }

    public string? UpdateSettings(RequestSettings incoming)
    {
        lock (_gate)
        {
            Settings = incoming.Clamped();
            Save();
            return null;
        }
    }

    public string? Act(int id, string action, string? jobId)
    {
        lock (_gate)
        {
            var r = _requests.FirstOrDefault(x => x.Id == id);
            if (r == null)
                return "no such request";
            switch (action)
            {
                case "approve" when r.State is "pending" or "rejected" or "failed" or "cancelled" && r.Checksum != null:
                    SetState(r, "approved", null);
                    break;
                case "reject" when r.State is "pending" or "approved" or "missing" or "downloading":
                    SetState(r, "rejected", "rejected by the streamer");
                    break;
                case "remove" when r.State != "playing":
                    _requests.Remove(r);
                    break;
                case "top" when r.State == "approved":
                    foreach (var o in _requests)
                        o.Pinned = false;
                    r.Pinned = true;
                    break;
                case "download" when r.State == "missing" && jobId != null && Regex.IsMatch(jobId, "^[0-9a-f]{12}$"):
                    r.JobId = jobId;
                    SetState(r, "downloading", "downloading from enchor.us");
                    break;
                case "played" when r.State is "queued" or "playing":
                    SetState(r, "played", "marked played by the streamer");
                    break;
                default:
                    return $"can't {action} a request that is {r.State}";
            }
            Save();
            return null;
        }
    }

    void SetState(SongRequest r, string state, string? note)
    {
        r.State = state;
        r.Note = note;
        r.Updated = DateTime.UtcNow;
        if (state != "queued" && state != "playing")
            r.Position = null;
        if (state != "approved")
            r.Pinned = false;
        _dirty = true;
    }

    // --- feeder ----------------------------------------------------------------------------------

    public async Task FeedLoop(CancellationToken stop)
    {
        while (!stop.IsCancellationRequested)
        {
            try
            {
                await Tick();
            }
            catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException)
            {
                FeedProblem = "Unexpected answer from Pick.";
            }
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(2), stop);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    async Task Tick()
    {
        TrackDownloads();
        var (status, body) = await Pick.Call(_pick, HttpMethod.Get, "setlist");
        if (status != 200)
        {
            Setlist = null;
            FeedProblem = status == Pick.Offline ? "Clone Hero with Pick isn't running." : "Pick isn't ready.";
            return;
        }
        var snap = ParseSetlist(body);
        var (ok, scanning, _) = await Pick.Status(_pick);
        SongRequest? next;
        lock (_gate)
        {
            Setlist = snap;
            if (ok && !scanning) // during a scan the setlist is empty until Pick restores it
                Reconcile(snap);
            int ahead = snap.Playing ? snap.Songs.Count - 1 - Math.Max(snap.Index, 0) : snap.Songs.Count;
            next = ahead < Settings.FeedAhead && !scanning ? UpcomingLocked().FirstOrDefault(r => r.State == "approved") : null;
            if (next == null)
                FeedProblem = null;
        }
        if (next != null)
            await Feed(next);
        lock (_gate)
            if (_dirty)
                Save();
    }

    // Follows the setlist: a queued request becomes playing when the setlist reaches its position,
    // and played after that. If its slot no longer holds its song (the setlist was cleared or
    // changed in the game) for two polls in a row, it goes back to approved and is sent again.
    void Reconcile(SetlistSnapshot s)
    {
        bool mismatch = false;
        foreach (var r in _requests.Where(r => r.State is "queued" or "playing" && r.Position != null).ToList())
        {
            int slot = r.Position!.Value - 1;
            bool holds = slot < s.Songs.Count && string.Equals(s.Songs[slot].Checksum, r.Checksum, StringComparison.OrdinalIgnoreCase);
            if (!holds)
            {
                mismatch = true;
                if (_mismatchStreak >= 1)
                {
                    if (r.State == "playing")
                        MarkPlayed(r);
                    else
                        SetState(r, "approved", "was no longer in the game's setlist; sending it again");
                }
                continue;
            }
            if (!s.Playing)
                continue;
            if (s.Index == slot && r.State != "playing")
                SetState(r, "playing", null);
            else if (s.Index > slot)
                MarkPlayed(r);
        }
        _mismatchStreak = mismatch ? _mismatchStreak + 1 : 0;
    }

    void MarkPlayed(SongRequest r)
    {
        SetState(r, "played", null);
        _playedPerUser[r.UserId] = _playedPerUser.GetValueOrDefault(r.UserId) + 1;
    }

    async Task Feed(SongRequest r)
    {
        var (status, body) = await Pick.Call(_pick, HttpMethod.Post, "queue", JsonSerializer.Serialize(new { checksum = r.Checksum }));
        string result = "";
        int? position = null;
        try
        {
            using var doc = JsonDocument.Parse(body);
            result = doc.RootElement.TryGetProperty("result", out var res) ? res.GetString() ?? "" : "";
            if (doc.RootElement.TryGetProperty("position", out var p) && p.ValueKind == JsonValueKind.Number)
                position = p.GetInt32();
        }
        catch (JsonException)
        {
        }
        lock (_gate)
        {
            if (r.State != "approved")
                return; // changed in the dashboard meanwhile
            if (status == 200 && position != null)
            {
                SetState(r, "queued", null);
                r.Position = position;
                FeedProblem = null;
                Console.WriteLine($"[twitch] {r.Display}'s request {r.Label} -> setlist position {position}");
            }
            else if (status == 404)
                SetState(r, "failed", "not in the library any more (rescan?)");
            else
                FeedProblem = result switch
                {
                    "quickplay" => "A single song is playing. Requests go into a setlist: start one from the song list (see README).",
                    "not_in_song_list" => "Open the song list or start a setlist so requests can be added.",
                    "practice" => "Practice mode has no setlist.",
                    "loading" => "The next song is loading…",
                    _ => status == Pick.Offline ? "Clone Hero with Pick isn't running." : $"Pick answered {result} ({status}).",
                };
        }
    }

    // enchor.us downloads the streamer started for a missing request: the download queues the
    // song itself once Clone Hero has it.
    void TrackDownloads()
    {
        lock (_gate)
            foreach (var r in _requests.Where(r => r.State == "downloading" && r.JobId != null).ToList())
            {
                var job = _downloads.Job(r.JobId!);
                if (job == null)
                    SetState(r, "missing", "download not found");
                else if (job.State == "queued" && job.Position != null)
                {
                    r.Checksum = job.Checksum?.ToUpperInvariant();
                    r.Title = Norm(job.Chart.Title);
                    r.Artist = Norm(job.Chart.Artist);
                    SetState(r, "queued", null);
                    r.Position = job.Position;
                }
                else if (job.State == "failed")
                    SetState(r, "missing", job.Message);
            }
    }

    static SetlistSnapshot ParseSetlist(string body)
    {
        using var doc = JsonDocument.Parse(body);
        var r = doc.RootElement;
        var songs = r.GetProperty("songs").EnumerateArray().Select(s => new SetlistSong(
            s.GetProperty("title").GetString() ?? "", s.GetProperty("artist").GetString() ?? "",
            s.GetProperty("checksum").GetString() ?? "")).ToList();
        return new SetlistSnapshot(r.GetProperty("playing").GetBoolean(), r.GetProperty("index").GetInt32(), songs);
    }

    void Save()
    {
        _dirty = false;
        // Keep open requests and the latest 200 finished ones.
        var finished = _requests.Where(r => !r.IsOpen && r.State != "playing").OrderByDescending(r => r.Updated).Skip(200).ToHashSet();
        _requests.RemoveAll(finished.Contains);
        try
        {
            Directory.CreateDirectory(AppData.Dir);
            string tmp = FilePath + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(new SavedState { Settings = Settings, Requests = _requests }));
            File.Move(tmp, FilePath, overwrite: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Console.WriteLine($"[twitch] can't save requests: {e.Message}");
        }
    }
}
