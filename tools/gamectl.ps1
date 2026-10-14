<#
.SYNOPSIS
  Test helper: post a key to Clone Hero's Unity window, or save a capture of it as PNG.

.DESCRIPTION
  Posted WM_KEYDOWN messages reach Encore's window procedure (good enough to test the F5 handler),
  but not the game itself: Clone Hero reads input through Rewired/raw input, so menus cannot be
  navigated this way. PrintWindow captures of the DirectX surface may come out black.

.EXAMPLE
  .\gamectl.ps1 -ProcId 12012 -Action key -Arg 0x74       # F5
  .\gamectl.ps1 -ProcId 12012 -Action shot -Arg C:\temp\shot.png
#>
param([int]$ProcId, [string]$Action, [string]$Arg)
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Drawing; using System.Drawing.Imaging;
public static class GC2 {
  delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EP cb, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(uint pid) { IntPtr found = IntPtr.Zero; EnumWindows((h,l)=>{ uint p; GetWindowThreadProcessId(h, out p); var c=new StringBuilder(64); GetClassName(h,c,64); if(p==pid && c.ToString()=="UnityWndClass"){found=h; return false;} return true; }, IntPtr.Zero); return found; }
  public static void Key(IntPtr h, int vk) { PostMessage(h, 0x100, (IntPtr)vk, (IntPtr)1); PostMessage(h, 0x101, (IntPtr)vk, (IntPtr)unchecked((int)0xC0000001)); }
  public static void Shot(IntPtr h, string path) { RECT r; GetClientRect(h, out r); using (var bmp = new Bitmap(Math.Max(1,r.R), Math.Max(1,r.B))) { using (var g = Graphics.FromImage(bmp)) { var dc = g.GetHdc(); PrintWindow(h, dc, 3); g.ReleaseHdc(dc); } bmp.Save(path, ImageFormat.Png); } }
}
"@
$h = [GC2]::Find([uint32]$ProcId)
if ($h -eq [IntPtr]::Zero) { throw "no Unity window for pid $ProcId" }
switch ($Action) {
  'key'  { [GC2]::Key($h, [int]$Arg); "posted vk $Arg" }
  'shot' { [GC2]::Shot($h, $Arg); "saved $Arg" }
}
