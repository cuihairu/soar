import Foundation

/// The Swift-side playback facade — the type app code talks to. Owns an
/// engine, republishes its state on the main actor, and exposes the event
/// stream as `AsyncStream` (UI subscribes once; no per-event closures).
///
/// Not tied to SwiftUI: usable from any UI stack. SwiftUI views can still
/// observe it (`@StateObject`) since mutation happens on the main actor.
@MainActor
public final class PlayerController: ObservableObject {
    /// Published playback state (engine.state mirrored at delivery time).
    @Published public private(set) var state: PlaybackState = .stopped
    @Published public private(set) var position: Int64 = 0
    @Published public private(set) var mediaInfo: MediaInfo = MediaInfo()

    /// Last error message surfaced by the engine ("" when none).
    @Published public private(set) var lastError: String = ""

    /// Live event stream. `finish()` on deinit closes it; re-create the
    /// controller to resubscribe (one stream per controller, by design).
    public private(set) lazy var events: AsyncStream<SoarEvent> = makeEventStream()

    private let engine: MediaEngine
    private var continuations: [UUID: AsyncStream<SoarEvent>.Continuation] = [:]
    private var ticker: Task<Void, Never>?

    public init(engine: MediaEngine = NullEngine()) {
        self.engine = engine
        engine.onEvent = { [weak self] event in
            guard let self else { return }
            if Thread.isMainThread {
                // Fast path: main-thread engines (NullEngine in tests/UI)
                // deliver in order with zero hops.
                MainActor.assumeIsolated { self.apply(event) }
            } else {
                // Cross-thread hop. Ordering across multiple queued hops is
                // not guaranteed — when the Cxx engine lands, replace this
                // with a single serial pump if its callbacks are concurrent.
                Task { @MainActor [weak self] in
                    self?.apply(event)
                }
            }
        }
        mirror()
    }

    deinit {
        continuations.values.forEach { $0.finish() }
        ticker?.cancel()
    }

    // MARK: - Control (delegate to the engine, then mirror)

    @discardableResult
    public func open(_ source: MediaSource) -> Bool {
        let ok = engine.open(source)
        mirror()
        return ok
    }

    public func close() {
        stopTicker()
        engine.close()
        mirror()
    }

    @discardableResult
    public func play() -> Bool {
        let ok = engine.play()
        mirror()
        if ok, state == .playing { startTickerIfNeeded() }
        return ok
    }

    @discardableResult
    public func pause() -> Bool {
        let ok = engine.pause()
        mirror()
        return ok
    }

    @discardableResult
    public func stop() -> Bool {
        let ok = engine.stop()
        mirror()
        return ok
    }

    @discardableResult
    public func seek(to milliseconds: Int64) -> Bool {
        let ok = engine.seek(to: milliseconds)
        mirror()
        return ok
    }

    @discardableResult
    public func setRate(_ rate: Double) -> Bool { engine.setRate(rate) }

    @discardableResult
    public func setVolume(_ volume01: Double) -> Bool { engine.setVolume(volume01) }

    @discardableResult
    public func setMuted(_ muted: Bool) -> Bool { engine.setMuted(muted) }

    // MARK: - Event delivery

    private func makeEventStream() -> AsyncStream<SoarEvent> {
        AsyncStream { continuation in
            let id = UUID()
            continuations[id] = continuation
            continuation.onTermination = { [weak self] _ in
                Task { @MainActor [weak self] in
                    self?.continuations[id] = nil
                }
            }
        }
    }

    /// Engines may emit from their own thread; the init-wired callback
    /// funnels events through the main actor (apply) so published state
    /// and the event stream stay consistent.
    private func apply(_ event: SoarEvent) {
        mirror()
        continuations.values.forEach { $0.yield(event) }
    }

    private func mirror() {
        state = engine.state
        position = engine.position
        mediaInfo = engine.mediaInfo
        lastError = engine.lastError
    }

    // MARK: - Ticker (real-clock driver for UI preview; NullEngine only
    // advances when explicitly told, so tests never race the wall clock)

    private func startTickerIfNeeded() {
        guard ticker == nil else { return }
        // The Task inherits the main-actor context, so engine access and
        // mirror() are properly isolated; NullEngine only moves its
        // simulated clock when told, so tests never race this ticker.
        ticker = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 250_000_000)
                guard let self, self.state == .playing else { continue }
                if let null = self.engine as? NullEngine {
                    null.advance(by: 250)
                    self.mirror()
                }
            }
        }
    }

    private func stopTicker() {
        ticker?.cancel()
        ticker = nil
    }
}
