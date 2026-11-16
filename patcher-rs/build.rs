// Embed the tiny Windows launcher (../launcher, built with rust-lld for
// i686-pc-windows-msvc) so the patcher can write "Game (mkxp-z).exe" on any OS.
use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("launcher.exe");
    let candidates: Vec<PathBuf> = [
        std::env::var("MKXPZ_LAUNCHER_EXE").ok().map(PathBuf::from),
        Some(manifest.join("../launcher/target/i686-pc-windows-msvc/release/mkxpz-launcher.exe")),
        Some(manifest.join("assets/mkxpz-launcher.exe")),
    ].into_iter().flatten().collect();
    println!("cargo:rerun-if-env-changed=MKXPZ_LAUNCHER_EXE");
    for c in &candidates {
        println!("cargo:rerun-if-changed={}", c.display());
    }
    let data = candidates.iter().find_map(|c| std::fs::read(c).ok()).unwrap_or_default();
    if data.is_empty() {
        println!("cargo:warning=Windows launcher not found; Windows patches will get a .cmd launcher instead. \
                  Build it with: cd launcher && cargo build --release");
    }
    std::fs::write(out, data).unwrap();

    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        let ico = manifest.join("assets/mkxpz.ico");
        println!("cargo:rerun-if-changed={}", ico.display());
        let res = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("patcher.res");
        std::fs::write(&res, windows_res(&std::fs::read(&ico).unwrap())).unwrap();
        println!("cargo:rustc-link-arg-bins={}", res.display());
    }
}

// ---------------------------------------------------------------------------
// Windows: give the patcher an icon and an application manifest (modern
// controls via Common Controls 6, DPI awareness, no admin rights) by linking a
// .res file. rust-lld reads .res files directly, so no windres/rc is needed.

const MANIFEST: &str = r#"<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="OpenRPGM.mkxpz.Patcher" version="1.0.0.0" processorArchitecture="*"/>
  <description>MKXP-Z Patcher</description>
  <dependency><dependentAssembly>
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
                      processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
  </dependentAssembly></dependency>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges>
    <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
  </requestedPrivileges></security></trustInfo>
  <application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>
    <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware>
    <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
  </windowsSettings></application>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application>
    <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    <supportedOS Id="{1f676c76-80e1-4239-95bb-83d0f6d0da78}"/>
    <supportedOS Id="{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}"/>
    <supportedOS Id="{35138b9a-5d96-4fbd-8e2d-a2440225f93a}"/>
  </application></compatibility>
</assembly>
"#;

fn res_entry(out: &mut Vec<u8>, typ: u16, name: u16, data: &[u8]) {
    let lang: u16 = if typ == 0 { 0 } else { 0x409 };
    let flags: u16 = if typ == 0 { 0 } else { 0x1030 };
    for v in [data.len() as u32, 32] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    for v in [0xFFFF, typ, 0xFFFF, name] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    out.extend_from_slice(&0u32.to_le_bytes()); // data version
    out.extend_from_slice(&flags.to_le_bytes());
    out.extend_from_slice(&lang.to_le_bytes());
    out.extend_from_slice(&[0u8; 8]); // version, characteristics
    out.extend_from_slice(data);
    while out.len() % 4 != 0 {
        out.push(0);
    }
}

fn windows_res(ico: &[u8]) -> Vec<u8> {
    let mut res = Vec::new();
    res_entry(&mut res, 0, 0, &[]); // .res files start with an empty entry
    // icon: each image as RT_ICON, plus an RT_GROUP_ICON directory pointing at them
    let n = u16::from_le_bytes([ico[4], ico[5]]) as usize;
    let mut group = vec![0u8, 0, 1, 0];
    group.extend_from_slice(&(n as u16).to_le_bytes());
    for i in 0..n {
        let e = &ico[6 + i * 16..6 + i * 16 + 16];
        let size = u32::from_le_bytes([e[8], e[9], e[10], e[11]]) as usize;
        let off = u32::from_le_bytes([e[12], e[13], e[14], e[15]]) as usize;
        res_entry(&mut res, 3, (i + 1) as u16, &ico[off..off + size]);
        group.extend_from_slice(&e[..12]);
        group.extend_from_slice(&((i + 1) as u16).to_le_bytes());
    }
    res_entry(&mut res, 14, 1, &group);
    res_entry(&mut res, 24, 1, MANIFEST.as_bytes()); // CREATEPROCESS_MANIFEST_RESOURCE_ID
    res
}
