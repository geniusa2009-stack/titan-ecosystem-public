//! Resource-aware execution: memory-pressure detection and shedding.
//!
//! Every 5 s the server reads available memory (`/proc/meminfo` `MemAvailable`/`MemTotal`)
//! and, inside a cgroup v2 container, `memory.current`/`memory.max`. Pressure levels:
//! * `normal`;
//! * `high` (available < 15 % or < 512 MiB, or cgroup usage > 85 %): graph rebuilds are
//!   paused and the per-file facts cache of files that are not open is dropped;
//! * `critical` (available < 5 % or < 256 MiB, or cgroup usage > 95 %): the project graph
//!   itself is released as well.
//!
//! Editing, recovery journals, parsing of open documents and the language server are never
//! shed automatically: they are what the user is working with. The language server can be
//! stopped and started explicitly (`intel.server`). Nothing is evicted below `high`, so
//! semantic features stay usable on normal machines. On platforms without `/proc` the level
//! stays `unknown` and nothing is shed. `--simulate-memory-pressure` forces a level for tests.

use std::fs;
use std::time::{Duration, Instant};

use serde_json::{Value, json};
use titan_protocol::messages::notifications;

use super::Session;

const CHECK_EVERY: Duration = Duration::from_secs(5);

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(super) enum Pressure {
    Unknown,
    Normal,
    High,
    Critical,
}

impl Pressure {
    fn as_str(self) -> &'static str {
        match self {
            Self::Unknown => "unknown",
            Self::Normal => "normal",
            Self::High => "high",
            Self::Critical => "critical",
        }
    }
}

pub(super) struct ResourceState {
    pub level: Pressure,
    pub last_check: Option<Instant>,
    pub available_kib: Option<u64>,
    pub total_kib: Option<u64>,
    pub cgroup: Option<(u64, u64)>,
    pub sheds: u64,
}

impl Default for ResourceState {
    fn default() -> Self {
        Self {
            level: Pressure::Unknown,
            last_check: None,
            available_kib: None,
            total_kib: None,
            cgroup: None,
            sheds: 0,
        }
    }
}

fn meminfo() -> Option<(u64, u64)> {
    let text = fs::read_to_string("/proc/meminfo").ok()?;
    let field = |name: &str| -> Option<u64> {
        text.lines()
            .find(|l| l.starts_with(name))
            .and_then(|l| l.split_whitespace().nth(1))
            .and_then(|v| v.parse().ok())
    };
    Some((field("MemAvailable:")?, field("MemTotal:")?))
}

fn cgroup() -> Option<(u64, u64)> {
    let current: u64 = fs::read_to_string("/sys/fs/cgroup/memory.current")
        .ok()?
        .trim()
        .parse()
        .ok()?;
    let max: u64 = fs::read_to_string("/sys/fs/cgroup/memory.max")
        .ok()?
        .trim()
        .parse()
        .ok()?; // "max" (no limit) does not parse: no cgroup pressure
    Some((current, max))
}

impl Session {
    pub(super) fn check_resources(&mut self) {
        if self
            .resources
            .last_check
            .is_some_and(|t| t.elapsed() < CHECK_EVERY)
        {
            return;
        }
        self.resources.last_check = Some(Instant::now());
        let level = match self.options.simulated_pressure.as_deref() {
            Some("high") => Pressure::High,
            Some("critical") => Pressure::Critical,
            Some(_) => Pressure::Normal,
            None => {
                let mem = meminfo();
                let cg = cgroup();
                self.resources.available_kib = mem.map(|m| m.0);
                self.resources.total_kib = mem.map(|m| m.1);
                self.resources.cgroup = cg;
                match (mem, cg) {
                    (None, None) => Pressure::Unknown,
                    _ => {
                        let mut level = Pressure::Normal;
                        if let Some((avail, total)) = mem {
                            let frac = avail as f64 / total.max(1) as f64;
                            if frac < 0.05 || avail < 256 * 1024 {
                                level = Pressure::Critical;
                            } else if frac < 0.15 || avail < 512 * 1024 {
                                level = Pressure::High;
                            }
                        }
                        if let Some((current, max)) = cg {
                            let used = current as f64 / max.max(1) as f64;
                            let cg_level = if used > 0.95 {
                                Pressure::Critical
                            } else if used > 0.85 {
                                Pressure::High
                            } else {
                                Pressure::Normal
                            };
                            level = level.max(cg_level);
                        }
                        level
                    }
                }
            }
        };
        let previous = self.resources.level;
        self.resources.level = level;
        if level >= Pressure::High {
            self.shed(level);
        } else if previous >= Pressure::High {
            // Pressure is gone: resume graph maintenance.
            if self
                .graph
                .paused
                .as_deref()
                .is_some_and(|r| r.starts_with("memory pressure"))
            {
                self.graph.paused = None;
                self.mark_graph_stale();
            }
        }
        if level != previous && self.caps.semantic_notifications {
            self.push_notification(notifications::RESOURCE_PRESSURE, self.resources_json());
        }
    }

    /// Why graph maintenance must stay paused right now, if it must.
    pub(super) fn memory_pressure_reason(&self) -> Option<String> {
        (self.resources.level >= Pressure::High)
            .then(|| format!("memory pressure ({})", self.resources.level.as_str()))
    }

    fn shed(&mut self, level: Pressure) {
        self.resources.sheds += 1;
        if let Some(task) = self.graph.building.take() {
            self.pool.cancel(task);
        }
        self.graph.paused = self.memory_pressure_reason();
        let open: std::collections::HashSet<std::path::PathBuf> = self
            .documents
            .values()
            .filter_map(|od| self.workspace.as_ref()?.relative(od.doc.path()))
            .collect();
        self.graph.facts.retain(|path, _| open.contains(path));
        if level == Pressure::Critical {
            self.graph.graph = None;
        }
    }

    pub(super) fn resources_json(&self) -> Value {
        json!({
            "pressure": self.resources.level.as_str(),
            "simulated": self.options.simulated_pressure.is_some(),
            "memAvailableKib": self.resources.available_kib,
            "memTotalKib": self.resources.total_kib,
            "cgroup": self.resources.cgroup.map(|(c, m)| json!({ "currentBytes": c, "maxBytes": m })),
            "sheds": self.resources.sheds,
            "graphPaused": self.graph.paused,
            "workers": self.options.workers,
            "queueCapacity": self.options.queue_capacity,
            "policy": {
                "high": "pause graph rebuilds; drop cached facts of files that are not open",
                "critical": "also release the project graph",
                "neverShed": ["editing", "recovery journals", "parsing of open documents", "language server"],
            },
        })
    }
}
