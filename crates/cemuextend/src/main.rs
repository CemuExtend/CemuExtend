//! Binary entrypoint for the CemuExtend CLI.

use std::process::ExitCode;

fn main() -> ExitCode {
    match cemuextend::run_cli(std::env::args_os()) {
        Ok(code) => ExitCode::from(code),
        Err(error) => {
            eprintln!("Cemu: {error}");
            ExitCode::FAILURE
        }
    }
}
