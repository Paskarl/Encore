// enchor.us client: chart search and .sng downloads, then rescan and queue through Encore.
//
// enchor.us has no published API, terms or limits; this uses the same endpoints as Bridge
// (github.com/Geomitron/Bridge) and is careful with them:
//   - only user-initiated requests: one search per submitted query, one download per tap, no
//     background crawling and no bulk downloads;
//   - at most one search per second and one download at a time;
//   - it honours X-RateLimit-Remaining/Reset and backs off on 429;
//   - a descriptive User-Agent with the project URL.
// Downloads are only allowed for charts that appeared in a search result of this session.
using System.Collections.Concurrent;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

record EnchorChart(string Md5, string Title, string Artist, string Album, string Charter, int? Length, bool HasVideo,
                   string[] Instruments);

sealed class EnchorBusyException(string message, int retryAfterSeconds) : Exception(message)
{
    public int RetryAfterSeconds { get; } = retryAfterSeconds;
}

sealed class EnchorClient
{
    public const string UserAgent = "EncoreCompanion/0.2 (+https://github.com/Paskarl/Encore)";
    const string SearchUrl = "https://api.enchor.us/search";
    const string FilesUrl = "https://files.enchor.us/";
    public const int PerPage = 10;

    readonly HttpClient _http;
    readonly SemaphoreSlim _searchLock = new(1, 1);
    readonly object _gate = new();
    DateTime _blockedUntil = DateTime.MinValue;
    DateTime _lastSearch = DateTime.MinValue;
    readonly ConcurrentDictionary<string, EnchorChart> _seen = new(StringComparer.OrdinalIgnoreCase);

    public EnchorClient(HttpClient http)
    {
        _http = http;
        _http.DefaultRequestHeaders.UserAgent.ParseAdd(UserAgent);
    }

    static readonly Regex Tags = new("<[^>]*>");
    public static string StripTags(string? s) => Tags.Replace(s ?? "", "").Trim();

    public EnchorChart? Seen(string md5) => _seen.TryGetValue(md5, out var c) ? c : null;

    void ThrowIfBlocked()
    {
        lock (_gate)
        {
            var wait = _blockedUntil - DateTime.UtcNow;
            if (wait > TimeSpan.Zero)
                throw new EnchorBusyException("enchor.us asked to slow down", (int)Math.Ceiling(wait.TotalSeconds));
        }
    }

    // Applies enchor.us's rate-limit headers; on 429 waits for the reset (at least 30 s).
    void NoteLimits(HttpResponseMessage resp)
    {
        DateTime? until = null;
        if (resp.StatusCode == HttpStatusCode.TooManyRequests)
        {
            var retry = resp.Headers.RetryAfter;
            until = retry?.Delta != null ? DateTime.UtcNow + retry.Delta.Value
                  : retry?.Date != null ? retry.Date.Value.UtcDateTime
                  : DateTime.UtcNow.AddSeconds(60);
        }
        if (Header(resp, "X-RateLimit-Remaining") is long remaining && remaining <= 0)
            until = Header(resp, "X-RateLimit-Reset") is long reset ? DateTimeOffset.FromUnixTimeSeconds(reset).UtcDateTime
                                                                   : DateTime.UtcNow.AddSeconds(60);
        if (until == null)
            return;
        if (resp.StatusCode == HttpStatusCode.TooManyRequests && until < DateTime.UtcNow.AddSeconds(30))
            until = DateTime.UtcNow.AddSeconds(30);
        lock (_gate)
            if (until > _blockedUntil)
                _blockedUntil = until.Value;
    }

    static long? Header(HttpResponseMessage resp, string name) =>
        resp.Headers.TryGetValues(name, out var v) && long.TryParse(v.FirstOrDefault(), out long n) ? n : null;

    public async Task<(int found, List<EnchorChart> charts)> Search(string query, int page)
    {
        await _searchLock.WaitAsync();
        try
        {
            ThrowIfBlocked();
            var since = DateTime.UtcNow - _lastSearch;
            if (since < TimeSpan.FromSeconds(1))
                await Task.Delay(TimeSpan.FromSeconds(1) - since);
            _lastSearch = DateTime.UtcNow;
            var body = new
            {
                search = query, per_page = PerPage, page, instrument = (string?)null, difficulty = (string?)null,
                drumType = (string?)null, drumsReviewed = false, sort = (string?)null, source = "api",
            };
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
            using var resp = await _http.PostAsync(SearchUrl, new StringContent(JsonSerializer.Serialize(body), Encoding.UTF8, "application/json"),
                                                   timeout.Token);
            NoteLimits(resp);
            if (resp.StatusCode == HttpStatusCode.TooManyRequests)
                ThrowIfBlocked();
            if (!resp.IsSuccessStatusCode)
                throw new HttpRequestException($"enchor.us answered {(int)resp.StatusCode}");
            using var doc = JsonDocument.Parse(await resp.Content.ReadAsStreamAsync());
            var root = doc.RootElement;
            int found = root.TryGetProperty("found", out var f) && f.TryGetInt32(out int fn) ? fn : 0;
            var charts = new List<EnchorChart>();
            foreach (var c in root.GetProperty("data").EnumerateArray())
            {
                string md5 = Str(c, "md5");
                if (!Regex.IsMatch(md5, "^[0-9a-fA-F]{32}$") || charts.Any(x => x.Md5 == md5))
                    continue;
                int? length = c.TryGetProperty("song_length", out var l) && l.TryGetInt64(out long ms) && ms > 0
                    ? (int)((ms + 500) / 1000) : null;
                var instruments = c.TryGetProperty("notesData", out var nd) && nd.TryGetProperty("instruments", out var ins) &&
                                  ins.ValueKind == JsonValueKind.Array
                    ? ins.EnumerateArray().Select(i => i.GetString() ?? "").Where(i => i.Length > 0).ToArray()
                    : Array.Empty<string>();
                var chart = new EnchorChart(md5.ToLowerInvariant(), StripTags(Str(c, "name")), StripTags(Str(c, "artist")),
                    StripTags(Str(c, "album")), StripTags(Str(c, "charter")), length,
                    c.TryGetProperty("hasVideoBackground", out var v) && v.ValueKind == JsonValueKind.True, instruments);
                charts.Add(chart);
                _seen[chart.Md5] = chart;
            }
            return (found, charts);
        }
        finally
        {
            _searchLock.Release();
        }
    }

    static string Str(JsonElement e, string name) =>
        e.TryGetProperty(name, out var p) && p.ValueKind == JsonValueKind.String ? p.GetString() ?? "" : "";

    // Downloads a chart's .sng into path. Charts with a video background are fetched without the
    // video, which Clone Hero doesn't need and which makes the download much larger.
    public async Task Download(EnchorChart chart, string path, Action<long, long?> progress, CancellationToken ct)
    {
        ThrowIfBlocked();
        string url = FilesUrl + chart.Md5 + (chart.HasVideo ? "_novideo" : "") + ".sng";
        using var resp = await _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct);
        NoteLimits(resp);
        if (resp.StatusCode == HttpStatusCode.TooManyRequests)
            ThrowIfBlocked();
        if (resp.StatusCode == HttpStatusCode.NotFound)
            throw new HttpRequestException("enchor.us doesn't have this chart file (anymore)");
        if (!resp.IsSuccessStatusCode)
            throw new HttpRequestException($"enchor.us answered {(int)resp.StatusCode}");
        long? total = resp.Content.Headers.ContentLength;
        const long maxSize = 1L << 30;
        if (total > maxSize)
            throw new InvalidDataException("chart file is larger than 1 GB");
        await using var src = await resp.Content.ReadAsStreamAsync(ct);
        await using var dst = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None);
        byte[] buf = new byte[81920];
        long done = 0;
        int n;
        while ((n = await src.ReadAsync(buf, ct)) > 0)
        {
            done += n;
            if (done > maxSize)
                throw new InvalidDataException("chart file is larger than 1 GB");
            await dst.WriteAsync(buf.AsMemory(0, n), ct);
            progress(done, total);
        }
    }
}

// A download from enchor.us, followed by a rescan and POST /queue. The phone polls its state.
sealed class DownloadJob
{
    public required string Id { get; init; }
    public required EnchorChart Chart { get; init; }
    public string State { get; set; } = "downloading"; // downloading, waiting, scanning, queued, failed
    public string Message { get; set; } = "Downloading…";
    public long Bytes { get; set; }
    public long? Total { get; set; }
    public string? Checksum { get; set; }
    public int? Position { get; set; }
    public DateTime Updated { get; set; } = DateTime.UtcNow;
}

sealed class DownloadManager(EnchorClient enchor, IHttpClientFactory encore, string songsDir)
{
    readonly object _gate = new();
    readonly List<DownloadJob> _jobs = new();
    DownloadJob? _active;
    static readonly TimeSpan WaitLimit = TimeSpan.FromMinutes(30);

    public string DownloadDir { get; } = Path.Combine(songsDir, "Encore Downloads");
    readonly DownloadRecord _record = new();

    // Clone Hero checksum of a chart this companion downloaded before, if its file is still there.
    public string? KnownChecksum(string md5) => _record.Get(md5) is { } e && File.Exists(e.File) ? e.Checksum : null;

    public List<object> Snapshot()
    {
        lock (_gate)
            return _jobs.Select(j => (object)new
            {
                id = j.Id, md5 = j.Chart.Md5, title = j.Chart.Title, artist = j.Chart.Artist, charter = j.Chart.Charter,
                state = j.State, message = j.Message, bytes = j.Bytes, total = j.Total, position = j.Position,
            }).ToList();
    }

    // Starts a job; null (with the reason) if another download is still running.
    public DownloadJob? Start(EnchorChart chart, out string? refusal)
    {
        lock (_gate)
        {
            if (_active != null)
            {
                refusal = $"“{_active.Chart.Title}” is still downloading. One download at a time.";
                return null;
            }
            var job = new DownloadJob { Id = Guid.NewGuid().ToString("N")[..12], Chart = chart };
            _active = job;
            _jobs.Insert(0, job);
            if (_jobs.Count > 10)
                _jobs.RemoveAt(_jobs.Count - 1);
            refusal = null;
            _ = Task.Run(() => Run(job));
            return job;
        }
    }

    void Set(DownloadJob job, string state, string message)
    {
        lock (_gate)
        {
            job.State = state;
            job.Message = message;
            job.Updated = DateTime.UtcNow;
        }
        Console.WriteLine($"[download] {job.Chart.Artist} - {job.Chart.Title} ({job.Chart.Charter}): {state}: {message}");
    }

    async Task Run(DownloadJob job)
    {
        try
        {
            var (checksum, title, artist) = await Fetch(job);
            job.Checksum = checksum;
            lock (_gate)
                if (_active == job)
                    _active = null; // the next download may start while this one waits for a rescan
            await RescanAndQueue(job, checksum, title, artist);
        }
        catch (EnchorBusyException e)
        {
            Set(job, "failed", $"enchor.us asked to slow down. Try again in {e.RetryAfterSeconds} s.");
        }
        catch (Exception e)
        {
            Set(job, "failed", e is HttpRequestException or InvalidDataException or IOException
                ? e.Message : "Download failed: " + e.Message);
        }
        finally
        {
            lock (_gate)
                if (_active == job)
                    _active = null;
        }
    }

    // Downloads the .sng (unless this companion already has it) and returns Clone Hero's checksum.
    async Task<(string checksum, string title, string artist)> Fetch(DownloadJob job)
    {
        var chart = job.Chart;
        if (_record.Get(chart.Md5) is { } known && File.Exists(known.File))
        {
            Set(job, "waiting", "Already downloaded earlier.");
            return (known.Checksum, chart.Title, chart.Artist);
        }
        Directory.CreateDirectory(DownloadDir);
        // The .part extension keeps song-folder watchers (and Clone Hero) away from the partial file.
        string part = Path.Combine(DownloadDir, chart.Md5 + ".sng.part");
        try
        {
            await enchor.Download(chart, part, (done, total) =>
            {
                lock (_gate)
                {
                    job.Bytes = done;
                    job.Total = total;
                }
            }, CancellationToken.None);
            SngInfo info;
            try
            {
                info = Sng.Read(part);
            }
            catch (Exception e) when (e is InvalidDataException or EndOfStreamException or OverflowException)
            {
                throw new InvalidDataException("The downloaded file isn't a valid chart (" + e.Message + ").");
            }
            string title = info.Metadata.GetValueOrDefault("name") is { Length: > 0 } n ? EnchorClient.StripTags(n) : chart.Title;
            string artist = info.Metadata.GetValueOrDefault("artist") is { Length: > 0 } a ? EnchorClient.StripTags(a) : chart.Artist;

            var (_, existing) = await Encore.FindByChecksum(encore, info.Checksum, title, artist);
            if (existing != null)
            {
                File.Delete(part);
                Set(job, "waiting", "This exact chart is already in your library.");
                return (info.Checksum, title, artist);
            }
            string final = FreeName(Path.Combine(DownloadDir, SafeName($"{chart.Artist} - {chart.Title} ({chart.Charter})") + ".sng"));
            File.Move(part, final);
            _record.Add(chart.Md5, info.Checksum, final);
            return (info.Checksum, title, artist);
        }
        finally
        {
            if (File.Exists(part))
                File.Delete(part);
        }
    }

    // Waits until Clone Hero has the song (rescanning when the song list is open), then queues it.
    async Task RescanAndQueue(DownloadJob job, string checksum, string title, string artist)
    {
        var deadline = DateTime.UtcNow + WaitLimit;
        int rescans = 0;
        bool sawScan = false;
        while (DateTime.UtcNow < deadline)
        {
            var (reachable, song) = await Encore.FindByChecksum(encore, checksum, title, artist);
            var status = await Encore.Status(encore);
            if (song != null && !status.scanning)
            {
                var (code, body) = await Encore.Call(encore, HttpMethod.Post, "queue", JsonSerializer.Serialize(new { checksum }));
                if (code == 200)
                {
                    using var doc = JsonDocument.Parse(body);
                    job.Position = doc.RootElement.GetProperty("position").GetInt32();
                    Set(job, "queued", $"Queued at position {job.Position}.");
                    return;
                }
                string result = code == Encore.Offline ? "encore_offline" : Result(body);
                if (result is not ("not_in_song_list" or "loading" or "timeout" or "not_ready" or "encore_offline"))
                {
                    Set(job, "failed", $"Downloaded, but queueing failed ({result}). Queue it from the library search.");
                    return;
                }
                Set(job, "waiting", "Downloaded. Open the song list (or play a setlist) to queue it.");
            }
            else if (!reachable || !status.ok)
                Set(job, "waiting", "Downloaded. Waiting for Clone Hero with Encore.");
            else if (status.scanning)
            {
                sawScan = true;
                Set(job, "scanning", "Clone Hero is scanning songs…");
            }
            else if (song == null && status.songList)
            {
                if (rescans >= 2 && sawScan)
                {
                    Set(job, "failed", "Downloaded, but Clone Hero didn't load the chart after a rescan. Check badsongs.txt in Documents\\Clone Hero.");
                    return;
                }
                var (code, _) = await Encore.Call(encore, HttpMethod.Post, "rescan");
                if (code == 202)
                {
                    ++rescans;
                    sawScan = true;
                    Set(job, "scanning", "Clone Hero is scanning songs…");
                }
            }
            else if (song == null)
                Set(job, "waiting", "Downloaded. Open the song list in Clone Hero to load it.");
            await Task.Delay(TimeSpan.FromSeconds(2));
        }
        Set(job, "failed", "Downloaded, but gave up waiting. The chart shows up after the next song scan.");
    }

    static string Result(string body)
    {
        try
        {
            using var doc = JsonDocument.Parse(body);
            return doc.RootElement.TryGetProperty("result", out var r) ? r.GetString() ?? "error" : "error";
        }
        catch (JsonException)
        {
            return "error";
        }
    }

    static string SafeName(string name)
    {
        var bad = Path.GetInvalidFileNameChars();
        var sb = new StringBuilder();
        foreach (char ch in name)
            sb.Append(bad.Contains(ch) || char.IsControl(ch) ? '_' : ch);
        string s = sb.ToString().Trim().TrimEnd('.');
        if (s.Length > 120)
            s = s[..120].Trim();
        return s.Length == 0 ? "chart" : s;
    }

    static string FreeName(string path)
    {
        string dir = Path.GetDirectoryName(path)!, stem = Path.GetFileNameWithoutExtension(path), ext = Path.GetExtension(path);
        for (int i = 2; File.Exists(path); ++i)
            path = Path.Combine(dir, $"{stem} ({i}){ext}");
        return path;
    }
}

// Remembers which enchor.us charts were downloaded, and their Clone Hero checksums, in
// %LOCALAPPDATA%\Encore\downloads.json, so a chart is never fetched twice.
sealed class DownloadRecord
{
    public record Entry(string Checksum, string File, DateTime Time);

    static string FilePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Encore", "downloads.json");
    readonly object _gate = new();
    readonly Dictionary<string, Entry> _entries;

    public DownloadRecord()
    {
        try
        {
            _entries = JsonSerializer.Deserialize<Dictionary<string, Entry>>(File.ReadAllText(FilePath)) ?? new();
        }
        catch (Exception)
        {
            _entries = new();
        }
    }

    public Entry? Get(string md5)
    {
        lock (_gate)
            return _entries.GetValueOrDefault(md5);
    }

    public void Add(string md5, string checksum, string file)
    {
        lock (_gate)
        {
            _entries[md5] = new Entry(checksum, file, DateTime.UtcNow);
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(_entries, new JsonSerializerOptions { WriteIndented = true }));
        }
    }
}
