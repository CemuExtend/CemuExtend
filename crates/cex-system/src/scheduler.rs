//! A guest-cycle-driven deterministic scheduler.

use cex_types::GuestCycle;
use std::collections::{BTreeMap, VecDeque};
use thiserror::Error;

/// Stable identifier allocated monotonically to a scheduled task.
#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub struct TaskId(u64);

impl TaskId {
    /// Return the stable numeric identifier.
    pub const fn get(self) -> u64 {
        self.0
    }
}

/// Result of executing one deterministic task slice.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TaskControl {
    /// Put the task at the back of the runnable queue.
    Yield,
    /// Remove the task permanently.
    Complete,
}

type Task = Box<dyn FnMut(GuestCycle) -> TaskControl + Send + 'static>;

/// FIFO cooperative scheduler whose only clock is guest cycles.
#[derive(Default)]
pub struct DeterministicScheduler {
    cycle: GuestCycle,
    next_task_id: u64,
    tasks: BTreeMap<TaskId, Task>,
    runnable: VecDeque<TaskId>,
}

impl DeterministicScheduler {
    /// Construct an empty scheduler at guest cycle zero.
    pub fn new() -> Self {
        Self::default()
    }

    /// Current guest time.
    pub const fn cycle(&self) -> GuestCycle {
        self.cycle
    }

    /// Add a task to the end of the runnable queue.
    pub fn spawn<F>(&mut self, task: F) -> Result<TaskId, ScheduleError>
    where
        F: FnMut(GuestCycle) -> TaskControl + Send + 'static,
    {
        let id = TaskId(self.next_task_id);
        self.next_task_id = self
            .next_task_id
            .checked_add(1)
            .ok_or(ScheduleError::TaskIdExhausted)?;
        self.tasks.insert(id, Box::new(task));
        self.runnable.push_back(id);
        Ok(id)
    }

    /// Run at most `budget` task slices, advancing one guest cycle per slice.
    pub fn run_slices(&mut self, budget: u64) -> Result<u64, ScheduleError> {
        let mut executed = 0_u64;
        while executed < budget {
            if self.runnable.is_empty() {
                break;
            }
            // Preflight guest time before invoking a task. A cycle overflow
            // must not allow task side effects to occur without advancing time.
            let next_cycle = self
                .cycle
                .checked_add(1)
                .ok_or(ScheduleError::GuestCycleExhausted)?;
            let id = self
                .runnable
                .pop_front()
                .ok_or(ScheduleError::RunnableQueueInvariant)?;
            let control = self
                .tasks
                .get_mut(&id)
                .ok_or(ScheduleError::MissingRunnableTask(id.get()))?(
                self.cycle
            );
            self.cycle = next_cycle;
            executed += 1;
            match control {
                TaskControl::Yield => self.runnable.push_back(id),
                TaskControl::Complete => {
                    self.tasks.remove(&id);
                }
            }
        }
        Ok(executed)
    }

    /// Whether no runnable tasks remain.
    pub fn is_idle(&self) -> bool {
        self.runnable.is_empty()
    }
}

/// A deterministic scheduler invariant failed.
#[derive(Clone, Debug, Error, Eq, PartialEq)]
pub enum ScheduleError {
    /// No more stable task identifiers can be allocated.
    #[error("task identifier space is exhausted")]
    TaskIdExhausted,
    /// Guest cycle time cannot advance further.
    #[error("guest cycle counter is exhausted")]
    GuestCycleExhausted,
    /// The runnable queue referenced a task that was not registered.
    #[error("runnable task {0} is missing")]
    MissingRunnableTask(u64),
    /// A non-empty queue became empty within a single scheduler operation.
    #[error("runnable queue invariant failed")]
    RunnableQueueInvariant,
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    };

    #[test]
    fn scheduling_order_is_repeatable() {
        let log = Arc::new(Mutex::new(Vec::new()));
        let mut scheduler = DeterministicScheduler::new();
        for label in ['a', 'b'] {
            let log = Arc::clone(&log);
            let mut calls = 0;
            scheduler
                .spawn(move |cycle| {
                    log.lock()
                        .expect("log mutex poisoned")
                        .push((label, cycle.get()));
                    calls += 1;
                    if calls == 2 {
                        TaskControl::Complete
                    } else {
                        TaskControl::Yield
                    }
                })
                .expect("spawn should succeed");
        }

        assert_eq!(scheduler.run_slices(10), Ok(4));
        assert!(scheduler.is_idle());
        assert_eq!(
            *log.lock().expect("log mutex poisoned"),
            [('a', 0), ('b', 1), ('a', 2), ('b', 3)]
        );
    }

    #[test]
    fn cycle_overflow_is_detected_before_task_side_effects() {
        let called = Arc::new(AtomicBool::new(false));
        let task_called = Arc::clone(&called);
        let mut scheduler = DeterministicScheduler::new();
        scheduler.cycle = GuestCycle::MAX;
        scheduler
            .spawn(move |_| {
                task_called.store(true, Ordering::SeqCst);
                TaskControl::Complete
            })
            .expect("spawn should succeed");

        assert_eq!(
            scheduler.run_slices(1),
            Err(ScheduleError::GuestCycleExhausted)
        );
        assert!(!called.load(Ordering::SeqCst));
    }
}
