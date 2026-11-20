use std::env;
use std::fs;
use std::path::PathBuf;

fn main() {
    let include_src = PathBuf::from(
        env::var("DEP_PEARTUBE_SOULSEEK_INCLUDE")
            .expect("DEP_PEARTUBE_SOULSEEK_INCLUDE from peartube-soulseek"),
    )
    .join("soulseek.h");

    let target_dir = env::var_os("CARGO_TARGET_DIR").map_or_else(
        || {
            PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"))
                .join("target")
        },
        PathBuf::from,
    );
    let out_inc = target_dir.join("include");
    fs::create_dir_all(&out_inc).expect("create target/include");
    fs::copy(&include_src, out_inc.join("soulseek.h")).expect("stage soulseek.h");
}
