import XCTest
@testable import SoarKit

/// NullEngine state machine — mirrors the desktop NullBackend contract
/// (open → stopped, play → playing / ended-restarts, pause only while
/// playing, stop resets, natural end → ended). Clock is manual.
final class NullEngineTests: XCTestCase {
    private func openedEngine(duration: Int64 = 10_000) -> NullEngine {
        let engine = NullEngine()
        XCTAssertTrue(engine.open(MediaSource(uri: "file:///tmp/clip.mp4")))
        engine.setDuration(duration)
        return engine
    }

    func testOpenRejectsEmptySource() {
        let engine = NullEngine()
        XCTAssertFalse(engine.open(MediaSource(uri: "")))
        XCTAssertEqual(engine.state, .error)
        XCTAssertEqual(engine.lastError, "empty media source")
    }

    func testOpenPublishesInfoAndStops() {
        let engine = openedEngine()
        XCTAssertEqual(engine.state, .stopped)
        XCTAssertEqual(engine.mediaInfo.title, "clip.mp4")
        XCTAssertTrue(engine.mediaInfo.seekable)
        XCTAssertEqual(engine.position, 0)
    }

    func testPlayPauseStopTransitions() {
        let engine = openedEngine()
        XCTAssertTrue(engine.play())
        XCTAssertEqual(engine.state, .playing)
        XCTAssertTrue(engine.pause())
        XCTAssertEqual(engine.state, .paused)
        // pause is only valid while playing (desktop contract)
        XCTAssertFalse(engine.pause())
        XCTAssertTrue(engine.play())
        XCTAssertTrue(engine.stop())
        XCTAssertEqual(engine.state, .stopped)
        XCTAssertEqual(engine.position, 0)
    }

    func testPlayWithoutSourceFails() {
        let engine = NullEngine()
        XCTAssertFalse(engine.play())
        XCTAssertEqual(engine.lastError, "no media source")
    }

    func testAdvanceConsumesOnlyWhilePlayingAndEndsAtDuration() {
        let engine = openedEngine(duration: 5_000)
        engine.advance(by: 1_000)
        XCTAssertEqual(engine.position, 0, "idle clock must not move")

        XCTAssertTrue(engine.play())
        engine.advance(by: 3_000)
        XCTAssertEqual(engine.position, 3_000)

        engine.advance(by: 9_000)
        XCTAssertEqual(engine.position, 5_000, "clamp at duration")
        XCTAssertEqual(engine.state, .ended)
    }

    func testPlayFromEndedRestarts() {
        let engine = openedEngine(duration: 1_000)
        XCTAssertTrue(engine.play())
        engine.advance(by: 1_000)
        XCTAssertEqual(engine.state, .ended)
        XCTAssertTrue(engine.play())
        XCTAssertEqual(engine.state, .playing)
        XCTAssertEqual(engine.position, 0)
    }

    func testSeekClampsToZero() {
        let engine = openedEngine()
        XCTAssertTrue(engine.seek(to: -50))
        XCTAssertEqual(engine.position, 0)
        XCTAssertTrue(engine.seek(to: 2_500))
        XCTAssertEqual(engine.position, 2_500)
    }

    func testRateAndVolumeValidation() {
        let engine = openedEngine()
        XCTAssertFalse(engine.setRate(0))
        XCTAssertFalse(engine.setRate(-1.5))
        XCTAssertTrue(engine.setRate(2.0))
        XCTAssertFalse(engine.setVolume(1.5))
        XCTAssertTrue(engine.setVolume(0.4))
        XCTAssertTrue(engine.setMuted(true))
    }

    func testEventsFollowTransitions() {
        // onEvent must be attached BEFORE open — the engine emits
        // synchronously, so a late subscriber misses those events.
        let engine = NullEngine()
        var events: [SoarEvent] = []
        engine.onEvent = { events.append($0) }

        XCTAssertTrue(engine.open(MediaSource(uri: "file:///tmp/clip.mp4")))
        engine.setDuration(10_000)

        XCTAssertTrue(engine.play())
        engine.advance(by: 500)

        // open → mediaInfoChanged only (init is already .stopped, and
        // transition() skips no-op transitions); play → stateChanged;
        // advance below duration → positionChanged.
        XCTAssertEqual(events.map(\.type),
                       [.mediaInfoChanged, .stateChanged, .positionChanged])
    }
}

/// Controller: state mirroring, error surfacing, and the event stream
/// (continuation is registered on first access, so subscriptions placed
/// before an action never miss its events — unbounded buffering).
@MainActor
final class PlayerControllerTests: XCTestCase {
    func testControlMethodsMirrorEngineState() {
        let controller = PlayerController()
        XCTAssertEqual(controller.state, .stopped)

        XCTAssertTrue(controller.open(MediaSource(uri: "file:///tmp/a.mp4")))
        XCTAssertEqual(controller.state, .stopped)
        XCTAssertEqual(controller.mediaInfo.title, "a.mp4")

        XCTAssertTrue(controller.play())
        XCTAssertEqual(controller.state, .playing)

        XCTAssertTrue(controller.pause())
        XCTAssertEqual(controller.state, .paused)

        XCTAssertTrue(controller.stop())
        XCTAssertEqual(controller.state, .stopped)
        XCTAssertEqual(controller.position, 0)
    }

    func testOpenFailureSurfacesError() {
        let controller = PlayerController()
        XCTAssertFalse(controller.open(MediaSource(uri: "")))
        XCTAssertEqual(controller.state, .error)
        XCTAssertEqual(controller.lastError, "empty media source")
    }

    func testEventStreamDeliversControlEvents() async throws {
        let controller = PlayerController()
        let stream = controller.events  // registers the continuation now

        let seen = expectation(description: "state + mediaInfo + position seen")
        let collector = Task {
            var types: [EventType] = []
            for await event in stream {
                types.append(event.type)
                if types.count == 3 {
                    seen.fulfill()
                    break
                }
            }
        }

        XCTAssertTrue(controller.open(MediaSource(uri: "file:///tmp/a.mp4")))
        XCTAssertTrue(controller.play())
        XCTAssertTrue(controller.seek(to: 1_000))

        await fulfillment(of: [seen], timeout: 2)
        collector.cancel()

        XCTAssertEqual(controller.position, 1_000)
    }

    func testEndedThenPlayRestartsThroughController() {
        let engine = NullEngine()
        let controller = PlayerController(engine: engine)
        XCTAssertTrue(controller.open(MediaSource(uri: "file:///tmp/a.mp4")))
        engine.setDuration(1)
        XCTAssertTrue(controller.play())
        engine.advance(by: 1)
        XCTAssertEqual(controller.state, .ended)
        XCTAssertTrue(controller.play())
        XCTAssertEqual(controller.state, .playing)
        XCTAssertEqual(controller.position, 0)
    }
}
