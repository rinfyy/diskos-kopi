// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 diskOS contributors
//
// Spotify for diskOS (kopi.35): a Spotify Connect receiver and a small library browser on librespot, linked into
// mq_ui as a static library and run as the helper process "spot-disc" (mq_ui --spotify, see spotify_helper.c).
//
// Audio: librespot decodes; the samples go to spot_c_pcm() (C), which paces them into the Disc's own AirPlay
// receiver on 127.0.0.1 - mq_player owns the audio hardware, exactly as for Music Assistant.
//
// UI link (like Music Assistant's /tmp/ma):
//   /tmp/spot/status    key=value lines, rewritten atomically (state, user, msg, title, artist, album, track,
//                       duration_ms, progress_ms, progress_at_ms, speed, list=<seq> <kind>, list_err)
//   /tmp/spot/cmd       FIFO, one command per line: toggle | next | previous | seek <ms> | vol <0-100>
//                       | play <context-uri> [<track-uri>] | shuffle <context-uri> | tracks <index> <uri>...
//                       | list playlists | list liked | list playlist <id> | search <words> | logout
//   /tmp/spot/list.tsv  the answer to the last list/search (tab-separated, see write_list)
//   /tmp/spot/a<N>/     the playing song's "track" path and its cover.jpg (Music finds the cover next to it)

use std::ffi::CStr;
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::os::raw::{c_char, c_int};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use librespot_connect::{ConnectConfig, LoadContextOptions, LoadRequest, LoadRequestOptions, Options, PlayingTrack, Spirc};
use librespot_core::{
    authentication::Credentials, cache::Cache, config::DeviceType, session::Session, SessionConfig,
};
use librespot_metadata::audio::{AudioItem, UniqueFields};
use librespot_playback::{
    audio_backend::{Sink, SinkResult},
    config::{AudioFormat, Bitrate, PlayerConfig},
    convert::Converter,
    decoder::AudioPacket,
    mixer::{self, MixerConfig},
    player::{Player, PlayerEvent},
};
use tokio::sync::mpsc;

const DIR: &str = "/tmp/spot";
const SCOPES: &str = "playlist-read-private,playlist-read-collaborative,user-library-read,user-read-private";

extern "C" {
    fn spot_c_pcm(pcm: *const i16, frames: usize) -> c_int; // paced into AirPlay; blocks while ahead
    fn spot_c_audio(ev: c_int); // 1 start, 0 stop (drops what the receiver holds, so a pause is a pause)
}

// ------------------------------------------------------------------------------------------------ log to stderr
struct Log;
impl log::Log for Log {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.level() <= log::Level::Warn || (m.level() <= log::Level::Info && m.target().starts_with("spotdisc"))
    }
    fn log(&self, r: &log::Record) {
        if self.enabled(r.metadata()) {
            eprintln!("{} {}: {}", r.level(), r.target(), r.args());
        }
    }
    fn flush(&self) {}
}
static LOG: Log = Log;

// the sound leaves ~0.5 s after it is decoded (spot_c_pcm's cushion): positions are stamped that much later
const LEAD_MS: i64 = 500;
fn mono_ms() -> i64 {
    let mut t = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut t) };
    t.tv_sec as i64 * 1000 + t.tv_nsec as i64 / 1_000_000
}

// ------------------------------------------------------------------------------------------------ the sink
struct DiscSink;
impl Sink for DiscSink {
    fn start(&mut self) -> SinkResult<()> {
        unsafe { spot_c_audio(1) };
        Ok(())
    }
    fn stop(&mut self) -> SinkResult<()> {
        unsafe { spot_c_audio(0) };
        Ok(())
    }
    fn write(&mut self, packet: AudioPacket, converter: &mut Converter) -> SinkResult<()> {
        if let AudioPacket::Samples(s) = packet {
            let v = converter.f64_to_s16(&s);
            unsafe { spot_c_pcm(v.as_ptr(), v.len() / 2) };
        }
        Ok(())
    }
}

// ------------------------------------------------------------------------------------------------ status
#[derive(Default, Clone)]
struct Status {
    state: &'static str, // off | waiting | connecting | ready | playing | error
    user: String,
    msg: String,
    title: String,
    artist: String,
    album: String,
    track: String,
    dur: u32,
    pos: u32,
    pos_at: i64,
    playing: bool,
    list_seq: u32,
    list_kind: String,
    list_err: String,
    art_n: u32,
}
type St = Arc<Mutex<Status>>;

fn clean(s: &str) -> String {
    s.chars().map(|c| if c == '\t' || c == '\n' || c == '\r' { ' ' } else { c }).collect()
}
fn write_atomic(path: &str, data: &str) {
    let tmp = format!("{path}.tmp");
    if fs::write(&tmp, data).is_ok() {
        let _ = fs::rename(&tmp, path);
    }
}
fn status_write(st: &St) {
    let s = st.lock().unwrap().clone();
    let mut o = String::new();
    o += &format!("pid={}\nstate={}\n", std::process::id(), s.state);
    if !s.user.is_empty() { o += &format!("user={}\n", clean(&s.user)); }
    if !s.msg.is_empty() { o += &format!("msg={}\n", clean(&s.msg)); }
    if !s.title.is_empty() {
        o += &format!("title={}\nartist={}\nalbum={}\ntrack={}\n", clean(&s.title), clean(&s.artist), clean(&s.album), s.track);
        o += &format!("duration_ms={}\nprogress_ms={}\nprogress_at_ms={}\nspeed={}\n", s.dur, s.pos, s.pos_at, if s.playing { 1 } else { 0 });
    }
    if s.list_seq > 0 { o += &format!("list={} {}\n", s.list_seq, s.list_kind); }
    if !s.list_err.is_empty() { o += &format!("list_err={}\n", clean(&s.list_err)); }
    write_atomic(&format!("{DIR}/status"), &o);
}
fn set_state(st: &St, state: &'static str, msg: &str) {
    {
        let mut s = st.lock().unwrap();
        s.state = state;
        s.msg = msg.to_string();
    }
    status_write(st);
}

// ------------------------------------------------------------------------------------------------ Web API (lists)
async fn webapi(session: &Session, client_id: &str, path: &str) -> Result<serde_json::Value, String> {
    let token = if client_id.is_empty() {
        session.token_provider().get_token(SCOPES).await
    } else {
        session.token_provider().get_token_with_client_id(SCOPES, client_id).await
    }
    .map_err(|e| format!("No access token ({e})"))?;
    let url = if path.starts_with("https://") { path.to_string() } else { format!("https://api.spotify.com/v1/{path}") };
    let req = http::Request::builder()
        .method("GET")
        .uri(url)
        .header("Authorization", format!("Bearer {}", token.access_token))
        .header("Accept", "application/json")
        .body(bytes::Bytes::new())
        .map_err(|e| e.to_string())?;
    let body = session.http_client().request_body(req).await.map_err(|e| format!("Spotify said no ({e})"))?;
    serde_json::from_slice(&body).map_err(|e| format!("Bad answer ({e})"))
}
fn js<'a>(v: &'a serde_json::Value, k: &str) -> &'a str { v.get(k).and_then(|x| x.as_str()).unwrap_or("") }
fn artists(t: &serde_json::Value) -> String {
    t.get("artists").and_then(|a| a.as_array()).map(|a| a.iter().map(|x| js(x, "name")).collect::<Vec<_>>().join(", ")).unwrap_or_default()
}
fn pct(q: &str) -> String {
    let mut o = String::new();
    for b in q.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => o.push(b as char),
            b' ' => o.push_str("%20"),
            _ => o.push_str(&format!("%{b:02X}")),
        }
    }
    o
}
// rows: "C\t<context uri>" first (tracks), then "P\t<name>\t<uri>\t<detail>" or "T\t<title>\t<artist>\t<uri>\t<ms>"
async fn make_list(session: &Session, client_id: &str, what: &str) -> Result<String, String> {
    let mut out = String::new();
    let mut words = what.splitn(2, ' ');
    let kind = words.next().unwrap_or("");
    let arg = words.next().unwrap_or("").trim();
    match kind {
        "playlists" => {
            out += &format!("P\tLiked Songs\tspotify:user:{}:collection\tYour liked songs\n", session.username());
            let mut next = "me/playlists?limit=50".to_string();
            for _ in 0..4 {
                let v = webapi(session, client_id, &next).await?;
                for p in v.get("items").and_then(|x| x.as_array()).into_iter().flatten() {
                    if p.is_null() { continue; }
                    let n = p.get("tracks").and_then(|t| t.get("total")).and_then(|t| t.as_u64()).unwrap_or(0);
                    let owner = p.get("owner").map(|o| js(o, "display_name")).unwrap_or("");
                    out += &format!("P\t{}\t{}\t{} songs{}{}\n", clean(js(p, "name")), js(p, "uri"), n, if owner.is_empty() { "" } else { " · " }, clean(owner));
                }
                match v.get("next").and_then(|x| x.as_str()) { Some(u) => next = u.to_string(), None => break }
            }
        }
        "liked" | "playlist" => {
            let (ctx, mut next) = if kind == "liked" {
                (format!("spotify:user:{}:collection", session.username()), "me/tracks?limit=50".to_string())
            } else {
                let id = arg.trim_start_matches("spotify:playlist:");
                (format!("spotify:playlist:{id}"), format!("playlists/{id}/tracks?limit=100&fields=items(track(name,uri,duration_ms,artists(name))),next"))
            };
            out += &format!("C\t{ctx}\n");
            for _ in 0..(if kind == "liked" { 6 } else { 3 }) {
                let v = webapi(session, client_id, &next).await?;
                for it in v.get("items").and_then(|x| x.as_array()).into_iter().flatten() {
                    let t = match it.get("track") { Some(t) if !t.is_null() => t, _ => continue };
                    let uri = js(t, "uri");
                    if !uri.starts_with("spotify:track:") { continue; }
                    out += &format!("T\t{}\t{}\t{}\t{}\n", clean(js(t, "name")), clean(&artists(t)), uri, t.get("duration_ms").and_then(|d| d.as_u64()).unwrap_or(0));
                }
                match v.get("next").and_then(|x| x.as_str()) { Some(u) => next = u.to_string(), None => break }
            }
        }
        "search" => {
            out += "C\t\n";
            let v = webapi(session, client_id, &format!("search?type=track&limit=30&q={}", pct(arg))).await?;
            for t in v.get("tracks").and_then(|x| x.get("items")).and_then(|x| x.as_array()).into_iter().flatten() {
                out += &format!("T\t{}\t{}\t{}\t{}\n", clean(js(t, "name")), clean(&artists(t)), js(t, "uri"), t.get("duration_ms").and_then(|d| d.as_u64()).unwrap_or(0));
            }
        }
        _ => return Err("Unknown list".into()),
    }
    Ok(out)
}

// ------------------------------------------------------------------------------------------------ now playing
fn sweep(keep: &str) {
    // the old song folders go (a<N>, a<N>c), all but `keep`
    if let Ok(rd) = fs::read_dir(DIR) {
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().to_string();
            let digits = name.strip_prefix('a').map(|r| r.trim_end_matches('c')).unwrap_or("");
            if !digits.is_empty() && digits.parse::<u32>().is_ok() && name != keep && e.path().is_dir() {
                let _ = fs::remove_dir_all(e.path());
            }
        }
    }
}
async fn track_changed(session: Session, st: St, item: AudioItem) {
    let (artist, album) = match &item.unique_fields {
        UniqueFields::Track { artists, album, .. } => (artists.iter().map(|a| a.name.clone()).collect::<Vec<_>>().join(", "), album.clone()),
        UniqueFields::Episode { show_name, .. } => (show_name.clone(), String::new()),
        UniqueFields::Local { artists, album, .. } => (artists.clone().unwrap_or_default(), album.clone().unwrap_or_default()),
    };
    let n = {
        let mut s = st.lock().unwrap();
        s.art_n = s.art_n.wrapping_add(1);
        s.art_n
    };
    let dir = format!("{DIR}/a{n}");
    let _ = fs::create_dir_all(&dir);
    sweep(&format!("a{n}"));
    let _ = fs::write(format!("{dir}/track"), b"");
    {
        let mut s = st.lock().unwrap();
        s.title = item.name.clone();
        s.artist = artist;
        s.album = album;
        s.track = format!("{dir}/track");
        s.dur = item.duration_ms;
    }
    status_write(&st);
    // the cover: the biggest one up to 640 px, after the song shows (it can take a moment). It goes into a new folder
    // with its own "track": Music sees a new path and looks for the cover again (as for Music Assistant).
    let best = item.covers.iter().filter(|c| c.width <= 640).max_by_key(|c| c.width).or_else(|| item.covers.first());
    if let Some(c) = best {
        if let Ok(req) = http::Request::builder().method("GET").uri(c.url.clone()).body(bytes::Bytes::new()) {
            if let Ok(Ok(b)) = tokio::time::timeout(Duration::from_secs(15), session.http_client().request_body(req)).await {
                let dir2 = format!("{DIR}/a{n}c");
                let _ = fs::create_dir_all(&dir2);
                if fs::write(format!("{dir2}/cover.jpg"), &b).is_ok() && fs::write(format!("{dir2}/track"), b"").is_ok() {
                    let same = { let mut s = st.lock().unwrap(); if s.art_n == n { s.track = format!("{dir2}/track"); true } else { false } };
                    if same { status_write(&st); sweep(&format!("a{n}c")); } else { let _ = fs::remove_dir_all(&dir2); }
                }
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------ commands (FIFO)
fn cmd_reader(tx: mpsc::UnboundedSender<String>) {
    let path = format!("{DIR}/cmd");
    let _ = fs::remove_file(&path);
    let cpath = std::ffi::CString::new(path.clone()).unwrap();
    unsafe { libc::mkfifo(cpath.as_ptr(), 0o600) };
    std::thread::spawn(move || {
        // O_RDWR: our own write end keeps the FIFO open, so reads never see end-of-file between writers
        let f = match fs::OpenOptions::new().read(true).write(true).open(&path) { Ok(f) => f, Err(_) => return };
        for line in BufReader::new(f).lines() {
            match line {
                Ok(l) => { if tx.send(l).is_err() { return; } }
                Err(_) => std::thread::sleep(Duration::from_millis(200)),
            }
        }
    });
}

fn opts(track: Option<PlayingTrack>, shuffle: bool) -> LoadRequestOptions {
    LoadRequestOptions {
        start_playing: true,
        seek_to: 0,
        context_options: if shuffle { Some(LoadContextOptions::Options(Options { shuffle: true, repeat: false, repeat_track: false })) } else { None },
        playing_track: track,
    }
}

// ------------------------------------------------------------------------------------------------ main
#[no_mangle]
pub extern "C" fn spot_rs_main(name: *const c_char, device_id: *const c_char, cache_dir: *const c_char, client_id: *const c_char) -> c_int {
    let s = |p: *const c_char| if p.is_null() { String::new() } else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() };
    let (name, device_id, cache_dir, client_id) = (s(name), s(device_id), s(cache_dir), s(client_id));
    let _ = log::set_logger(&LOG).map(|()| log::set_max_level(log::LevelFilter::Info));
    let _ = fs::create_dir_all(DIR);
    let rt = match tokio::runtime::Builder::new_current_thread().enable_all().build() {
        Ok(r) => r,
        Err(e) => { eprintln!("spot: no runtime: {e}"); return 1; }
    };
    rt.block_on(run(name, device_id, cache_dir, client_id))
}

async fn run(name: String, device_id: String, cache_dir: String, client_id: String) -> c_int {
    let st: St = Arc::new(Mutex::new(Status { state: "connecting", ..Default::default() }));
    status_write(&st);
    let (ctx, mut crx) = mpsc::unbounded_channel::<String>();
    cmd_reader(ctx);

    let mut session_config = SessionConfig::default();
    session_config.device_id = device_id.clone();
    session_config.tmp_dir = std::path::PathBuf::from(DIR);
    let cache = match Cache::new(Some(cache_dir.as_str()), Some(cache_dir.as_str()), None, None) {
        Ok(c) => Some(c),
        Err(e) => { log::warn!("spotdisc: no cache: {e}"); None }
    };
    let mut last_credentials: Option<Credentials> = cache.as_ref().and_then(|c| c.credentials());

    // Spotify Connect discovery: the Disc shows in the Spotify app; picking it signs the Disc in (first time)
    let mut discovery = None;
    for attempt in 0..30 {
        match librespot_discovery::Discovery::builder(device_id.clone(), session_config.client_id.clone())
            .name(name.clone())
            .device_type(DeviceType::Speaker)
            .launch()
        {
            Ok(d) => { discovery = Some(d); break; }
            Err(e) => {
                if attempt == 0 { log::warn!("spotdisc: discovery not up yet: {e}"); }
                set_state(&st, "connecting", "Waiting for Wi-Fi");
                tokio::time::sleep(Duration::from_secs(5)).await;
            }
        }
    }
    if discovery.is_none() && last_credentials.is_none() {
        set_state(&st, "error", "No network - turn on Wi-Fi");
        return 2;
    }

    let mixer = match mixer::find(None).map(|f| f(MixerConfig::default())) {
        Some(Ok(m)) => m,
        _ => { set_state(&st, "error", "No mixer"); return 3; }
    };
    let mut session = Session::new(session_config.clone(), cache.clone());
    let player_config = PlayerConfig { bitrate: Bitrate::Bitrate320, gapless: true, position_update_interval: None, ..Default::default() };
    let _fmt = AudioFormat::S16;
    let player = Player::new(player_config, session.clone(), mixer.get_soft_volume(), || Box::new(DiscSink) as Box<dyn Sink>);
    let mut events = player.get_player_event_channel();

    let mut spirc: Option<Spirc> = None;
    let mut spirc_task: Option<std::pin::Pin<Box<dyn std::future::Future<Output = ()> + Send>>> = None;
    let mut connecting = last_credentials.is_some();
    if !connecting { set_state(&st, "waiting", ""); }
    let mut fails = 0u32;

    loop {
        tokio::select! {
            creds = async { match discovery.as_mut() { Some(d) => futures_util::StreamExt::next(d).await, None => std::future::pending().await } } => {
                match creds {
                    Some(c) => {
                        log::info!("spotdisc: picked in the Spotify app");
                        last_credentials = Some(c);
                        if let Some(s) = spirc.take() { let _ = s.shutdown(); }
                        if let Some(t) = spirc_task.take() { tokio::spawn(t); }
                        if !session.is_invalid() { session.shutdown(); }
                        connecting = true; fails = 0;
                    }
                    None => { discovery = None; }
                }
            },
            _ = async {}, if connecting => {
                connecting = false;
                set_state(&st, "connecting", "Signing in");
                if session.is_invalid() {
                    session = Session::new(session_config.clone(), cache.clone());
                    player.set_session(session.clone());
                }
                let cc = ConnectConfig { name: name.clone(), device_type: DeviceType::Speaker, initial_volume: u16::MAX,   /* full: the Disc's own volume keys set the loudness */ ..Default::default() };
                match Spirc::new(cc, session.clone(), last_credentials.clone().unwrap_or_default(), player.clone(), mixer.clone()).await {
                    Ok((s, t)) => {
                        spirc = Some(s); spirc_task = Some(Box::pin(t));
                        st.lock().unwrap().user = session.username();
                        set_state(&st, "ready", "");
                    }
                    Err(e) => {
                        log::warn!("spotdisc: sign-in failed: {e}");
                        fails += 1;
                        if fails >= 3 { last_credentials = None; set_state(&st, "waiting", "Sign-in failed - pick the Disc in Spotify again"); }
                        else { set_state(&st, "error", "Can't reach Spotify - trying again"); tokio::time::sleep(Duration::from_secs(5 * fails as u64)).await; connecting = true; }
                    }
                }
            },
            _ = async { if let Some(t) = spirc_task.as_mut() { t.await } }, if spirc_task.is_some() => {
                spirc_task = None; spirc = None;
                log::warn!("spotdisc: connection to Spotify ended");
                { let mut s = st.lock().unwrap(); s.playing = false; }
                if last_credentials.is_some() { set_state(&st, "connecting", "Reconnecting"); tokio::time::sleep(Duration::from_secs(3)).await; if !session.is_invalid() { session.shutdown(); } connecting = true; }
                else { set_state(&st, "waiting", ""); }
            },
            ev = events.recv() => {
                let Some(ev) = ev else { set_state(&st, "error", "Player stopped"); return 4; };
                match ev {
                    PlayerEvent::TrackChanged { audio_item } => { tokio::spawn(track_changed(session.clone(), st.clone(), *audio_item)); }
                    PlayerEvent::Playing { position_ms, .. } | PlayerEvent::Seeked { position_ms, .. } | PlayerEvent::PositionCorrection { position_ms, .. } => {
                        let playing_now = !matches!(ev, PlayerEvent::Seeked { .. }) || st.lock().unwrap().playing;
                        { let mut s = st.lock().unwrap(); s.pos = position_ms; s.pos_at = mono_ms() + LEAD_MS; s.playing = playing_now; if s.state != "error" { s.state = if playing_now { "playing" } else { "ready" }; } }
                        status_write(&st);
                    }
                    PlayerEvent::Paused { position_ms, .. } => {
                        { let mut s = st.lock().unwrap(); s.pos = position_ms; s.pos_at = mono_ms(); s.playing = false; s.state = "ready"; }
                        status_write(&st);
                    }
                    PlayerEvent::Stopped { .. } => {
                        { let mut s = st.lock().unwrap(); s.playing = false; s.title.clear(); s.state = "ready"; }
                        status_write(&st);
                    }
                    PlayerEvent::SessionClientChanged { client_name, .. } => { log::info!("spotdisc: controlled from {client_name}"); }
                    _ => {}
                }
            },
            cmd = crx.recv() => {
                let Some(cmd) = cmd else { continue };
                let cmd = cmd.trim().to_string();
                let (verb, rest) = cmd.split_once(' ').map(|(a, b)| (a, b.trim())).unwrap_or((cmd.as_str(), ""));
                if verb == "logout" {
                    if let Some(c) = cache.as_ref() { let _ = fs::remove_file(std::path::Path::new(&cache_dir).join("credentials.json")); let _ = c; }
                    if let Some(s) = spirc.take() { let _ = s.shutdown(); }
                    set_state(&st, "waiting", "Signed out");
                    return 0;
                }
                if verb == "list" || verb == "search" {
                    // "list <n> <what>" / "search <n> <words>": n is the UI's request number, echoed in "list=<n> <kind>"
                    let (n, rest) = rest.split_once(' ').unwrap_or((rest, ""));
                    let seq: u32 = n.parse().unwrap_or(0);
                    let what = if verb == "search" { format!("search {rest}") } else { rest.to_string() };
                    let sess = session.clone(); let st2 = st.clone(); let cid = client_id.clone();
                    tokio::spawn(async move {
                        let r = if sess.is_invalid() || sess.username().is_empty() { Err("Not signed in yet".to_string()) } else { make_list(&sess, &cid, &what).await };
                        match r {
                            Ok(body) => { write_atomic(&format!("{DIR}/list.tsv"), &body); let mut s = st2.lock().unwrap(); s.list_err.clear(); s.list_seq = seq; s.list_kind = what.split(' ').next().unwrap_or("").to_string(); }
                            Err(e) => { log::warn!("spotdisc: list {what}: {e}"); let mut s = st2.lock().unwrap(); s.list_err = e; s.list_seq = seq; s.list_kind = "error".into(); }
                        }
                        status_write(&st2);
                    });
                    continue;
                }
                let Some(sp) = spirc.as_ref() else { set_state(&st, st.lock().unwrap().state, "Not signed in yet"); continue };
                let r = match verb {
                    "toggle" => sp.play_pause(),
                    "play" if rest.is_empty() => sp.play(),
                    "pause" => sp.pause(),
                    "next" => sp.next(),
                    "previous" => sp.prev(),
                    "seek" => sp.set_position_ms(rest.parse().unwrap_or(0)),
                    "vol" => sp.set_volume((rest.parse::<u32>().unwrap_or(50).min(100) * 65535 / 100) as u16),
                    "play" | "shuffle" => {
                        let mut it = rest.splitn(2, ' ');
                        let ctxu = it.next().unwrap_or("").to_string();
                        let track = it.next().filter(|t| !t.is_empty()).map(|t| PlayingTrack::Uri(t.to_string()));
                        let _ = sp.activate();
                        sp.load(LoadRequest::from_context_uri(ctxu, opts(track, verb == "shuffle")))
                    }
                    "tracks" => {
                        let mut it = rest.split_whitespace();
                        let idx: u32 = it.next().and_then(|i| i.parse().ok()).unwrap_or(0);
                        let uris: Vec<String> = it.map(|u| u.to_string()).collect();
                        let _ = sp.activate();
                        sp.load(LoadRequest::from_tracks(uris, opts(Some(PlayingTrack::Index(idx)), false)))
                    }
                    _ => Ok(()),
                };
                if let Err(e) = r { log::warn!("spotdisc: {verb}: {e}"); }
            },
        }
    }
}

#[allow(dead_code)]
fn flush_stderr() { let _ = std::io::stderr().flush(); }
