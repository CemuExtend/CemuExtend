//! Emits one exact synthetic RPX/RPL imported-data fixture.

use std::ffi::{OsStr, OsString};
use std::fs::{self, File};
use std::io::{self, Write};
use std::path::Path;
use std::process::ExitCode;

const HELP: &str = "Usage: cex-rpl-data-fixture --module main|provider --output FILE|-\n";

fn main() -> ExitCode {
    match run(std::env::args_os()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::from(error.exit_code())
        }
    }
}

fn run(arguments: impl IntoIterator<Item = OsString>) -> Result<(), FixtureError> {
    match Options::parse(arguments)? {
        Invocation::Help => {
            let stdout = io::stdout();
            let mut stdout = stdout.lock();
            write_fixture(&mut stdout, HELP.as_bytes())
        }
        Invocation::Run { module, output } => {
            publish_fixture(output.as_os_str(), &fixture(module)?)
        }
    }
}

struct Options;

#[derive(Clone, Eq, PartialEq)]
enum Invocation {
    Help,
    Run { module: Module, output: OsString },
}

impl std::fmt::Debug for Invocation {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Help => formatter.write_str("Help"),
            Self::Run { module, .. } => formatter
                .debug_struct("Run")
                .field("module", module)
                .field("output", &"<redacted>")
                .finish(),
        }
    }
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
        let mut module = None;
        let mut output = None;

        while let Some(argument) = arguments.next() {
            if matches!(argument.to_str(), Some("-h" | "--help")) {
                return if module.is_none() && output.is_none() && arguments.next().is_none() {
                    Ok(Invocation::Help)
                } else {
                    Err(FixtureError::Usage)
                };
            }
            match argument.to_str() {
                Some("--module") if module.is_none() => {
                    let value = arguments.next().ok_or(FixtureError::Usage)?;
                    module = Some(match value.to_str() {
                        Some("main") => Module::Main,
                        Some("provider") => Module::Provider,
                        _ => return Err(FixtureError::Usage),
                    });
                }
                Some("--output") if output.is_none() => {
                    output = Some(arguments.next().ok_or(FixtureError::Usage)?);
                }
                _ => return Err(FixtureError::Usage),
            }
        }

        match (module, output) {
            (Some(module), Some(output)) => Ok(Invocation::Run { module, output }),
            _ => Err(FixtureError::Usage),
        }
    }
}

fn fixture(module: Module) -> Result<Vec<u8>, FixtureError> {
    let bytes = match module {
        Module::Main => cex_system::synthetic_rpx_rpl_data_main_fixture(),
        Module::Provider => cex_system::synthetic_rpx_rpl_data_provider_fixture(),
    }
    .map_err(|_| FixtureError::Harness)?;
    if bytes.len() > cex_system::MAX_RPX_IMAGE_SIZE {
        return Err(FixtureError::Harness);
    }
    Ok(bytes)
}

fn publish_fixture(destination: &OsStr, bytes: &[u8]) -> Result<(), FixtureError> {
    if destination == OsStr::new("-") {
        let stdout = io::stdout();
        let mut stdout = stdout.lock();
        return write_fixture(&mut stdout, bytes);
    }

    let destination = Path::new(destination);
    let parent = destination
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let mut staged = private_tempfile_in(parent)?;
    staged
        .as_file_mut()
        .write_all(bytes)
        .and_then(|()| staged.as_file_mut().flush())
        .and_then(|()| staged.as_file().sync_all())
        .map_err(|_| FixtureError::Output)?;
    let published = staged
        .persist_noclobber(destination)
        .map_err(|_| FixtureError::Output)?;
    published.sync_all().map_err(|_| FixtureError::Output)?;
    File::open(parent)
        .and_then(|directory| directory.sync_all())
        .map_err(|_| FixtureError::Output)
}

fn private_tempfile_in(parent: &Path) -> Result<tempfile::NamedTempFile, FixtureError> {
    let mut builder = tempfile::Builder::new();
    set_private_permissions(&mut builder);
    let staged = builder
        .tempfile_in(parent)
        .map_err(|_| FixtureError::Output)?;
    enforce_private_permissions(staged.as_file())?;
    Ok(staged)
}

#[cfg(unix)]
fn set_private_permissions(builder: &mut tempfile::Builder<'_, '_>) {
    use std::os::unix::fs::PermissionsExt as _;
    builder.permissions(fs::Permissions::from_mode(0o600));
}

#[cfg(not(unix))]
fn set_private_permissions(_builder: &mut tempfile::Builder<'_, '_>) {}

#[cfg(unix)]
fn enforce_private_permissions(file: &File) -> Result<(), FixtureError> {
    use std::os::unix::fs::PermissionsExt as _;
    file.set_permissions(fs::Permissions::from_mode(0o600))
        .map_err(|_| FixtureError::Output)
}

#[cfg(not(unix))]
fn enforce_private_permissions(_file: &File) -> Result<(), FixtureError> {
    Ok(())
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

impl FixtureError {
    const fn exit_code(self) -> u8 {
        match self {
            Self::Harness => 1,
            Self::Usage | Self::Output => 2,
        }
    }
}

impl std::fmt::Display for FixtureError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(match self {
            Self::Usage => "cex-rpl-data-fixture: invalid arguments",
            Self::Output => "cex-rpl-data-fixture: failed to write fixture",
            Self::Harness => "cex-rpl-data-fixture: failed to construct fixture",
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn arguments_are_strict() {
        assert_eq!(
            Options::parse(["fixture", "--module", "main", "--output", "-"].map(OsString::from))
                .unwrap(),
            Invocation::Run {
                module: Module::Main,
                output: OsString::from("-"),
            }
        );
        assert_eq!(
            Options::parse(
                ["fixture", "--output", "result.rpx", "--module", "provider"].map(OsString::from)
            )
            .unwrap(),
            Invocation::Run {
                module: Module::Provider,
                output: OsString::from("result.rpx"),
            }
        );
        assert_eq!(
            Options::parse(["fixture", "--help"].map(OsString::from)).unwrap(),
            Invocation::Help
        );
        for arguments in [
            [
                "fixture", "--module", "main", "--module", "provider", "--output", "-",
            ]
            .as_slice(),
            [
                "fixture", "--module", "main", "--output", "-", "--output", "again",
            ]
            .as_slice(),
            ["fixture", "--module", "main"].as_slice(),
            ["fixture", "--output", "-"].as_slice(),
            ["fixture", "--unknown", "value"].as_slice(),
        ] {
            assert_eq!(
                Options::parse(arguments.iter().map(|argument| OsString::from(*argument))),
                Err(FixtureError::Usage)
            );
        }
    }

    #[test]
    fn selectors_emit_the_exact_production_fixtures() {
        assert_eq!(
            fixture(Module::Main).unwrap(),
            cex_system::synthetic_rpx_rpl_data_main_fixture().unwrap()
        );
        assert_eq!(
            fixture(Module::Provider).unwrap(),
            cex_system::synthetic_rpx_rpl_data_provider_fixture().unwrap()
        );
    }

    #[test]
    fn publication_never_clobbers_an_existing_fixture() {
        let directory = tempfile::tempdir().expect("temporary directory must be created");
        let destination = directory.path().join("existing.rpx");
        fs::write(&destination, b"preserve").expect("existing fixture must be written");

        assert_eq!(
            publish_fixture(destination.as_os_str(), b"replacement"),
            Err(FixtureError::Output)
        );
        assert_eq!(
            fs::read(&destination).expect("fixture must remain readable"),
            b"preserve"
        );
    }

    #[test]
    fn errors_do_not_echo_output_paths_in_display_or_debug() {
        let sentinel = OsString::from("/private/RPL_CALL_FIXTURE_SENTINEL.rpl");
        let error = Options::parse([
            OsString::from("fixture"),
            OsString::from("--output"),
            sentinel.clone(),
            OsString::from("--unexpected"),
        ])
        .unwrap_err();

        assert_eq!(error, FixtureError::Usage);
        assert!(
            !error
                .to_string()
                .contains(sentinel.to_string_lossy().as_ref())
        );
        assert!(!format!("{error:?}").contains(sentinel.to_string_lossy().as_ref()));
    }

    #[test]
    fn parsed_invocation_debug_does_not_echo_output_paths() {
        let sentinel = OsString::from("/private/RPL_CALL_FIXTURE_SENTINEL.rpl");
        let invocation = Options::parse([
            OsString::from("fixture"),
            OsString::from("--module"),
            OsString::from("main"),
            OsString::from("--output"),
            sentinel.clone(),
        ])
        .expect("valid invocation must parse");

        assert!(!format!("{invocation:?}").contains(sentinel.to_string_lossy().as_ref()));
    }

    #[cfg(unix)]
    #[test]
    fn published_fixture_is_private() {
        use std::os::unix::fs::PermissionsExt as _;

        let directory = tempfile::tempdir().unwrap();
        let destination = directory.path().join("fixture.rpx");
        publish_fixture(destination.as_os_str(), b"fixture").unwrap();
        assert_eq!(
            fs::metadata(destination).unwrap().permissions().mode() & 0o777,
            0o600
        );
    }
}
