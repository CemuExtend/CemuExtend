//! Emits the deterministic synthetic RPX fixture used by contract tests.

use std::io::{self, Write};
use std::process::ExitCode;

fn main() -> ExitCode {
    match run(std::env::args_os().skip(1)) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::from(2)
        }
    }
}

fn run(arguments: impl IntoIterator<Item = std::ffi::OsString>) -> Result<(), FixtureError> {
    if arguments.into_iter().next().is_some() {
        return Err(FixtureError::Usage);
    }

    let stdout = io::stdout();
    let mut stdout = stdout.lock();
    write_fixture(&mut stdout)
}

fn write_fixture(output: &mut impl Write) -> Result<(), FixtureError> {
    output
        .write_all(&cex_system::builtin_rpx_fixture())
        .map_err(|_| FixtureError::Output)?;
    output.flush().map_err(|_| FixtureError::Output)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum FixtureError {
    Usage,
    Output,
}

impl std::fmt::Display for FixtureError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            Self::Usage => "cex-rpx-fixture: invalid arguments",
            Self::Output => "cex-rpx-fixture: failed to write fixture",
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writes_the_exact_builtin_fixture_bytes() {
        let mut output = Vec::new();
        write_fixture(&mut output).expect("fixture must be writable to memory");

        assert_eq!(output, cex_system::builtin_rpx_fixture());
    }

    #[test]
    fn arguments_are_rejected_without_echoing_them() {
        let secret = std::ffi::OsString::from("/private/fixture-secret.rpx");
        let error = run([secret.clone()]).expect_err("arguments must be rejected");

        assert_eq!(error, FixtureError::Usage);
        assert!(
            !error
                .to_string()
                .contains(secret.to_string_lossy().as_ref())
        );
        assert!(!format!("{error:?}").contains(secret.to_string_lossy().as_ref()));
    }
}
