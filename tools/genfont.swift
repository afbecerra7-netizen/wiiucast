// Genera un atlas de fuente monoespaciada en escala de grises (PGM).
// 96 glifos ASCII (32..127) en una rejilla de 16x6 celdas.
import Foundation
import CoreGraphics
import CoreText
import ImageIO
import UniformTypeIdentifiers

let cellW = 12, cellH = 24
let cols = 16, rows = 6
let W = cellW * cols, H = cellH * rows

let cs = CGColorSpaceCreateDeviceGray()
guard let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8,
                          bytesPerRow: W, space: cs,
                          bitmapInfo: CGImageAlphaInfo.none.rawValue) else {
   fatalError("no se pudo crear el contexto")
}
ctx.setFillColor(gray: 0, alpha: 1)
ctx.fill(CGRect(x: 0, y: 0, width: W, height: H))
ctx.setAllowsAntialiasing(true)
ctx.setShouldAntialias(true)

let font = CTFontCreateWithName("Menlo-Bold" as CFString, 18.0, nil)

for code in 32...127 {
   let idx = code - 32
   let cx = (idx % cols) * cellW
   let cy = H - ((idx / cols) + 1) * cellH
   let s = String(UnicodeScalar(UInt8(code)))
   let attrs: [CFString: Any] = [
      kCTFontAttributeName: font,
      kCTForegroundColorAttributeName: CGColor(gray: 1, alpha: 1),
   ]
   let astr = CFAttributedStringCreate(nil, s as CFString, attrs as CFDictionary)!
   let line = CTLineCreateWithAttributedString(astr)
   ctx.textPosition = CGPoint(x: CGFloat(cx) + 1, y: CGFloat(cy) + 6)
   CTLineDraw(line, ctx)
}

guard let data = ctx.data else { fatalError("sin datos") }
let bytes = data.bindMemory(to: UInt8.self, capacity: W * H)

// PGM con las filas en orden de arriba a abajo
var out = Data("P5\n\(W) \(H)\n255\n".utf8)
out.append(contentsOf: UnsafeBufferPointer(start: bytes, count: W * H))
try! out.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
print("atlas \(W)x\(H), celda \(cellW)x\(cellH)")
