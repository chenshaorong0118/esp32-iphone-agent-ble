import CoreImage
import Foundation
import ImageIO
import ScreenCaptureKit
import UIKit

/// What the agent and the pointer need from the screen (VideoFeed, or a fake in tests). Port of screen.py.
@MainActor protocol ScreenSource: AnyObject {
    var live: Bool { get }
    var frame: Data? { get }
    var updated: Double { get }
    var capturedAt: Double { get }
    var aspect: Double { get }
    /// After a gesture that ended at `since`: wait until the screen shows its result (and fetch it, in snap mode).
    func afterAction(since: Double) async
}

/// How a run gets its frames: the live stream (`stream on`, the default: a frame in under a second), or one
/// snap after each action (`stream off`; waits for the board's whole GOP, often 20-30 s).
enum FrameMode: String {
    case stream, snap
    static let key = "frameModeV2"  // V2: drops the old saved "snap" left by earlier automatic fallbacks
    /// A command timeout switches the current run to snap; not saved, so the next run streams again.
    static var fallback: FrameMode?
    static var current: FrameMode {
        get { fallback ?? FrameMode(rawValue: UserDefaults.standard.string(forKey: key) ?? "") ?? .stream }
        set { UserDefaults.standard.set(newValue.rawValue, forKey: key); fallback = nil }
    }
}

extension ScreenSource {
    func afterAction(since: Double) async { await settled() }

    /// Wait until the screen stops changing (AirPlay only sends frames on change). Mid-animation frames are big and
    /// can reach us late over BLE, so a gap is only "quiet" once it also covers the last frame's transport lag.
    func settled(quiet: Double = 0.5, least: Double = 0.3, most: Double = 4) async {
        let started = now()
        try? await Task.sleep(nanoseconds: UInt64(least * 1e9))
        while now() - updated < quiet + lag && now() - started < most {
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
    }

    /// How late the shown frame arrived after the board got it (0 when unknown), capped.
    private var lag: Double { capturedAt > 0 ? min(max(updated - capturedAt, 0), 1) : 0 }

    /// The first frame captured after `since` (received, when capture times are unknown), or nil if none in time.
    func fresh(since: Double, timeout: Double = 0.8) async -> Data? {
        let deadline = now() + timeout
        while (capturedAt > 0 ? capturedAt : updated) <= since {
            if now() > deadline { return nil }
            try? await Task.sleep(nanoseconds: 20_000_000)
        }
        return frame
    }
}

/// The phone's screen as JPEG frames: mirrored over AirPlay to the board and forwarded over BLE, or (iOS 27, a
/// board without video such as hid_ble) captured on the phone itself with ScreenCaptureKit (`localCapture`).
@MainActor final class VideoFeed: ObservableObject, ScreenSource {
    @Published private(set) var frame: Data?
    @Published private(set) var size = CGSize.zero
    @Published private(set) var note = ""  // last decoder problem, shown in Diagnostics
    private(set) var updated = 0.0
    /// When the board received the shown frame, in local time (≈; see `boardClock`).
    private(set) var capturedAt = 0.0
    /// A decode error or a lost chunk since the last good frame: the shown frame is stale until the next IDR.
    private(set) var broken = false
    private var clockOffset: Double?  // local s − board s, the smallest seen (≈ the link latency)
    var mirroring = false  // board status says ap == streaming
    var metrics: Metrics?
    weak var device: HIDDevice?
    /// The screen comes from ScreenCaptureKit on this phone, not from the board.
    @Published private(set) var localCapture = false
    @Published private(set) var captureNote = ""
    private var capture: AnyObject?  // ScreenCapture (iOS 27)
    private var confirmedAt = 0.0     // last "screen unchanged" from ScreenCaptureKit
    var onCaptureChange: ((Bool) -> Void)?

    private var assembler = VideoAssembler()
    private let decoder = H264Decoder()

    init() {
        decoder.onFrame = { [weak self] f in DispatchQueue.main.async { self?.publish(f) } }
        decoder.onError = { [weak self] msg in
            DispatchQueue.main.async {
                let bg = UIApplication.shared.applicationState == .background
                self?.note = bg ? "\(msg) [in background]" : msg
                self?.metrics?.decodeError(background: bg)
                self?.broken = true
            }
        }
    }

    var live: Bool { (localCapture || mirroring) && frame != nil && (device?.connected ?? false) }

    /// Frame width / height (portrait iPhone as a fallback).
    var aspect: Double { size.width > 0 && size.height > 0 ? size.width / size.height : 9 / 19.5 }

    /// One `video` notify from BoardLink.
    func receive(_ chunk: Data) {
        metrics?.videoBytes(chunk.count)
        let dropsBefore = assembler.drops
        let unit = assembler.add(chunk)
        if assembler.drops > dropsBefore { metrics?.videoDrops(assembler.drops - dropsBefore); broken = true }
        guard let unit else { return }
        if !unit.snap { boardClock(ms: Int(unit.boardMs)) }  // snap units are resent late: their ages say nothing
        if unit.idr { metrics?.idrSeen() }
        decoder.queue.async { [decoder] in decoder.decode(unit) }
    }

    /// The link (re)connected: chunk numbering starts over.
    func resetStream() { assembler.reset(); clockOffset = nil }  // the board may have rebooted: its clock too

    /// A board timestamp (status `up`, or a streamed unit) just arrived: keep the smallest offset.
    func boardClock(ms: Int) {
        let offset = now() - Double(ms) / 1000
        clockOffset = min(clockOffset ?? offset, offset)
    }

    /// Show a still frame (demo data).
    func show(_ jpeg: Data, width: Int, height: Int) {
        publish(H264Decoder.Frame(jpeg: jpeg, width: width, height: height, decodeMs: 4.2,
                                  unit: AccessUnit(data: Data(), idr: false, config: false, snap: false, boardMs: 0)))
    }

    private func publish(_ f: H264Decoder.Frame) {
        frame = f.jpeg
        updated = now()
        capturedAt = clockOffset.map { Double(f.unit.boardMs) / 1000 + $0 } ?? updated
        broken = false
        if size.width != CGFloat(f.width) || size.height != CGFloat(f.height) {
            size = CGSize(width: f.width, height: f.height)
        }
        metrics?.frameDecoded(ms: f.decodeMs)
    }

    /// The app asked the board to forward the stream (`stream on`).
    private(set) var streamOn = false

    /// Seconds a snap may take: the buffered GOP over the measured BLE rate, doubled, at least 8 s.
    var snapTimeout: Double {
        let gopBytes = Double(metrics?.board?.gop ?? 0)
        let kbps = max(metrics?.bleKbps ?? 0, metrics?.board?.ble?.kbps ?? 0, 40)
        return max(8, gopBytes * 8 / 1000 / kbps * 2 + 3)
    }

    /// A current frame for the model. While the board streams, the live stream has it (an IDR comes every
    /// ~2 s): use a frame younger than `maxAge`, or wait for the next one. Snap only as a fallback, retried
    /// once with a longer timeout. Returns where the frame came from, for the log.
    @discardableResult
    func currentFrame(maxAge: Double = 1, wait: Double = 3) async throws -> String {
        if localCapture {
            if frame != nil && now() - max(updated, confirmedAt) < maxAge { return "capture" }
            if await fresh(since: now() - maxAge, timeout: wait) != nil { return "capture (waited)" }
            if frame != nil { return "capture (unchanged)" }  // ScreenCaptureKit only sends frames on change
            throw DeviceError(message: String(localized: "Screen sharing is off. Start it on the See step."))
        }
        if streamOn && mirroring {
            if frame != nil && now() - updated < maxAge { return "live" }
            if await fresh(since: now() - maxAge, timeout: wait) != nil { return "live (waited)" }
        }
        do {
            try await snap()
        } catch is BoardTimeout {
            DebugLog.write("snap timed out; retrying once with a longer timeout")
            try await snap(timeout: snapTimeout * 1.5)
        }
        return "snap"
    }

    /// Ask the board for the current GOP and wait until it is decoded. Returns the latency in ms.
    @discardableResult
    func snap(timeout: Double? = nil) async throws -> Double {
        if localCapture { return 0 }
        guard let device else { throw DeviceError(message: "board not connected") }
        let started = now()
        if let channel = device as? CommandChannel {
            try await channel.send("snap", timeout: timeout ?? snapTimeout)
        } else {
            try await device.send("snap")
        }
        await withCheckedContinuation { c in decoder.queue.async { c.resume() } }  // the GOP is decoded
        try? await Task.sleep(nanoseconds: 30_000_000)  // let the last frame reach the main queue
        let ms = (now() - started) * 1000
        metrics?.snapLatency(ms)
        return ms
    }

    func stream(_ on: Bool) async throws {
        if localCapture { streamOn = on; return }  // the phone captures itself; the board has no video
        try await device?.send("stream \(on ? "on" : "off")")
        streamOn = on
    }

    /// The model must never see a frame from before the action that ended at `since`. Stream: let the screen
    /// settle, then wait (bounded) for a frame captured after the action; none while the stream is healthy means
    /// the screen did not change, but a broken stream (waiting for an IDR) needs a snap. Snap: give the UI a moment,
    /// snap, and snap once more if the GOP still ends before the action.
    func afterAction(since: Double) async {
        if localCapture {
            await settled()
            if await newer(than: since, timeout: 1.5) { return }
            return DebugLog.write("frame: none after the action; screen unchanged")
        }
        do {
            if FrameMode.current == .snap {
                try await Task.sleep(nanoseconds: 700_000_000)
                try await currentFrame()
                guard capturedAt < since else { return }
                DebugLog.write(String(format: "frame: snap ends %.1f s before the action; snapping again", since - capturedAt))
                try await Task.sleep(nanoseconds: 500_000_000)
                try await currentFrame()
            } else {
                await settled()
                if await newer(than: since, timeout: 1.5) { return }
                guard broken || !streamOn else { return DebugLog.write("frame: none after the action; screen unchanged") }
                DebugLog.write("frame: stream stale (waiting for an IDR); snapping")
                try await snap()
            }
        } catch {
            DebugLog.write("frame after action failed: \(error.localizedDescription)")
        }
        if capturedAt < since { DebugLog.write(String(format: "frame: still %.1f s older than the action", since - capturedAt)) }
    }

    /// Waits until the shown frame was captured after `t`; false on timeout.
    private func newer(than t: Double, timeout: Double) async -> Bool {
        let deadline = now() + timeout
        while capturedAt < t {
            if now() > deadline { return false }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        return true
    }

    /// The link dropped: the board forgets `stream on`.
    func linkLost() { if !localCapture { streamOn = false } }

    // MARK: - ScreenCaptureKit (iOS 27)

    static var canCapture: Bool { if #available(iOS 27.0, *) { return true } else { return false } }

    /// Shows the system picker; the user chooses the entire screen. Frames then flow until sharing stops.
    func startCapture() {
        guard #available(iOS 27.0, *) else { captureNote = String(localized: "Screen sharing needs iOS 27."); return }
        let c = (capture as? ScreenCapture) ?? ScreenCapture()
        capture = c
        c.onFrame = { [weak self] jpeg, w, h, at in self?.receiveCaptured(jpeg, width: w, height: h, capturedAt: at) }
        c.onIdle = { [weak self] at in self?.confirmedAt = at }
        c.onState = { [weak self] running, note in
            guard let self else { return }
            localCapture = running
            captureNote = note
            if !running { frame = nil }
            onCaptureChange?(running)
            DebugLog.write("capture: \(running ? "running" : "stopped") \(note)")
        }
        c.present()
    }

    func stopCapture() {
        if #available(iOS 27.0, *) { (capture as? ScreenCapture)?.stop() }
    }

    private func receiveCaptured(_ jpeg: Data, width: Int, height: Int, capturedAt at: Double) {
        frame = jpeg
        updated = now()
        capturedAt = at
        broken = false
        if size.width != CGFloat(width) || size.height != CGFloat(height) { size = CGSize(width: width, height: height) }
        metrics?.frameDecoded(ms: (updated - at) * 1000)
    }
}

/// The whole screen of this phone through ScreenCaptureKit. Frames arrive only when the screen changes (`.complete`);
/// an unchanged screen reports `.idle`. Changed frames are encoded at most every `minGap` s, always ending on the
/// latest one, scaled so the long side is `longSide` px. Callbacks run on the main queue.
@available(iOS 27.0, *)
final class ScreenCapture: NSObject, SCContentSharingPickerObserver, SCStreamOutput, SCStreamDelegate {
    var onFrame: ((Data, Int, Int, Double) -> Void)?
    var onIdle: ((Double) -> Void)?
    var onState: ((Bool, String) -> Void)?
    private let longSide: CGFloat = 1280
    private let minGap = 0.15
    private let queue = DispatchQueue(label: "screen.capture")
    private let context = CIContext()
    private var stream: SCStream?
    private var pending: (CVPixelBuffer, Double)?  // queue only
    private var lastEncode = 0.0
    private var scheduled = false

    func present() {
        let picker = SCContentSharingPicker.shared
        picker.add(self)
        picker.isActive = true
        picker.present(using: .display)
    }

    func stop() {
        stream?.stopCapture { _ in }
        stream = nil
        SCContentSharingPicker.shared.isActive = false
        state(false, String(localized: "Screen sharing stopped."))
    }

    private func state(_ running: Bool, _ note: String) { DispatchQueue.main.async { self.onState?(running, note) } }

    func contentSharingPicker(_ picker: SCContentSharingPicker, didUpdateWith filter: SCContentFilter, for stream: SCStream?) {
        let config = SCStreamConfiguration()
        config.capturesAudio = false
        let scale = CGFloat(filter.pointPixelScale)
        config.width = max(1, Int(filter.contentRect.width * scale))
        config.height = max(1, Int(filter.contentRect.height * scale))
        self.stream?.stopCapture { _ in }
        let next = SCStream(filter: filter, configuration: config, delegate: self)
        do {
            try next.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        } catch {
            return state(false, error.localizedDescription)
        }
        self.stream = next
        next.startCapture { [weak self] error in
            if let error { self?.state(false, error.localizedDescription) } else { self?.state(true, "") }
        }
    }

    func contentSharingPicker(_ picker: SCContentSharingPicker, didCancelFor stream: SCStream?) {
        if self.stream == nil { state(false, String(localized: "Screen sharing was cancelled.")) }
    }

    func contentSharingPickerStartDidFailWithError(_ error: Error) { state(false, error.localizedDescription) }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        if stream === self.stream { self.stream = nil }
        state(false, error.localizedDescription)
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer buffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, stream === self.stream, CMSampleBufferIsValid(buffer),
              let info = CMSampleBufferGetSampleAttachmentsArray(buffer, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let raw = info.first?[.status] as? Int, let status = SCFrameStatus(rawValue: raw) else { return }
        let t = now()
        switch status {
        case .complete:
            guard let pixels = CMSampleBufferGetImageBuffer(buffer) else { return }
            pending = (pixels, t)
            flush()
        case .idle:
            if pending == nil { DispatchQueue.main.async { self.onIdle?(t) } }
        default:
            break
        }
    }

    /// Encode the latest changed frame now, or once `minGap` has passed since the last one (on `queue`).
    private func flush() {
        guard let (pixels, at) = pending else { return }
        let wait = lastEncode + minGap - now()
        if wait > 0 {
            if !scheduled {
                scheduled = true
                queue.asyncAfter(deadline: .now() + wait) { [weak self] in self?.scheduled = false; self?.flush() }
            }
            return
        }
        pending = nil
        lastEncode = now()
        var image = CIImage(cvPixelBuffer: pixels)
        let factor = min(1, longSide / max(image.extent.width, image.extent.height))
        if factor < 1 { image = image.transformed(by: CGAffineTransform(scaleX: factor, y: factor)) }
        let space = CGColorSpace(name: CGColorSpace.sRGB)!
        let key = CIImageRepresentationOption(rawValue: kCGImageDestinationLossyCompressionQuality as String)
        guard let jpeg = context.jpegRepresentation(of: image, colorSpace: space, options: [key: 0.6]) else { return }
        let w = Int(image.extent.width.rounded()), h = Int(image.extent.height.rounded())
        DispatchQueue.main.async { self.onFrame?(jpeg, w, h, at) }
    }
}
