//! Deterministic backend used by tests and for smoke-checking the pipeline.

use super::{Input, Provider};
use crate::error::Result;

pub struct Mock;

impl Provider for Mock {
    fn transcribe(&self, _input: &Input<'_>) -> Result<String> {
        Ok("hello world".to_string())
    }
}
