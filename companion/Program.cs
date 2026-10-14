// Encore companion: a small web server on the PC that lets phones on the local network search the
// Clone Hero library and queue songs. It forwards a fixed set of requests to Encore's loopback
// API (127.0.0.1:47321) and never exposes Encore itself.
//
// Security model:
//   - Every /api request must carry the pairing token (Authorization: Bearer <token>). The token
//     is random, stored in %LOCALAPPDATA%\Encore\companion-token.txt and shown on the PC only
//     (console QR code, and the /pair page that answers to the PC itself).
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
    Console.Error.WriteLine("usage: EncoreCompanion [--port 47322] [--encore-port 47321] [--new-token]");
    return 2;
}
Console.OutputEncoding = Encoding.UTF8;

bool tokenIsNew;
string token = Pairing.LoadOrCreateToken(options.NewToken, out tokenIsNew);
byte[] tokenBytes = Encoding.UTF8.GetBytes(token);

var builder = WebApplication.CreateBuilder();
builder.Logging.SetMinimumLevel(LogLevel.Warning);
builder.WebHost.ConfigureKestrel(k => k.ListenAnyIP(options.Port));
builder.Services.AddHttpClient("encore", c =>
{
    c.BaseAddress = new Uri($"http://127.0.0.1:{options.EncorePort}/");
    c.Timeout = TimeSpan.FromSeconds(6);
});
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
    var ip = ctx.Connection.RemoteIpAddress;
    if (ip == null || !IPAddress.IsLoopback(ip) || !Network.IsLoopbackHost(ctx.Request.Host.Host))
        return Results.StatusCode(StatusCodes.Status403Forbidden);
    return Results.Content(Pairing.PageHtml(Network.PairUrls(options.Port, token)), "text/html; charset=utf-8");
});

var checksumPattern = new Regex("^[0-9A-Fa-f]{32}$");

app.MapGet("/api/status", (IHttpClientFactory f) => Encore.Forward(f, HttpMethod.Get, "status"));
app.MapGet("/api/setlist", (IHttpClientFactory f) => Encore.Forward(f, HttpMethod.Get, "setlist"));
app.MapPost("/api/rescan", (IHttpClientFactory f) => Encore.Forward(f, HttpMethod.Post, "rescan"));
app.MapGet("/api/library", (IHttpClientFactory f, string? q, int? limit) =>
{
    q = (q ?? "").Trim();
    if (q.Length > 200)
        return Task.FromResult(Results.BadRequest(new { result = "bad_request", message = "search text too long" }));
    int n = Math.Clamp(limit ?? 50, 1, 200);
    return Encore.Forward(f, HttpMethod.Get, $"library?q={Uri.EscapeDataString(q)}&limit={n}");
});
app.MapPost("/api/queue", async (IHttpClientFactory f, HttpRequest req) =>
{
    QueueRequest? body = null;
    try
    {
        body = await req.ReadFromJsonAsync<QueueRequest>();
    }
    catch (JsonException)
    {
    }
    if (body?.Checksum == null || !checksumPattern.IsMatch(body.Checksum))
        return Results.BadRequest(new { result = "bad_request", message = "give the song's checksum" });
    return await Encore.Forward(f, HttpMethod.Post, "queue", JsonSerializer.Serialize(new { checksum = body.Checksum }));
});

app.Lifetime.ApplicationStarted.Register(() =>
{
    var urls = Network.PairUrls(options.Port, token);
    Console.WriteLine();
    Console.WriteLine("Encore companion is running. Scan the QR code with your phone (same Wi-Fi):");
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
    Console.WriteLine($"Talking to Encore on http://127.0.0.1:{options.EncorePort}. Stop with Ctrl+C.");
    if (tokenIsNew)
        Pairing.OpenInBrowser($"http://localhost:{options.Port}/pair");
});

app.Run();
return 0;

record QueueRequest(string? Checksum);

sealed class Options
{
    public int Port = 47322;
    public int EncorePort = 47321;
    public bool NewToken;

    public static Options? Parse(string[] args)
    {
        var o = new Options();
        if (int.TryParse(Environment.GetEnvironmentVariable("ENCORE_PORT"), out int envPort) && envPort > 0)
            o.EncorePort = envPort;
        for (int i = 0; i < args.Length; ++i)
        {
            switch (args[i])
            {
                case "--port" when i + 1 < args.Length && int.TryParse(args[i + 1], out int p) && p is > 0 and < 65536:
                    o.Port = p;
                    ++i;
                    break;
                case "--encore-port" when i + 1 < args.Length && int.TryParse(args[i + 1], out int e) && e is > 0 and < 65536:
                    o.EncorePort = e;
                    ++i;
                    break;
                case "--new-token":
                    o.NewToken = true;
                    break;
                default:
                    return null;
            }
        }
        return o;
    }
}

static class Encore
{
    // Forwards a request to Encore and passes its status code and JSON answer through.
    public static async Task<IResult> Forward(IHttpClientFactory factory, HttpMethod method, string path, string? json = null)
    {
        var client = factory.CreateClient("encore");
        using var msg = new HttpRequestMessage(method, path);
        if (json != null)
            msg.Content = new StringContent(json, Encoding.UTF8, "application/json");
        try
        {
            using var resp = await client.SendAsync(msg);
            string body = await resp.Content.ReadAsStringAsync();
            return Results.Content(body, "application/json; charset=utf-8", Encoding.UTF8, (int)resp.StatusCode);
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException)
        {
            return Results.Json(new { result = "encore_offline", message = "Clone Hero with Encore is not running on the PC" },
                                statusCode: StatusCodes.Status502BadGateway);
        }
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

    // Pairing URLs for this PC's private IPv4 addresses; interfaces with a default gateway first.
    // The token is in the fragment, so it never appears in requests or server logs.
    public static List<string> PairUrls(int port, string token)
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
        return found.OrderByDescending(f => f.gateway).Select(f => $"http://{f.ip}:{port}/#pair={token}").Distinct().ToList();
    }
}

static class Pairing
{
    static string TokenPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Encore", "companion-token.txt");

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
          .Append("<h1>Pair your phone with Encore</h1><p>Connect the phone to the same Wi-Fi as this PC and scan a code with its camera. ")
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
