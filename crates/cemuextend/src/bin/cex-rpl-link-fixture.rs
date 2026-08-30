//! Emits one exact synthetic RPX/RPL link fixture to standard output.

use std::ffi::OsString;
use std::io::{self, Write};
use std::process::ExitCode;

const HELP: &str = "Usage: cex-rpl-link-fixture --module main|provider\n";

fn main() -> ExitCode {
    match run(std::env::args_os()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::from(2)
        }
    }
}

fn run(arguments: impl IntoIterator<Item = OsString>) -> Result<(), FixtureError> {
    match Options::parse(arguments)? {
        Invocation::Help => {
            let stdout = io::stdout();
            let mut stdout = stdout.lock();
            stdout
                .write_all(HELP.as_bytes())
                .and_then(|()| stdout.flush())
                .map_err(|_| FixtureError::Output)
        }
        Invocation::Run(module) => {
            let bytes = fixture(module)?;
            let stdout = io::stdout();
            let mut stdout = stdout.lock();
            write_fixture(&mut stdout, &bytes)
        }
    }
}

struct Options;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Invocation {
    Help,
    Run(Module),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Module {
    Main,
    Provider,
}

impl Options {
    fn parse(arguments: impl IntoIterator<Item = OsString>) -> Result<Invocation, FixtureError> {
        let mut arguments = arguments.into_iter();
        let _program_name = arguments.next();
        let Some(argument) = arguments.next() else {
            return Err(FixtureError::Usage);
        };
        if matches!(argument.to_str(), Some("-h" | "--help")) {
            return if arguments.next().is_none() {
                Ok(Invocation::Help)
            } else {
                Err(FixtureError::Usage)
            };
        }
        if argument.to_str() != Some("--module") {
            return Err(FixtureError::Usage);
        }
        let value = arguments.next().ok_or(FixtureError::Usage)?;
        if arguments.next().is_some() {
            return Err(FixtureError::Usage);
        }
        let module = match value.into_string().ok() {
            Some(value) if value == "main" => Module::Main,
            Some(value) if value == "provider" => Module::Provider,
            _ => return Err(FixtureError::Usage),
        };
        Ok(Invocation::Run(module))
    }
}

fn fixture(module: Module) -> Result<Vec<u8>, FixtureError> {
    match module {
        Module::Main => cex_system::synthetic_rpx_rpl_link_main_fixture(),
        Module::Provider => cex_system::synthetic_rpx_rpl_link_provider_fixture(),
    }
    .map_err(|_| FixtureError::Harness)
}

fn write_fixture(output: &mut impl Write, bytes: &[u8]) -> Result<(), FixtureError> {
    output
        .write_all(bytes)
        .and_then(|()| output.flush())
        .map_err(|_| FixtureError::Output)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum FixtureError {
    Usage,
    Output,
    Harness,
}

impl std::fmt::Display for FixtureError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            Self::Usage => "cex-rpl-link-fixture: invalid arguments",
            Self::Output => "cex-rpl-link-fixture: failed to write fixture",
            Self::Harness => "cex-rpl-link-fixture: failed to construct fixture",
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn selectors_emit_the_exact_production_fixtures() {
        assert_eq!(
            fixture(Module::Main).unwrap(),
            cex_system::synthetic_rpx_rpl_link_main_fixture().unwrap()
        );
        assert_eq!(
            fixture(Module::Provider).unwrap(),
            cex_system::synthetic_rpx_rpl_link_provider_fixture().unwrap()
        );
    }

    #[test]
    fn arguments_are_strict_and_never_echoed() {
        assert_eq!(
            Options::parse(["fixture", "--module", "main"].map(OsString::from)).unwrap(),
            Invocation::Run(Module::Main)
        );
        assert_eq!(
            Options::parse(["fixture", "--module", "provider"].map(OsString::from)).unwrap(),
            Invocation::Run(Module::Provider)
        );
        let sentinel = OsString::from("/private/RPL_FIXTURE_SENTINEL.rpl");
        let error = Options::parse([OsString::from("fixture"), sentinel.clone()]).unwrap_err();
        assert_eq!(error, FixtureError::Usage);
        assert!(
            !error
                .to_string()
                .contains(sentinel.to_string_lossy().as_ref())
        );
        assert!(!format!("{error:?}").contains(sentinel.to_string_lossy().as_ref()));
    }
}
