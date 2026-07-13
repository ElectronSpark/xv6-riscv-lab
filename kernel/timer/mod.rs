//! Rust port of the kernel timer subsystem (Phase 2 Wave 8, see
//! `docs/rustify/phase2_plan.md`). `timer/timer.c` (the rb-tree/list timer
//! wheel + jiffies) is [`timer_core`]; `timer/sched_timer.c` (scheduler-tick
//! consumer + sleep/timeout API) is [`sched_timer`]; `timer/goldfish_rtc.c`
//! (RTC MMIO driver) is [`goldfish_rtc`]. This completes the `timer` module:
//! 100% Rust, no C or assembly left (unlike `irq`/`proc`, this directory
//! never had any `.S` sources).

pub mod goldfish_rtc;
pub mod sched_timer;
pub mod timer_core;
