import Foundation

/// The kernel abstraction, Swift side: what `PlayerController` needs from a
/// playback engine. The desktop `IBackend` + `Player` facade is the
/// reference contract. The eventual `CxxEngine` (core xcframework batch)
/// implements this over `soar::Player`; `NullEngine` is the in-process
/// test double — same role as the desktop `NullBackend`.
public protocol MediaEngine: AnyObject {
    func open(_ source: MediaSource) -> Bool
    func close()
    func play() -> Bool
    func pause() -> Bool
    func stop() -> Bool

    func seek(to milliseconds: Int64) -> Bool
    func setRate(_ rate: Double) -> Bool
    func setVolume(_ volume01: Double) -> Bool
    func setMuted(_ muted: Bool) -> Bool

    var state: PlaybackState { get }
    var position: Int64 { get }
    var mediaInfo: MediaInfo { get }
    var lastError: String { get }

    /// Synchronous event emission — the engine calls this from whatever
    /// thread its work happens on; `PlayerController` hops to its own
    /// delivery context before surfacing to UI.
    var onEvent: ((SoarEvent) -> Void)? { get set }
}

/// In-process engine with the desktop `NullBackend` state machine —
/// open → stopped, play → playing (from ended: restart), pause → paused,
/// stop → stopped, natural end → ended. Time never advances on its own:
/// tests (and the controller's ticker, when attached) drive it with
/// `advance(by:)`, so no assertion ever bets on wall-clock speed.
///
/// State-only: no media is decoded — this is the iOS test double, not a
/// decoder. Real playback arrives with the core xcframework batch.
public final class NullEngine: MediaEngine {
    public private(set) var state: PlaybackState = .stopped
    public private(set) var position: Int64 = 0
    public private(set) var mediaInfo: MediaInfo = MediaInfo()
    public private(set) var lastError: String = ""

    public var onEvent: ((SoarEvent) -> Void)?

    /// Simulated clock rate; 1.0 = advance(by: 1000) consumes 1000ms.
    public var rate: Double = 1.0
    public private(set) var volume01: Double = 1.0
    public private(set) var muted: Bool = false

    private var source: MediaSource?

    public init() {}

    public func open(_ source: MediaSource) -> Bool {
        // Empty uri is the desktop contract's open failure.
        guard !source.uri.isEmpty else {
            lastError = "empty media source"
            transition(to: .error)
            return false
        }
        self.source = source
        position = 0
        lastError = ""
        mediaInfo = MediaInfo(
            duration: 0,
            seekable: true,
            title: URL(string: source.uri)?.lastPathComponent ?? source.uri
        )
        transition(to: .stopped)
        emit(.mediaInfoChanged)
        return true
    }

    public func close() {
        source = nil
        position = 0
        mediaInfo = MediaInfo()
        transition(to: .stopped)
    }

    public func play() -> Bool {
        guard source != nil else { return fail("no media source") }
        // ended → restart from the top (desktop null-backend contract).
        if state == .ended { position = 0 }
        transition(to: .playing)
        return true
    }

    public func pause() -> Bool {
        guard state == .playing else { return false }
        transition(to: .paused)
        return true
    }

    public func stop() -> Bool {
        guard source != nil else { return false }
        position = 0
        transition(to: .stopped)
        return true
    }

    public func seek(to milliseconds: Int64) -> Bool {
        guard source != nil else { return fail("no media source") }
        position = max(0, milliseconds)
        emit(.positionChanged)
        return true
    }

    public func setRate(_ newValue: Double) -> Bool {
        guard newValue > 0 else { return fail("invalid rate") }
        rate = newValue
        return true
    }

    public func setVolume(_ newValue: Double) -> Bool {
        guard (0.0...1.0).contains(newValue) else { return fail("volume out of range") }
        volume01 = newValue
        return true
    }

    public func setMuted(_ newValue: Bool) -> Bool {
        muted = newValue
        return true
    }

    /// Drive the simulated clock. Consumed only while playing; reaching
    /// duration ends playback (`.ended`), past duration clamps there.
    public func advance(by milliseconds: Int64) {
        guard state == .playing, milliseconds > 0 else { return }
        position += milliseconds
        let duration = mediaInfo.duration
        if duration > 0, position >= duration {
            position = duration
            transition(to: .ended)
        } else {
            emit(.positionChanged)
        }
    }

    public func setDuration(_ milliseconds: Int64) {
        mediaInfo.duration = max(0, milliseconds)
    }

    private func transition(to newState: PlaybackState) {
        guard state != newState else { return }
        state = newState
        emit(.stateChanged)
    }

    private func fail(_ message: String) -> Bool {
        lastError = message
        emit(.error, message: message)
        return false
    }

    private func emit(_ type: EventType, message: String = "") {
        onEvent?(SoarEvent(type: type,
                           position: position,
                           message: message))
    }
}
