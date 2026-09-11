$emojiDir = "C:\Users\silva\momentics-workspace\Beport\assets\emoji"
$outFile = "C:\Users\silva\momentics-workspace\Beport\assets\emojimap.js"

$files = Get-ChildItem $emojiDir -Filter *.png | Sort-Object Name

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// Generated from twemoji (jdecked/twemoji v17.0.3, CC-BY 4.0) 72x72 PNG")
[void]$sb.AppendLine("// filenames -- see tools/gen_emojimap.ps1. Keys are JS string literals built")
[void]$sb.AppendLine("// from the exact Unicode codepoint sequence encoded in each filename, written")
[void]$sb.AppendLine("// as \\uXXXX escapes so the file itself can stay plain ASCII regardless of how")
[void]$sb.AppendLine("// any tool along the way re-saves it.")
[void]$sb.AppendLine("var EMOJI_MAP = {")

$count = 0
foreach ($f in $files) {
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
    $parts = $stem -split '-'
    $jsKey = ""
    $valid = $true
    foreach ($p in $parts) {
        try {
            $cp = [Convert]::ToInt32($p, 16)
        } catch {
            $valid = $false
            break
        }
        $s = [char]::ConvertFromUtf32($cp)
        foreach ($ch in $s.ToCharArray()) {
            $jsKey += "\u" + ([int]$ch).ToString("x4")
        }
    }
    if (-not $valid) { continue }
    [void]$sb.AppendLine("`"$jsKey`": `"$($f.Name)`",")
    $count++
}

[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("function _lookup(ch) {")
[void]$sb.AppendLine("    return Object.prototype.hasOwnProperty.call(EMOJI_MAP, ch) ? EMOJI_MAP[ch] : undefined;")
[void]$sb.AppendLine("}")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("// Resolves an emoji string to its bundled asset path, or `"`" if this exact")
[void]$sb.AppendLine("// sequence isn't in the map. Matrix clients disagree on whether to include the")
[void]$sb.AppendLine("// U+FE0F emoji-presentation variation selector on a reaction key, so a plain")
[void]$sb.AppendLine("// lookup miss also retries with it added/removed before giving up.")
[void]$sb.AppendLine("function emojiAsset(ch) {")
[void]$sb.AppendLine("    if (!ch) return `"`";")
[void]$sb.AppendLine("    var f = _lookup(ch);")
[void]$sb.AppendLine("    if (f) return `"asset:///emoji/`" + f;")
[void]$sb.AppendLine("    var VS16 = `"\uFE0F`";")
[void]$sb.AppendLine("    if (ch.length >= 1 && ch.charAt(ch.length - 1) !== VS16) {")
[void]$sb.AppendLine("        f = _lookup(ch + VS16);")
[void]$sb.AppendLine("        if (f) return `"asset:///emoji/`" + f;")
[void]$sb.AppendLine("    }")
[void]$sb.AppendLine("    if (ch.length >= 1 && ch.charAt(ch.length - 1) === VS16) {")
[void]$sb.AppendLine("        f = _lookup(ch.substring(0, ch.length - 1));")
[void]$sb.AppendLine("        if (f) return `"asset:///emoji/`" + f;")
[void]$sb.AppendLine("    }")
[void]$sb.AppendLine("    return `"`";")
[void]$sb.AppendLine("}")

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($outFile, $sb.ToString(), $utf8NoBom)

Write-Host "Wrote $count entries to $outFile"
$len = (Get-Item $outFile).Length
Write-Host "File size: $([math]::Round($len/1KB,1)) KB"
