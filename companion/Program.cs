// Pick companion: a small web server on the PC that lets phones on the local network search the
// Clone Hero library and queue songs. It forwards a fixed set of requests to Pick's loopback
// API (127.0.0.1:47321) and never exposes Pick itself.
//
// Security model:
//   - Every /api request must carry the pairing token (Authorization: Bearer <token>). The token
//     is random, stored in %LOCALAPPDATA%\Pick\companion-token.txt and shown on the PC only
//     (console QR code, and the /pair page that answers to the PC itself).
//   - Short pairing codes: Pick's "Pick" main-menu entry asks for a 6-digit code over loopback
//     (POST /pair/code) and shows it in the game; the phone trades it for the token
//     (POST /pair/claim). One code at a time, valid for 10 minutes, void after 5 wrong guesses.
//   - Requests from addresses outside private networks (loopback, RFC 1918, link-local, IPv6 ULA)
//     are refused, so a port forward does not expose the companion to the internet.
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using QRCoder;

var options = Options.Parse(args);
if (options == null)
{
    Console.Error.WriteLine("usage: PickCompanion [--port 47322] [--pick-port 47321] [--songs-dir <folder>] [--new-token] [--no-browser]");
    return 2;
}
Console.OutputEncoding = Encoding.UTF8;

bool tokenIsNew;
string token = Pairing.LoadOrCreateToken(options.NewToken, out tokenIsNew);
byte[] tokenBytes = Encoding.UTF8.GetBytes(token);

var builder = WebApplication.CreateBuilder();
builder.Logging.SetMinimumLevel(LogLevel.Warning);
builder.WebHost.ConfigureKestrel(k =>
{
    k.ListenAnyIP(options.Port);
    k.Limits.MaxRequestBodySize = 16 * 1024; // the largest valid request is a short JSON object
});
builder.Services.AddHttpClient("pick", c =>
{
    c.BaseAddress = new Uri($"http://127.0.0.1:{options.PickPort}/");
    c.Timeout = TimeSpan.FromSeconds(6);
});
string songsDir = options.SongsDir ?? SongsFolder.Find();
builder.Services.AddSingleton(new EnchorClient(new HttpClient { Timeout = TimeSpan.FromMinutes(10) }));
builder.Services.AddSingleton(sp => new DownloadManager(sp.GetRequiredService<EnchorClient>(),
                                                        sp.GetRequiredService<IHttpClientFactory>(), songsDir));
var app = builder.Build();

app.Use(async (ctx, next) =>
{
    if (!Network.IsPrivate(ctx.Connection.RemoteIpAddress))
    {
        ctx.Response.StatusCode = StatusCodes.Status403Forbidden;
        return;
    }
    var h = ctx.Response.Headers;
    h["X-Content-Type-Options"] = "nosniff";
    h["Referrer-Policy"] = "no-referrer";
    h["Cache-Control"] = "no-store";
    h["Content-Security-Policy"] =
        "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' data:; " +
        "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'";
    if (ctx.Request.Path.StartsWithSegments("/api"))
    {
        string auth = ctx.Request.Headers.Authorization.ToString();
        byte[] given = Encoding.UTF8.GetBytes(auth.StartsWith("Bearer ", StringComparison.Ordinal) ? auth[7..] : "");
        if (!CryptographicOperations.FixedTimeEquals(given, tokenBytes))
        {
            ctx.Response.StatusCode = StatusCodes.Status401Unauthorized;
            await ctx.Response.WriteAsJsonAsync(new { result = "unpaired", message = "pair this device: scan the QR code shown on the PC" });
            return;
        }
    }
    await next();
});

string indexHtml;
using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("index.html")!)
using (var r = new StreamReader(s))
    indexHtml = r.ReadToEnd();

app.MapGet("/", () => Results.Content(indexHtml, "text/html; charset=utf-8"));

// The pairing page shows the token, so it only answers to a browser on the PC itself. The Host
// check stops a web page from reading it through DNS rebinding.
app.MapGet("/pair", (HttpContext ctx) =>
{
    if (!Network.IsFromThisPc(ctx))
        return Results.StatusCode(StatusCodes.Status403Forbidden);
    return Results.Content(Pairing.PageHtml(Network.PairUrls(options.Port, token)), "text/html; charset=utf-8");
});

// A new short pairing code, for the game's main menu. Same loopback and Host checks as /pair.
var pairCodes = new PairCodes();
app.MapPost("/pair/code", (HttpContext ctx) =>
{
    if (!Network.IsFromThisPc(ctx))
        return Results.StatusCode(StatusCodes.Status403Forbidden);
    string code = pairCodes.Issue();
    return Results.Json(new { code, expiresIn = (int)PairCodes.Lifetime.TotalSeconds, urls = Network.BaseUrls(options.Port) });
});
// The phone trades the code for the token.
app.MapPost("/pair/claim", async (HttpRequest req) =>
{
    var body = await RequestJson.Read<PairClaim>(req);
    if (!pairCodes.Claim(body?.Code))
        return Results.Json(new { result = "bad_code", message = "Wrong or expired code. Choose Pick in the game's main menu for a new one." },
                            statusCode: StatusCodes.Status403Forbidden);
    return Results.Json(new { result = "paired", token });
});

var checksumPattern = new Regex("^[0-9A-Fa-f]{32}$");

app.MapGet("/api/status", (IHttpClientFactory f) => Pick.Forward(f, HttpMethod.Get, "status"));
app.MapGet("/api/setlist", (IHttpClientFactory f) => Pick.Forward(f, HttpMethod.Get, "setlist"));
app.MapPost("/api/rescan", (IHttpClientFactory f) => Pick.Forward(f, HttpMethod.Post, "rescan"));
app.MapGet("/api/library", (IHttpClientFactory f, string? q, int? limit) =>
{
    q = (q ?? "").Trim();
    if (q.Length > 200)
        return Task.FromResult(Results.BadRequest(new { result = "bad_request", message = "search text too long" }));
    int n = Math.Clamp(limit ?? 50, 1, 200);
    return Pick.Forward(f, HttpMethod.Get, $"library?q={Uri.EscapeDataString(q)}&limit={n}");
});
app.MapPost("/api/queue", async (IHttpClientFactory f, HttpRequest req) =>
{
    var body = await RequestJson.Read<QueueRequest>(req);
    if (body?.Checksum == null || !checksumPattern.IsMatch(body.Checksum))
        return Results.BadRequest(new { result = "bad_request", message = "give the song's checksum" });
    return await Pick.Forward(f, HttpMethod.Post, "queue", JsonSerializer.Serialize(new { checksum = body.Checksum }));
});

// --- enchor.us (step 2): only on explicit requests from the phone, see Enchor.cs ---

app.MapPost("/api/enchor/search", async (HttpRequest req, EnchorClient enchor, DownloadManager downloads, IHttpClientFactory f) =>
{
    var body = await RequestJson.Read<EnchorSearchRequest>(req);
    string q = (body?.Q ?? "").Trim();
    int page = body?.Page ?? 1;
    if (q.Length is < 1 or > 100 || page is < 1 or > 50)
        return Results.BadRequest(new { result = "bad_request", message = "give a search text (1–100 characters)" });
    int found;
    List<EnchorChart> charts;
    try
    {
        (found, charts) = await enchor.Search(q, page);
    }
    catch (EnchorBusyException e)
    {
        return Results.Json(new { result = "enchor_busy", message = $"enchor.us asked to slow down. Try again in {e.RetryAfterSeconds} s.", retryAfter = e.RetryAfterSeconds },
                            statusCode: StatusCodes.Status429TooManyRequests);
    }
    catch (Exception e) when (e is HttpRequestException or TaskCanceledException or JsonException or KeyNotFoundException or InvalidOperationException)
    {
        return Results.Json(new { result = "enchor_error", message = "enchor.us didn't answer properly. Try again later." },
                            statusCode: StatusCodes.Status502BadGateway);
    }

    // Mark charts the library already has. enchor.us's md5 is not Clone Hero's checksum, so match
    // a chart downloaded here by its recorded checksum, anything else by title and artist.
    bool libraryChecked = true;
    var library = new Dictionary<string, List<Pick.LibrarySong>>(StringComparer.OrdinalIgnoreCase);
    foreach (string title in charts.Select(c => c.Title).Where(t => t.Length > 0).Distinct(StringComparer.OrdinalIgnoreCase))
    {
        var songs = await Pick.Search(f, title, 50);
        if (songs == null)
        {
            libraryChecked = false;
            break;
        }
        library[title] = songs;
    }
    static string Norm(string s) => Regex.Replace(EnchorClient.StripTags(s), @"\s+", " ").Trim();
    var result = charts.Select(c =>
    {
        var candidates = library.GetValueOrDefault(c.Title) ?? new();
        string? known = downloads.KnownChecksum(c.Md5);
        var exact = known == null ? null : candidates.FirstOrDefault(s => s.Checksum.Equals(known, StringComparison.OrdinalIgnoreCase));
        var same = exact ?? candidates.FirstOrDefault(s => Norm(s.Title).Equals(Norm(c.Title), StringComparison.OrdinalIgnoreCase) &&
                                                           Norm(s.Artist).Equals(Norm(c.Artist), StringComparison.OrdinalIgnoreCase));
        return new
        {
            md5 = c.Md5, title = c.Title, artist = c.Artist, album = c.Album, charter = c.Charter, length = c.Length,
            instruments = c.Instruments,
            inLibrary = same == null ? null : new { checksum = same.Checksum, exact = exact != null },
        };
    }).ToList();
    return Results.Json(new { found, page, perPage = EnchorClient.PerPage, libraryChecked, charts = result });
});

app.MapPost("/api/enchor/download", async (HttpRequest req, EnchorClient enchor, DownloadManager downloads) =>
{
    var body = await RequestJson.Read<QueueRequest>(req);
    string md5 = body?.Md5 ?? "";
    if (!checksumPattern.IsMatch(md5))
        return Results.BadRequest(new { result = "bad_request", message = "give the chart's md5" });
    // Only charts from a search result: the phone can't make the companion fetch arbitrary files.
    var chart = enchor.Seen(md5);
    if (chart == null)
        return Results.BadRequest(new { result = "unknown_chart", message = "search enchor.us for the chart first" });
    var job = downloads.Start(chart, out string? refusal);
    if (job == null)
        return Results.Json(new { result = "busy", message = refusal }, statusCode: StatusCodes.Status409Conflict);
    return Results.Json(new { result = "started", id = job.Id }, statusCode: StatusCodes.Status202Accepted);
});

app.MapGet("/api/downloads", (DownloadManager downloads) => Results.Json(new { jobs = downloads.Snapshot() }));

app.Lifetime.ApplicationStarted.Register(() =>
{
    var urls = Network.PairUrls(options.Port, token);
    Console.WriteLine();
    Console.WriteLine("Pick companion is running. Scan the QR code with your phone (same Wi-Fi):");
    Console.WriteLine();
    if (urls.Count > 0)
    {
        Console.WriteLine(new AsciiQRCode(new QRCodeGenerator().CreateQrCode(urls[0], QRCodeGenerator.ECCLevel.L))
            .GetGraphicSmall(drawQuietZones: true, invert: false)); // blocks = light modules, for dark consoles
        foreach (var u in urls)
            Console.WriteLine("  " + u);
    }
    else
        Console.WriteLine("  No network address found. Is the PC connected to the network?");
    Console.WriteLine();
    Console.WriteLine($"If the QR code above doesn't scan, open http://localhost:{options.Port}/pair on this PC.");
    Console.WriteLine($"Talking to Pick on http://127.0.0.1:{options.PickPort}. Stop with Ctrl+C.");
    Console.WriteLine($"Charts from enchor.us are saved to {app.Services.GetRequiredService<DownloadManager>().DownloadDir}");
    if (tokenIsNew && !options.NoBrowser)
        Pairing.OpenInBrowser($"http://localhost:{options.Port}/pair");
});

app.Run();
return 0;

static class RequestJson
{
    // The request's JSON body, or null if it is missing, too large or not JSON (a wrong content
    // type included, which ReadFromJsonAsync reports as InvalidOperationException).
    public static async Task<T?> Read<T>(HttpRequest req) where T : class
    {
        try
        {
            return await req.ReadFromJsonAsync<T>();
        }
        catch (Exception e) when (e is JsonException or InvalidOperationException or BadHttpRequestException)
        {
            return null;
        }
    }
}

record QueueRequest(string? Checksum, string? Md5 = null);
record EnchorSearchRequest(string? Q, int? Page);
record PairClaim(string? Code);

sealed class PairCodes
{
    public static readonly TimeSpan Lifetime = TimeSpan.FromMinutes(10);
    const int MaxWrongGuesses = 5;
    readonly object gate = new();
    string? code;
    DateTime expires;
    int wrong;

    // Replaces the previous code.
    public string Issue()
    {
        lock (gate)
        {
            code = RandomNumberGenerator.GetInt32(0, 1_000_000).ToString("D6");
            expires = DateTime.UtcNow + Lifetime;
            wrong = 0;
            return code;
        }
    }

    // True once for the current code; wrong guesses count against it.
    public bool Claim(string? given)
    {
        given = new string((given ?? "").Where(char.IsAsciiDigit).ToArray());
        lock (gate)
        {
            if (code == null || DateTime.UtcNow > expires)
                return false;
            if (given.Length != 6 ||
                !CryptographicOperations.FixedTimeEquals(Encoding.ASCII.GetBytes(given), Encoding.ASCII.GetBytes(code)))
            {
                if (++wrong >= MaxWrongGuesses)
                    code = null;
                return false;
            }
            code = null;
            return true;
        }
    }
}

// Clone Hero's first song folder: [directories] path0 in Documents\Clone Hero\settings.ini, or
// the default Documents\Clone Hero\Songs when that is empty.
static class SongsFolder
{
    public static string Find()
    {
        string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Clone Hero");
        try
        {
            bool inDirectories = false;
            foreach (string raw in File.ReadLines(Path.Combine(root, "settings.ini")))
            {
                string line = raw.Trim();
                if (line.StartsWith('['))
                    inDirectories = line.Equals("[directories]", StringComparison.OrdinalIgnoreCase);
                else if (inDirectories && line.StartsWith("path", StringComparison.OrdinalIgnoreCase) && line.Contains('='))
                {
                    string value = line[(line.IndexOf('=') + 1)..].Trim();
                    if (value.Length > 0 && Directory.Exists(value))
                        return value;
                }
            }
        }
        catch (IOException)
        {
        }
        return Path.Combine(root, "Songs");
    }
}

sealed class Options
{
    public int Port = 47322;
    public int PickPort = 47321;
    public bool NewToken;
    public bool NoBrowser;
    public string? SongsDir;

    public static Options? Parse(string[] args)
    {
        var o = new Options();
        if (int.TryParse(Environment.GetEnvironmentVariable("PICK_PORT"), out int envPort) && envPort > 0)
            o.PickPort = envPort;
        for (int i = 0; i < args.Length; ++i)
        {
            switch (args[i])
            {
                case "--port" when i + 1 < args.Length && int.TryParse(args[i + 1], out int p) && p is > 0 and < 65536:
                    o.Port = p;
                    ++i;
                    break;
                case "--pick-port" when i + 1 < args.Length && int.TryParse(args[i + 1], out int e) && e is > 0 and < 65536:
                    o.PickPort = e;
                    ++i;
                    break;
                case "--songs-dir" when i + 1 < args.Length && Directory.Exists(args[i + 1]):
                    o.SongsDir = args[++i];
                    break;
                case "--new-token":
                    o.NewToken = true;
                    break;
                case "--no-browser":
                    o.NoBrowser = true;
                    break;
                default:
                    return null;
            }
        }
        return o;
    }
}

static class Pick
{
    public const int Offline = 0;

    // Sends a request to Pick. Status 0 (Offline) means Pick did not answer at all.
    public static async Task<(int status, string body)> Call(IHttpClientFactory factory, HttpMethod method, string path, string? json = null)
    {
        var client = factory.CreateClient("pick");
        using var msg = new HttpRequestMessage(method, path);
        if (json != null)
            msg.Content = new StringContent(json, Encoding.UTF8, "application/json");
        try
        {
            using var resp = await client.SendAsync(msg);
            return ((int)resp.StatusCode, await resp.Content.ReadAsStringAsync());
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException)
        {
            return (Offline, "");
        }
    }

    // Forwards a request to Pick and passes its status code and JSON answer through.
    public static async Task<IResult> Forward(IHttpClientFactory factory, HttpMethod method, string path, string? json = null)
    {
        var (status, body) = await Call(factory, method, path, json);
        if (status == Offline)
            return Results.Json(new { result = "pick_offline", message = "Clone Hero with Pick is not running on the PC" },
                                statusCode: StatusCodes.Status502BadGateway);
        return Results.Content(body, "application/json; charset=utf-8", Encoding.UTF8, status);
    }

    public record LibrarySong(string Title, string Artist, string Album, string Checksum);

    // Library songs matching a search text (GET /library); null if Pick is not reachable or not ready.
    public static async Task<List<LibrarySong>?> Search(IHttpClientFactory factory, string q, int limit = 200)
    {
        var (status, body) = await Call(factory, HttpMethod.Get, $"library?q={Uri.EscapeDataString(q)}&limit={limit}");
        if (status != 200)
            return null;
        using var doc = JsonDocument.Parse(body);
        return doc.RootElement.GetProperty("songs").EnumerateArray().Select(s => new LibrarySong(
            s.GetProperty("title").GetString() ?? "", s.GetProperty("artist").GetString() ?? "",
            s.GetProperty("album").GetString() ?? "", s.GetProperty("checksum").GetString() ?? "")).ToList();
    }

    // Finds a library song by its checksum. /library searches text only, so look it up by title
    // and, failing that, by artist.
    public static async Task<(bool reachable, LibrarySong? song)> FindByChecksum(IHttpClientFactory factory, string checksum, string title, string artist)
    {
        bool reachable = false;
        foreach (string q in new[] { title, artist }.Where(s => s.Trim().Length > 0).Distinct())
        {
            var songs = await Search(factory, q);
            if (songs == null)
                continue;
            reachable = true;
            var hit = songs.FirstOrDefault(s => string.Equals(s.Checksum, checksum, StringComparison.OrdinalIgnoreCase));
            if (hit != null)
                return (true, hit);
        }
        return (reachable, null);
    }

    public static async Task<(bool ok, bool scanning, bool songList)> Status(IHttpClientFactory factory)
    {
        var (status, body) = await Call(factory, HttpMethod.Get, "status");
        if (status != 200)
            return (false, false, false);
        using var doc = JsonDocument.Parse(body);
        var r = doc.RootElement;
        return (true, r.GetProperty("scanning").GetBoolean(), r.GetProperty("songList").GetBoolean());
    }
}

static class Network
{
    public static bool IsPrivate(IPAddress? ip)
    {
        if (ip == null)
            return false;
        if (ip.IsIPv4MappedToIPv6)
            ip = ip.MapToIPv4();
        if (IPAddress.IsLoopback(ip))
            return true;
        if (ip.AddressFamily == AddressFamily.InterNetwork)
        {
            byte[] b = ip.GetAddressBytes();
            return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168) ||
                   (b[0] == 169 && b[1] == 254);
        }
        if (ip.AddressFamily == AddressFamily.InterNetworkV6)
            return ip.IsIPv6LinkLocal || (ip.GetAddressBytes()[0] & 0xFE) == 0xFC; // fe80::/10, fc00::/7
        return false;
    }

    public static bool IsLoopbackHost(string host) =>
        host is "localhost" or "127.0.0.1" or "[::1]" or "::1";

    // A request from a browser or program on this PC. The Host check stops a web page from reading
    // the answer through DNS rebinding.
    public static bool IsFromThisPc(HttpContext ctx)
    {
        var ip = ctx.Connection.RemoteIpAddress;
        return ip != null && IPAddress.IsLoopback(ip) && IsLoopbackHost(ctx.Request.Host.Host);
    }

    // Pairing URLs for this PC's private IPv4 addresses; interfaces with a default gateway first.
    // The token is in the fragment, so it never appears in requests or server logs.
    public static List<string> PairUrls(int port, string token) =>
        Addresses().Select(ip => $"http://{ip}:{port}/#pair={token}").ToList();

    // The companion's address on each of those addresses, without the token.
    public static List<string> BaseUrls(int port) => Addresses().Select(ip => $"http://{ip}:{port}/").ToList();

    static List<string> Addresses()
    {
        var found = new List<(bool gateway, string ip)>();
        foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (nic.OperationalStatus != OperationalStatus.Up || nic.NetworkInterfaceType is NetworkInterfaceType.Loopback or NetworkInterfaceType.Tunnel)
                continue;
            var props = nic.GetIPProperties();
            bool gateway = props.GatewayAddresses.Any(g => g.Address.AddressFamily == AddressFamily.InterNetwork && !g.Address.Equals(IPAddress.Any));
            foreach (var a in props.UnicastAddresses)
                if (a.Address.AddressFamily == AddressFamily.InterNetwork && IsPrivate(a.Address) && !IPAddress.IsLoopback(a.Address) &&
                    !a.Address.ToString().StartsWith("169.254.", StringComparison.Ordinal))
                    found.Add((gateway, a.Address.ToString()));
        }
        return found.OrderByDescending(f => f.gateway).Select(f => f.ip).Distinct().ToList();
    }
}

static class Pairing
{
    static string TokenPath => Path.Combine(AppData.Dir, "companion-token.txt");

    public static string LoadOrCreateToken(bool renew, out bool isNew)
    {
        string path = TokenPath;
        if (!renew && File.Exists(path))
        {
            string existing = File.ReadAllText(path).Trim();
            if (Regex.IsMatch(existing, "^[0-9a-f]{32}$"))
            {
                isNew = false;
                return existing;
            }
        }
        string token = Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, token);
        isNew = true;
        return token;
    }

    public static void OpenInBrowser(string url)
    {
        try
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true });
        }
        catch (Exception)
        {
        }
    }

    public static string PageHtml(List<string> urls)
    {
        var sb = new StringBuilder();
        sb.Append("<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>")
          .Append("<title>Pair a phone</title><style>body{font-family:system-ui,sans-serif;margin:0;padding:24px 16px;background:#14161a;color:#eceef2;text-align:center}")
          .Append("h1{font-size:1.4rem}.qr{background:#fff;display:inline-block;padding:16px;border-radius:12px;margin:12px}.qr svg{width:min(70vw,320px);height:auto;display:block}")
          .Append("code{word-break:break-all;color:#9fb4ff}p{color:#a9aeb8;max-width:36rem;margin:12px auto}</style></head><body>")
          .Append("<h1>Pair your phone with Pick</h1><p>Connect the phone to the same Wi-Fi as this PC and scan a code with its camera. ")
          .Append("The page that opens remembers the pairing.</p>");
        if (urls.Count == 0)
            sb.Append("<p>No network address found. Is this PC connected to the network?</p>");
        foreach (string u in urls)
        {
            string svg = new SvgQRCode(new QRCodeGenerator().CreateQrCode(u, QRCodeGenerator.ECCLevel.M)).GetGraphic(8);
            sb.Append("<div><div class=qr>").Append(svg).Append("</div><p><code>").Append(WebUtility.HtmlEncode(u)).Append("</code></p></div>");
        }
        sb.Append("<p>Keep this code private: anyone with it can queue songs while the companion runs. ")
          .Append("To revoke it, restart the companion with <code>--new-token</code>.</p></body></html>");
        return sb.ToString();
    }
}

// %LOCALAPPDATA%\Pick: pairing token and download list. Versions before the rename (Encore) used
// %LOCALAPPDATA%\Encore; that folder is moved over once, so paired phones stay paired.
static class AppData
{
    public static string Dir { get; } = Init();

    static string Init()
    {
        string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        string dir = Path.Combine(local, "Pick"), old = Path.Combine(local, "Encore");
        try
        {
            if (!Directory.Exists(dir) && Directory.Exists(old))
                Directory.Move(old, dir);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
        return dir;
    }
}
