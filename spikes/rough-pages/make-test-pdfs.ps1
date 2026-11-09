# Writes the two test PDFs for the Rough Page spike (throwaway, never merged):
#   scan.pdf  160 pages, each one 2480x3508 colour JPEG (A4 at 300 dpi), like a scanned book
#   text.pdf  300 pages of plain Helvetica text, like a born-digital book
# Usage: .\make-test-pdfs.ps1            (writes next to this script)
param([string]$dir = $PSScriptRoot, [int]$scanPages = 160, [int]$textPages = 300)

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Drawing;
using System.Drawing.Imaging;
using System.Drawing.Drawing2D;
using System.Collections.Generic;

public static class RoughPdf {
    class Writer {
        MemoryStream ms = new MemoryStream();
        List<long> offs = new List<long>();
        public Writer() { W("%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"); }
        void W(string s) { var b = Encoding.GetEncoding(28591).GetBytes(s); ms.Write(b, 0, b.Length); }
        public int Reserve() { offs.Add(0); return offs.Count; }
        public void Obj(int id, string body) { offs[id - 1] = ms.Position; W(id + " 0 obj\n" + body + "\nendobj\n"); }
        public void Stream(int id, string dict, byte[] data) {
            offs[id - 1] = ms.Position;
            W(id + " 0 obj\n<< " + dict + " /Length " + data.Length + " >>\nstream\n");
            ms.Write(data, 0, data.Length);
            W("\nendstream\nendobj\n");
        }
        public void Save(string path, int root) {
            long xref = ms.Position;
            W("xref\n0 " + (offs.Count + 1) + "\n0000000000 65535 f \n");
            foreach (var o in offs) W(o.ToString("D10") + " 00000 n \n");
            W("trailer\n<< /Size " + (offs.Count + 1) + " /Root " + root + " 0 R >>\nstartxref\n" + xref + "\n%%EOF\n");
            File.WriteAllBytes(path, ms.ToArray());
        }
    }

    static string Words(Random r, int n) {
        string[] w = { "the", "page", "turned", "slowly", "under", "her", "hand", "and", "light", "fell", "across",
                       "paper", "river", "morning", "letter", "quiet", "station", "window", "garden", "evening",
                       "of", "a", "in", "was", "had", "not", "long", "before", "after", "story", "chapter" };
        var sb = new StringBuilder();
        for (int i = 0; i < n; i++) { if (i > 0) sb.Append(' '); sb.Append(w[r.Next(w.Length)]); }
        return sb.ToString();
    }

    static byte[] ScanJpeg(int pageNo, Random r, ImageCodecInfo codec, EncoderParameters ep) {
        const int W = 2480, H = 3508;
        using (var bmp = new Bitmap(W, H, PixelFormat.Format24bppRgb)) {
            // paper texture: low-frequency noise (1/4 resolution, scaled up), like a real scan's grain
            using (var tex = new Bitmap(W / 4, H / 4, PixelFormat.Format24bppRgb)) {
                var d = tex.LockBits(new Rectangle(0, 0, tex.Width, tex.Height), ImageLockMode.WriteOnly, tex.PixelFormat);
                var row = new byte[d.Stride];
                for (int y = 0; y < tex.Height; y++) {
                    for (int x = 0; x < tex.Width; x++) {
                        int n = r.Next(-9, 10);
                        row[x * 3 + 0] = (byte)(222 + n); row[x * 3 + 1] = (byte)(236 + n); row[x * 3 + 2] = (byte)(242 + n);
                    }
                    System.Runtime.InteropServices.Marshal.Copy(row, 0, d.Scan0 + y * d.Stride, d.Stride);
                }
                tex.UnlockBits(d);
                using (var g = Graphics.FromImage(bmp)) {
                    g.InterpolationMode = InterpolationMode.Bilinear;
                    g.DrawImage(tex, 0, 0, W, H);
                    g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAlias;
                    g.TranslateTransform(W / 2, H / 2); g.RotateTransform((float)(r.NextDouble() - 0.5)); g.TranslateTransform(-W / 2, -H / 2);
                    using (var f = new Font("Georgia", 40)) using (var big = new Font("Georgia", 220, FontStyle.Bold))
                    using (var ink = new SolidBrush(Color.FromArgb(40, 34, 30))) {
                        g.DrawString(pageNo.ToString(), big, ink, 900, 260);
                        for (int l = 0; l < 52; l++) g.DrawString(Words(r, 9), f, ink, 260, 760 + l * 52);
                    }
                    if (pageNo % 5 == 0) {
                        // a photo-like block every fifth page
                        for (int k = 0; k < 400; k++) {
                            using (var b = new SolidBrush(Color.FromArgb(r.Next(256), r.Next(256), r.Next(256))))
                                g.FillEllipse(b, 300 + r.Next(1700), 1500 + r.Next(1100), 40 + r.Next(200), 40 + r.Next(200));
                        }
                    }
                }
            }
            using (var ms = new MemoryStream()) { bmp.Save(ms, codec, ep); return ms.ToArray(); }
        }
    }

    public static void Scan(string path, int n) {
        var r = new Random(7);
        ImageCodecInfo codec = null;
        foreach (var c in ImageCodecInfo.GetImageEncoders()) if (c.MimeType == "image/jpeg") codec = c;
        var ep = new EncoderParameters(1);
        ep.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 80L);
        var w = new Writer();
        int catalog = w.Reserve(), pages = w.Reserve();
        var kids = new StringBuilder();
        for (int i = 1; i <= n; i++) {
            int page = w.Reserve(), img = w.Reserve(), content = w.Reserve();
            w.Stream(img, "/Type /XObject /Subtype /Image /Width 2480 /Height 3508 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode", ScanJpeg(i, r, codec, ep));
            w.Stream(content, "", Encoding.ASCII.GetBytes("q 595.2 0 0 841.9 0 0 cm /Im0 Do Q"));
            w.Obj(page, "<< /Type /Page /Parent " + pages + " 0 R /MediaBox [0 0 595.2 841.9] /Resources << /XObject << /Im0 " + img + " 0 R >> >> /Contents " + content + " 0 R >>");
            kids.Append(page + " 0 R ");
        }
        w.Obj(pages, "<< /Type /Pages /Count " + n + " /Kids [" + kids + "] >>");
        w.Obj(catalog, "<< /Type /Catalog /Pages " + pages + " 0 R >>");
        w.Save(path, catalog);
    }

    public static void Text(string path, int n) {
        var r = new Random(3);
        var w = new Writer();
        int catalog = w.Reserve(), pages = w.Reserve(), font = w.Reserve(), bold = w.Reserve();
        w.Obj(font, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
        w.Obj(bold, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>");
        var kids = new StringBuilder();
        for (int i = 1; i <= n; i++) {
            int page = w.Reserve(), content = w.Reserve();
            var sb = new StringBuilder();
            sb.Append("BT /F2 28 Tf 72 760 Td (" + i + ") Tj ET\nBT /F1 10.5 Tf 13 TL 72 720 Td\n");
            for (int l = 0; l < 48; l++) sb.Append("(" + Words(r, 13) + ") '\n");
            sb.Append("ET");
            w.Stream(content, "", Encoding.ASCII.GetBytes(sb.ToString()));
            w.Obj(page, "<< /Type /Page /Parent " + pages + " 0 R /MediaBox [0 0 595.2 841.9] /Resources << /Font << /F1 " + font + " 0 R /F2 " + bold + " 0 R >> >> /Contents " + content + " 0 R >>");
            kids.Append(page + " 0 R ");
        }
        w.Obj(pages, "<< /Type /Pages /Count " + n + " /Kids [" + kids + "] >>");
        w.Obj(catalog, "<< /Type /Catalog /Pages " + pages + " 0 R >>");
        w.Save(path, catalog);
    }
}
'@

$scan = Join-Path $dir "scan.pdf"; $text = Join-Path $dir "text.pdf"
if (-not (Test-Path $text)) { [RoughPdf]::Text($text, $textPages); "wrote $text" }
if (-not (Test-Path $scan)) { [RoughPdf]::Scan($scan, $scanPages); "wrote $scan" }
Get-Item $scan, $text | Select-Object Name, @{n = "MB"; e = { [math]::Round($_.Length / 1MB, 1) } }
