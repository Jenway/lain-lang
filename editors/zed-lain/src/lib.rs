//! Lain 的 Zed 扩展：注册 Lain 语言服务器（lain-lsp）。

use zed_extension_api::{self as zed, Command, LanguageServerId, Result, Worktree};

const LANGUAGE_SERVER_ID: &str = "lain-lsp";
const EXTENSION_BINARY: &str = "D:/codeFiles/lain-lang/build/lain_lsp.exe";
const PATH_BINARY: &str = "lain-lsp";

struct LainExtension;

impl LainExtension {
    fn binary_path(&self, worktree: &Worktree) -> Result<String> {
        let configured = zed::settings::LspSettings::for_worktree(LANGUAGE_SERVER_ID, worktree)?
            .binary
            .and_then(|binary| binary.path);
        if let Some(path) = configured {
            return Ok(path);
        }
        if std::fs::metadata(EXTENSION_BINARY).is_ok_and(|meta| meta.is_file()) {
            return Ok(EXTENSION_BINARY.to_string());
        }
        match worktree.which(PATH_BINARY) {
            Some(path) => Ok(path),
            None => Err(format!(
                "找不到 Lain 语言服务器：请把 {EXTENSION_BINARY} 编译出来，或把 lain-lsp 放进 PATH，或在 Zed 设置里写 {{\"lsp\": {{\"{LANGUAGE_SERVER_ID}\": {{\"binary\": {{\"path\": \"...\"}}}}}}}}"
            )),
        }
    }
}

impl zed::Extension for LainExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<Command> {
        if language_server_id.as_ref() != LANGUAGE_SERVER_ID {
            return Err(format!("未知的语言服务器：{language_server_id}"));
        }
        let command = self.binary_path(worktree)?;
        Ok(Command {
            command,
            args: Vec::new(),
            env: Default::default(),
        })
    }
}

zed::register_extension!(LainExtension);
