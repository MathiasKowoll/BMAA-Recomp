// Lists the windows of the dr-* processes (or of the pid given): id, on screen or not, size, layer, owner, name.
import CoreGraphics
import Foundation
let want = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : ""
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] ?? []
for w in list {
    let owner = (w[kCGWindowOwnerName as String] as? String) ?? ""
    let pid = (w[kCGWindowOwnerPID as String] as? NSNumber)?.intValue ?? 0
    guard owner.contains("dr-") || String(pid) == want else { continue }
    let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
    let on = (w[kCGWindowIsOnscreen as String] as? NSNumber)?.boolValue ?? false
    print((w[kCGWindowNumber as String] as? NSNumber)?.intValue ?? 0, on ? "onscreen" : "offscreen",
          (b["Width"] as? NSNumber)?.intValue ?? 0, "x", (b["Height"] as? NSNumber)?.intValue ?? 0,
          "layer", (w[kCGWindowLayer as String] as? NSNumber)?.intValue ?? 0, "owner:", owner,
          "name:", (w[kCGWindowName as String] as? String) ?? "")
}
