//! Rust port of the kernel IRQ subsystem (Phase 2 Waves 5-7, see
//! `docs/rustify/phase2_plan.md`). `irq/plic.c` + `irq/irq.c` (Wave 5) are
//! [`plic`]/[`irq_core`]; `irq/trap.c` (Wave 6) is [`trap`] -- it owns the
//! `kernelvec.S`/`trampoline.S` asm contract (trap entry, syscall dispatch
//! entry, page-fault path, signal-frame push/restore); `irq/syscall.c`
//! (Wave 7) is [`syscall`] -- the syscall dispatch table and the
//! `fetchaddr`/`fetchstr`/`argraw`/`argint`/`argint64`/`argaddr`/`argstr`
//! argument-fetch helpers. This completes the `irq` module: 100% Rust
//! except `kernelvec.S`/`trampoline.S` themselves, which stay assembly.

pub mod irq_core;
pub mod plic;
pub mod syscall;
pub mod trap;
