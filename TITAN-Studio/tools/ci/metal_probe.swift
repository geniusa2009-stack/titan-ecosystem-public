// Reports whether this machine exposes a Metal device (TITAN Studio's viewport renders through QRhi on
// Metal). CI uses it to decide whether the GUI smoke test can run. Original TITAN code.
import Foundation
import Metal

if let device = MTLCreateSystemDefaultDevice() {
    print("Metal device: \(device.name)")
    exit(0)
}
print("No Metal device available")
exit(1)
