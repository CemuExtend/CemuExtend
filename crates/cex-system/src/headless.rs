//! Deterministic composition of loader, memory, and the PPC interpreter.

use std::collections::BTreeMap;

use cex_cpu::{Cpu, CpuFault, CpuState, ExecutionBudget, ExecutionOutcome};
use cex_memory::{GuestMemory, MemoryFault, PAGE_SIZE, Permissions};
use cex_types::GuestAddress;
use sha2::{Digest, Sha256};
use thiserror::Error;

use crate::rpl_link::commit_rpx_rpl_link_owned;
use crate::{
    ParsedRpx, ProgramDecodeError, RplModuleName, RpxError, RpxRplCommitError, RpxRplLinkProof,
    RpxRplPlanError, SyntheticProgram, parse_rpl, parse_rpx, plan_rpx_rpl_link,
};

const STACK_RESERVE: u64 = 64 * 1024;
const RPX_STACK_POINTER: GuestAddress = GuestAddress::new(0x4000_0000);
const DEFAULT_BUDGET: u64 = 10_000;

/// Stateless deterministic headless runtime.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HeadlessSystem {
    budget: ExecutionBudget,
}

impl HeadlessSystem {
    /// Construct a runner with independent instruction and guest-cycle limits.
    pub fn with_budget(max_instructions: u64, max_cycles: u64) -> Result<Self, HeadlessError> {
        if max_instructions == 0 || max_cycles == 0 {
            return Err(HeadlessError::ZeroBudget);
        }
        Ok(Self {
            budget: ExecutionBudget::new(max_instructions, max_cycles),
        })
    }

    /// Validate, map, and execute one synthetic program.
    ///
    /// Mapping is transactional from the caller's point of view: all work is
    /// performed in a fresh address space and no partially initialized system
    /// is returned if validation, protection, or execution fails.
    pub fn run(&self, program: &SyntheticProgram) -> Result<HeadlessRun, HeadlessError> {
        // `SyntheticProgram` fields are public for fixture ergonomics. Re-run
        // the complete wire validation here so callers cannot bypass size,
        // range, or entry-point constraints by constructing the struct directly.
        let canonical_image = program.encode()?;
        let program = SyntheticProgram::decode(&canonical_image)?;
        let mut memory = GuestMemory::new();
        map_code(&mut memory, &program)?;
        self.execute(
            memory,
            program.entry_point,
            program.stack_pointer,
            &canonical_image,
        )
    }

    /// Parse, map, and execute one complete uncompressed main RPX image.
    ///
    /// Imports, relocations, compressed sections, and RPL modules are outside
    /// this deliberately strict first slice. The image is parsed before a fresh
    /// sparse address space is created, and a page-aware mapping plan is fully
    /// validated before any mapping is installed.
    pub fn run_rpx(&self, image: &[u8]) -> Result<HeadlessRun, HeadlessError> {
        let rpx = parse_rpx(image)?;
        let mapping_plan = plan_rpx_mappings(&rpx)?;
        validate_rpx_stack(&mapping_plan)?;
        let mut memory = GuestMemory::new();

        // Loading never exposes an executable writable page: all ranges are
        // staged as data, populated, and only then changed to final permissions.
        for mapping in &mapping_plan {
            memory.map(
                mapping.start,
                mapping.len,
                Permissions::READ | Permissions::WRITE,
            )?;
        }
        for section in rpx
            .sections()
            .iter()
            .filter(|section| section.is_allocated() && !section.data().is_empty())
        {
            memory.write(GuestAddress::new(section.virtual_address()), section.data())?;
        }
        for mapping in &mapping_plan {
            memory.protect(mapping.start, mapping.len, mapping.permissions)?;
        }

        self.execute(
            memory,
            GuestAddress::new(rpx.entry_point()),
            RPX_STACK_POINTER,
            image,
        )
    }

    /// Parse, link, and execute one main RPX with one named RPL provider.
    ///
    /// The complete link is planned before fresh guest memory is committed.
    /// Committed text, data, and loader ranges already have their final
    /// permissions before the stack is mapped and the CPU can fetch from them.
    /// Any stack mapping or execution failure drops that private address space
    /// and returns neither a run nor a link proof.
    pub fn run_rpx_rpl(
        &self,
        main_image: &[u8],
        provider_name: RplModuleName,
        provider_image: &[u8],
    ) -> Result<RpxRplHeadlessRun, HeadlessError> {
        let main = parse_rpx(main_image)?;
        let provider = parse_rpl(provider_image)?;
        let plan = plan_rpx_rpl_link(main, provider_name, provider)?;
        let committed = commit_rpx_rpl_link_owned(plan)?;
        let (memory, link_proof) = committed.into_parts();
        let execution = self.execute_with_program_hash(
            memory,
            GuestAddress::new(link_proof.main_entry()),
            RPX_STACK_POINTER,
            linked_program_hash(&link_proof),
        )?;

        Ok(RpxRplHeadlessRun {
            execution,
            link_proof,
        })
    }

    fn execute(
        &self,
        memory: GuestMemory,
        entry_point: GuestAddress,
        stack_pointer: GuestAddress,
        artifact: &[u8],
    ) -> Result<HeadlessRun, HeadlessError> {
        self.execute_with_program_hash(
            memory,
            entry_point,
            stack_pointer,
            Sha256::digest(artifact).into(),
        )
    }

    fn execute_with_program_hash(
        &self,
        mut memory: GuestMemory,
        entry_point: GuestAddress,
        stack_pointer: GuestAddress,
        program_hash: [u8; 32],
    ) -> Result<HeadlessRun, HeadlessError> {
        map_stack(&mut memory, stack_pointer)?;

        let mut state = CpuState::new(entry_point);
        state.gprs[1] = stack_pointer.get();
        let mut cpu = Cpu::with_state(state);
        let outcome = cpu.run_with_budget(&mut memory, self.budget)?;
        let final_state = cpu.into_state();

        Ok(HeadlessRun {
            final_state,
            outcome,
            memory_hash: memory.deterministic_hash(),
            mapped_page_count: memory.mapped_page_count(),
            program_hash,
        })
    }
}

impl Default for HeadlessSystem {
    fn default() -> Self {
        Self {
            budget: ExecutionBudget::new(DEFAULT_BUDGET, DEFAULT_BUDGET),
        }
    }
}

/// Canonical results retained after a successful bounded execution.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HeadlessRun {
    /// Final architectural register and cycle state.
    pub final_state: CpuState,
    /// Explicit halt, syscall, or budget outcome.
    pub outcome: ExecutionOutcome,
    /// Deterministic digest of mapped ranges, permissions, and contents.
    pub memory_hash: [u8; 32],
    /// Number of logical pages represented by the memory digest.
    pub mapped_page_count: u64,
    /// SHA-256 artifact identity: the direct input for one image, or a
    /// domain-separated composition of canonical module hashes for a link.
    pub program_hash: [u8; 32],
}

/// Successful bounded execution of one transactionally linked RPX/RPL pair.
///
/// This retains only architectural execution state plus numeric and hash
/// evidence. It contains no input payloads, paths, or module-name strings.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RpxRplHeadlessRun {
    /// Canonical CPU outcome and post-execution memory evidence.
    pub execution: HeadlessRun,
    /// Immutable evidence captured after relocation and final protection.
    pub link_proof: RpxRplLinkProof,
}

fn linked_program_hash(proof: &RpxRplLinkProof) -> [u8; 32] {
    let mut hash = Sha256::new();
    hash.update(b"CemuExtend linked RPX/RPL headless program v1\0");
    hash.update(proof.main_sha256());
    hash.update(proof.provider_sha256());
    hash.finalize().into()
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct PlannedMapping {
    start: GuestAddress,
    len: u64,
    permissions: Permissions,
}

fn plan_rpx_mappings(rpx: &ParsedRpx) -> Result<Vec<PlannedMapping>, HeadlessError> {
    let page_mask = PAGE_SIZE - 1;
    let mut pages = BTreeMap::<u32, Permissions>::new();

    for section in rpx
        .sections()
        .iter()
        .filter(|section| section.is_allocated())
    {
        if section.data().is_empty() {
            continue;
        }
        let start = u64::from(section.virtual_address());
        let len =
            u64::try_from(section.data().len()).map_err(|_| HeadlessError::AddressOverflow)?;
        let end = start
            .checked_add(len)
            .ok_or(HeadlessError::AddressOverflow)?;
        let first_page = u32::try_from((start & !page_mask) / PAGE_SIZE)
            .map_err(|_| HeadlessError::AddressOverflow)?;
        let end_page = u32::try_from(
            end.checked_add(page_mask)
                .ok_or(HeadlessError::AddressOverflow)?
                / PAGE_SIZE,
        )
        .map_err(|_| HeadlessError::AddressOverflow)?;
        let mut permissions = Permissions::READ;
        if section.is_writable() {
            permissions |= Permissions::WRITE;
        }
        if section.is_executable() {
            permissions |= Permissions::EXECUTE;
        }

        for page in first_page..end_page {
            *pages.entry(page).or_insert(Permissions::empty()) |= permissions;
        }
    }

    if let Some((&page, _)) = pages
        .iter()
        .find(|(_, permissions)| permissions.contains(Permissions::WRITE | Permissions::EXECUTE))
    {
        let address = GuestAddress::try_from(u64::from(page) * PAGE_SIZE)
            .map_err(|_| HeadlessError::AddressOverflow)?;
        return Err(HeadlessError::RpxPageWriteExecute(address));
    }

    let mut mappings = Vec::new();
    let mut current: Option<(u32, u32, Permissions)> = None;
    for (page, permissions) in pages {
        match current {
            Some((start, end, current_permissions))
                if page == end && permissions == current_permissions =>
            {
                current = Some((start, end + 1, current_permissions));
            }
            Some(mapping) => {
                mappings.push(finish_mapping(mapping)?);
                current = Some((page, page + 1, permissions));
            }
            None => current = Some((page, page + 1, permissions)),
        }
    }
    if let Some(mapping) = current {
        mappings.push(finish_mapping(mapping)?);
    }
    Ok(mappings)
}

fn finish_mapping(
    (start_page, end_page, permissions): (u32, u32, Permissions),
) -> Result<PlannedMapping, HeadlessError> {
    let start = u64::from(start_page) * PAGE_SIZE;
    Ok(PlannedMapping {
        start: GuestAddress::try_from(start).map_err(|_| HeadlessError::AddressOverflow)?,
        len: u64::from(end_page - start_page) * PAGE_SIZE,
        permissions,
    })
}

fn validate_rpx_stack(mapping_plan: &[PlannedMapping]) -> Result<(), HeadlessError> {
    let stack_end = u64::from(RPX_STACK_POINTER.get());
    let stack_start = stack_end - STACK_RESERVE;
    for mapping in mapping_plan {
        let mapping_start = u64::from(mapping.start.get());
        let mapping_end = mapping_start
            .checked_add(mapping.len)
            .ok_or(HeadlessError::AddressOverflow)?;
        if mapping_start < stack_end && stack_start < mapping_end {
            let overlap = mapping_start.max(stack_start);
            let address =
                GuestAddress::try_from(overlap).map_err(|_| HeadlessError::AddressOverflow)?;
            return Err(HeadlessError::RpxStackOverlap(address));
        }
    }
    Ok(())
}

fn map_code(memory: &mut GuestMemory, program: &SyntheticProgram) -> Result<(), HeadlessError> {
    if program.code.is_empty() || !program.code.len().is_multiple_of(4) {
        return Err(HeadlessError::InvalidCodeLength(program.code.len()));
    }
    let page_mask = PAGE_SIZE - 1;
    let load = u64::from(program.load_address.get());
    let mapping_start = load & !page_mask;
    let offset = load - mapping_start;
    let occupied = offset
        .checked_add(u64::try_from(program.code.len()).map_err(|_| HeadlessError::AddressOverflow)?)
        .ok_or(HeadlessError::AddressOverflow)?;
    let mapping_len = occupied
        .checked_add(page_mask)
        .ok_or(HeadlessError::AddressOverflow)?
        & !page_mask;
    let mapping_start =
        GuestAddress::try_from(mapping_start).map_err(|_| HeadlessError::AddressOverflow)?;

    memory.map(
        mapping_start,
        mapping_len,
        Permissions::READ | Permissions::WRITE,
    )?;
    memory.write(program.load_address, &program.code)?;
    memory.protect(
        mapping_start,
        mapping_len,
        Permissions::READ | Permissions::EXECUTE,
    )?;
    Ok(())
}

fn map_stack(memory: &mut GuestMemory, stack_pointer: GuestAddress) -> Result<(), HeadlessError> {
    let raw = u64::from(stack_pointer.get());
    if raw < STACK_RESERVE || raw % PAGE_SIZE != 0 {
        return Err(HeadlessError::InvalidStackPointer(stack_pointer));
    }
    let start =
        GuestAddress::try_from(raw - STACK_RESERVE).map_err(|_| HeadlessError::AddressOverflow)?;
    memory.map(start, STACK_RESERVE, Permissions::READ | Permissions::WRITE)?;
    Ok(())
}

/// Validation, mapping, or execution failure in the headless system.
#[derive(Debug, Error)]
pub enum HeadlessError {
    /// Both execution bounds must allow at least one step.
    #[error("instruction and cycle budgets must both be non-zero")]
    ZeroBudget,
    /// Code must contain complete PPC words.
    #[error("synthetic program has invalid code length {0}")]
    InvalidCodeLength(usize),
    /// The stack pointer must be a page-aligned exclusive stack end.
    #[error("invalid headless stack pointer {0}")]
    InvalidStackPointer(GuestAddress),
    /// Host arithmetic could not represent a guest mapping.
    #[error("headless program mapping overflows guest address space")]
    AddressOverflow,
    /// Synthetic program wire validation failed at the execution boundary.
    #[error(transparent)]
    Program(#[from] ProgramDecodeError),
    /// Strict RPX parsing failed before mapping began.
    #[error(transparent)]
    Rpx(#[from] RpxError),
    /// Transactional RPX/RPL link planning failed before mapping began.
    #[error(transparent)]
    RpxRplPlan(#[from] RpxRplPlanError),
    /// Transactional RPX/RPL commit failed without publishing guest memory.
    #[error(transparent)]
    RpxRplCommit(#[from] RpxRplCommitError),
    /// Page-granular permissions cannot safely represent the section layout.
    #[error("RPX sections require writable executable guest page {0}")]
    RpxPageWriteExecute(GuestAddress),
    /// An RPX section collides with the deterministic headless stack reserve.
    #[error("RPX section overlaps the headless stack at guest address {0}")]
    RpxStackOverlap(GuestAddress),
    /// Guest memory mapping or access failed.
    #[error("failed to initialize guest memory: {0}")]
    Memory(#[from] MemoryFault),
    /// PPC execution faulted before reaching an explicit stop.
    #[error("PPC execution failed: {0}")]
    Cpu(#[from] CpuFault),
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        builtin_fixture, synthetic_rpx_rpl_call_main_fixture,
        synthetic_rpx_rpl_call_provider_fixture,
    };
    use cex_cpu::{BudgetKind, StopReason};

    fn call_fixture() -> (Vec<u8>, RplModuleName, Vec<u8>) {
        (
            synthetic_rpx_rpl_call_main_fixture().expect("main fixture must build"),
            RplModuleName::new("linkmod.rpl").expect("fixture module name is canonical"),
            synthetic_rpx_rpl_call_provider_fixture().expect("provider fixture must build"),
        )
    }

    #[test]
    fn bundled_fixture_returns_42_and_stops() {
        let run = HeadlessSystem::default()
            .run(&builtin_fixture())
            .expect("public fixture must execute");

        assert_eq!(run.final_state.gpr(3), Some(42));
        assert_eq!(run.final_state.instructions_retired, 3);
        assert_eq!(run.outcome.reason, StopReason::StopSentinel);
        assert_eq!(run.outcome.instructions_executed, 3);
    }

    #[test]
    fn looping_program_stops_at_the_instruction_bound() {
        let mut program = builtin_fixture();
        program.code = 0x4800_0000_u32.to_be_bytes().to_vec(); // b .
        let run = HeadlessSystem::with_budget(4, 10)
            .expect("non-zero budgets are valid")
            .run(&program)
            .expect("bounded loop must be a normal outcome");

        assert_eq!(
            run.outcome.reason,
            StopReason::BudgetExhausted {
                kind: BudgetKind::Instructions
            }
        );
        assert_eq!(run.outcome.instructions_executed, 4);
    }

    #[test]
    fn linked_rel24_call_returns_42_with_exact_state_and_repeatable_proof() {
        let (main, provider_name, provider) = call_fixture();
        let first = HeadlessSystem::default()
            .run_rpx_rpl(&main, provider_name.clone(), &provider)
            .expect("linked call fixture must execute");
        let second = HeadlessSystem::default()
            .run_rpx_rpl(&main, provider_name, &provider)
            .expect("repeated linked call fixture must execute");

        assert_eq!(first, second);
        assert_eq!(first.link_proof.main_entry(), 0x0200_0000);
        assert_eq!(first.link_proof.import_relocations()[0].site(), 0x0200_0000);
        assert_eq!(
            first.link_proof.import_relocations()[0].after(),
            0x4800_2001
        );
        assert_eq!(first.link_proof.mapped_page_count(), 5);
        assert_eq!(first.execution.mapped_page_count, 21);
        assert_eq!(first.execution.final_state.gpr(1), Some(0x4000_0000));
        assert_eq!(first.execution.final_state.gpr(3), Some(42));
        assert_eq!(first.execution.final_state.link_register, 0x0200_0004);
        assert_eq!(
            first.execution.final_state.instruction_pointer,
            GuestAddress::new(0x0200_0008)
        );
        assert_eq!(first.execution.final_state.instructions_retired, 4);
        assert_eq!(first.execution.final_state.cycles.get(), 4);
        assert_eq!(first.execution.outcome.reason, StopReason::StopSentinel);
        assert_eq!(first.execution.outcome.instructions_executed, 4);
        assert_eq!(first.execution.outcome.cycles_elapsed, 4);
    }

    #[test]
    fn linked_rel24_call_reports_cycle_budget_before_stop() {
        let (main, provider_name, provider) = call_fixture();
        let run = HeadlessSystem::with_budget(10, 3)
            .expect("non-zero budgets are valid")
            .run_rpx_rpl(&main, provider_name, &provider)
            .expect("budget exhaustion is a normal outcome");

        assert_eq!(
            run.execution.outcome.reason,
            StopReason::BudgetExhausted {
                kind: BudgetKind::Cycles
            }
        );
        assert_ne!(run.execution.outcome.reason, StopReason::StopSentinel);
        assert_eq!(run.execution.final_state.instructions_retired, 3);
        assert_eq!(run.execution.final_state.cycles.get(), 3);
    }

    #[test]
    fn linked_image_is_final_protected_without_writable_executable_pages() {
        let (main, provider_name, provider) = call_fixture();
        let plan = plan_rpx_rpl_link(
            parse_rpx(&main).expect("main fixture must parse"),
            provider_name,
            parse_rpl(&provider).expect("provider fixture must parse"),
        )
        .expect("call fixture must plan");
        let (memory, proof) = commit_rpx_rpl_link_owned(plan)
            .expect("call fixture must commit")
            .into_parts();
        let mappings: Vec<_> = memory.mappings().collect();

        assert_eq!(memory.mapped_page_count(), 5);
        assert_eq!(proof.mapped_page_count(), 5);
        assert!(mappings.iter().all(|mapping| {
            !mapping
                .permissions
                .contains(Permissions::WRITE | Permissions::EXECUTE)
        }));
        assert!(
            mappings
                .iter()
                .any(|mapping| { mapping.permissions == Permissions::READ | Permissions::EXECUTE })
        );
        assert!(
            mappings
                .iter()
                .any(|mapping| mapping.permissions == Permissions::READ | Permissions::WRITE)
        );
        assert!(
            mappings
                .iter()
                .any(|mapping| mapping.permissions == Permissions::READ)
        );
    }

    #[test]
    fn linked_parse_and_plan_failures_publish_no_run_or_proof() {
        let (main, _, provider) = call_fixture();
        let malformed_main = HeadlessSystem::default().run_rpx_rpl(
            &[0],
            RplModuleName::new("linkmod.rpl").expect("fixture name is canonical"),
            &provider,
        );
        assert!(matches!(malformed_main, Err(HeadlessError::Rpx(_))));

        let malformed_provider = HeadlessSystem::default().run_rpx_rpl(
            &main,
            RplModuleName::new("linkmod.rpl").expect("fixture name is canonical"),
            &[0],
        );
        assert!(matches!(malformed_provider, Err(HeadlessError::Rpx(_))));

        let unresolved = HeadlessSystem::default().run_rpx_rpl(
            &main,
            RplModuleName::new("other.rpl").expect("alternate name is canonical"),
            &provider,
        );
        assert!(matches!(
            unresolved,
            Err(HeadlessError::RpxRplPlan(
                RpxRplPlanError::UnresolvedImport { .. }
            ))
        ));
    }
}
