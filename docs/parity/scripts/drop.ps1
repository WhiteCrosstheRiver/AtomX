param([string]$path)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class D {
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("kernel32.dll")] public static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);
  [DllImport("kernel32.dll")] public static extern IntPtr GlobalLock(IntPtr h);
  [DllImport("kernel32.dll")] public static extern bool GlobalUnlock(IntPtr h);
}
'@
$p = Get-Process AtomX -ErrorAction Stop | Where-Object {$_.MainWindowHandle -ne 0} | Select-Object -First 1
$hwnd = $p.MainWindowHandle
$wide = [System.Text.Encoding]::Unicode.GetBytes($path + [char]0 + [char]0)
$total = 20 + $wide.Length
$hglobal = [D]::GlobalAlloc(0x2002, [UIntPtr][uint32]$total)
$ptr = [D]::GlobalLock($hglobal)
# DROPFILES real layout: pFiles @0, pt @4, fNC @12, fWide @16; file list @20
[Runtime.InteropServices.Marshal]::WriteInt32($ptr, 0, 20)             # pFiles = 20
[Runtime.InteropServices.Marshal]::WriteInt32($ptr, 4, 100)            # pt.x
[Runtime.InteropServices.Marshal]::WriteInt32($ptr, 8, 100)            # pt.y
[Runtime.InteropServices.Marshal]::WriteInt32($ptr, 12, 0)             # fNC
[Runtime.InteropServices.Marshal]::WriteInt32($ptr, 16, 1)             # fWide = TRUE
[Runtime.InteropServices.Marshal]::Copy($wide, 0, [IntPtr]($ptr.ToInt64()+20), $wide.Length)
[D]::GlobalUnlock($hglobal) | Out-Null
$ok = [D]::PostMessageW($hwnd, 0x233, $hglobal, [IntPtr]::Zero)
Write-Output "posted WM_DROPFILES ok=$ok"
