# Packs media/ogrethrone<size>.png into media/ogrethrone.ico (PNG-compressed icon
# entries, read by Windows Vista and later). Run again after editing the PNGs.
$media = Join-Path $PSScriptRoot '../media'
$pngs = Get-ChildItem $media -Filter 'ogrethrone*.png' |
    Sort-Object { [int]($_.BaseName -replace '\D', '') } |
    ForEach-Object { , [IO.File]::ReadAllBytes($_.FullName) }
$stream = New-Object IO.MemoryStream
$out = New-Object IO.BinaryWriter $stream
$out.Write([UInt16]0); $out.Write([UInt16]1); $out.Write([UInt16]$pngs.Count)
$offset = 6 + 16 * $pngs.Count
foreach ($png in $pngs) {
    # The PNG header holds the big-endian width and height at bytes 16-23; 0 means 256.
    $out.Write([byte]($png[19] % 256)); $out.Write([byte]($png[23] % 256))
    $out.Write([byte]0); $out.Write([byte]0); $out.Write([UInt16]1); $out.Write([UInt16]32)
    $out.Write([UInt32]$png.Length); $out.Write([UInt32]$offset)
    $offset += $png.Length
}
foreach ($png in $pngs) { $out.Write($png) }
[IO.File]::WriteAllBytes((Join-Path $media 'ogrethrone.ico'), $stream.ToArray())
