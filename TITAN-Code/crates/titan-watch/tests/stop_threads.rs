//! `stop()` leaves no threads behind. Its own test binary (one test) so that no other test
//! starts or ends threads concurrently. Linux only: it counts `/proc/self/task`.

#![cfg(target_os = "linux")]

use std::fs;
use std::time::{Duration, Instant};

use titan_watch::{WatchConfig, Watcher};

fn threads() -> usize {
    fs::read_dir("/proc/self/task")
        .map(|dir| dir.count())
        .unwrap_or(0)
}

#[test]
fn stop_joins_the_aggregator_and_the_backend_thread_exits() {
    let tmp = tempfile::tempdir().unwrap();
    let baseline = threads();
    assert!(baseline > 0);

    for round in 0..3 {
        let watcher = Watcher::start(WatchConfig::new(tmp.path()), |_| {}).unwrap();
        fs::write(tmp.path().join(format!("f{round}.txt")), b"x").unwrap();
        let running = threads();
        // Aggregator + notify's inotify loop.
        assert!(
            running >= baseline + 2,
            "running {running}, baseline {baseline}"
        );

        let begun = Instant::now();
        watcher.stop();
        assert!(begun.elapsed() < Duration::from_secs(2));

        // Our aggregator is joined by `stop`; notify's inotify thread is detached and exits
        // after its shutdown message. Wait for it rather than sleeping a fixed time.
        let deadline = Instant::now() + Duration::from_secs(10);
        while threads() > baseline {
            assert!(
                Instant::now() < deadline,
                "threads still running after stop: {} > {baseline}",
                threads()
            );
            std::thread::sleep(Duration::from_millis(10));
        }
    }
}
