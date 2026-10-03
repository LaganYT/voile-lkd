//! epic-launch SECRETS_DIR GAME_DIR LANGUAGE
//! Reads SECRETS_DIR/epic-session.json ({accountId, displayName, refreshToken}), restores the Epic session, writes the
//! rotated credentials back to the same file, asks Epic for the launch plan of the installed game in GAME_DIR (receipt
//! .sailover-install.json) and writes SECRETS_DIR/launch-plan.json (arguments and working directory) and, if the game
//! needs one, SECRETS_DIR/ownership.bin. All files mode 0600. Prints only counts and key names, never values.
use std::fs;
use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;
use std::path::Path;

use epic_kit::auth::Credentials;
use epic_kit::client::Client;

fn write_private(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    let tmp = path.with_extension("tmp");
    let mut f = fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp)?;
    f.write_all(bytes)?;
    f.sync_all()?;
    fs::rename(tmp, path)
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 4 {
        eprintln!("usage: epic-launch SECRETS_DIR GAME_DIR LANGUAGE");
        std::process::exit(2);
    }
    let secrets = Path::new(&args[1]);
    let game = Path::new(&args[2]);
    let session: serde_json::Value = serde_json::from_slice(&fs::read(secrets.join("epic-session.json"))?)?;
    let field = |k: &str| session.get(k).and_then(|v| v.as_str()).map(str::to_owned).ok_or(format!("missing {k}"));
    let creds = Credentials { account_id: field("accountId")?, display_name: field("displayName")?, refresh_token: field("refreshToken")? };
    let receipt: serde_json::Value = serde_json::from_slice(&fs::read(game.join(".sailover-install.json"))?)?;
    let rfield = |k: &str| receipt.get(k).and_then(|v| v.as_str()).map(str::to_owned).ok_or(format!("receipt missing {k}"));

    let client = Client::restore(creds).await.map_err(|e| format!("restore: {e}"))?;
    // Persist the rotated refresh token first, so a later failure cannot lose the session.
    let rotated = client.credentials().await.map_err(|e| format!("credentials: {e}"))?;
    let out = serde_json::json!({"accountId": rotated.account_id, "displayName": rotated.display_name, "refreshToken": rotated.refresh_token});
    write_private(&secrets.join("epic-session.json"), serde_json::to_vec(&out)?.as_slice())?;

    let plan = client
        .launch_plan(&rfield("namespace")?, &rfield("catalog_item_id")?, &rfield("app_name")?, &rfield("platform")?, game, &args[3])
        .await
        .map_err(|e| format!("launch_plan: {e}"))?;
    let doc = serde_json::json!({
        "executable": plan.target.executable,
        "arguments": plan.target.arguments,
        "working_directory": plan.target.working_directory,
        "has_ownership_token": plan.ownership_token.is_some(),
    });
    write_private(&secrets.join("launch-plan.json"), serde_json::to_vec_pretty(&doc)?.as_slice())?;
    if let Some(tok) = &plan.ownership_token {
        write_private(&secrets.join("ownership.bin"), tok)?;
    }
    let names: Vec<String> = plan.target.arguments.iter().map(|a| a.split('=').next().unwrap_or("").to_owned()).collect();
    println!("launch plan ready: {} arguments ({}), ownership token: {}", names.len(), names.join(" "), plan.ownership_token.is_some());
    Ok(())
}
