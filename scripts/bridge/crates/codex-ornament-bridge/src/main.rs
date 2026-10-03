use chrono::{DateTime, FixedOffset, Local, NaiveDate, Timelike};
use quota_core::{
    get_quota_snapshot, read_state, state_path, write_state, QuotaSnapshot, SnapshotStatus,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    borrow::Cow,
    collections::{hash_map::DefaultHasher, HashMap, HashSet, VecDeque},
    env,
    fs::{self, File, OpenOptions},
    hash::{Hash, Hasher},
    io::{self, BufRead, BufReader, Read, Seek, SeekFrom, Write},
    net::{IpAddr, Ipv4Addr, SocketAddr, TcpListener, TcpStream, UdpSocket},
    path::{Path, PathBuf},
    process::{Child, ChildStdout, Command, ExitStatus, Stdio},
    sync::{
        mpsc::{self, Receiver, SyncSender, TrySendError},
        Arc, Mutex,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

const DEFAULT_BIND: &str = "0.0.0.0:8787";
const MAX_BODY_BYTES: usize = 64 * 1024;
const DEFAULT_STREAM_COPY_BYTES: usize = 8192;
const MAX_STATE_TASKS: usize = 8;
const MAX_TASK_HISTORY: usize = 16;
const TASK_EVENT_QUEUE_CAPACITY: usize = 64;
const TASK_EVENT_ACK_TIMEOUT: Duration = Duration::from_millis(750);
const DEFAULT_EVENT_LOG_MAX_BYTES: u64 = 8 * 1024 * 1024;
const DEFAULT_EVENT_LOG_COMPACT_KEEP_EVENTS: usize = 4096;
const QUOTA_REFRESH_INTERVAL: Duration = Duration::from_secs(60);
const QUOTA_CACHE_TTL: Duration = QUOTA_REFRESH_INTERVAL;
const DEFAULT_WEATHER_CACHE_TTL: Duration = Duration::from_secs(10 * 60);
const MUSIC_RESOLVE_CACHE_TTL: Duration = Duration::from_secs(10 * 60);
const MUSIC_RESOLVE_CACHE_MAX: usize = 32;
const MUSIC_COVER_SIZE: u32 = 96;
const MUSIC_COVER_BYTES: usize = MUSIC_COVER_SIZE as usize * MUSIC_COVER_SIZE as usize * 2;
const YAOHUD_RESOLVE_ATTEMPTS: usize = 3;
const YAOHUD_RESOLVE_RETRY_DELAY: Duration = Duration::from_millis(300);
const YAOHUD_MUSIC_TYPE: &str = "wy";
const MUSIC_STREAM_FALLBACK_ATTEMPTS: u32 = 6;
const MUSIC_DURATION_PROBE_TIMEOUT: Duration = Duration::from_secs(8);
const XIAOZHI_PCM_CHUNK_BYTES: usize = 3200;
const XIAOZHI_UPLINK_BUFFER_MAX_BYTES: usize = 16000 * 2 * 5;
const XIAOZHI_DOWNLINK_BUFFER_MAX_BYTES: usize = 16000 * 2 * 8;
const XIAOZHI_DOWNLINK_WAIT_MS: u64 = 250;
const XIAOZHI_UPLINK_READ_WAIT_MS: u64 = 250;
const CAIYUN_DAILY_CALL_BUDGET: u32 = 10_000;
const CAIYUN_NIGHT_END_HOUR: u32 = 6;
const CAIYUN_NIGHT_SECONDS: u64 = 6 * 60 * 60;
const CAIYUN_DAY_SECONDS: u64 = 18 * 60 * 60;
const CAIYUN_NIGHT_REFRESH_SECONDS: u64 = 30 * 60;
const CAIYUN_NIGHT_CALL_BUDGET: u32 = (CAIYUN_NIGHT_SECONDS / CAIYUN_NIGHT_REFRESH_SECONDS) as u32;
const CAIYUN_DAY_CALL_BUDGET: u32 = CAIYUN_DAILY_CALL_BUDGET - CAIYUN_NIGHT_CALL_BUDGET;
const CAIYUN_NIGHT_REFRESH_INTERVAL: Duration = Duration::from_secs(CAIYUN_NIGHT_REFRESH_SECONDS);
const CAIYUN_RATE_LIMIT_BACKOFF: Duration = Duration::from_secs(30 * 60);
const WEATHER_FETCH_TIMEOUT: Duration = Duration::from_secs(5);
const ACTIVE_RECOVERY_SCAN_TTL: Duration = Duration::from_secs(5);
const DEFAULT_WEATHER_LATITUDE: f64 = 39.99540087499999;
const DEFAULT_WEATHER_LONGITUDE: f64 = 116.34162524999999;
const DEFAULT_WEATHER_LABEL: &str = "HAIDIAN";
const DEFAULT_WEATHER_PROVIDER: WeatherProvider = WeatherProvider::Auto;
const DEFAULT_STANDBY_WALLPAPER_DIR: &str = r"D:\AssaultLilyViewer_v0.5";
const DEFAULT_STANDBY_WALLPAPER_WIDTH: u32 = 240;
const DEFAULT_STANDBY_WALLPAPER_HEIGHT: u32 = 240;
const RECONCILED_DONE_NOTIFY_WINDOW: Duration = Duration::from_secs(120);
const COMBINED_DONE_SOURCE_WINDOW: Duration = Duration::from_secs(5);
const RECOVER_UNSCOPED_ACTIVE_TASK_WINDOW: Duration = Duration::from_secs(30 * 60);
const RECOVER_ACTIVE_SESSION_SCAN_LIMIT: usize = 24;
const SESSION_TASK_SCAN_TAIL_BYTES: u64 = 2 * 1024 * 1024;
const ACTIVE_SESSION_FILE_MISSING_GRACE: Duration = Duration::from_secs(30);
const ACTIVE_SESSION_IDLE_STALE_GRACE: Duration = Duration::from_secs(2 * 60 * 60);
const SESSION_FORK_CHAIN_LIMIT: usize = 8;
const DISCOVERY_MAGIC: &str = "codex-ornament-discover-v1";

type SharedBridgeState = Arc<Mutex<BridgeState>>;
type TaskEventSender = SyncSender<QueuedTaskEvent>;
type TaskEventReceiver = Receiver<QueuedTaskEvent>;

#[derive(Clone, Debug, Default)]
struct BridgeState {
    task: Option<TaskEvent>,
    active_tasks: HashMap<String, TaskEvent>,
    active_order: VecDeque<String>,
    done_tasks: VecDeque<TaskEvent>,
    unmatched_stops: VecDeque<TaskEvent>,
    done_seq: u64,
    codex_done_seq: u64,
    claude_done_seq: u64,
    next_anonymous_task_id: u64,
    quota: Option<CachedQuota>,
    quota_refreshing: bool,
    weather: Option<CachedWeather>,
    weather_refreshing: bool,
    weather_last_attempt: Option<Instant>,
    weather_caiyun_backoff_until: Option<Instant>,
    active_recovery: Option<CachedActiveRecovery>,
    music_resolves: HashMap<String, CachedMusicResolve>,
    xiaozhi: XiaozhiProxySession,
}

#[derive(Clone, Debug)]
struct CachedQuota {
    snapshot: QuotaSnapshot,
    fetched_at: Instant,
}

#[derive(Clone, Debug)]
struct CachedActiveRecovery {
    events: Vec<TaskEvent>,
    fetched_at: Instant,
}

#[derive(Clone, Debug)]
struct BridgeConfig {
    bind: String,
    token: Option<String>,
    event_log_path: Option<PathBuf>,
    event_log_max_bytes: u64,
    event_log_compact_keep_events: usize,
    yaohud_key: Option<String>,
    music_public_base_url: Option<String>,
    music_public_base_url_locked: bool,
    tracked_session_id: Option<String>,
    codex_home: PathBuf,
    weather_latitude: f64,
    weather_longitude: f64,
    weather_label: String,
    weather_provider: WeatherProvider,
    qweather_host: Option<String>,
    qweather_token: Option<String>,
    caiyun_token: Option<String>,
    xiaozhi_ws_url: Option<String>,
    xiaozhi_token: Option<String>,
    standby_wallpaper_dir: PathBuf,
    standby_wallpaper_fixed: Option<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum WeatherProvider {
    Auto,
    OpenMeteo,
    QWeather,
    Caiyun,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct HttpRequest {
    method: String,
    path: String,
    raw_path: String,
    headers: HashMap<String, String>,
    body: Vec<u8>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct TaskEvent {
    kind: String,
    status: String,
    title: String,
    message: String,
    received_at: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    source: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    session_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    turn_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    cwd: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    model: Option<String>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct OrnamentState {
    status: String,
    active_task_count: usize,
    task: Option<TaskEvent>,
    active_tasks: Vec<TaskEvent>,
    source_tasks: SourceTasks,
    done_seq: u64,
    last_done_task: Option<TaskEvent>,
    done_task_count: usize,
    unmatched_stop_count: usize,
    quota: QuotaSnapshot,
    weather: WeatherSnapshot,
    #[serde(skip_serializing_if = "Option::is_none")]
    standby_wallpaper: Option<StandbyWallpaperInfo>,
    bridge: BridgeInfo,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct StandbyWallpaperInfo {
    mode: String,
    id: String,
    name: String,
    index: usize,
    total: usize,
    width: u32,
    height: u32,
    url: String,
    selected_at: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct ResolvedStandbyWallpaper {
    mode: &'static str,
    id: String,
    name: String,
    path: PathBuf,
    index: usize,
    total: usize,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
struct WeatherSnapshot {
    status: String,
    label: String,
    summary: String,
    icon: String,
    temperature_c: Option<i32>,
    wind_kmh: Option<i32>,
    weather_code: Option<i32>,
    observed_at: String,
}

#[derive(Clone, Debug)]
struct CachedWeather {
    snapshot: WeatherSnapshot,
    fetched_at: Instant,
}

#[derive(Deserialize)]
struct OpenMeteoResponse {
    current: Option<OpenMeteoCurrent>,
}

#[derive(Deserialize)]
struct OpenMeteoCurrent {
    temperature_2m: Option<f64>,
    weather_code: Option<i32>,
    wind_speed_10m: Option<f64>,
}

#[derive(Deserialize)]
struct QWeatherResponse {
    code: String,
    now: Option<QWeatherNow>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct QWeatherNow {
    obs_time: Option<String>,
    temp: Option<String>,
    icon: Option<String>,
    text: Option<String>,
    wind_speed: Option<String>,
}

#[derive(Deserialize)]
struct CaiyunResponse {
    status: String,
    result: Option<CaiyunResult>,
}

#[derive(Deserialize)]
struct CaiyunResult {
    realtime: Option<CaiyunRealtime>,
}

#[derive(Deserialize)]
struct CaiyunRealtime {
    temperature: Option<f64>,
    skycon: Option<String>,
    wind: Option<CaiyunWind>,
}

#[derive(Deserialize)]
struct CaiyunWind {
    speed: Option<f64>,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct SourceTasks {
    codex: SourceTaskSummary,
    claude: SourceTaskSummary,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
struct SourceTaskSummary {
    status: String,
    active_count: usize,
    done_seq: u64,
    task: Option<TaskEvent>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct BridgeInfo {
    service: &'static str,
    observed_at: String,
    event_log_enabled: bool,
    event_log_path: Option<String>,
    event_log_bytes: Option<u64>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ReadyInfo {
    ok: bool,
    service: &'static str,
    observed_at: String,
    bind: String,
    event_log_enabled: bool,
    event_log_path: Option<String>,
    event_log_bytes: Option<u64>,
    event_log_writable: bool,
    event_log_error: Option<String>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct DiscoveryInfo {
    service: &'static str,
    local_ip: String,
    state_url: String,
    health_url: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct TaskSnapshot {
    status: String,
    active_task_count: usize,
    task: Option<TaskEvent>,
    active_tasks: Vec<TaskEvent>,
    source_tasks: SourceTasks,
    done_seq: u64,
    last_done_task: Option<TaskEvent>,
    done_task_count: usize,
    unmatched_stop_count: usize,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct TerminalTurn {
    kind: String,
    timestamp: Option<String>,
    message: Option<String>,
}

#[derive(Clone, Debug)]
struct PendingFinalAnswer {
    turn_id: String,
    timestamp: Option<String>,
    message: Option<String>,
}

struct QueuedTaskEvent {
    event: TaskEvent,
    completion: SyncSender<TaskDispatchResult>,
}

struct TaskEventJournal {
    path: PathBuf,
    file: File,
    max_bytes: u64,
    compact_keep_events: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TaskDispatchResult {
    Applied,
    Filtered,
    Queued,
    LockUnavailable,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TaskQueueError {
    Full,
    Closed,
    CompletionDropped,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum DoneSource {
    Claude,
    Codex,
    Other,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct MusicResolveResponse {
    ok: bool,
    song: String,
    artist: String,
    index: u32,
    source: &'static str,
    title: String,
    album: String,
    picture: String,
    cover_url: String,
    url: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    duration_ms: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    lyrics: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct MusicRequest {
    song: String,
    artist: Option<String>,
    index: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct ResolvedSong {
    source: &'static str,
    title: String,
    artist: String,
    album: String,
    picture: String,
    url: String,
    duration_ms: Option<u32>,
    lyrics: Option<String>,
}

struct ReadyPcmStream {
    child: Child,
    stdout: ChildStdout,
    stderr_thread: Option<JoinHandle<String>>,
    first_chunk: Vec<u8>,
}

impl Drop for ReadyPcmStream {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        if let Some(stderr_thread) = self.stderr_thread.take() {
            let _ = stderr_thread.join();
        }
    }
}

#[derive(Clone, Debug)]
struct CachedMusicResolve {
    song: ResolvedSong,
    fetched_at: Instant,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(dead_code)]
enum XiaozhiProxyState {
    Idle,
    ConfigMissing,
    Connecting,
    Listening,
    Speaking,
    Error,
}

#[derive(Clone, Debug)]
struct XiaozhiProxySession {
    state: XiaozhiProxyState,
    session_requested: bool,
    connected: bool,
    configured: bool,
    session_id: String,
    client_id: Option<String>,
    last_error: Option<String>,
    last_stt: Option<String>,
    last_tts: Option<String>,
    uplink_frames: u64,
    downlink_frames: u64,
    uplink_pcm: VecDeque<u8>,
    downlink_pcm: VecDeque<u8>,
    upstream_configured: bool,
    upstream_running: bool,
    upstream_worker_active: bool,
    upstream_ws_url: Option<String>,
    upstream_token: Option<String>,
    updated_at: Instant,
}

impl Default for XiaozhiProxySession {
    fn default() -> Self {
        Self {
            state: XiaozhiProxyState::Idle,
            session_requested: false,
            connected: false,
            configured: false,
            session_id: "bridge".to_string(),
            client_id: None,
            last_error: None,
            last_stt: None,
            last_tts: None,
            uplink_frames: 0,
            downlink_frames: 0,
            uplink_pcm: VecDeque::new(),
            downlink_pcm: VecDeque::new(),
            upstream_configured: false,
            upstream_running: false,
            upstream_worker_active: false,
            upstream_ws_url: None,
            upstream_token: None,
            updated_at: Instant::now(),
        }
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct XiaozhiProxyStatus {
    ok: bool,
    state: &'static str,
    configured: bool,
    connected: bool,
    session_requested: bool,
    runtime_config: bool,
    activation_pending: bool,
    upstream_configured: bool,
    upstream_running: bool,
    session_id: String,
    client_id: Option<String>,
    last_error: String,
    last_stt: String,
    last_tts: String,
    activation_code: String,
    activation_message: String,
    uplink_frames: u64,
    downlink_frames: u64,
    observed_at: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct XiaozhiSessionRequest {
    client_id: Option<String>,
    sample_rate: Option<u32>,
    channels: Option<u8>,
    format: Option<String>,
    ws_url: Option<String>,
    token: Option<String>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct XiaozhiStatusUpdate {
    state: Option<String>,
    connected: Option<bool>,
    configured: Option<bool>,
    upstream_running: Option<bool>,
    session_id: Option<String>,
    last_error: Option<String>,
    last_stt: Option<String>,
    last_tts: Option<String>,
}

#[derive(Debug, Deserialize)]
struct YaohudMusicResponse {
    #[serde(default)]
    code: Option<i32>,
    #[serde(default)]
    msg: Option<String>,
    #[serde(default)]
    data: Option<YaohudMusicData>,
    #[serde(flatten)]
    top_level: YaohudMusicData,
}

#[derive(Debug, Default, Deserialize)]
struct YaohudMusicData {
    #[serde(default)]
    id: Option<String>,
    #[serde(default)]
    mid: Option<String>,
    #[serde(default)]
    songmid: Option<String>,
    #[serde(default)]
    hash: Option<String>,
    #[serde(default)]
    name: Option<String>,
    #[serde(default)]
    songname: Option<String>,
    #[serde(default)]
    songtitle: Option<String>,
    #[serde(default)]
    title: Option<String>,
    #[serde(default)]
    song: Option<String>,
    #[serde(default)]
    artist: Option<String>,
    #[serde(default)]
    singer: Option<String>,
    #[serde(default)]
    album: Option<String>,
    #[serde(default)]
    picture: Option<String>,
    #[serde(default)]
    pic: Option<String>,
    #[serde(default)]
    cover: Option<String>,
    #[serde(default)]
    img: Option<String>,
    #[serde(default)]
    image: Option<String>,
    #[serde(default)]
    picurl: Option<String>,
    #[serde(default)]
    pic_url: Option<String>,
    #[serde(default)]
    album_pic: Option<String>,
    #[serde(default)]
    musicurl: Option<String>,
    #[serde(default)]
    url: Option<String>,
    #[serde(default)]
    duration_ms: Option<Value>,
    #[serde(default, rename = "durationMs")]
    duration_ms_camel: Option<Value>,
    #[serde(default, rename = "duration")]
    duration: Option<Value>,
    #[serde(default)]
    interval: Option<Value>,
    #[serde(default, rename = "songTime")]
    song_time: Option<Value>,
    #[serde(default)]
    lrc: Option<String>,
    #[serde(default)]
    lyrics: Option<String>,
    #[serde(default)]
    lrctxt: Option<String>,
}

impl YaohudMusicData {
    fn from_response(response: YaohudMusicResponse) -> Self {
        response.data.unwrap_or(response.top_level)
    }
}

#[derive(Debug, Deserialize)]
struct NeteaseSearchResponse {
    result: Option<NeteaseSearchResult>,
    code: i32,
}

#[derive(Debug, Deserialize)]
struct NeteaseSearchResult {
    songs: Option<Vec<NeteaseSong>>,
}

#[derive(Debug, Deserialize)]
struct NeteaseSong {
    id: u64,
    name: String,
    #[serde(default)]
    duration: Option<u32>,
    #[serde(default)]
    artists: Vec<NeteaseArtist>,
    album: Option<NeteaseAlbum>,
}

#[derive(Debug, Deserialize)]
struct NeteaseArtist {
    name: String,
}

#[derive(Debug, Deserialize)]
struct NeteaseAlbum {
    name: String,
    #[serde(default, rename = "picUrl")]
    pic_url: Option<String>,
}

#[derive(Debug, Deserialize)]
struct NeteaseLyricResponse {
    #[serde(default)]
    lrc: Option<NeteaseLyricData>,
}

#[derive(Debug, Deserialize)]
struct NeteaseLyricData {
    #[serde(default)]
    lyric: Option<String>,
}

#[derive(Debug, Deserialize)]
struct NeteaseSongDetailResponse {
    #[serde(default)]
    songs: Vec<NeteaseSong>,
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

fn run() -> io::Result<()> {
    let codex_home = codex_home();
    let config = BridgeConfig {
        bind: env::var("CODEX_ORNAMENT_BIND").unwrap_or_else(|_| DEFAULT_BIND.to_string()),
        token: env::var("CODEX_ORNAMENT_TOKEN")
            .ok()
            .filter(|value| !value.trim().is_empty()),
        event_log_path: bridge_event_log_path(&codex_home),
        event_log_max_bytes: env_u64("CODEX_ORNAMENT_EVENT_LOG_MAX_BYTES")
            .unwrap_or(DEFAULT_EVENT_LOG_MAX_BYTES),
        event_log_compact_keep_events: env_usize("CODEX_ORNAMENT_EVENT_LOG_COMPACT_KEEP_EVENTS")
            .unwrap_or(DEFAULT_EVENT_LOG_COMPACT_KEEP_EVENTS),
        yaohud_key: env_text("CODEX_ORNAMENT_YAOHUD_KEY"),
        music_public_base_url: env_text("CODEX_ORNAMENT_MUSIC_PUBLIC_BASE_URL"),
        music_public_base_url_locked: env_truthy("CODEX_ORNAMENT_LOCK_MUSIC_PUBLIC_BASE_URL"),
        tracked_session_id: env_text("CODEX_ORNAMENT_SESSION_ID"),
        codex_home,
        weather_latitude: env_f64("CODEX_ORNAMENT_WEATHER_LAT").unwrap_or(DEFAULT_WEATHER_LATITUDE),
        weather_longitude: env_f64("CODEX_ORNAMENT_WEATHER_LON")
            .unwrap_or(DEFAULT_WEATHER_LONGITUDE),
        weather_label: env_text("CODEX_ORNAMENT_WEATHER_LABEL")
            .unwrap_or_else(|| DEFAULT_WEATHER_LABEL.to_string()),
        weather_provider: env_weather_provider("CODEX_ORNAMENT_WEATHER_PROVIDER")
            .unwrap_or(DEFAULT_WEATHER_PROVIDER),
        qweather_host: env_text("CODEX_ORNAMENT_QWEATHER_HOST"),
        qweather_token: env_text("CODEX_ORNAMENT_QWEATHER_TOKEN")
            .or_else(|| env_text("CODEX_ORNAMENT_QWEATHER_KEY")),
        caiyun_token: env_text("CODEX_ORNAMENT_CAIYUN_TOKEN")
            .or_else(|| env_text("CODEX_ORNAMENT_CAIYUN_KEY")),
        xiaozhi_ws_url: env_text("CODEX_ORNAMENT_XIAOZHI_WS_URL"),
        xiaozhi_token: env_text("CODEX_ORNAMENT_XIAOZHI_TOKEN"),
        standby_wallpaper_dir: env_text("CODEX_ORNAMENT_STANDBY_WALLPAPER_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from(DEFAULT_STANDBY_WALLPAPER_DIR)),
        standby_wallpaper_fixed: env_text("CODEX_ORNAMENT_STANDBY_WALLPAPER_FIXED"),
    };
    let listener = TcpListener::bind(&config.bind)?;
    let restored_state = restore_bridge_state_from_event_log(config.event_log_path.as_deref());
    let state = Arc::new(Mutex::new(restored_state));
    let event_journal = TaskEventJournal::open(
        config.event_log_path.as_deref(),
        config.event_log_max_bytes,
        config.event_log_compact_keep_events,
    );
    let (task_events, task_event_receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
    spawn_task_event_consumer(
        Arc::clone(&state),
        config.clone(),
        event_journal,
        task_event_receiver,
    );
    spawn_discovery_responder(config.clone());
    spawn_quota_refresh_loop(Arc::clone(&state));
    configure_xiaozhi_proxy_if_configured(config.clone(), Arc::clone(&state));

    eprintln!("codex ornament bridge listening on http://{}", config.bind);
    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                let state = Arc::clone(&state);
                let config = config.clone();
                let task_events = task_events.clone();
                std::thread::spawn(move || {
                    if let Err(error) = handle_connection(stream, state, task_events, config) {
                        eprintln!("request failed: {error}");
                    }
                });
            }
            Err(error) => eprintln!("connection failed: {error}"),
        }
    }

    Ok(())
}

fn handle_connection(
    mut stream: TcpStream,
    state: SharedBridgeState,
    task_events: TaskEventSender,
    config: BridgeConfig,
) -> io::Result<()> {
    let peer = stream.peer_addr().ok();
    let request = read_request(&mut stream)?;

    match (request.method.as_str(), request.path.as_str()) {
        ("OPTIONS", _) => write_response(&mut stream, 204, "text/plain; charset=utf-8", b""),
        ("GET", "/health") => {
            write_response(&mut stream, 200, "text/plain; charset=utf-8", b"ok\n")
        }
        ("GET", "/ready") => {
            let ready = ready_info(&config);
            let status = if ready.ok { 200 } else { 503 };
            write_json(&mut stream, status, &ready)
        }
        ("GET", "/metrics") => {
            let metrics = bridge_metrics(&state, &config);
            write_response(
                &mut stream,
                200,
                "text/plain; version=0.0.4; charset=utf-8",
                metrics.as_bytes(),
            )
        }
        ("GET", "/discover") => write_json(&mut stream, 200, &discover_info(&config)?),
        ("GET", "/quota") => {
            let quota = cached_or_refresh_quota(&state);
            write_json(&mut stream, 200, &quota)
        }
        ("GET", "/state") => {
            let snapshot = task_snapshot(&state, &config);
            let response = OrnamentState {
                status: snapshot.status,
                active_task_count: snapshot.active_task_count,
                task: snapshot.task,
                active_tasks: snapshot.active_tasks,
                source_tasks: snapshot.source_tasks,
                done_seq: snapshot.done_seq,
                last_done_task: snapshot.last_done_task,
                done_task_count: snapshot.done_task_count,
                unmatched_stop_count: snapshot.unmatched_stop_count,
                quota: cached_or_refresh_quota_background(&state),
                weather: cached_or_refresh_weather(&state, &config),
                standby_wallpaper: standby_wallpaper_info(&config, peer),
                bridge: bridge_info(&config),
            };
            write_json(&mut stream, 200, &response)
        }
        ("GET", "/v1/standby-wallpaper") => {
            handle_standby_wallpaper(&mut stream, peer, &request, &config)
        }
        ("GET", "/v1/music/resolve") | ("GET", "/stream_pcm") => {
            handle_music_resolve(&mut stream, peer, &request, &state, &config)
        }
        ("GET", "/v1/music/stream") => {
            handle_music_stream(&mut stream, peer, &request, &state, &config)
        }
        ("GET", "/v1/music/cover") => {
            handle_music_cover(&mut stream, peer, &request, &state, &config)
        }
        ("GET", "/v1/xiaozhi/session/status") => handle_xiaozhi_status(&mut stream, peer, &state),
        ("POST", "/v1/xiaozhi/session/start") => {
            handle_xiaozhi_session_start(&mut stream, peer, &request, &state, &config)
        }
        ("POST", "/v1/xiaozhi/session/stop") => {
            handle_xiaozhi_session_stop(&mut stream, peer, &state)
        }
        ("POST", "/v1/xiaozhi/audio/uplink") => {
            handle_xiaozhi_audio_uplink(&mut stream, peer, &request, &state)
        }
        ("GET", "/v1/xiaozhi/audio/downlink") => {
            handle_xiaozhi_audio_downlink(&mut stream, peer, &request, &state)
        }
        ("POST", "/v1/xiaozhi/audio/inject") => {
            handle_xiaozhi_audio_inject(&mut stream, peer, &request, &state)
        }
        ("GET", "/v1/xiaozhi/proxy/uplink") => {
            handle_xiaozhi_proxy_uplink(&mut stream, peer, &request, &state)
        }
        ("POST", "/v1/xiaozhi/proxy/status") => {
            handle_xiaozhi_proxy_status(&mut stream, peer, &request, &state)
        }
        ("POST", "/hook/codex") | ("POST", "/event") => {
            if !post_allowed(peer, &request, &config) {
                return write_json(
                    &mut stream,
                    403,
                    &json!({"ok": false, "error": "forbidden"}),
                );
            }

            let payload = match parse_hook_payload(&request.body) {
                Ok(payload) => payload,
                Err(error) => {
                    eprintln!(
                        "invalid hook json ignored: {error}; body={}",
                        clip(&String::from_utf8_lossy(&request.body), 160)
                    );
                    return write_json(
                        &mut stream,
                        400,
                        &json!({"ok": false, "error": "invalid hook json"}),
                    );
                }
            };
            if hook_payload_is_control_only(&payload) {
                return write_json(&mut stream, 200, &json!({"ok": true, "filtered": true}));
            }
            let event = normalize_event(&payload);
            match dispatch_task_event(&task_events, event.clone()) {
                Ok(TaskDispatchResult::Applied) => {
                    write_json(&mut stream, 200, &json!({"ok": true, "event": event}))
                }
                Ok(TaskDispatchResult::Filtered) => write_json(
                    &mut stream,
                    200,
                    &json!({"ok": true, "filtered": true, "event": event}),
                ),
                Ok(TaskDispatchResult::Queued) => write_json(
                    &mut stream,
                    202,
                    &json!({"ok": true, "queued": true, "event": event}),
                ),
                Ok(TaskDispatchResult::LockUnavailable) => write_json(
                    &mut stream,
                    500,
                    &json!({"ok": false, "error": "task state lock unavailable", "event": event}),
                ),
                Err(TaskQueueError::Full) => write_json(
                    &mut stream,
                    503,
                    &json!({"ok": false, "error": "task queue full", "event": event}),
                ),
                Err(TaskQueueError::Closed | TaskQueueError::CompletionDropped) => write_json(
                    &mut stream,
                    503,
                    &json!({"ok": false, "error": "task consumer unavailable", "event": event}),
                ),
            }
        }
        ("POST", "/restart") => {
            if !private_post_allowed(peer, &request, &config) {
                return write_json(
                    &mut stream,
                    403,
                    &json!({"ok": false, "error": "forbidden"}),
                );
            }
            schedule_bridge_restart();
            write_json(&mut stream, 202, &json!({"ok": true, "restarting": true}))
        }
        _ => write_json(
            &mut stream,
            404,
            &json!({"ok": false, "error": "not found"}),
        ),
    }
}

fn parse_hook_payload(body: &[u8]) -> Result<Value, serde_json::Error> {
    match serde_json::from_slice::<Value>(body) {
        Ok(payload) => Ok(payload),
        Err(error) => {
            let text = String::from_utf8_lossy(body);
            let sanitized = sanitize_unpaired_json_surrogates(text.as_ref());
            if sanitized != text.as_ref() {
                if let Ok(payload) = serde_json::from_str::<Value>(&sanitized) {
                    return Ok(payload);
                }
            }
            Err(error)
        }
    }
}

fn sanitize_unpaired_json_surrogates(text: &str) -> String {
    let mut sanitized = String::with_capacity(text.len());
    let mut index = 0;
    while index < text.len() {
        if let Some(codepoint) = json_unicode_escape_value(text, index) {
            if is_high_surrogate(codepoint) {
                let next_index = index + 6;
                if let Some(next_codepoint) = json_unicode_escape_value(text, next_index) {
                    if is_low_surrogate(next_codepoint) {
                        sanitized.push_str(&text[index..next_index + 6]);
                        index = next_index + 6;
                        continue;
                    }
                }
                sanitized.push_str("\\uFFFD");
                index += 6;
                continue;
            }
            if is_low_surrogate(codepoint) {
                sanitized.push_str("\\uFFFD");
                index += 6;
                continue;
            }
        }

        let Some(ch) = text[index..].chars().next() else {
            break;
        };
        sanitized.push(ch);
        index += ch.len_utf8();
    }
    sanitized
}

fn json_unicode_escape_value(text: &str, index: usize) -> Option<u16> {
    let bytes = text.as_bytes();
    if index + 6 > bytes.len()
        || bytes.get(index) != Some(&b'\\')
        || bytes.get(index + 1) != Some(&b'u')
    {
        return None;
    }

    let mut value = 0_u16;
    for byte in &bytes[index + 2..index + 6] {
        value = (value << 4) | hex_digit_value(*byte)?;
    }
    Some(value)
}

fn hex_digit_value(byte: u8) -> Option<u16> {
    match byte {
        b'0'..=b'9' => Some((byte - b'0') as u16),
        b'a'..=b'f' => Some((byte - b'a' + 10) as u16),
        b'A'..=b'F' => Some((byte - b'A' + 10) as u16),
        _ => None,
    }
}

fn is_high_surrogate(codepoint: u16) -> bool {
    (0xD800..=0xDBFF).contains(&codepoint)
}

fn is_low_surrogate(codepoint: u16) -> bool {
    (0xDC00..=0xDFFF).contains(&codepoint)
}

fn hook_payload_is_control_only(payload: &Value) -> bool {
    let Some(object) = payload.as_object() else {
        return false;
    };
    !object.is_empty() && object.keys().all(|key| key == "exclude")
}

fn spawn_task_event_consumer(
    state: SharedBridgeState,
    config: BridgeConfig,
    journal: Option<TaskEventJournal>,
    receiver: TaskEventReceiver,
) {
    std::thread::spawn(move || consume_task_events(state, config, journal, receiver));
}

fn spawn_discovery_responder(config: BridgeConfig) {
    std::thread::spawn(move || {
        if let Err(error) = run_discovery_responder(config) {
            eprintln!("discovery responder failed: {error}");
        }
    });
}

fn spawn_quota_refresh_loop(state: SharedBridgeState) {
    std::thread::spawn(move || loop {
        maybe_spawn_quota_refresh_if_due(Arc::clone(&state));
        std::thread::sleep(QUOTA_REFRESH_INTERVAL);
    });
}

fn configure_xiaozhi_proxy_if_configured(config: BridgeConfig, state: SharedBridgeState) {
    let Some(ws_url) = config.xiaozhi_ws_url.clone() else {
        return;
    };
    let mut state = state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    state.xiaozhi.upstream_configured = true;
    state.xiaozhi.configured = true;
    state.xiaozhi.upstream_ws_url = Some(ws_url);
    state.xiaozhi.upstream_token = config.xiaozhi_token.clone();
    state.xiaozhi.last_error = Some("Xiaozhi upstream proxy configured".to_string());
    state.xiaozhi.updated_at = Instant::now();
}

fn start_xiaozhi_proxy_thread(
    state: &SharedBridgeState,
    bridge_base: String,
    ws_url: String,
    token: Option<String>,
) {
    {
        let mut state = state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        state.xiaozhi.upstream_configured = true;
        state.xiaozhi.configured = true;
        state.xiaozhi.upstream_ws_url = Some(ws_url.clone());
        state.xiaozhi.upstream_token = token.clone();
        if state.xiaozhi.upstream_worker_active {
            state.xiaozhi.updated_at = Instant::now();
            return;
        }
        state.xiaozhi.upstream_worker_active = true;
        state.xiaozhi.last_error = Some("Xiaozhi upstream proxy configured".to_string());
        state.xiaozhi.updated_at = Instant::now();
    }

    let state = Arc::clone(state);
    thread::spawn(move || loop {
        let Some(script) = find_xiaozhi_proxy_script() else {
            eprintln!("Xiaozhi proxy script not found; upstream disabled");
            let mut state = state
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            state.xiaozhi.upstream_worker_active = false;
            state.xiaozhi.upstream_running = false;
            state.xiaozhi.last_error = Some("Xiaozhi proxy script not found".to_string());
            state.xiaozhi.updated_at = Instant::now();
            return;
        };

        let (ws_url, token) = {
            let mut state = state
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            if !state.xiaozhi.session_requested {
                state.xiaozhi.upstream_worker_active = false;
                state.xiaozhi.upstream_running = false;
                state.xiaozhi.connected = false;
                state.xiaozhi.updated_at = Instant::now();
                return;
            }
            let Some(ws_url) = state.xiaozhi.upstream_ws_url.clone() else {
                state.xiaozhi.upstream_worker_active = false;
                state.xiaozhi.upstream_running = false;
                state.xiaozhi.connected = false;
                state.xiaozhi.last_error = Some("Xiaozhi upstream URL missing".to_string());
                state.xiaozhi.updated_at = Instant::now();
                return;
            };
            (ws_url, state.xiaozhi.upstream_token.clone())
        };

        {
            let mut state = state
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            state.xiaozhi.upstream_running = true;
            state.xiaozhi.updated_at = Instant::now();
        }

        let mut command = Command::new("python");
        command
            .arg(script)
            .arg("--bridge")
            .arg(&bridge_base)
            .arg("--ws-url")
            .arg(&ws_url)
            .stdin(Stdio::null());
        if let Some(token) = token.as_deref().filter(|value| !value.is_empty()) {
            command.arg("--token").arg(token);
        }

        match command.status() {
            Ok(status) => eprintln!("Xiaozhi proxy exited: {status}"),
            Err(error) => eprintln!("Xiaozhi proxy failed to start: {error}"),
        }

        {
            let mut state = state
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            state.xiaozhi.upstream_running = false;
            state.xiaozhi.connected = false;
            if !state.xiaozhi.session_requested {
                state.xiaozhi.upstream_worker_active = false;
                state.xiaozhi.last_error = None;
                state.xiaozhi.updated_at = Instant::now();
                return;
            }
            state.xiaozhi.last_error = Some("Xiaozhi proxy exited; restarting".to_string());
            state.xiaozhi.updated_at = Instant::now();
        }
        thread::sleep(Duration::from_secs(2));
    });
}

fn find_xiaozhi_proxy_script() -> Option<PathBuf> {
    if let Some(path) = env_text("CODEX_ORNAMENT_XIAOZHI_PROXY_SCRIPT") {
        let path = PathBuf::from(path);
        if path.is_file() {
            return Some(path);
        }
    }
    let cwd_path = PathBuf::from("crates/codex-ornament-bridge/scripts/xiaozhi_proxy.py");
    if cwd_path.is_file() {
        return Some(cwd_path);
    }
    let local_path = PathBuf::from("scripts/xiaozhi_proxy.py");
    if local_path.is_file() {
        return Some(local_path);
    }
    env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(|parent| parent.join("xiaozhi_proxy.py")))
        .filter(|path| path.is_file())
}

fn run_discovery_responder(config: BridgeConfig) -> io::Result<()> {
    let port = bind_port(&config.bind).unwrap_or(8787);
    let socket = UdpSocket::bind(("0.0.0.0", port))?;
    socket.set_broadcast(true)?;
    eprintln!("codex ornament discovery listening on udp://0.0.0.0:{port}");

    let mut buffer = [0_u8; 256];
    loop {
        let (length, peer) = socket.recv_from(&mut buffer)?;
        let request = String::from_utf8_lossy(&buffer[..length]);
        if request.trim() != DISCOVERY_MAGIC {
            continue;
        }

        let Ok(info) = discover_info_for_peer(&config, peer) else {
            continue;
        };
        let Ok(response) = serde_json::to_vec(&info) else {
            continue;
        };
        let _ = socket.send_to(&response, peer);
    }
}

fn bridge_info(config: &BridgeConfig) -> BridgeInfo {
    BridgeInfo {
        service: "codex-ornament-bridge",
        observed_at: now_local(),
        event_log_enabled: config.event_log_path.is_some(),
        event_log_path: config
            .event_log_path
            .as_ref()
            .map(|path| path.display().to_string()),
        event_log_bytes: config.event_log_path.as_deref().and_then(event_log_size),
    }
}

fn ready_info(config: &BridgeConfig) -> ReadyInfo {
    let (event_log_writable, event_log_error) = match config.event_log_path.as_deref() {
        Some(path) => match probe_event_log_writable(path) {
            Ok(()) => (true, None),
            Err(error) => (false, Some(error.to_string())),
        },
        None => (true, None),
    };

    ReadyInfo {
        ok: event_log_writable,
        service: "codex-ornament-bridge",
        observed_at: now_local(),
        bind: config.bind.clone(),
        event_log_enabled: config.event_log_path.is_some(),
        event_log_path: config
            .event_log_path
            .as_ref()
            .map(|path| path.display().to_string()),
        event_log_bytes: config.event_log_path.as_deref().and_then(event_log_size),
        event_log_writable,
        event_log_error,
    }
}

fn bridge_metrics(state: &SharedBridgeState, config: &BridgeConfig) -> String {
    let ready = ready_info(config);
    let (
        active_task_count,
        done_seq,
        codex_done_seq,
        claude_done_seq,
        done_task_count,
        unmatched_stop_count,
        quota_refreshing,
        weather_refreshing,
    ) = match state.lock() {
        Ok(state) => (
            state.active_tasks.len(),
            state.done_seq,
            state.codex_done_seq,
            state.claude_done_seq,
            state.done_tasks.len(),
            state.unmatched_stops.len(),
            state.quota_refreshing,
            state.weather_refreshing,
        ),
        Err(_) => (0, 0, 0, 0, 0, 0, false, false),
    };

    let event_log_bytes = ready.event_log_bytes.unwrap_or(0);
    let mut metrics = String::with_capacity(768);
    push_metric_help(
        &mut metrics,
        "codex_ornament_bridge_ready",
        "1 when the bridge readiness checks pass.",
    );
    push_metric_gauge(
        &mut metrics,
        "codex_ornament_bridge_ready",
        bool_metric(ready.ok),
    );
    push_metric_help(
        &mut metrics,
        "codex_ornament_bridge_event_log_writable",
        "1 when the task event journal can be opened for append.",
    );
    push_metric_gauge(
        &mut metrics,
        "codex_ornament_bridge_event_log_writable",
        bool_metric(ready.event_log_writable),
    );
    push_metric_help(
        &mut metrics,
        "codex_ornament_bridge_event_log_bytes",
        "Current task event journal size in bytes.",
    );
    push_metric_gauge_u64(
        &mut metrics,
        "codex_ornament_bridge_event_log_bytes",
        event_log_bytes,
    );
    push_metric_help(
        &mut metrics,
        "codex_ornament_bridge_active_tasks",
        "Current number of active tasks tracked in memory.",
    );
    push_metric_gauge_usize(
        &mut metrics,
        "codex_ornament_bridge_active_tasks",
        active_task_count,
    );
    push_metric_help(
        &mut metrics,
        "codex_ornament_bridge_done_seq",
        "Monotonic done-task sequence counters.",
    );
    push_metric_gauge_u64_with_label(
        &mut metrics,
        "codex_ornament_bridge_done_seq",
        "source",
        "all",
        done_seq,
    );
    push_metric_gauge_u64_with_label(
        &mut metrics,
        "codex_ornament_bridge_done_seq",
        "source",
        "codex",
        codex_done_seq,
    );
    push_metric_gauge_u64_with_label(
        &mut metrics,
        "codex_ornament_bridge_done_seq",
        "source",
        "claude",
        claude_done_seq,
    );
    push_metric_help(
        &mut metrics,
        "codex_ornament_bridge_done_tasks",
        "Bounded in-memory done-task history length.",
    );
    push_metric_gauge_usize(
        &mut metrics,
        "codex_ornament_bridge_done_tasks",
        done_task_count,
    );
    push_metric_help(
        &mut metrics,
        "codex_ornament_bridge_unmatched_stops",
        "Bounded in-memory unmatched stop history length.",
    );
    push_metric_gauge_usize(
        &mut metrics,
        "codex_ornament_bridge_unmatched_stops",
        unmatched_stop_count,
    );
    push_metric_help(
        &mut metrics,
        "codex_ornament_bridge_quota_refreshing",
        "1 while a quota refresh worker is running.",
    );
    push_metric_gauge(
        &mut metrics,
        "codex_ornament_bridge_quota_refreshing",
        bool_metric(quota_refreshing),
    );
    push_metric_help(
        &mut metrics,
        "codex_ornament_bridge_weather_refreshing",
        "1 while a weather refresh worker is running.",
    );
    push_metric_gauge(
        &mut metrics,
        "codex_ornament_bridge_weather_refreshing",
        bool_metric(weather_refreshing),
    );
    metrics
}

fn bool_metric(value: bool) -> u8 {
    if value {
        1
    } else {
        0
    }
}

fn push_metric_help(metrics: &mut String, name: &str, help: &str) {
    metrics.push_str("# HELP ");
    metrics.push_str(name);
    metrics.push(' ');
    metrics.push_str(help);
    metrics.push('\n');
    metrics.push_str("# TYPE ");
    metrics.push_str(name);
    metrics.push_str(" gauge\n");
}

fn push_metric_gauge(metrics: &mut String, name: &str, value: u8) {
    metrics.push_str(name);
    metrics.push(' ');
    metrics.push_str(&value.to_string());
    metrics.push('\n');
}

fn push_metric_gauge_u64(metrics: &mut String, name: &str, value: u64) {
    metrics.push_str(name);
    metrics.push(' ');
    metrics.push_str(&value.to_string());
    metrics.push('\n');
}

fn push_metric_gauge_usize(metrics: &mut String, name: &str, value: usize) {
    metrics.push_str(name);
    metrics.push(' ');
    metrics.push_str(&value.to_string());
    metrics.push('\n');
}

fn push_metric_gauge_u64_with_label(
    metrics: &mut String,
    name: &str,
    label_name: &str,
    label_value: &str,
    value: u64,
) {
    metrics.push_str(name);
    metrics.push('{');
    metrics.push_str(label_name);
    metrics.push_str("=\"");
    metrics.push_str(label_value);
    metrics.push_str("\"} ");
    metrics.push_str(&value.to_string());
    metrics.push('\n');
}

fn event_log_size(path: &Path) -> Option<u64> {
    fs::metadata(path).ok().map(|metadata| metadata.len())
}

fn probe_event_log_writable(path: &Path) -> io::Result<()> {
    let mut file = open_task_event_journal(path)?;
    file.write_all(b"")?;
    file.flush()
}

fn consume_task_events(
    state: SharedBridgeState,
    config: BridgeConfig,
    mut journal: Option<TaskEventJournal>,
    receiver: TaskEventReceiver,
) {
    for queued in receiver {
        let result = consume_task_event(&state, &config, journal.as_mut(), queued.event);
        let _ = queued.completion.send(result);
    }
}

fn consume_task_event(
    state: &SharedBridgeState,
    config: &BridgeConfig,
    journal: Option<&mut TaskEventJournal>,
    event: TaskEvent,
) -> TaskDispatchResult {
    if event_is_non_task_lifecycle_hook(&event) {
        log_filtered_task_event("non-task lifecycle hook", &event);
        return TaskDispatchResult::Filtered;
    }

    if !event_in_scope(&event, config) {
        return TaskDispatchResult::Filtered;
    }

    if let Some(journal) = journal {
        if let Err(error) = journal.append(&event) {
            eprintln!(
                "task event journal append failed path={}: {error}",
                journal.path.display()
            );
        }
    }

    let Ok(mut state) = state.lock() else {
        return TaskDispatchResult::LockUnavailable;
    };
    apply_task_event(&mut state, event);
    TaskDispatchResult::Applied
}

impl TaskEventJournal {
    fn open(path: Option<&Path>, max_bytes: u64, compact_keep_events: usize) -> Option<Self> {
        let path = path?;
        match open_task_event_journal(path) {
            Ok(file) => Some(Self {
                path: path.to_path_buf(),
                file,
                max_bytes,
                compact_keep_events,
            }),
            Err(error) => {
                eprintln!(
                    "task event journal disabled path={}: {error}",
                    path.display()
                );
                None
            }
        }
    }

    fn append(&mut self, event: &TaskEvent) -> io::Result<()> {
        serde_json::to_writer(&mut self.file, event).map_err(io::Error::other)?;
        self.file.write_all(b"\n")?;
        self.file.flush()?;
        self.compact_if_needed()
    }

    fn compact_if_needed(&mut self) -> io::Result<()> {
        if self.max_bytes == 0 {
            return Ok(());
        }
        let size = self.file.metadata()?.len();
        if size <= self.max_bytes {
            return Ok(());
        }
        if self.compact_keep_events == 0 {
            return Ok(());
        }

        compact_task_event_journal(&self.path, self.compact_keep_events)?;
        self.file = open_task_event_journal(&self.path)?;
        Ok(())
    }
}

fn open_task_event_journal(path: &Path) -> io::Result<File> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    OpenOptions::new().create(true).append(true).open(path)
}

fn compact_task_event_journal(path: &Path, keep_events: usize) -> io::Result<()> {
    let mut events = VecDeque::new();
    if let Ok(file) = open_shared_read(path) {
        for line in BufReader::new(file).lines() {
            let line = line?;
            let line = line.trim();
            if line.is_empty() || serde_json::from_str::<TaskEvent>(line).is_err() {
                continue;
            }
            events.push_back(line.to_string());
            while events.len() > keep_events {
                events.pop_front();
            }
        }
    }

    let temp_path = path.with_extension("jsonl.tmp");
    {
        let mut temp = File::create(&temp_path)?;
        for event in &events {
            temp.write_all(event.as_bytes())?;
            temp.write_all(b"\n")?;
        }
        temp.flush()?;
    }
    fs::rename(&temp_path, path)?;
    eprintln!(
        "task event journal compacted path={} kept_events={}",
        path.display(),
        events.len()
    );
    Ok(())
}

fn restore_bridge_state_from_event_log(path: Option<&Path>) -> BridgeState {
    let Some(path) = path else {
        return BridgeState::default();
    };

    match bridge_state_from_event_log(path) {
        Ok(state) => state,
        Err(error) => {
            eprintln!(
                "task event journal restore skipped path={}: {error}",
                path.display()
            );
            BridgeState::default()
        }
    }
}

fn bridge_state_from_event_log(path: &Path) -> io::Result<BridgeState> {
    let file = match open_shared_read(path) {
        Ok(file) => file,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(BridgeState::default()),
        Err(error) => return Err(error),
    };
    let reader = BufReader::new(file);
    let mut state = BridgeState::default();
    let mut restored = 0_usize;
    let mut skipped = 0_usize;

    for (index, line) in reader.lines().enumerate() {
        let line = line?;
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        match serde_json::from_str::<TaskEvent>(line) {
            Ok(event) => {
                apply_task_event(&mut state, event);
                restored += 1;
            }
            Err(error) => {
                skipped += 1;
                eprintln!(
                    "task event journal line skipped path={} line={}: {error}",
                    path.display(),
                    index + 1
                );
            }
        }
    }

    if restored > 0 || skipped > 0 {
        eprintln!(
            "task event journal restored path={} events={} skipped={}",
            path.display(),
            restored,
            skipped
        );
    }
    Ok(state)
}

fn dispatch_task_event(
    sender: &TaskEventSender,
    event: TaskEvent,
) -> Result<TaskDispatchResult, TaskQueueError> {
    let (completion, completed) = mpsc::sync_channel(0);
    let queued = QueuedTaskEvent { event, completion };

    match sender.try_send(queued) {
        Ok(()) => match completed.recv_timeout(TASK_EVENT_ACK_TIMEOUT) {
            Ok(result) => Ok(result),
            Err(mpsc::RecvTimeoutError::Timeout) => Ok(TaskDispatchResult::Queued),
            Err(mpsc::RecvTimeoutError::Disconnected) => Err(TaskQueueError::CompletionDropped),
        },
        Err(TrySendError::Full(_)) => Err(TaskQueueError::Full),
        Err(TrySendError::Disconnected(_)) => Err(TaskQueueError::Closed),
    }
}

fn task_snapshot(state: &SharedBridgeState, config: &BridgeConfig) -> TaskSnapshot {
    let Ok(mut state) = state.lock() else {
        return TaskSnapshot {
            status: "error".to_string(),
            active_task_count: 0,
            task: None,
            active_tasks: Vec::new(),
            source_tasks: SourceTasks::default(),
            done_seq: 0,
            last_done_task: None,
            done_task_count: 0,
            unmatched_stop_count: 0,
        };
    };

    recover_active_task_if_needed(&mut state, config);
    dedupe_active_tasks_by_turn(&mut state);
    dedupe_active_tasks_by_session_forks(&mut state, config);
    reconcile_active_tasks(&mut state, config);
    let active_task_count = state.active_tasks.len();
    let active_tasks = ordered_active_tasks(&state);
    let source_tasks = source_task_summaries(&state, &active_tasks);
    let mut task = display_task(&state, &active_tasks);
    if matches!(
        task.as_ref().map(|event| event.status.as_str()),
        Some("done")
    ) {
        if let Some(combined) = combined_recent_done_task(&state) {
            task = Some(combined);
        }
    }
    let status = display_status(active_task_count, task.as_ref());

    TaskSnapshot {
        status,
        active_task_count,
        task,
        active_tasks,
        source_tasks,
        done_seq: state.done_seq,
        last_done_task: state.done_tasks.back().cloned(),
        done_task_count: state.done_tasks.len(),
        unmatched_stop_count: state.unmatched_stops.len(),
    }
}

fn apply_task_event(state: &mut BridgeState, event: TaskEvent) {
    if event_is_non_task_lifecycle_hook(&event) {
        return;
    }

    match event.status.as_str() {
        "running" => {
            let key = active_task_key_for_start(state, &event);
            insert_active_task(state, key, event.clone());
            state.task = Some(event);
        }
        "done" => {
            let matched_active_task = finish_active_task(state, &event);
            if matched_active_task {
                remember_done_task(state, event.clone());
                state.task = Some(event);
            } else if state.active_tasks.is_empty() {
                if event_has_task_identity(&event) {
                    remember_unmatched_stop(state, event);
                }
            } else if !event_has_task_identity(&event)
                && active_task_count_for_source(state, &event) == 1
            {
                remove_recent_active_task_for_source(state, &event);
                remember_done_task(state, event.clone());
                remember_unmatched_stop(state, event.clone());
                state.task = Some(event);
            } else {
                remember_unmatched_stop(state, event);
            }
        }
        _ => {
            state.task = Some(event);
        }
    }
}

fn event_is_non_task_lifecycle_hook(event: &TaskEvent) -> bool {
    event_is_empty_codex_start_hook(event)
        || event_is_control_payload_lifecycle_hook(event)
        || event_is_codex_desktop_lifecycle_hook(event)
}

fn event_is_empty_codex_start_hook(event: &TaskEvent) -> bool {
    event.kind == "UserPromptSubmit"
        && done_source(event) == DoneSource::Codex
        && event.turn_id.is_none()
        && event.message == event.kind
}

fn event_is_control_payload_lifecycle_hook(event: &TaskEvent) -> bool {
    event_is_lifecycle_hook(event)
        && done_source(event) == DoneSource::Codex
        && hook_payload_text_is_control_only(&event.message)
}

fn event_is_codex_desktop_lifecycle_hook(event: &TaskEvent) -> bool {
    event_is_lifecycle_hook(event)
        && done_source(event) == DoneSource::Codex
        && event
            .cwd
            .as_deref()
            .map(cwd_is_codex_desktop_app)
            .unwrap_or(false)
}

fn event_is_lifecycle_hook(event: &TaskEvent) -> bool {
    matches!(
        event.kind.as_str(),
        "UserPromptSubmit" | "Stop" | "agent-turn-complete"
    )
}

fn cwd_is_codex_desktop_app(cwd: &str) -> bool {
    let normalized = cwd.replace('/', "\\").to_ascii_lowercase();
    normalized.contains("\\windowsapps\\openai.codex_") && normalized.ends_with("\\app")
}

fn hook_payload_text_is_control_only(text: &str) -> bool {
    serde_json::from_str::<Value>(text)
        .ok()
        .map(|payload| hook_payload_is_control_only(&payload))
        .unwrap_or(false)
}

fn log_filtered_task_event(reason: &str, event: &TaskEvent) {
    eprintln!(
        "task event filtered: reason={reason}; kind={}; status={}; session_id={}; turn_id={}; cwd={}; message={}",
        event.kind,
        event.status,
        event.session_id.as_deref().unwrap_or("-"),
        event.turn_id.as_deref().unwrap_or("-"),
        event.cwd.as_deref().unwrap_or("-"),
        clip(&event.message, 120),
    );
}

fn event_has_task_identity(event: &TaskEvent) -> bool {
    event.session_id.is_some() || event.turn_id.is_some()
}

fn active_task_key_for_start(state: &mut BridgeState, event: &TaskEvent) -> String {
    if event.turn_id.is_some() {
        remove_active_tasks_for_same_session(state, event);
        remove_active_tasks_for_same_turn(state, event);
        if let Some(key) = stable_task_key(event) {
            return key;
        }
    }

    state.next_anonymous_task_id += 1;
    let source = task_source_key(event);
    match event.session_id.as_deref() {
        Some(session_id) => format!(
            "source:{source}:session:{session_id}:anonymous:{}",
            state.next_anonymous_task_id
        ),
        None => format!("source:{source}:anonymous:{}", state.next_anonymous_task_id),
    }
}

fn insert_active_task(state: &mut BridgeState, key: String, event: TaskEvent) {
    if !state.active_tasks.contains_key(&key) {
        state.active_order.push_back(key.clone());
    }
    state.active_tasks.insert(key, event);
}

fn finish_active_task(state: &mut BridgeState, event: &TaskEvent) -> bool {
    let matched_exact = stable_task_key(event)
        .and_then(|key| remove_active_task(state, &key))
        .is_some();
    let matched_same_turn = remove_active_tasks_for_same_turn(state, event) > 0;
    if matched_exact || matched_same_turn {
        return true;
    }

    if event.turn_id.is_none() {
        if let Some(session_id) = event.session_id.as_deref() {
            return remove_recent_active_task_for_session(state, session_id, event).is_some();
        }
    }

    false
}

fn remove_active_tasks_for_same_turn(state: &mut BridgeState, event: &TaskEvent) -> usize {
    let Some(turn_id) = event.turn_id.as_deref() else {
        return 0;
    };

    let keys = state
        .active_order
        .iter()
        .filter_map(|key| {
            state.active_tasks.get(key).and_then(|active| {
                (active.turn_id.as_deref() == Some(turn_id) && task_sources_match(active, event))
                    .then(|| key.clone())
            })
        })
        .collect::<Vec<_>>();
    let count = keys.len();
    for key in keys {
        remove_active_task(state, &key);
    }
    count
}

fn remove_active_tasks_for_same_session(state: &mut BridgeState, event: &TaskEvent) -> usize {
    let Some(session_id) = event.session_id.as_deref() else {
        return 0;
    };
    if event.turn_id.is_none() {
        return 0;
    }

    let keys = state
        .active_order
        .iter()
        .filter_map(|key| {
            state.active_tasks.get(key).and_then(|active| {
                (active.session_id.as_deref() == Some(session_id)
                    && task_sources_match(active, event))
                .then(|| key.clone())
            })
        })
        .collect::<Vec<_>>();
    let count = keys.len();
    for key in keys {
        remove_active_task(state, &key);
    }
    count
}

fn dedupe_active_tasks_by_turn(state: &mut BridgeState) {
    let mut latest_by_turn = HashMap::new();
    for key in state.active_order.iter() {
        let Some(event) = state.active_tasks.get(key) else {
            continue;
        };
        let Some(turn_id) = event.turn_id.as_deref() else {
            continue;
        };
        latest_by_turn.insert((task_source_key(event), turn_id.to_string()), key.clone());
    }

    let stale_keys = state
        .active_order
        .iter()
        .filter_map(|key| {
            let event = state.active_tasks.get(key)?;
            let turn_id = event.turn_id.as_deref()?;
            let latest_key = latest_by_turn.get(&(task_source_key(event), turn_id.to_string()))?;
            (latest_key != key).then(|| key.clone())
        })
        .collect::<Vec<_>>();

    for key in stale_keys {
        clear_active_task(state, &key);
    }
}

fn dedupe_active_tasks_by_session_forks(state: &mut BridgeState, config: &BridgeConfig) {
    let active_sessions = state
        .active_order
        .iter()
        .filter_map(|key| {
            let event = state.active_tasks.get(key)?;
            Some((
                key.clone(),
                event.clone(),
                event.session_id.as_deref()?.to_string(),
            ))
        })
        .collect::<Vec<_>>();
    if active_sessions.len() < 2 {
        return;
    }

    let mut ancestor_cache: HashMap<String, HashSet<String>> = HashMap::new();
    let mut stale_keys = HashSet::new();

    for (descendant_key, descendant_event, descendant_session) in &active_sessions {
        let ancestors = ancestor_cache
            .entry(descendant_session.clone())
            .or_insert_with(|| {
                session_ancestor_ids(&config.codex_home, descendant_session)
                    .into_iter()
                    .collect()
            });
        if ancestors.is_empty() {
            continue;
        }

        for (candidate_key, candidate_event, candidate_session) in &active_sessions {
            if candidate_key == descendant_key
                || !task_sources_match(candidate_event, descendant_event)
            {
                continue;
            }
            if ancestors.contains(candidate_session)
                && session_fork_happened_after_event(
                    &config.codex_home,
                    descendant_session,
                    candidate_event,
                )
            {
                stale_keys.insert(candidate_key.clone());
            }
        }
    }

    for key in stale_keys {
        clear_active_task(state, &key);
    }
}

fn remove_active_task(state: &mut BridgeState, key: &str) -> Option<TaskEvent> {
    let removed = state.active_tasks.remove(key);
    if removed.is_some() {
        state.active_order.retain(|candidate| candidate != key);
    }
    if let Some(event) = removed.as_ref() {
        forget_active_recovery_event(state, event);
    }
    removed
}

fn forget_active_recovery_event(state: &mut BridgeState, event: &TaskEvent) {
    let Some(event_key) = stable_task_key(event) else {
        return;
    };
    let Some(cache) = state.active_recovery.as_mut() else {
        return;
    };
    cache.events.retain(|candidate| {
        stable_task_key(candidate)
            .map(|candidate_key| candidate_key != event_key)
            .unwrap_or(true)
    });
}

fn remove_recent_active_task_for_session(
    state: &mut BridgeState,
    session_id: &str,
    terminal_event: &TaskEvent,
) -> Option<TaskEvent> {
    let key = state.active_order.iter().rev().find_map(|key| {
        state.active_tasks.get(key).and_then(|event| {
            (event.session_id.as_deref() == Some(session_id)
                && event.turn_id.is_none()
                && task_sources_match(event, terminal_event))
            .then(|| key.clone())
        })
    })?;
    remove_active_task(state, &key)
}

fn remove_recent_active_task_for_source(
    state: &mut BridgeState,
    terminal_event: &TaskEvent,
) -> Option<TaskEvent> {
    let key = state.active_order.iter().rev().find_map(|key| {
        state
            .active_tasks
            .get(key)
            .and_then(|event| task_sources_match(event, terminal_event).then(|| key.clone()))
    })?;
    remove_active_task(state, &key)
}

fn active_task_count_for_source(state: &BridgeState, terminal_event: &TaskEvent) -> usize {
    state
        .active_tasks
        .values()
        .filter(|event| task_sources_match(event, terminal_event))
        .count()
}

fn remember_done_task(state: &mut BridgeState, event: TaskEvent) {
    state.done_seq = state.done_seq.saturating_add(1);
    match done_source(&event) {
        DoneSource::Claude => state.claude_done_seq = state.claude_done_seq.saturating_add(1),
        DoneSource::Codex => state.codex_done_seq = state.codex_done_seq.saturating_add(1),
        DoneSource::Other => {}
    }
    state.done_tasks.push_back(event);
    trim_history(&mut state.done_tasks);
}

fn recover_active_task_if_needed(state: &mut BridgeState, config: &BridgeConfig) {
    let events = if let Some(session_id) = config.tracked_session_id.as_deref() {
        active_tasks_for_session(&config.codex_home, session_id)
    } else {
        cached_active_recovery_events(state, config)
    };

    for event in events {
        if !event_in_scope(&event, config) || active_task_is_already_tracked(state, &event) {
            continue;
        }
        apply_task_event(state, event);
    }
}

fn cached_active_recovery_events(state: &mut BridgeState, config: &BridgeConfig) -> Vec<TaskEvent> {
    if let Some(cache) = state.active_recovery.as_ref() {
        if cache.fetched_at.elapsed() < ACTIVE_RECOVERY_SCAN_TTL {
            return cache.events.clone();
        }
    }

    let events = active_tasks_in_recent_session_files(&config.codex_home);
    state.active_recovery = Some(CachedActiveRecovery {
        events: events.clone(),
        fetched_at: Instant::now(),
    });
    events
}

fn active_tasks_for_session(codex_home: &Path, session_id: &str) -> Vec<TaskEvent> {
    find_session_file(codex_home, session_id)
        .and_then(|session_file| active_tasks_in_session_file(&session_file, session_id).ok())
        .unwrap_or_default()
}

fn active_task_is_already_tracked(state: &BridgeState, event: &TaskEvent) -> bool {
    stable_task_key(event)
        .map(|key| state.active_tasks.contains_key(&key))
        .unwrap_or(false)
}

fn reconcile_active_tasks(state: &mut BridgeState, config: &BridgeConfig) {
    let active_keys = state.active_order.iter().cloned().collect::<Vec<_>>();
    for key in active_keys {
        let Some(event) = state.active_tasks.get(&key).cloned() else {
            continue;
        };

        if !event_in_scope(&event, config) {
            clear_active_task(state, &key);
            continue;
        }

        let Some(terminal) = terminal_turn_for_event(&config.codex_home, &event) else {
            if active_task_is_stale_against_logs(&config.codex_home, &event) {
                clear_active_task(state, &key);
            }
            continue;
        };

        clear_active_task(state, &key);
        if terminal.kind == "task_complete"
            && terminal
                .timestamp
                .as_deref()
                .map(|timestamp| timestamp_is_recent(timestamp, RECONCILED_DONE_NOTIFY_WINDOW))
                .unwrap_or(false)
        {
            let done_event = reconciled_done_event(&event, &terminal);
            remember_done_task(state, done_event.clone());
            state.task = Some(done_event);
        }
    }
}

fn clear_active_task(state: &mut BridgeState, key: &str) {
    if let Some(removed) = remove_active_task(state, key) {
        let task_matches_removed = state.task.as_ref() == Some(&removed);
        let task_matches_key =
            state.task.as_ref().and_then(stable_task_key).as_deref() == Some(key);
        if task_matches_removed || task_matches_key {
            state.task = None;
        }
    }
}

fn reconciled_done_event(start: &TaskEvent, terminal: &TerminalTurn) -> TaskEvent {
    TaskEvent {
        kind: terminal.kind.clone(),
        status: "done".to_string(),
        title: task_title_for("done", start.source.as_deref()),
        message: terminal
            .message
            .as_deref()
            .map(|message| clip(message.trim(), 160))
            .filter(|message| !message.is_empty())
            .unwrap_or_else(|| task_title_for("done", start.source.as_deref())),
        received_at: terminal.timestamp.clone().unwrap_or_else(now_local),
        source: start.source.clone(),
        session_id: start.session_id.clone(),
        turn_id: start.turn_id.clone(),
        cwd: start.cwd.clone(),
        model: start.model.clone(),
    }
}

fn event_in_scope(event: &TaskEvent, config: &BridgeConfig) -> bool {
    let Some(tracked_session_id) = config.tracked_session_id.as_deref() else {
        return true;
    };

    event
        .session_id
        .as_deref()
        .map(|session_id| session_id == tracked_session_id)
        .unwrap_or(false)
}

fn remember_unmatched_stop(state: &mut BridgeState, event: TaskEvent) {
    state.unmatched_stops.push_back(event);
    trim_history(&mut state.unmatched_stops);
}

fn trim_history(history: &mut VecDeque<TaskEvent>) {
    while history.len() > MAX_TASK_HISTORY {
        history.pop_front();
    }
}

fn ordered_active_tasks(state: &BridgeState) -> Vec<TaskEvent> {
    let mut tasks = Vec::new();
    for key in state.active_order.iter() {
        if let Some(event) = state.active_tasks.get(key) {
            tasks.push(event.clone());
            if tasks.len() >= MAX_STATE_TASKS {
                break;
            }
        }
    }
    tasks
}

fn display_task(state: &BridgeState, active_tasks: &[TaskEvent]) -> Option<TaskEvent> {
    if let Some(active_task) = active_tasks.first() {
        return Some(active_task.clone());
    }
    if let Some(task) = state.task.as_ref() {
        if matches!(task.status.as_str(), "done" | "error" | "event")
            && active_tasks
                .iter()
                .any(|active| stable_task_key(active) == stable_task_key(task))
        {
            return active_tasks.first().cloned().or_else(|| state.task.clone());
        }
    }
    if matches!(
        state.task.as_ref().map(|event| event.status.as_str()),
        Some("done" | "error" | "event")
    ) {
        return state.task.clone();
    }
    active_tasks.first().cloned().or_else(|| state.task.clone())
}

impl Default for SourceTasks {
    fn default() -> Self {
        Self {
            codex: SourceTaskSummary::empty(),
            claude: SourceTaskSummary::empty(),
        }
    }
}

impl SourceTaskSummary {
    fn empty() -> Self {
        Self {
            status: "done".to_string(),
            active_count: 0,
            done_seq: 0,
            task: None,
        }
    }
}

fn source_task_summaries(state: &BridgeState, active_tasks: &[TaskEvent]) -> SourceTasks {
    SourceTasks {
        codex: source_task_summary(state, active_tasks, DoneSource::Codex, state.codex_done_seq),
        claude: source_task_summary(
            state,
            active_tasks,
            DoneSource::Claude,
            state.claude_done_seq,
        ),
    }
}

fn source_task_summary(
    state: &BridgeState,
    active_tasks: &[TaskEvent],
    source: DoneSource,
    done_seq: u64,
) -> SourceTaskSummary {
    let active = active_tasks
        .iter()
        .filter(|event| done_source(event) == source)
        .cloned()
        .collect::<Vec<_>>();
    let active_count = active.len();
    let task = source_display_task(state, &active, source);
    let status = display_status(active_count, task.as_ref());
    SourceTaskSummary {
        status,
        active_count,
        done_seq,
        task,
    }
}

fn source_display_task(
    state: &BridgeState,
    active_tasks: &[TaskEvent],
    source: DoneSource,
) -> Option<TaskEvent> {
    if let Some(active_task) = active_tasks.first() {
        return Some(active_task.clone());
    }
    if let Some(task) = state.task.as_ref() {
        if matches!(task.status.as_str(), "done" | "error" | "event") && done_source(task) == source
        {
            return Some(task.clone());
        }
    }
    latest_done_task_for_source(state, source)
}

fn latest_done_task_for_source(state: &BridgeState, source: DoneSource) -> Option<TaskEvent> {
    state
        .done_tasks
        .iter()
        .rev()
        .find(|event| event.status == "done" && done_source(event) == source)
        .cloned()
}

fn combined_recent_done_task(state: &BridgeState) -> Option<TaskEvent> {
    let latest = state.done_tasks.back()?;
    if latest.status != "done" {
        return None;
    }

    let latest_at = DateTime::parse_from_rfc3339(&latest.received_at).ok()?;
    let mut saw_claude = false;
    let mut saw_codex = false;

    for event in state.done_tasks.iter().rev() {
        if event.status != "done" {
            continue;
        }
        let Some(age) = age_from_latest_done(latest_at, event) else {
            continue;
        };
        if age > COMBINED_DONE_SOURCE_WINDOW {
            break;
        }

        match done_source(event) {
            DoneSource::Claude => saw_claude = true,
            DoneSource::Codex => saw_codex = true,
            DoneSource::Other => {}
        }

        if saw_claude && saw_codex {
            let mut combined = latest.clone();
            combined.title = "Claude + Codex done".to_string();
            combined.message = "Claude + Codex done".to_string();
            combined.source = Some("claude+codex".to_string());
            return Some(combined);
        }
    }

    None
}

fn age_from_latest_done(latest_at: DateTime<FixedOffset>, event: &TaskEvent) -> Option<Duration> {
    let event_at = DateTime::parse_from_rfc3339(&event.received_at).ok()?;
    latest_at.signed_duration_since(event_at).to_std().ok()
}

fn done_source(event: &TaskEvent) -> DoneSource {
    if event
        .source
        .as_deref()
        .map(source_is_claude)
        .unwrap_or(false)
        || event.title.eq_ignore_ascii_case("Claude running")
        || event.title.eq_ignore_ascii_case("Claude done")
    {
        return DoneSource::Claude;
    }
    if event
        .source
        .as_deref()
        .map(source_is_codex)
        .unwrap_or(false)
        || event.source.is_none()
        || event.title.eq_ignore_ascii_case("Codex running")
        || event.title.eq_ignore_ascii_case("Codex done")
    {
        return DoneSource::Codex;
    }
    DoneSource::Other
}

fn source_is_claude(source: &str) -> bool {
    matches!(canonical_source(source).as_str(), "claude" | "claudecode")
}

fn source_is_codex(source: &str) -> bool {
    canonical_source(source) == "codex"
}

fn canonical_source(source: &str) -> String {
    source
        .chars()
        .filter(|ch| *ch != '_' && *ch != '-' && !ch.is_whitespace())
        .collect::<String>()
        .to_ascii_lowercase()
}

fn task_source_key(event: &TaskEvent) -> String {
    match done_source(event) {
        DoneSource::Claude => "claude".to_string(),
        DoneSource::Codex => "codex".to_string(),
        DoneSource::Other => event
            .source
            .as_deref()
            .map(canonical_source)
            .filter(|source| !source.is_empty())
            .unwrap_or_else(|| "other".to_string()),
    }
}

fn task_sources_match(active_event: &TaskEvent, terminal_event: &TaskEvent) -> bool {
    task_source_key(active_event) == task_source_key(terminal_event)
}

fn display_status(active_task_count: usize, task: Option<&TaskEvent>) -> String {
    if active_task_count > 0 {
        return "running".to_string();
    }
    if let Some(task) = task {
        if task.status == "done" || task.status == "error" || task.status == "event" {
            return task.status.clone();
        }
    }
    task.map(|event| event.status.clone())
        .unwrap_or_else(|| "done".to_string())
}

fn stable_task_key(event: &TaskEvent) -> Option<String> {
    let source = task_source_key(event);
    match (event.session_id.as_deref(), event.turn_id.as_deref()) {
        (Some(session_id), Some(turn_id)) => Some(format!(
            "source:{source}:session:{session_id}:turn:{turn_id}"
        )),
        (Some(session_id), None) => Some(format!("source:{source}:session:{session_id}")),
        (None, Some(turn_id)) => Some(format!("source:{source}:turn:{turn_id}")),
        (None, None) => None,
    }
}

fn read_request(stream: &mut TcpStream) -> io::Result<HttpRequest> {
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    let mut reader = BufReader::new(stream.try_clone()?);
    let mut request_line = String::new();
    reader.read_line(&mut request_line)?;
    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or_default().to_string();
    let raw_path = parts.next().unwrap_or("/").to_string();
    let path = raw_path
        .split_once('?')
        .map(|(path, _)| path.to_string())
        .unwrap_or_else(|| raw_path.clone());

    let mut headers = HashMap::new();
    loop {
        let mut line = String::new();
        reader.read_line(&mut line)?;
        let trimmed = line.trim_end_matches(['\r', '\n']);
        if trimmed.is_empty() {
            break;
        }
        if let Some((name, value)) = trimmed.split_once(':') {
            headers.insert(name.trim().to_ascii_lowercase(), value.trim().to_string());
        }
    }

    let content_length = headers
        .get("content-length")
        .and_then(|value| value.parse::<usize>().ok())
        .unwrap_or(0)
        .min(MAX_BODY_BYTES);
    let mut body = vec![0; content_length];
    if content_length > 0 {
        reader.read_exact(&mut body)?;
    }

    Ok(HttpRequest {
        method,
        path,
        raw_path,
        headers,
        body,
    })
}

fn discover_info(config: &BridgeConfig) -> io::Result<DiscoveryInfo> {
    let port = bind_port(&config.bind).unwrap_or(8787);
    let local_ip = local_lan_ip().unwrap_or_else(|| "127.0.0.1".to_string());
    Ok(DiscoveryInfo {
        service: "codex-ornament-bridge",
        local_ip: local_ip.clone(),
        state_url: format!("http://{local_ip}:{port}/state"),
        health_url: format!("http://{local_ip}:{port}/health"),
    })
}

fn discover_info_for_peer(config: &BridgeConfig, peer: SocketAddr) -> io::Result<DiscoveryInfo> {
    let port = bind_port(&config.bind).unwrap_or(8787);
    let local_ip = match peer.ip() {
        IpAddr::V4(ip) if !ip.is_loopback() => local_lan_ip_for_peer(IpAddr::V4(ip)),
        _ => local_lan_ip(),
    }
    .unwrap_or_else(|| "127.0.0.1".to_string());

    Ok(DiscoveryInfo {
        service: "codex-ornament-bridge",
        local_ip: local_ip.clone(),
        state_url: format!("http://{local_ip}:{port}/state"),
        health_url: format!("http://{local_ip}:{port}/health"),
    })
}

fn bind_port(bind: &str) -> Option<u16> {
    bind.rsplit_once(':')
        .and_then(|(_, port)| port.parse::<u16>().ok())
}

fn local_lan_ip() -> Option<String> {
    if let Some(ip) = configured_lan_ip() {
        return Some(ip);
    }

    [
        "192.168.1.1:80",
        "192.168.0.1:80",
        "10.0.0.1:80",
        "172.16.0.1:80",
        "8.8.8.8:80",
    ]
    .into_iter()
    .filter_map(|peer| peer.parse::<SocketAddr>().ok())
    .find_map(|peer| local_lan_ip_for_peer(peer.ip()))
}

fn local_lan_ip_for_peer(peer: IpAddr) -> Option<String> {
    let socket = UdpSocket::bind("0.0.0.0:0").ok()?;
    socket.connect(SocketAddr::new(peer, 80)).ok()?;
    let ip = socket.local_addr().ok()?.ip();
    match ip {
        IpAddr::V4(ip) if lan_discovery_ip_is_usable(ip) => Some(ip.to_string()),
        _ => None,
    }
}

fn configured_lan_ip() -> Option<String> {
    let ip = env_text("CODEX_ORNAMENT_LAN_IP")?;
    ip.parse::<Ipv4Addr>()
        .ok()
        .filter(|ip| lan_discovery_ip_is_usable(*ip))
        .map(|ip| ip.to_string())
}

fn lan_discovery_ip_is_usable(ip: Ipv4Addr) -> bool {
    let [first, second, ..] = ip.octets();
    let is_benchmark_or_proxy = first == 198 && matches!(second, 18 | 19);
    !ip.is_loopback()
        && !ip.is_unspecified()
        && !ip.is_link_local()
        && !ip.is_broadcast()
        && !ip.is_documentation()
        && !is_benchmark_or_proxy
}

fn post_allowed(peer: Option<SocketAddr>, request: &HttpRequest, config: &BridgeConfig) -> bool {
    if peer.map(|addr| is_loopback(addr.ip())).unwrap_or(false) {
        return true;
    }

    let Some(expected) = config.token.as_deref() else {
        return false;
    };
    request
        .headers
        .get("x-codex-ornament-token")
        .map(|actual| actual == expected)
        .unwrap_or(false)
}

fn private_post_allowed(
    peer: Option<SocketAddr>,
    request: &HttpRequest,
    config: &BridgeConfig,
) -> bool {
    peer.map(|addr| private_get_allowed(addr.ip()))
        .unwrap_or(false)
        || post_allowed(peer, request, config)
}

fn music_get_allowed(peer: Option<SocketAddr>) -> bool {
    peer.map(|addr| private_get_allowed(addr.ip()))
        .unwrap_or(false)
}

fn private_get_allowed(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(ip) => ip.is_loopback() || ip.is_private(),
        IpAddr::V6(ip) => ip.is_loopback() || ip.is_unique_local(),
    }
}

fn is_loopback(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(ip) => ip.is_loopback(),
        IpAddr::V6(ip) => ip.is_loopback(),
    }
}

fn schedule_bridge_restart() {
    std::thread::spawn(|| {
        std::thread::sleep(Duration::from_millis(150));
        if let Err(error) = restart_bridge_process() {
            eprintln!("bridge restart failed: {error}");
        }
    });
}

fn find_bridge_restart_root(exe: &Path) -> Option<PathBuf> {
    exe.ancestors().skip(1).find_map(|candidate| {
        let script = candidate
            .join("scripts")
            .join("start-codex-ornament-bridge.ps1");
        script.is_file().then(|| candidate.to_path_buf())
    })
}

fn restart_bridge_process() -> io::Result<()> {
    let exe = env::current_exe()?;
    let Some(root) = find_bridge_restart_root(&exe) else {
        return Err(io::Error::other(format!(
            "cannot determine repository root from {}",
            exe.display()
        )));
    };
    let script = root.join("scripts").join("start-codex-ornament-bridge.ps1");
    if !script.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("restart script not found: {}", script.display()),
        ));
    }

    Command::new("powershell.exe")
        .arg("-NoProfile")
        .arg("-ExecutionPolicy")
        .arg("Bypass")
        .arg("-File")
        .arg(script)
        .current_dir(root)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()?;

    std::process::exit(0);
}

fn handle_music_resolve(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    request: &HttpRequest,
    state: &SharedBridgeState,
    config: &BridgeConfig,
) -> io::Result<()> {
    if !music_get_allowed(peer) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }

    let music_request = match parse_music_request(request) {
        Ok(request) => request,
        Err(error) => {
            return write_json(
                stream,
                400,
                &json!({"ok": false, "error": error.to_string()}),
            );
        }
    };

    match resolve_ready_music_stream(state, config, &music_request) {
        Ok((resolved_request, song, _pcm_stream)) => write_json(
            stream,
            200,
            &music_resolve_response(config, peer, &resolved_request, &song),
        ),
        Err(error) => {
            eprintln!("music resolve failed: {error}");
            write_json(
                stream,
                map_music_status(&error),
                &json!({"ok": false, "error": error.to_string()}),
            )
        }
    }
}

fn handle_music_stream(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    request: &HttpRequest,
    state: &SharedBridgeState,
    config: &BridgeConfig,
) -> io::Result<()> {
    if !music_get_allowed(peer) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }

    let music_request = match parse_music_request(request) {
        Ok(request) => request,
        Err(error) => {
            return write_json(
                stream,
                400,
                &json!({"ok": false, "error": error.to_string()}),
            );
        }
    };

    let (_resolved_request, resolved, mut pcm_stream) =
        match resolve_ready_music_stream(state, config, &music_request) {
            Ok(stream) => stream,
            Err(error) => {
                eprintln!("music stream open failed: {error}");
                return write_json(
                    stream,
                    map_music_status(&error),
                    &json!({"ok": false, "error": error.to_string()}),
                );
            }
        };

    let headers = [
        (
            "X-Ornament-Music-Title",
            sanitize_header_value(&resolved.title),
        ),
        (
            "X-Ornament-Music-Artist",
            sanitize_header_value(&resolved.artist),
        ),
        (
            "X-Ornament-Music-Album",
            sanitize_header_value(&resolved.album),
        ),
    ];
    write_streaming_response(stream, 200, "audio/L16; rate=16000; channels=1", &headers)?;

    stream_ready_pcm_to_http(stream, &mut pcm_stream)
}

fn handle_music_cover(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    request: &HttpRequest,
    state: &SharedBridgeState,
    config: &BridgeConfig,
) -> io::Result<()> {
    if !music_get_allowed(peer) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }

    let music_request = match parse_music_request(request) {
        Ok(request) => request,
        Err(error) => {
            return write_json(
                stream,
                400,
                &json!({"ok": false, "error": error.to_string()}),
            );
        }
    };

    let query = request_query(request);
    let resolved = match query
        .get("coverKey")
        .and_then(|cover_key| cached_music_resolve_by_cover_key(state, cover_key))
    {
        Some(song) => song,
        None => match resolve_song_cached(state, config, &music_request) {
            Ok(song) => song,
            Err(error) => {
                eprintln!("music cover resolve failed: {error}");
                return write_json(
                    stream,
                    map_music_status(&error),
                    &json!({"ok": false, "error": error.to_string()}),
                );
            }
        },
    };
    if resolved.picture.trim().is_empty() {
        return write_json(
            stream,
            404,
            &json!({"ok": false, "error": "cover unavailable"}),
        );
    }

    match render_music_cover_rgb565(&resolved.picture) {
        Ok(cover) => {
            let headers = [
                ("X-Ornament-Cover-Width", MUSIC_COVER_SIZE.to_string()),
                ("X-Ornament-Cover-Height", MUSIC_COVER_SIZE.to_string()),
                (
                    "X-Ornament-Music-Title",
                    sanitize_header_value(&resolved.title),
                ),
            ];
            write_response_with_headers(stream, 200, "application/octet-stream", &headers, &cover)
        }
        Err(error) => {
            eprintln!("music cover render failed: {error}");
            write_json(
                stream,
                map_music_status(&error),
                &json!({"ok": false, "error": error.to_string()}),
            )
        }
    }
}

fn handle_xiaozhi_status(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    state: &SharedBridgeState,
) -> io::Result<()> {
    if !music_get_allowed(peer) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }

    write_json(stream, 200, &xiaozhi_status_snapshot(state))
}

fn handle_xiaozhi_session_start(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    request: &HttpRequest,
    state: &SharedBridgeState,
    config: &BridgeConfig,
) -> io::Result<()> {
    if !music_get_allowed(peer) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }

    let body = if request.body.is_empty() {
        XiaozhiSessionRequest {
            client_id: None,
            sample_rate: None,
            channels: None,
            format: None,
            ws_url: None,
            token: None,
        }
    } else {
        match serde_json::from_slice::<XiaozhiSessionRequest>(&request.body) {
            Ok(body) => body,
            Err(error) => {
                return write_json(
                    stream,
                    400,
                    &json!({"ok": false, "error": format!("invalid Xiaozhi session json: {error}")}),
                );
            }
        }
    };

    let bridge_base = format!(
        "http://127.0.0.1:{}",
        bind_port(&config.bind).unwrap_or(8787)
    );
    let response = xiaozhi_start_session(state, body, Some(bridge_base));
    write_json(stream, 200, &response)
}

fn handle_xiaozhi_session_stop(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    state: &SharedBridgeState,
) -> io::Result<()> {
    if !music_get_allowed(peer) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }

    let response = xiaozhi_stop_session(state);
    write_json(stream, 200, &response)
}

fn handle_xiaozhi_audio_uplink(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    request: &HttpRequest,
    state: &SharedBridgeState,
) -> io::Result<()> {
    if !music_get_allowed(peer) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }
    if !looks_like_pcm_s16le(request) {
        return write_json(
            stream,
            400,
            &json!({"ok": false, "error": "expected pcm_s16le body"}),
        );
    }

    let response = xiaozhi_push_uplink_pcm(state, &request.body);
    write_json(stream, 200, &response)
}

fn handle_xiaozhi_audio_downlink(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    request: &HttpRequest,
    state: &SharedBridgeState,
) -> io::Result<()> {
    if !music_get_allowed(peer) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }

    let query = request_query(request);
    let max_bytes = query
        .get("max")
        .and_then(|value| value.parse::<usize>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(XIAOZHI_PCM_CHUNK_BYTES)
        .min(MAX_BODY_BYTES);
    let wait_ms = query
        .get("wait_ms")
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(XIAOZHI_DOWNLINK_WAIT_MS)
        .min(1000);
    let deadline = Instant::now() + Duration::from_millis(wait_ms);

    loop {
        if let Some(pcm) = xiaozhi_pop_downlink_pcm(state, max_bytes) {
            return write_response(stream, 200, "audio/L16; rate=16000; channels=1", &pcm);
        }
        if Instant::now() >= deadline {
            return write_response(stream, 204, "application/octet-stream", b"");
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn handle_xiaozhi_audio_inject(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    request: &HttpRequest,
    state: &SharedBridgeState,
) -> io::Result<()> {
    if !music_get_allowed(peer) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }
    if !looks_like_pcm_s16le(request) {
        return write_json(
            stream,
            400,
            &json!({"ok": false, "error": "expected pcm_s16le body"}),
        );
    }

    let response = xiaozhi_push_downlink_pcm(state, &request.body);
    write_json(stream, 200, &response)
}

fn handle_xiaozhi_proxy_uplink(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    request: &HttpRequest,
    state: &SharedBridgeState,
) -> io::Result<()> {
    if !peer.map(|addr| is_loopback(addr.ip())).unwrap_or(false) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }

    let query = request_query(request);
    let max_bytes = query
        .get("max")
        .and_then(|value| value.parse::<usize>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(XIAOZHI_PCM_CHUNK_BYTES)
        .min(MAX_BODY_BYTES);
    let wait_ms = query
        .get("wait_ms")
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(XIAOZHI_UPLINK_READ_WAIT_MS)
        .min(1000);
    let deadline = Instant::now() + Duration::from_millis(wait_ms);

    loop {
        if let Some(pcm) = xiaozhi_pop_uplink_pcm(state, max_bytes) {
            return write_response(stream, 200, "audio/L16; rate=16000; channels=1", &pcm);
        }
        if Instant::now() >= deadline {
            return write_response(stream, 204, "application/octet-stream", b"");
        }
        thread::sleep(Duration::from_millis(20));
    }
}

fn handle_xiaozhi_proxy_status(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    request: &HttpRequest,
    state: &SharedBridgeState,
) -> io::Result<()> {
    if !peer.map(|addr| is_loopback(addr.ip())).unwrap_or(false) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }

    let update = match serde_json::from_slice::<XiaozhiStatusUpdate>(&request.body) {
        Ok(update) => update,
        Err(error) => {
            return write_json(
                stream,
                400,
                &json!({"ok": false, "error": format!("invalid Xiaozhi status json: {error}")}),
            );
        }
    };
    let response = xiaozhi_apply_status_update(state, update);
    write_json(stream, 200, &response)
}

fn xiaozhi_start_session(
    state: &SharedBridgeState,
    request: XiaozhiSessionRequest,
    bridge_base: Option<String>,
) -> XiaozhiProxyStatus {
    let request_ws_url = request
        .ws_url
        .as_deref()
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string);
    let request_token = request
        .token
        .as_deref()
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_string);

    let mut guard = state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    guard.xiaozhi.session_requested = true;
    guard.xiaozhi.connected = true;
    guard.xiaozhi.configured = true;
    guard.xiaozhi.client_id = request.client_id.filter(|value| !value.trim().is_empty());
    if let Some(ws_url) = request_ws_url.as_ref() {
        guard.xiaozhi.upstream_configured = true;
        guard.xiaozhi.upstream_ws_url = Some(ws_url.clone());
        guard.xiaozhi.upstream_token = request_token.clone();
    }
    let start_ws_url = request_ws_url
        .clone()
        .or_else(|| guard.xiaozhi.upstream_ws_url.clone());
    let start_token = if request_ws_url.is_some() {
        request_token.clone()
    } else {
        guard.xiaozhi.upstream_token.clone()
    };
    if start_ws_url.is_some() {
        guard.xiaozhi.upstream_configured = true;
    }
    guard.xiaozhi.state = XiaozhiProxyState::Listening;
    guard.xiaozhi.last_stt = None;
    guard.xiaozhi.last_tts = None;
    guard.xiaozhi.last_error = Some(match (
        request.sample_rate,
        request.channels,
        request.format.as_deref(),
        guard.xiaozhi.upstream_configured,
    ) {
        (Some(sample_rate), Some(channels), Some(format), false) => format!(
            "Xiaozhi PCM bridge ready; upstream proxy pending ({sample_rate} Hz, {channels} ch, {format})"
        ),
        (Some(sample_rate), Some(channels), Some(format), true) => format!(
            "Xiaozhi PCM bridge ready ({sample_rate} Hz, {channels} ch, {format})"
        ),
        (_, _, _, true) => "Xiaozhi PCM bridge ready".to_string(),
        _ => "Xiaozhi PCM bridge ready; upstream proxy pending".to_string(),
    });
    guard.xiaozhi.uplink_pcm.clear();
    guard.xiaozhi.downlink_pcm.clear();
    guard.xiaozhi.updated_at = Instant::now();
    let status = xiaozhi_status_from_session(&guard.xiaozhi);
    drop(guard);

    if let (Some(bridge_base), Some(ws_url)) = (bridge_base, start_ws_url) {
        start_xiaozhi_proxy_thread(state, bridge_base, ws_url, start_token);
        return xiaozhi_status_snapshot(state);
    }

    status
}

fn xiaozhi_stop_session(state: &SharedBridgeState) -> XiaozhiProxyStatus {
    let mut state = state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    state.xiaozhi.session_requested = false;
    state.xiaozhi.connected = false;
    state.xiaozhi.configured = false;
    state.xiaozhi.upstream_running = false;
    state.xiaozhi.state = XiaozhiProxyState::Idle;
    state.xiaozhi.last_error = None;
    state.xiaozhi.uplink_pcm.clear();
    state.xiaozhi.downlink_pcm.clear();
    state.xiaozhi.updated_at = Instant::now();
    xiaozhi_status_from_session(&state.xiaozhi)
}

fn xiaozhi_push_uplink_pcm(state: &SharedBridgeState, pcm: &[u8]) -> XiaozhiProxyStatus {
    let mut state = state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    if !state.xiaozhi.session_requested {
        state.xiaozhi.session_requested = true;
        state.xiaozhi.connected = true;
        state.xiaozhi.configured = true;
        state.xiaozhi.state = XiaozhiProxyState::Listening;
    }
    push_limited_pcm(
        &mut state.xiaozhi.uplink_pcm,
        pcm,
        XIAOZHI_UPLINK_BUFFER_MAX_BYTES,
    );
    state.xiaozhi.uplink_frames = state
        .xiaozhi
        .uplink_frames
        .saturating_add((pcm.len() / 2) as u64);
    state.xiaozhi.updated_at = Instant::now();
    xiaozhi_status_from_session(&state.xiaozhi)
}

fn xiaozhi_push_downlink_pcm(state: &SharedBridgeState, pcm: &[u8]) -> XiaozhiProxyStatus {
    let mut state = state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    state.xiaozhi.session_requested = true;
    state.xiaozhi.connected = true;
    state.xiaozhi.configured = true;
    state.xiaozhi.state = XiaozhiProxyState::Speaking;
    push_limited_pcm(
        &mut state.xiaozhi.downlink_pcm,
        pcm,
        XIAOZHI_DOWNLINK_BUFFER_MAX_BYTES,
    );
    state.xiaozhi.updated_at = Instant::now();
    xiaozhi_status_from_session(&state.xiaozhi)
}

fn xiaozhi_pop_uplink_pcm(state: &SharedBridgeState, max_bytes: usize) -> Option<Vec<u8>> {
    let mut state = state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    if !state.xiaozhi.session_requested || state.xiaozhi.uplink_pcm.is_empty() {
        return None;
    }

    let available = max_bytes.min(state.xiaozhi.uplink_pcm.len());
    let byte_count = available.saturating_sub(available % 2);
    if byte_count == 0 {
        return None;
    }

    let mut pcm = Vec::with_capacity(byte_count);
    for _ in 0..byte_count {
        if let Some(byte) = state.xiaozhi.uplink_pcm.pop_front() {
            pcm.push(byte);
        }
    }
    Some(pcm)
}

fn xiaozhi_pop_downlink_pcm(state: &SharedBridgeState, max_bytes: usize) -> Option<Vec<u8>> {
    let mut state = state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    if !state.xiaozhi.session_requested || state.xiaozhi.downlink_pcm.is_empty() {
        return None;
    }

    let byte_count = max_bytes
        .min(state.xiaozhi.downlink_pcm.len())
        .saturating_sub(max_bytes.min(state.xiaozhi.downlink_pcm.len()) % 2);
    if byte_count == 0 {
        return None;
    }

    let mut pcm = Vec::with_capacity(byte_count);
    for _ in 0..byte_count {
        if let Some(byte) = state.xiaozhi.downlink_pcm.pop_front() {
            pcm.push(byte);
        }
    }
    state.xiaozhi.downlink_frames = state
        .xiaozhi
        .downlink_frames
        .saturating_add((pcm.len() / 2) as u64);
    state.xiaozhi.state = if state.xiaozhi.downlink_pcm.is_empty() {
        XiaozhiProxyState::Listening
    } else {
        XiaozhiProxyState::Speaking
    };
    state.xiaozhi.updated_at = Instant::now();
    Some(pcm)
}

fn push_limited_pcm(buffer: &mut VecDeque<u8>, pcm: &[u8], limit: usize) {
    let even_len = pcm.len().saturating_sub(pcm.len() % 2);
    if even_len == 0 || limit < 2 {
        return;
    }
    let incoming = even_len.min(limit);
    while buffer.len() + incoming > limit {
        buffer.pop_front();
    }
    buffer.extend(pcm[even_len - incoming..even_len].iter().copied());
}

fn xiaozhi_apply_status_update(
    state: &SharedBridgeState,
    update: XiaozhiStatusUpdate,
) -> XiaozhiProxyStatus {
    let mut state = state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    if let Some(configured) = update.configured {
        state.xiaozhi.configured = configured;
    }
    if let Some(connected) = update.connected {
        state.xiaozhi.connected = connected;
    }
    if let Some(upstream_running) = update.upstream_running {
        state.xiaozhi.upstream_running = upstream_running;
    }
    if let Some(session_id) = update.session_id.filter(|value| !value.trim().is_empty()) {
        state.xiaozhi.session_id = session_id;
    }
    if let Some(last_error) = update.last_error {
        state.xiaozhi.last_error = if last_error.is_empty() {
            None
        } else {
            Some(last_error)
        };
    }
    if let Some(last_stt) = update.last_stt {
        state.xiaozhi.last_stt = if last_stt.is_empty() {
            None
        } else {
            Some(last_stt)
        };
    }
    if let Some(last_tts) = update.last_tts {
        state.xiaozhi.last_tts = if last_tts.is_empty() {
            None
        } else {
            Some(last_tts)
        };
    }
    if let Some(state_text) = update.state.as_deref() {
        state.xiaozhi.state = xiaozhi_state_from_name(state_text);
    }
    state.xiaozhi.updated_at = Instant::now();
    xiaozhi_status_from_session(&state.xiaozhi)
}

fn looks_like_pcm_s16le(request: &HttpRequest) -> bool {
    request.body.len().is_multiple_of(2)
}

fn xiaozhi_status_snapshot(state: &SharedBridgeState) -> XiaozhiProxyStatus {
    let state = state
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    xiaozhi_status_from_session(&state.xiaozhi)
}

fn xiaozhi_status_from_session(session: &XiaozhiProxySession) -> XiaozhiProxyStatus {
    XiaozhiProxyStatus {
        ok: true,
        state: xiaozhi_state_name(session.state),
        configured: session.configured,
        connected: session.connected,
        session_requested: session.session_requested,
        runtime_config: false,
        activation_pending: false,
        upstream_configured: session.upstream_configured,
        upstream_running: session.upstream_running,
        session_id: session.session_id.clone(),
        client_id: session.client_id.clone(),
        last_error: session.last_error.clone().unwrap_or_default(),
        last_stt: session.last_stt.clone().unwrap_or_default(),
        last_tts: session.last_tts.clone().unwrap_or_default(),
        activation_code: String::new(),
        activation_message: String::new(),
        uplink_frames: session.uplink_frames,
        downlink_frames: session.downlink_frames,
        observed_at: now_local(),
    }
}

fn xiaozhi_state_name(state: XiaozhiProxyState) -> &'static str {
    match state {
        XiaozhiProxyState::Idle => "idle",
        XiaozhiProxyState::ConfigMissing => "configMissing",
        XiaozhiProxyState::Connecting => "connecting",
        XiaozhiProxyState::Listening => "listening",
        XiaozhiProxyState::Speaking => "speaking",
        XiaozhiProxyState::Error => "error",
    }
}

fn xiaozhi_state_from_name(name: &str) -> XiaozhiProxyState {
    match name {
        "idle" => XiaozhiProxyState::Idle,
        "configMissing" | "config_missing" => XiaozhiProxyState::ConfigMissing,
        "connecting" => XiaozhiProxyState::Connecting,
        "listening" => XiaozhiProxyState::Listening,
        "speaking" => XiaozhiProxyState::Speaking,
        "error" => XiaozhiProxyState::Error,
        _ => XiaozhiProxyState::Error,
    }
}

fn music_resolve_response(
    config: &BridgeConfig,
    peer: Option<SocketAddr>,
    request: &MusicRequest,
    song: &ResolvedSong,
) -> MusicResolveResponse {
    MusicResolveResponse {
        ok: true,
        song: request.song.clone(),
        artist: request
            .artist
            .clone()
            .unwrap_or_else(|| song.artist.clone()),
        index: request.index,
        source: song.source,
        title: song.title.clone(),
        album: song.album.clone(),
        picture: song.picture.clone(),
        cover_url: music_cover_url(config, peer, request, Some(song)),
        url: music_stream_url(config, peer, request),
        duration_ms: song.duration_ms,
        lyrics: song.lyrics.clone(),
    }
}

fn resolve_song_cached(
    state: &SharedBridgeState,
    config: &BridgeConfig,
    request: &MusicRequest,
) -> io::Result<ResolvedSong> {
    if let Some(song) = cached_music_resolve(state, request) {
        return Ok(song);
    }

    let song = resolve_song(config, request)?;
    cache_music_resolve(state, request, &song);
    Ok(song)
}

fn resolve_ready_music_stream(
    state: &SharedBridgeState,
    config: &BridgeConfig,
    request: &MusicRequest,
) -> io::Result<(MusicRequest, ResolvedSong, ReadyPcmStream)> {
    let mut last_error = None;

    for offset in 0..MUSIC_STREAM_FALLBACK_ATTEMPTS {
        let candidate_request =
            music_request_with_index(request, request.index.saturating_add(offset));
        let resolved = match resolve_song_cached(state, config, &candidate_request) {
            Ok(song) => song,
            Err(error) => {
                last_error = Some(error);
                continue;
            }
        };

        match open_ready_pcm_stream(&resolved.url) {
            Ok(pcm_stream) => {
                let resolved = resolved_song_with_probed_duration(resolved);
                cache_music_resolve(state, &candidate_request, &resolved);
                if offset > 0 {
                    eprintln!(
                        "music stream fallback selected index {} for song {}",
                        candidate_request.index, request.song
                    );
                    cache_music_resolve(state, request, &resolved);
                }
                return Ok((candidate_request, resolved, pcm_stream));
            }
            Err(error) => {
                eprintln!(
                    "music stream candidate failed: song={} index={} title={} error={}",
                    request.song, candidate_request.index, resolved.title, error
                );
                last_error = Some(error);
            }
        }
    }

    Err(last_error.unwrap_or_else(|| io::Error::other("no playable music stream found")))
}

fn resolved_song_with_probed_duration(mut song: ResolvedSong) -> ResolvedSong {
    if song.duration_ms.is_none() {
        song.duration_ms = ffprobe_music_duration_ms(&song.url);
    }
    song
}

fn music_request_with_index(request: &MusicRequest, index: u32) -> MusicRequest {
    MusicRequest {
        song: request.song.clone(),
        artist: request.artist.clone(),
        index: index.max(1),
    }
}

fn cached_music_resolve(state: &SharedBridgeState, request: &MusicRequest) -> Option<ResolvedSong> {
    let cache_key = music_request_cache_key(request);
    let Ok(mut state) = state.lock() else {
        return None;
    };

    prune_music_resolve_cache(&mut state);
    state
        .music_resolves
        .get(&cache_key)
        .map(|cached| cached.song.clone())
}

fn cached_music_resolve_by_cover_key(
    state: &SharedBridgeState,
    cover_key: &str,
) -> Option<ResolvedSong> {
    let Ok(mut state) = state.lock() else {
        return None;
    };

    prune_music_resolve_cache(&mut state);
    state
        .music_resolves
        .values()
        .find(|cached| music_cover_key(&cached.song) == cover_key)
        .map(|cached| cached.song.clone())
}

fn cache_music_resolve(state: &SharedBridgeState, request: &MusicRequest, song: &ResolvedSong) {
    let cache_key = music_request_cache_key(request);
    let Ok(mut state) = state.lock() else {
        return;
    };

    prune_music_resolve_cache(&mut state);
    if state.music_resolves.len() >= MUSIC_RESOLVE_CACHE_MAX {
        if let Some(oldest_key) = state
            .music_resolves
            .iter()
            .min_by_key(|(_, cached)| cached.fetched_at)
            .map(|(key, _)| key.clone())
        {
            state.music_resolves.remove(&oldest_key);
        }
    }
    state.music_resolves.insert(
        cache_key,
        CachedMusicResolve {
            song: song.clone(),
            fetched_at: Instant::now(),
        },
    );
}

fn prune_music_resolve_cache(state: &mut BridgeState) {
    state
        .music_resolves
        .retain(|_, cached| cached.fetched_at.elapsed() <= MUSIC_RESOLVE_CACHE_TTL);
}

fn music_request_cache_key(request: &MusicRequest) -> String {
    format!(
        "{}\n{}\n{}",
        request.song.trim(),
        request.artist.as_deref().unwrap_or("").trim(),
        request.index
    )
}

fn music_cover_key(song: &ResolvedSong) -> String {
    let mut hasher = DefaultHasher::new();
    song.source.hash(&mut hasher);
    song.title.hash(&mut hasher);
    song.artist.hash(&mut hasher);
    song.album.hash(&mut hasher);
    song.picture.hash(&mut hasher);
    format!("{:016x}", hasher.finish())
}

fn parse_music_request(request: &HttpRequest) -> io::Result<MusicRequest> {
    let query = request_query(request);

    let song = query
        .get("song")
        .cloned()
        .filter(|value| !value.is_empty())
        .ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidInput, "missing song query parameter")
        })?;
    let artist = query
        .get("artist")
        .cloned()
        .filter(|value| !value.is_empty());
    let index = query
        .get("index")
        .and_then(|value| value.parse::<u32>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(1);

    Ok(MusicRequest {
        song,
        artist,
        index,
    })
}

fn request_query(request: &HttpRequest) -> HashMap<String, String> {
    let raw_query = request
        .raw_path
        .split_once('?')
        .map(|(_, query)| query)
        .unwrap_or("");
    parse_query_params(raw_query)
}

fn music_stream_url(
    config: &BridgeConfig,
    peer: Option<SocketAddr>,
    request: &MusicRequest,
) -> String {
    let base = music_public_base_url(config, peer);
    let mut url = format!("{}/v1/music/stream?song=", base.trim_end_matches('/'));
    url.push_str(&form_urlencode(&request.song));
    if let Some(artist) = request.artist.as_deref().filter(|value| !value.is_empty()) {
        url.push_str("&artist=");
        url.push_str(&form_urlencode(artist));
    }
    url.push_str("&index=");
    url.push_str(&request.index.to_string());
    url
}

fn music_cover_url(
    config: &BridgeConfig,
    peer: Option<SocketAddr>,
    request: &MusicRequest,
    song: Option<&ResolvedSong>,
) -> String {
    let base = music_public_base_url(config, peer);
    let mut url = format!("{}/v1/music/cover?song=", base.trim_end_matches('/'));
    url.push_str(&form_urlencode(&request.song));
    if let Some(artist) = request.artist.as_deref().filter(|value| !value.is_empty()) {
        url.push_str("&artist=");
        url.push_str(&form_urlencode(artist));
    }
    url.push_str("&index=");
    url.push_str(&request.index.to_string());
    if let Some(song) = song.filter(|song| !song.picture.trim().is_empty()) {
        url.push_str("&coverKey=");
        url.push_str(&music_cover_key(song));
    }
    url
}

fn music_public_base_url(config: &BridgeConfig, peer: Option<SocketAddr>) -> String {
    if config.music_public_base_url_locked {
        if let Some(base) = config.music_public_base_url.clone() {
            return base;
        }
    }

    if let Some(peer) = peer {
        if let IpAddr::V4(ip) = peer.ip() {
            if !ip.is_loopback() {
                if let Some(host) = local_lan_ip_for_peer(IpAddr::V4(ip)) {
                    let port = bind_port(&config.bind).unwrap_or(8787);
                    return format!("http://{host}:{port}");
                }
            }
        }
    }

    config
        .music_public_base_url
        .clone()
        .unwrap_or_else(|| discover_music_public_base_url(config))
}

fn discover_music_public_base_url(config: &BridgeConfig) -> String {
    let port = bind_port(&config.bind).unwrap_or(8787);
    let host = configured_lan_ip()
        .or_else(local_lan_ip)
        .unwrap_or_else(|| {
            bind_host_for_url(&config.bind).unwrap_or_else(|| "127.0.0.1".to_string())
        });
    format!("http://{host}:{port}")
}

fn bind_host_for_url(bind: &str) -> Option<String> {
    let host = bind.rsplit_once(':').map(|(host, _)| host).unwrap_or(bind);
    let host = host.trim_matches(['[', ']']);
    if host.is_empty() || host == "0.0.0.0" || host == "::" {
        None
    } else {
        Some(host.to_string())
    }
}

fn parse_query_params(query: &str) -> HashMap<String, String> {
    let mut values = HashMap::new();
    for pair in query.split('&') {
        if pair.is_empty() {
            continue;
        }
        let (key, value) = pair.split_once('=').unwrap_or((pair, ""));
        values.insert(percent_decode(key), percent_decode(value));
    }
    values
}

fn percent_decode(value: &str) -> String {
    let bytes = value.as_bytes();
    let mut decoded = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        match bytes[index] {
            b'+' => {
                decoded.push(b' ');
                index += 1;
            }
            b'%' if index + 2 < bytes.len() => {
                if let (Some(high), Some(low)) = (
                    hex_digit_value(bytes[index + 1]),
                    hex_digit_value(bytes[index + 2]),
                ) {
                    decoded.push(((high << 4) | low) as u8);
                    index += 3;
                } else {
                    decoded.push(bytes[index]);
                    index += 1;
                }
            }
            byte => {
                decoded.push(byte);
                index += 1;
            }
        }
    }
    String::from_utf8_lossy(&decoded).into_owned()
}

fn resolve_song(config: &BridgeConfig, request: &MusicRequest) -> io::Result<ResolvedSong> {
    if config.yaohud_key.is_some() {
        match resolve_song_yaohud(config, request) {
            Ok(song) => return Ok(song),
            Err(error) => {
                eprintln!(
                    "Yaohud music resolve failed for song={} index={} kind={:?}; falling back to NetEase",
                    request.song,
                    request.index,
                    error.kind()
                );
                return resolve_song_netease(request).map_err(|fallback_error| {
                    io::Error::new(
                        fallback_error.kind(),
                        format!(
                            "Yaohud provider unavailable; NetEase fallback failed ({fallback_error})"
                        ),
                    )
                });
            }
        }
    }

    resolve_song_netease(request)
}

fn resolve_song_yaohud(config: &BridgeConfig, request: &MusicRequest) -> io::Result<ResolvedSong> {
    let key = config
        .yaohud_key
        .as_deref()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "missing Yaohud key"))?;

    let query = match request.artist.as_deref() {
        Some(artist) if !artist.is_empty() => format!("{} {}", request.song, artist),
        _ => request.song.clone(),
    };
    let url = format!(
        "https://api.yaohud.cn/api/music/{}?key={}&msg={}&n={}",
        YAOHUD_MUSIC_TYPE,
        form_urlencode(key),
        form_urlencode(&query),
        request.index
    );

    let client = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(15))
        .build()
        .map_err(io_other)?;
    let body = fetch_yaohud_body(&client, &url)?;
    let parsed: YaohudMusicResponse = serde_json::from_str(&body)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;

    let code = parsed.code.unwrap_or(200);
    let message = parsed.msg.clone();
    let data = YaohudMusicData::from_response(parsed);

    if code != 200 {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            message.unwrap_or_else(|| "Yaohud resolve failed".to_string()),
        ));
    }

    let title = first_nonempty(&[
        data.name.as_deref(),
        data.title.as_deref(),
        data.song.as_deref(),
        data.songtitle.as_deref(),
        Some(request.song.as_str()),
    ])
    .unwrap_or_default()
    .to_string();
    let artist = first_nonempty(&[
        data.artist.as_deref(),
        data.singer.as_deref(),
        data.songname.as_deref(),
        request.artist.as_deref(),
    ])
    .unwrap_or("")
    .to_string();
    let album = data.album.clone().unwrap_or_default();
    let mid = yaohud_music_mid(&data);
    let picture = yaohud_picture_url(&data);
    let lyric_seed = data
        .lrctxt
        .clone()
        .or(data.lyrics.clone())
        .or(data.lrc.clone());
    let lyrics = fetch_yaohud_lyrics_for_mid(&client, key, mid.as_deref(), YAOHUD_MUSIC_TYPE)
        .or_else(|| normalize_lyrics(&client, lyric_seed))
        .or_else(|| fetch_netease_lyrics_for_request(request));
    let duration_ms = yaohud_duration_ms(&data);
    let url = data
        .url
        .or(data.musicurl)
        .filter(|value| !value.trim().is_empty())
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                "Yaohud response missing playable url",
            )
        })?;

    Ok(ResolvedSong {
        source: "yaohud",
        title,
        artist,
        album,
        picture,
        url,
        duration_ms,
        lyrics,
    })
}

fn yaohud_music_mid(data: &YaohudMusicData) -> Option<String> {
    first_nonempty(&[
        data.mid.as_deref(),
        data.id.as_deref(),
        data.songmid.as_deref(),
        data.hash.as_deref(),
    ])
    .map(str::to_string)
}

fn yaohud_picture_url(data: &YaohudMusicData) -> String {
    first_nonempty(&[
        data.picture.as_deref(),
        data.pic.as_deref(),
        data.cover.as_deref(),
        data.img.as_deref(),
        data.image.as_deref(),
        data.picurl.as_deref(),
        data.pic_url.as_deref(),
        data.album_pic.as_deref(),
    ])
    .unwrap_or("")
    .to_string()
}

fn music_duration_ms_from_value(value: Option<&Value>) -> Option<u32> {
    let value = value?;
    if let Some(number) = value.as_u64() {
        return normalize_music_duration_number(number);
    }
    if let Some(number) = value.as_f64() {
        return normalize_music_duration_float(number);
    }
    value
        .as_str()
        .and_then(|text| music_duration_ms_from_str(text.trim()))
}

fn normalize_music_duration_number(value: u64) -> Option<u32> {
    if value == 0 {
        return None;
    }
    if value > 12 * 60 * 60 {
        return u32::try_from(value).ok();
    }
    value
        .checked_mul(1000)
        .and_then(|ms| u32::try_from(ms).ok())
}

fn normalize_music_duration_float(value: f64) -> Option<u32> {
    if !value.is_finite() || value <= 0.0 {
        return None;
    }
    if value > 12.0 * 60.0 * 60.0 {
        return (value <= u32::MAX as f64).then_some(value.round() as u32);
    }
    let ms = value * 1000.0;
    (ms <= u32::MAX as f64).then_some(ms.round() as u32)
}

fn music_duration_ms_from_str(text: &str) -> Option<u32> {
    if text.is_empty() {
        return None;
    }
    if let Ok(value) = text.parse::<u64>() {
        return normalize_music_duration_number(value);
    }
    if let Ok(value) = text.parse::<f64>() {
        return normalize_music_duration_float(value);
    }
    let mut total = 0u64;
    let mut saw_part = false;
    for part in text.split(':') {
        let value = part.trim().parse::<u64>().ok()?;
        total = total.checked_mul(60)?.checked_add(value)?;
        saw_part = true;
    }
    if saw_part {
        return total
            .checked_mul(1000)
            .and_then(|ms| u32::try_from(ms).ok());
    }
    None
}

fn yaohud_duration_ms(data: &YaohudMusicData) -> Option<u32> {
    [
        data.duration_ms.as_ref(),
        data.duration_ms_camel.as_ref(),
        data.duration.as_ref(),
        data.interval.as_ref(),
        data.song_time.as_ref(),
    ]
    .into_iter()
    .find_map(music_duration_ms_from_value)
}

fn ffprobe_music_duration_ms(url: &str) -> Option<u32> {
    let url = url.trim();
    if url.is_empty() {
        return None;
    }

    let mut child = Command::new("ffprobe")
        .args([
            "-v",
            "error",
            "-user_agent",
            "Mozilla/5.0",
            "-headers",
            "Referer: https://music.163.com/\r\n",
            "-show_entries",
            "format=duration",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
            url,
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .ok()?;

    let started_at = Instant::now();
    loop {
        if let Ok(Some(_status)) = child.try_wait() {
            break;
        }
        if started_at.elapsed() >= MUSIC_DURATION_PROBE_TIMEOUT {
            let _ = child.kill();
            let _ = child.wait();
            return None;
        }
        thread::sleep(Duration::from_millis(25));
    }

    let mut stdout = String::new();
    child.stdout.take()?.read_to_string(&mut stdout).ok()?;
    music_duration_ms_from_str(stdout.trim())
}

fn fetch_yaohud_lyrics_for_mid(
    client: &reqwest::blocking::Client,
    key: &str,
    mid: Option<&str>,
    music_type: &str,
) -> Option<String> {
    let mid = mid?.trim();
    if mid.is_empty() {
        return None;
    }
    let url = yaohud_lrc_url(key, mid, music_type);
    let body = fetch_yaohud_body(client, &url).ok()?;
    parse_yaohud_lyrics_body(&body)
}

fn yaohud_lrc_url(key: &str, mid: &str, music_type: &str) -> String {
    format!(
        "https://api.yaohud.cn/api/music/lrc?key={}&mid={}&type={}",
        form_urlencode(key),
        form_urlencode(mid),
        form_urlencode(music_type)
    )
}

fn parse_yaohud_lyrics_body(body: &str) -> Option<String> {
    let trimmed = body.trim();
    if trimmed.is_empty() {
        return None;
    }
    if !trimmed.starts_with('{') {
        return Some(trimmed.to_string());
    }
    let parsed: Value = serde_json::from_str(trimmed).ok()?;
    if parsed
        .get("code")
        .and_then(Value::as_i64)
        .is_some_and(|code| code != 200)
    {
        return None;
    }
    lyrics_from_json_value(parsed.get("data")).or_else(|| lyrics_from_json_value(Some(&parsed)))
}

fn lyrics_from_json_value(value: Option<&Value>) -> Option<String> {
    let value = value?;
    if let Some(text) = value.as_str() {
        return nonempty_trimmed(text);
    }
    first_nonempty(&[
        value.get("lrc").and_then(Value::as_str),
        value.get("lyrics").and_then(Value::as_str),
        value.get("lrctxt").and_then(Value::as_str),
        value.get("lyric").and_then(Value::as_str),
    ])
    .and_then(nonempty_trimmed)
}

fn nonempty_trimmed(value: &str) -> Option<String> {
    let value = value.trim();
    if value.is_empty() {
        None
    } else {
        Some(value.to_string())
    }
}

fn normalize_lyrics(client: &reqwest::blocking::Client, value: Option<String>) -> Option<String> {
    let value = value?.trim().to_string();
    if value.is_empty() {
        return None;
    }
    if value.starts_with("http://") || value.starts_with("https://") {
        return fetch_text_url(client, &value)
            .ok()
            .map(|text| text.trim().to_string())
            .filter(|text| !text.is_empty() && !text.starts_with('{'));
    }
    Some(value)
}

fn fetch_text_url(client: &reqwest::blocking::Client, url: &str) -> io::Result<String> {
    client
        .get(url)
        .header("User-Agent", "Mozilla/5.0")
        .send()
        .and_then(|response| response.error_for_status())
        .and_then(|response| response.text())
        .map_err(io_other)
}

fn fetch_yaohud_body(client: &reqwest::blocking::Client, url: &str) -> io::Result<String> {
    let mut last_error = None;
    for attempt in 1..=YAOHUD_RESOLVE_ATTEMPTS {
        match client
            .get(url)
            .send()
            .and_then(|response| response.error_for_status())
            .and_then(|response| response.text())
        {
            Ok(body) => return Ok(body),
            Err(error) => {
                last_error = Some(error);
                if attempt < YAOHUD_RESOLVE_ATTEMPTS {
                    std::thread::sleep(YAOHUD_RESOLVE_RETRY_DELAY);
                }
            }
        }
    }

    Err(last_error
        .map(io_other)
        .unwrap_or_else(|| io::Error::other("Yaohud resolve failed")))
}

fn resolve_song_netease(request: &MusicRequest) -> io::Result<ResolvedSong> {
    let query = match request.artist.as_deref() {
        Some(artist) if !artist.is_empty() => format!("{} {}", request.song, artist),
        _ => request.song.clone(),
    };
    let limit = request.index.clamp(1, 10);
    let url = format!(
        "https://music.163.com/api/search/get/web?csrf_token=&type=1&s={}&limit={limit}&offset=0",
        form_urlencode(&query)
    );

    let client = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(15))
        .build()
        .map_err(io_other)?;
    let response = client
        .get(url)
        .header("User-Agent", "Mozilla/5.0")
        .header("Referer", "https://music.163.com/")
        .send()
        .map_err(io_other)?
        .error_for_status()
        .map_err(io_other)?;
    let body = response.text().map_err(io_other)?;
    let parsed: NeteaseSearchResponse = serde_json::from_str(&body)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;

    if parsed.code != 200 {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("NetEase search failed with code {}", parsed.code),
        ));
    }

    let songs = parsed
        .result
        .and_then(|result| result.songs)
        .filter(|songs| !songs.is_empty())
        .ok_or_else(|| {
            io::Error::new(io::ErrorKind::NotFound, "NetEase search returned no songs")
        })?;
    let index = (request.index.saturating_sub(1) as usize).min(songs.len() - 1);
    let song = &songs[index];
    let artist = song
        .artists
        .iter()
        .map(|artist| artist.name.as_str())
        .filter(|name| !name.trim().is_empty())
        .collect::<Vec<_>>()
        .join("/");
    let album = song
        .album
        .as_ref()
        .map(|album| album.name.clone())
        .unwrap_or_default();
    let picture = song
        .album
        .as_ref()
        .and_then(|album| album.pic_url.clone())
        .or_else(|| fetch_netease_album_picture(&client, song.id))
        .unwrap_or_default();
    let lyrics = fetch_netease_lyrics(&client, song.id);
    let url = format!(
        "https://music.163.com/song/media/outer/url?id={}.mp3",
        song.id
    );

    Ok(ResolvedSong {
        source: "netease",
        title: song.name.clone(),
        artist,
        album,
        picture,
        url,
        duration_ms: song.duration,
        lyrics,
    })
}

fn fetch_netease_album_picture(client: &reqwest::blocking::Client, song_id: u64) -> Option<String> {
    let url = format!("https://music.163.com/api/song/detail?ids=[{song_id}]");
    let body = client
        .get(url)
        .header("User-Agent", "Mozilla/5.0")
        .header("Referer", "https://music.163.com/")
        .send()
        .ok()?
        .error_for_status()
        .ok()?
        .text()
        .ok()?;
    let parsed: NeteaseSongDetailResponse = serde_json::from_str(&body).ok()?;
    parsed
        .songs
        .first()
        .and_then(|song| song.album.as_ref())
        .and_then(|album| album.pic_url.clone())
        .filter(|url| !url.trim().is_empty())
}

fn fetch_netease_lyrics(client: &reqwest::blocking::Client, song_id: u64) -> Option<String> {
    let url = format!("https://music.163.com/api/song/lyric?id={song_id}&lv=1&kv=1&tv=-1");
    let body = client
        .get(url)
        .header("User-Agent", "Mozilla/5.0")
        .header("Referer", "https://music.163.com/")
        .send()
        .ok()?
        .error_for_status()
        .ok()?
        .text()
        .ok()?;
    let parsed: NeteaseLyricResponse = serde_json::from_str(&body).ok()?;
    parsed
        .lrc
        .and_then(|lrc| lrc.lyric)
        .filter(|lyric| !lyric.trim().is_empty())
}

fn fetch_netease_lyrics_for_request(request: &MusicRequest) -> Option<String> {
    resolve_song_netease(request)
        .ok()
        .and_then(|song| song.lyrics)
}

fn first_nonempty<'a>(values: &[Option<&'a str>]) -> Option<&'a str> {
    values
        .iter()
        .copied()
        .flatten()
        .find(|value| !value.trim().is_empty())
}

fn form_urlencode(value: &str) -> Cow<'_, str> {
    let mut encoded = String::with_capacity(value.len());
    let mut changed = false;
    for byte in value.bytes() {
        match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                encoded.push(byte as char)
            }
            b' ' => {
                encoded.push('+');
                changed = true;
            }
            _ => {
                encoded.push('%');
                encoded.push_str(&format!("{byte:02X}"));
                changed = true;
            }
        }
    }
    if changed {
        Cow::Owned(encoded)
    } else {
        Cow::Borrowed(value)
    }
}

fn render_music_cover_rgb565(url: &str) -> io::Result<Vec<u8>> {
    let vf = format!(
        "scale={0}:{0}:force_original_aspect_ratio=increase,crop={0}:{0},format=rgb565le",
        MUSIC_COVER_SIZE
    );
    let output = Command::new("ffmpeg")
        .args([
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-user_agent",
            "Mozilla/5.0",
            "-headers",
            "Referer: https://music.163.com/\r\n",
            "-i",
            url,
            "-frames:v",
            "1",
            "-vf",
            &vf,
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgb565le",
            "pipe:1",
        ])
        .stdin(Stdio::null())
        .output()
        .map_err(io_other)?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        return Err(ffmpeg_stream_error(output.status, &stderr));
    }
    if output.stdout.len() != MUSIC_COVER_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "ffmpeg cover output has {} bytes, expected {}",
                output.stdout.len(),
                MUSIC_COVER_BYTES
            ),
        ));
    }
    Ok(output.stdout)
}

fn render_standby_wallpaper_rgb565(path: &Path) -> io::Result<Vec<u8>> {
    let vf = standby_wallpaper_ffmpeg_filter();
    let path_text = path.to_str().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "wallpaper path is not valid UTF-8",
        )
    })?;
    let expected_bytes =
        DEFAULT_STANDBY_WALLPAPER_WIDTH as usize * DEFAULT_STANDBY_WALLPAPER_HEIGHT as usize * 2;
    let output = Command::new("ffmpeg")
        .args([
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            path_text,
            "-frames:v",
            "1",
            "-vf",
            &vf,
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgb565le",
            "pipe:1",
        ])
        .stdin(Stdio::null())
        .output()
        .map_err(io_other)?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        return Err(ffmpeg_stream_error(output.status, &stderr));
    }
    if output.stdout.len() != expected_bytes {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "ffmpeg wallpaper output has {} bytes, expected {}",
                output.stdout.len(),
                expected_bytes
            ),
        ));
    }
    Ok(output.stdout)
}

fn standby_wallpaper_ffmpeg_filter() -> String {
    format!(
        "scale={w}:{h}:force_original_aspect_ratio=increase:flags=lanczos,crop={w}:{h},format=rgb565le",
        w = DEFAULT_STANDBY_WALLPAPER_WIDTH,
        h = DEFAULT_STANDBY_WALLPAPER_HEIGHT
    )
}

fn spawn_ffmpeg_pcm_stream(url: &str) -> io::Result<Child> {
    Command::new("ffmpeg")
        .args([
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-user_agent",
            "Mozilla/5.0",
            "-headers",
            "Referer: https://music.163.com/\r\n",
            "-seekable",
            "0",
            "-reconnect",
            "1",
            "-reconnect_streamed",
            "1",
            "-reconnect_delay_max",
            "2",
            "-i",
            url,
            "-f",
            "s16le",
            "-ac",
            "1",
            "-ar",
            "16000",
            "pipe:1",
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
}

fn open_ready_pcm_stream(url: &str) -> io::Result<ReadyPcmStream> {
    let mut child = spawn_ffmpeg_pcm_stream(url)?;
    let mut stdout = child
        .stdout
        .take()
        .ok_or_else(|| io::Error::other("ffmpeg stdout unavailable"))?;
    let mut stderr = child
        .stderr
        .take()
        .ok_or_else(|| io::Error::other("ffmpeg stderr unavailable"))?;
    let stderr_thread = std::thread::spawn(move || -> String {
        let mut stderr_text = String::new();
        let _ = stderr.read_to_string(&mut stderr_text);
        stderr_text
    });

    let mut buffer = vec![0_u8; DEFAULT_STREAM_COPY_BYTES];
    let read = stdout.read(&mut buffer)?;
    if read > 0 {
        buffer.truncate(read);
        return Ok(ReadyPcmStream {
            child,
            stdout,
            stderr_thread: Some(stderr_thread),
            first_chunk: buffer,
        });
    }

    let status = child.wait()?;
    let stderr_text = stderr_thread.join().unwrap_or_default();
    Err(ffmpeg_stream_error(status, &stderr_text))
}

fn stream_ready_pcm_to_http(
    stream: &mut TcpStream,
    pcm_stream: &mut ReadyPcmStream,
) -> io::Result<()> {
    if !pcm_stream.first_chunk.is_empty() {
        stream.write_all(&pcm_stream.first_chunk)?;
    }

    let mut buffer = vec![0_u8; DEFAULT_STREAM_COPY_BYTES];
    loop {
        let read = pcm_stream.stdout.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        stream.write_all(&buffer[..read])?;
    }
    stream.flush()?;

    let status = pcm_stream.child.wait()?;
    let stderr_text = pcm_stream
        .stderr_thread
        .take()
        .map(|stderr_thread| stderr_thread.join().unwrap_or_default())
        .unwrap_or_default();
    if status.success() {
        Ok(())
    } else {
        Err(ffmpeg_stream_error(status, &stderr_text))
    }
}

fn ffmpeg_stream_error(status: ExitStatus, stderr_text: &str) -> io::Error {
    io::Error::other(format!(
        "ffmpeg exited with status {status}: {}",
        clip(stderr_text.trim(), 200)
    ))
}

fn sanitize_header_value(value: &str) -> String {
    value
        .chars()
        .map(|ch| if ch == '\r' || ch == '\n' { ' ' } else { ch })
        .collect::<String>()
}

fn write_streaming_response(
    stream: &mut TcpStream,
    status: u16,
    content_type: &str,
    extra_headers: &[(&str, String)],
) -> io::Result<()> {
    let reason = match status {
        200 => "OK",
        202 => "Accepted",
        204 => "No Content",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
        _ => "OK",
    };
    write!(
        stream,
        "HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type, Authorization, X-Codex-Ornament-Token, Access-Control-Request-Private-Network\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Private-Network: true\r\nConnection: close\r\n"
    )?;
    for (name, value) in extra_headers {
        write!(stream, "{name}: {value}\r\n")?;
    }
    write!(stream, "\r\n")?;
    Ok(())
}

fn map_music_status(error: &io::Error) -> u16 {
    match error.kind() {
        io::ErrorKind::InvalidInput => 400,
        io::ErrorKind::NotFound => 404,
        io::ErrorKind::PermissionDenied => 403,
        _ => 500,
    }
}

fn normalize_event(payload: &Value) -> TaskEvent {
    let kind = text_field(payload, &["hook_event_name", "type"])
        .unwrap_or_else(|| "codex-event".to_string());
    let source = event_source(payload);
    let session_id = event_text_field(
        payload,
        &["session_id", "sessionId", "thread-id", "thread_id"],
    );
    let turn_id = event_turn_id(payload, source.as_deref());
    let status = match kind.as_str() {
        "UserPromptSubmit" => "running",
        "Stop" | "agent-turn-complete" => "done",
        "InvalidJson" => "error",
        _ => "event",
    }
    .to_string();
    let title = task_title_for(&status, source.as_deref());
    let message = event_message(payload).unwrap_or_else(|| kind.clone());

    TaskEvent {
        kind,
        status,
        title,
        message,
        received_at: now_local(),
        source,
        session_id,
        turn_id,
        cwd: event_text_field(payload, &["cwd"]).map(|value| clip(&value, 120)),
        model: event_text_field(payload, &["model"]),
    }
}

fn event_source(payload: &Value) -> Option<String> {
    text_field(payload, &["source", "agent", "client"])
        .map(|value| value.trim().to_ascii_lowercase())
        .filter(|value| !value.is_empty())
        .or_else(|| payload_looks_like_claude_hook(payload).then(|| "claude".to_string()))
}

fn payload_looks_like_claude_hook(payload: &Value) -> bool {
    event_text_field(payload, &["transcript_path", "transcriptPath"])
        .map(|value| path_looks_like_claude_transcript(&value))
        .unwrap_or(false)
}

fn path_looks_like_claude_transcript(path: &str) -> bool {
    let normalized = normalize_path_for_identity(path);
    normalized.contains("\\.claude\\")
}

fn event_turn_id(payload: &Value, source: Option<&str>) -> Option<String> {
    derived_claude_turn_id(payload, source)
        .or_else(|| event_text_field(payload, &["turn_id", "turnId", "turn-id"]))
}

fn derived_claude_turn_id(payload: &Value, source: Option<&str>) -> Option<String> {
    if !source.map(source_is_claude).unwrap_or(false) {
        return None;
    }

    let transcript_path = event_text_field(payload, &["transcript_path", "transcriptPath"])?;
    if !path_looks_like_claude_transcript(&transcript_path) {
        return None;
    }

    Some(format!(
        "claude-transcript-{:016x}",
        stable_text_hash(&normalize_path_for_identity(&transcript_path))
    ))
}

fn normalize_path_for_identity(path: &str) -> String {
    let normalized = path.replace('/', "\\");
    normalized
        .strip_prefix("\\\\?\\")
        .unwrap_or(&normalized)
        .to_ascii_lowercase()
}

fn stable_text_hash(text: &str) -> u64 {
    let mut hash = 0xcbf29ce484222325_u64;
    for byte in text.as_bytes() {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash
}

fn task_title_for(status: &str, source: Option<&str>) -> String {
    let prefix = match source {
        Some(source) if source_is_claude(source) => "Claude",
        _ => "Codex",
    };
    match status {
        "running" => format!("{prefix} running"),
        "done" => format!("{prefix} done"),
        "error" => format!("{prefix} hook error"),
        _ => format!("{prefix} event"),
    }
}

fn event_message(payload: &Value) -> Option<String> {
    text_field(
        payload,
        &[
            "last_assistant_message",
            "last-assistant-message",
            "prompt",
            "message",
        ],
    )
    .or_else(|| {
        payload
            .get("input-messages")
            .and_then(Value::as_array)
            .and_then(|messages| messages.last())
            .and_then(Value::as_str)
            .map(str::to_string)
    })
    .map(|value| clip(value.trim(), 160))
    .filter(|value| !value.is_empty())
}

fn text_field(payload: &Value, keys: &[&str]) -> Option<String> {
    keys.iter()
        .filter_map(|key| payload.get(*key))
        .find_map(Value::as_str)
        .map(str::to_string)
        .filter(|value| !value.is_empty())
}

fn event_text_field(payload: &Value, keys: &[&str]) -> Option<String> {
    text_field(payload, keys).or_else(|| nested_event_text_field(payload, keys))
}

fn nested_event_text_field(payload: &Value, keys: &[&str]) -> Option<String> {
    ["message", "raw"].iter().find_map(|container_key| {
        let text = payload.get(*container_key)?.as_str()?.trim();
        text_field_from_json_text(text, keys)
            .or_else(|| text_field_from_partial_json_text(text, keys))
    })
}

fn text_field_from_json_text(text: &str, keys: &[&str]) -> Option<String> {
    let parsed = serde_json::from_str::<Value>(text).ok()?;
    text_field(&parsed, keys)
}

fn text_field_from_partial_json_text(text: &str, keys: &[&str]) -> Option<String> {
    keys.iter()
        .find_map(|key| quoted_json_string_value(text, key))
        .filter(|value| !value.is_empty())
}

fn quoted_json_string_value(text: &str, key: &str) -> Option<String> {
    let quoted_key = format!("\"{key}\"");
    let after_key = text.split_once(&quoted_key)?.1;
    let after_colon = after_key.trim_start().strip_prefix(':')?.trim_start();
    let mut chars = after_colon.chars();
    if chars.next()? != '"' {
        return None;
    }

    let mut value = String::new();
    let mut escaped = false;
    for ch in chars {
        if escaped {
            value.push(match ch {
                '"' => '"',
                '\\' => '\\',
                '/' => '/',
                'b' => '\u{0008}',
                'f' => '\u{000c}',
                'n' => '\n',
                'r' => '\r',
                't' => '\t',
                other => other,
            });
            escaped = false;
            continue;
        }

        match ch {
            '\\' => escaped = true,
            '"' => return Some(value),
            other => value.push(other),
        }
    }

    (!value.is_empty()).then_some(value)
}

fn terminal_turn_for_event(codex_home: &Path, event: &TaskEvent) -> Option<TerminalTurn> {
    let session_id = event.session_id.as_deref()?;
    let turn_id = event.turn_id.as_deref()?;
    let session_file = find_session_file(codex_home, session_id)?;
    terminal_turn_in_file(&session_file, turn_id).ok().flatten()
}

fn active_task_is_stale_against_logs(codex_home: &Path, event: &TaskEvent) -> bool {
    match done_source(event) {
        DoneSource::Codex => active_task_is_stale_against_session_log(codex_home, event),
        DoneSource::Claude => active_task_is_stale_against_claude_transcript(codex_home, event),
        DoneSource::Other => false,
    }
}

fn active_task_is_stale_against_session_log(codex_home: &Path, event: &TaskEvent) -> bool {
    let Some(session_id) = event.session_id.as_deref() else {
        return false;
    };
    let Some(turn_id) = event.turn_id.as_deref() else {
        return false;
    };

    let Some(session_file) = find_session_file(codex_home, session_id) else {
        return timestamp_is_older_than(&event.received_at, ACTIVE_SESSION_FILE_MISSING_GRACE);
    };

    if turn_is_terminal_in_session_file(&session_file, turn_id)
        .ok()
        .unwrap_or(false)
    {
        return true;
    }

    if active_task_has_newer_turn_in_session_file(&session_file, event)
        .ok()
        .unwrap_or(false)
    {
        return true;
    }

    match active_tasks_in_session_file(&session_file, session_id) {
        Ok(active_tasks) => {
            let session_idle_stale = session_file_last_timestamp(&session_file)
                .ok()
                .flatten()
                .is_some_and(|timestamp| {
                    timestamp_is_older_than(&timestamp, ACTIVE_SESSION_IDLE_STALE_GRACE)
                        && timestamp_is_older_than(
                            &event.received_at,
                            ACTIVE_SESSION_IDLE_STALE_GRACE,
                        )
                });

            if active_tasks
                .iter()
                .any(|active| active.turn_id.as_deref() == Some(turn_id))
            {
                return session_idle_stale;
            }

            let same_cwd_newer_active = event.cwd.as_deref().and_then(|cwd| {
                active_tasks
                    .iter()
                    .filter(|active| active.cwd.as_deref() == Some(cwd))
                    .filter_map(|active| active.turn_id.as_deref())
                    .find(|candidate_turn_id| *candidate_turn_id != turn_id)
            });
            if same_cwd_newer_active.is_some() {
                return true;
            }

            if session_idle_stale {
                return true;
            }

            active_tasks.is_empty()
                && timestamp_is_older_than(&event.received_at, ACTIVE_SESSION_FILE_MISSING_GRACE)
        }
        Err(_) => false,
    }
}

fn active_task_is_stale_against_claude_transcript(codex_home: &Path, event: &TaskEvent) -> bool {
    let Some(session_id) = event.session_id.as_deref() else {
        return false;
    };

    let Some(transcript_file) = find_claude_transcript_file(codex_home, session_id) else {
        return timestamp_is_older_than(&event.received_at, ACTIVE_SESSION_FILE_MISSING_GRACE);
    };

    let Ok(status) = claude_transcript_status(&transcript_file) else {
        return false;
    };

    if status
        .last_stop_hook_summary_timestamp
        .as_deref()
        .is_some_and(|timestamp| !timestamp_is_before(timestamp, &event.received_at))
    {
        return true;
    }

    status.last_timestamp.as_deref().is_some_and(|timestamp| {
        timestamp_is_older_than(timestamp, ACTIVE_SESSION_IDLE_STALE_GRACE)
            && timestamp_is_older_than(&event.received_at, ACTIVE_SESSION_IDLE_STALE_GRACE)
    })
}

fn standby_wallpaper_info(
    config: &BridgeConfig,
    peer: Option<SocketAddr>,
) -> Option<StandbyWallpaperInfo> {
    let resolved = resolve_standby_wallpaper(config).ok()?;
    Some(standby_wallpaper_response(config, peer, &resolved))
}

fn standby_wallpaper_response(
    config: &BridgeConfig,
    peer: Option<SocketAddr>,
    wallpaper: &ResolvedStandbyWallpaper,
) -> StandbyWallpaperInfo {
    StandbyWallpaperInfo {
        mode: wallpaper.mode.to_string(),
        id: wallpaper.id.clone(),
        name: wallpaper.name.clone(),
        index: wallpaper.index,
        total: wallpaper.total,
        width: DEFAULT_STANDBY_WALLPAPER_WIDTH,
        height: DEFAULT_STANDBY_WALLPAPER_HEIGHT,
        url: standby_wallpaper_url(config, peer, wallpaper),
        selected_at: now_local(),
    }
}

fn handle_standby_wallpaper(
    stream: &mut TcpStream,
    peer: Option<SocketAddr>,
    request: &HttpRequest,
    config: &BridgeConfig,
) -> io::Result<()> {
    if !music_get_allowed(peer) {
        return write_json(stream, 403, &json!({"ok": false, "error": "forbidden"}));
    }

    let requested_id = request
        .raw_path
        .split_once('?')
        .map(|(_, query)| parse_query_params(query))
        .and_then(|query| query.get("id").cloned())
        .filter(|value| !value.trim().is_empty());

    let wallpaper = match resolve_standby_wallpaper_by_id(config, requested_id.as_deref()) {
        Ok(wallpaper) => wallpaper,
        Err(error) => {
            eprintln!("standby wallpaper resolve failed: {error}");
            return write_json(
                stream,
                map_music_status(&error),
                &json!({"ok": false, "error": error.to_string()}),
            );
        }
    };

    match render_standby_wallpaper_rgb565(&wallpaper.path) {
        Ok(bitmap) => {
            let headers = [
                (
                    "X-Ornament-Wallpaper-Id",
                    sanitize_header_value(&wallpaper.id),
                ),
                (
                    "X-Ornament-Wallpaper-Name",
                    sanitize_header_value(&wallpaper.name),
                ),
                ("X-Ornament-Wallpaper-Mode", wallpaper.mode.to_string()),
                ("X-Ornament-Wallpaper-Index", wallpaper.index.to_string()),
                ("X-Ornament-Wallpaper-Total", wallpaper.total.to_string()),
                (
                    "X-Ornament-Wallpaper-Width",
                    DEFAULT_STANDBY_WALLPAPER_WIDTH.to_string(),
                ),
                (
                    "X-Ornament-Wallpaper-Height",
                    DEFAULT_STANDBY_WALLPAPER_HEIGHT.to_string(),
                ),
            ];
            write_response_with_headers(stream, 200, "application/octet-stream", &headers, &bitmap)
        }
        Err(error) => {
            eprintln!("standby wallpaper render failed: {error}");
            write_json(
                stream,
                map_music_status(&error),
                &json!({"ok": false, "error": error.to_string()}),
            )
        }
    }
}

fn resolve_standby_wallpaper(config: &BridgeConfig) -> io::Result<ResolvedStandbyWallpaper> {
    let entries = standby_wallpaper_entries(&config.standby_wallpaper_dir)?;
    if entries.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "no jpg wallpapers found in {}",
                config.standby_wallpaper_dir.display()
            ),
        ));
    }

    let total = entries.len();
    if let Some(fixed) = config
        .standby_wallpaper_fixed
        .as_deref()
        .filter(|value| !value.trim().is_empty())
    {
        if let Some((index, path)) = entries
            .iter()
            .enumerate()
            .find(|(_, path)| standby_wallpaper_matches(path, fixed))
        {
            return Ok(ResolvedStandbyWallpaper {
                mode: "fixed",
                id: standby_wallpaper_id(path),
                name: standby_wallpaper_name(path),
                path: path.clone(),
                index,
                total,
            });
        }
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "fixed wallpaper '{}' not found in {}",
                fixed,
                config.standby_wallpaper_dir.display()
            ),
        ));
    }

    let today = Local::now().date_naive();
    let epoch = NaiveDate::from_ymd_opt(1970, 1, 1)
        .ok_or_else(|| io::Error::other("failed to construct wallpaper epoch date"))?;
    let day_index = today
        .signed_duration_since(epoch)
        .num_days()
        .rem_euclid(total as i64) as usize;
    let path = entries
        .get(day_index)
        .cloned()
        .ok_or_else(|| io::Error::other("wallpaper day index out of range"))?;

    Ok(ResolvedStandbyWallpaper {
        mode: "daily",
        id: standby_wallpaper_id(&path),
        name: standby_wallpaper_name(&path),
        path,
        index: day_index,
        total,
    })
}

fn resolve_standby_wallpaper_by_id(
    config: &BridgeConfig,
    requested_id: Option<&str>,
) -> io::Result<ResolvedStandbyWallpaper> {
    if requested_id.is_none() {
        return resolve_standby_wallpaper(config);
    }

    let entries = standby_wallpaper_entries(&config.standby_wallpaper_dir)?;
    if entries.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "no jpg wallpapers found in {}",
                config.standby_wallpaper_dir.display()
            ),
        ));
    }

    let total = entries.len();
    let requested_id = requested_id.unwrap_or_default();
    let (index, path) = entries
        .iter()
        .enumerate()
        .find(|(_, path)| standby_wallpaper_id(path).eq_ignore_ascii_case(requested_id))
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                format!(
                    "wallpaper id '{}' not found in {}",
                    requested_id,
                    config.standby_wallpaper_dir.display()
                ),
            )
        })?;

    Ok(ResolvedStandbyWallpaper {
        mode: "requested",
        id: standby_wallpaper_id(path),
        name: standby_wallpaper_name(path),
        path: path.clone(),
        index,
        total,
    })
}

fn standby_wallpaper_entries(dir: &Path) -> io::Result<Vec<PathBuf>> {
    let mut entries = fs::read_dir(dir)?
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| {
            path.is_file()
                && path
                    .extension()
                    .and_then(|ext| ext.to_str())
                    .map(|ext| matches!(ext.to_ascii_lowercase().as_str(), "jpg" | "jpeg"))
                    .unwrap_or(false)
        })
        .collect::<Vec<_>>();

    entries.sort_by_cached_key(|path| {
        path.file_name()
            .map(|name| name.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default()
    });
    Ok(entries)
}

fn standby_wallpaper_matches(path: &Path, fixed: &str) -> bool {
    let fixed = fixed.trim();
    !fixed.is_empty()
        && (path
            .file_name()
            .map(|name| name.to_string_lossy().eq_ignore_ascii_case(fixed))
            .unwrap_or(false)
            || standby_wallpaper_id(path).eq_ignore_ascii_case(fixed))
}

fn standby_wallpaper_name(path: &Path) -> String {
    path.file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_else(|| path.display().to_string())
}

fn standby_wallpaper_id(path: &Path) -> String {
    let stem = path
        .file_stem()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_else(|| standby_wallpaper_name(path));
    sanitize_wallpaper_token(&stem)
}

fn sanitize_wallpaper_token(value: &str) -> String {
    let mut token = String::with_capacity(value.len());
    for ch in value.chars() {
        if ch.is_ascii_alphanumeric() {
            token.push(ch.to_ascii_lowercase());
        } else if matches!(ch, '-' | '_' | '.') {
            token.push(ch);
        }
    }
    if token.is_empty() {
        "wallpaper".to_string()
    } else {
        token
    }
}

fn standby_wallpaper_url(
    config: &BridgeConfig,
    peer: Option<SocketAddr>,
    wallpaper: &ResolvedStandbyWallpaper,
) -> String {
    let base = music_public_base_url(config, peer);
    format!(
        "{}/v1/standby-wallpaper?id={}",
        base.trim_end_matches('/'),
        form_urlencode(&wallpaper.id)
    )
}

fn find_claude_transcript_file(codex_home: &Path, session_id: &str) -> Option<PathBuf> {
    let home_dir = codex_home.parent()?;
    let claude_projects = home_dir.join(".claude").join("projects");
    find_file_name_containing(&claude_projects, session_id)
}

fn turn_is_terminal_in_session_file(path: &Path, turn_id: &str) -> io::Result<bool> {
    Ok(terminal_turn_in_file(path, turn_id)?.is_some())
}

#[derive(Default)]
struct ClaudeTranscriptStatus {
    last_stop_hook_summary_timestamp: Option<String>,
    last_timestamp: Option<String>,
}

fn claude_transcript_status(path: &Path) -> io::Result<ClaudeTranscriptStatus> {
    let file = open_shared_tail_read(path, SESSION_TASK_SCAN_TAIL_BYTES)?;
    let reader = BufReader::new(file);
    let mut status = ClaudeTranscriptStatus::default();

    for line in reader.lines() {
        let line = line?;
        let Ok(record) = serde_json::from_str::<Value>(&line) else {
            continue;
        };

        status.last_timestamp = record
            .get("timestamp")
            .and_then(Value::as_str)
            .map(str::to_string)
            .or(status.last_timestamp);

        if record.get("type").and_then(Value::as_str) == Some("system")
            && record.get("subtype").and_then(Value::as_str) == Some("stop_hook_summary")
        {
            status.last_stop_hook_summary_timestamp = record
                .get("timestamp")
                .and_then(Value::as_str)
                .map(str::to_string)
                .or(status.last_stop_hook_summary_timestamp);
        }
    }

    Ok(status)
}

fn session_file_last_timestamp(path: &Path) -> io::Result<Option<String>> {
    let file = open_shared_tail_read(path, SESSION_TASK_SCAN_TAIL_BYTES)?;
    let reader = BufReader::new(file);
    let mut last_timestamp = None;

    for line in reader.lines() {
        let line = line?;
        let Ok(record) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        if let Some(timestamp) = record.get("timestamp").and_then(Value::as_str) {
            last_timestamp = Some(timestamp.to_string());
        }
    }

    Ok(last_timestamp)
}

fn find_session_file(codex_home: &Path, session_id: &str) -> Option<PathBuf> {
    for root in [
        codex_home.join("sessions"),
        codex_home.join("archived_sessions"),
    ] {
        if let Some(path) = find_file_name_containing(&root, session_id) {
            return Some(path);
        }
    }
    None
}

fn active_tasks_in_recent_session_files(codex_home: &Path) -> Vec<TaskEvent> {
    let mut recovered = Vec::new();
    for path in recent_session_files(&codex_home.join("sessions")) {
        let Some(session_id) = session_id_from_file_name(&path) else {
            continue;
        };
        let session_is_recent = session_file_is_recent(&path, RECOVER_UNSCOPED_ACTIVE_TASK_WINDOW);
        let Ok(active_tasks) = active_tasks_in_session_file(&path, &session_id) else {
            continue;
        };
        for active in active_tasks {
            let has_newer_turn = active_task_has_newer_turn_in_session_file(&path, &active)
                .ok()
                .unwrap_or(false);
            let task_is_recent =
                timestamp_is_recent(&active.received_at, RECOVER_UNSCOPED_ACTIVE_TASK_WINDOW);
            if has_newer_turn
                || (!task_is_recent && !session_is_recent)
                || active_task_is_stale_against_session_log(codex_home, &active)
            {
                continue;
            }
            recovered.push(active);
        }
    }
    recovered
}

fn session_file_is_recent(path: &Path, window: Duration) -> bool {
    session_file_last_timestamp(path)
        .ok()
        .flatten()
        .is_some_and(|timestamp| timestamp_is_recent(&timestamp, window))
        || file_modified_at(path)
            .is_some_and(|modified_at| system_time_is_recent(modified_at, window))
}

fn session_ancestor_ids(codex_home: &Path, session_id: &str) -> Vec<String> {
    let mut ancestors = Vec::new();
    let mut current = session_id.to_string();

    for _ in 0..SESSION_FORK_CHAIN_LIMIT {
        let Some(parent) = session_fork_parent_id(codex_home, &current) else {
            break;
        };
        if parent.is_empty() || ancestors.iter().any(|ancestor| ancestor == &parent) {
            break;
        }

        current = parent.clone();
        ancestors.push(parent);
    }

    ancestors
}

fn session_fork_happened_after_event(
    codex_home: &Path,
    descendant_session_id: &str,
    event: &TaskEvent,
) -> bool {
    let Some(forked_at) = session_timestamp(codex_home, descendant_session_id) else {
        return false;
    };
    timestamp_is_before(&event.received_at, &forked_at)
}

fn session_fork_parent_id(codex_home: &Path, session_id: &str) -> Option<String> {
    let session_file = find_session_file(codex_home, session_id)?;
    session_fork_parent_id_in_file(&session_file)
}

fn session_timestamp(codex_home: &Path, session_id: &str) -> Option<String> {
    let session_file = find_session_file(codex_home, session_id)?;
    session_metadata_in_file(&session_file).and_then(|metadata| metadata.timestamp)
}

fn session_fork_parent_id_in_file(path: &Path) -> Option<String> {
    session_metadata_in_file(path).and_then(|metadata| metadata.forked_from_id)
}

struct SessionMetadata {
    forked_from_id: Option<String>,
    timestamp: Option<String>,
}

fn session_metadata_in_file(path: &Path) -> Option<SessionMetadata> {
    let file = open_shared_read(path).ok()?;
    let reader = BufReader::new(file);

    for line in reader.lines() {
        let line = line.ok()?;
        if !line.contains("\"session_meta\"") {
            continue;
        }
        let record = serde_json::from_str::<Value>(&line).ok()?;
        if record.get("type").and_then(Value::as_str) != Some("session_meta") {
            continue;
        }
        let payload = &record["payload"];
        return Some(SessionMetadata {
            forked_from_id: payload
                .get("forked_from_id")
                .and_then(Value::as_str)
                .map(str::to_string)
                .filter(|value| !value.is_empty()),
            timestamp: payload
                .get("timestamp")
                .and_then(Value::as_str)
                .map(str::to_string)
                .or_else(|| {
                    record
                        .get("timestamp")
                        .and_then(Value::as_str)
                        .map(str::to_string)
                }),
        });
    }

    None
}

fn line_may_contain_active_task_record(line: &str) -> bool {
    line.contains("\"task_started\"")
        || line.contains("\"turn_context\"")
        || line.contains("\"task_complete\"")
        || line.contains("\"turn_aborted\"")
        || line.contains("\"final_answer\"")
}

fn line_may_contain_terminal_task_record(line: &str) -> bool {
    line.contains("\"task_started\"")
        || line.contains("\"turn_context\"")
        || line.contains("\"task_complete\"")
        || line.contains("\"turn_aborted\"")
        || line.contains("\"final_answer\"")
}

fn recent_session_files(root: &Path) -> Vec<PathBuf> {
    let mut files = Vec::new();
    collect_session_files(root, &mut files);
    files.sort_by(|left, right| {
        file_modified_at(right)
            .cmp(&file_modified_at(left))
            .then_with(|| right.cmp(left))
    });
    files.truncate(RECOVER_ACTIVE_SESSION_SCAN_LIMIT);
    files
}

fn collect_session_files(root: &Path, files: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(root) else {
        return;
    };

    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            collect_session_files(&path, files);
        } else if path.extension().and_then(|extension| extension.to_str()) == Some("jsonl") {
            files.push(path);
        }
    }
}

fn file_modified_at(path: &Path) -> Option<std::time::SystemTime> {
    path.metadata()
        .and_then(|metadata| metadata.modified())
        .ok()
}

fn session_id_from_file_name(path: &Path) -> Option<String> {
    let file_name = path.file_name()?.to_str()?;
    let stem = file_name.strip_suffix(".jsonl")?;
    let session_id = stem.get(stem.len().checked_sub(36)?..)?;
    let looks_like_uuid = session_id.len() == 36
        && session_id.chars().filter(|ch| *ch == '-').count() == 4
        && session_id
            .chars()
            .all(|ch| ch.is_ascii_hexdigit() || ch == '-');
    looks_like_uuid.then(|| session_id.to_string())
}

fn find_file_name_containing(root: &Path, needle: &str) -> Option<PathBuf> {
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };

        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
                continue;
            }

            let Some(file_name) = path.file_name().and_then(|name| name.to_str()) else {
                continue;
            };
            if file_name.contains(needle) && file_name.ends_with(".jsonl") {
                return Some(path);
            }
        }
    }
    None
}

fn open_shared_read(path: &Path) -> io::Result<File> {
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        const FILE_SHARE_READ: u32 = 0x00000001;
        const FILE_SHARE_WRITE: u32 = 0x00000002;
        const FILE_SHARE_DELETE: u32 = 0x00000004;

        fs::OpenOptions::new()
            .read(true)
            .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
            .open(path)
    }

    #[cfg(not(windows))]
    {
        File::open(path)
    }
}

fn open_shared_tail_read(path: &Path, max_bytes: u64) -> io::Result<File> {
    let mut file = open_shared_read(path)?;
    let length = file.metadata()?.len();
    if length > max_bytes {
        file.seek(SeekFrom::Start(length - max_bytes))?;
    }
    Ok(file)
}

fn active_tasks_in_session_file(path: &Path, session_id: &str) -> io::Result<Vec<TaskEvent>> {
    let file = open_shared_tail_read(path, SESSION_TASK_SCAN_TAIL_BYTES)?;
    let reader = BufReader::new(file);
    let mut active: HashMap<String, TaskEvent> = HashMap::new();
    let mut active_order = VecDeque::new();
    let mut pending_final_answer = None;

    for line in reader.lines() {
        let line = line?;
        if !line_may_contain_active_task_record(&line) {
            continue;
        }
        let Ok(record) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        let payload = &record["payload"];
        if record_is_final_answer(&record, payload) {
            pending_final_answer =
                remember_latest_pending_final_answer(&active, &active_order, &record, payload);
            continue;
        }

        let Some(kind) = session_record_kind(&record, payload) else {
            continue;
        };

        match kind {
            "task_started" => {
                let Some(turn_id) = payload.get("turn_id").and_then(Value::as_str) else {
                    continue;
                };
                if should_finalize_pending_final_answer(
                    pending_final_answer.as_ref(),
                    Some(turn_id),
                    kind,
                ) {
                    apply_pending_final_answer(
                        &mut active,
                        &mut active_order,
                        pending_final_answer.take(),
                    );
                }
                if !active.contains_key(turn_id) {
                    active_order.push_back(turn_id.to_string());
                }
                active.insert(
                    turn_id.to_string(),
                    TaskEvent {
                        kind: "UserPromptSubmit".to_string(),
                        status: "running".to_string(),
                        title: "Codex running".to_string(),
                        message: "Codex running".to_string(),
                        received_at: record
                            .get("timestamp")
                            .and_then(Value::as_str)
                            .map(str::to_string)
                            .unwrap_or_else(now_local),
                        source: None,
                        session_id: Some(session_id.to_string()),
                        turn_id: Some(turn_id.to_string()),
                        cwd: None,
                        model: None,
                    },
                );
            }
            "turn_context" => {
                let Some(turn_id) = payload.get("turn_id").and_then(Value::as_str) else {
                    continue;
                };
                if pending_final_answer_turn_matches(pending_final_answer.as_ref(), turn_id) {
                    pending_final_answer = None;
                }
                if let Some(event) = active.get_mut(turn_id) {
                    event.cwd = text_field(payload, &["cwd"]).map(|value| clip(&value, 120));
                    event.model = text_field(payload, &["model"]);
                }
            }
            "task_complete" | "turn_aborted" => {
                if let Some(turn_id) = payload.get("turn_id").and_then(Value::as_str) {
                    if should_finalize_pending_final_answer(
                        pending_final_answer.as_ref(),
                        Some(turn_id),
                        kind,
                    ) {
                        apply_pending_final_answer(
                            &mut active,
                            &mut active_order,
                            pending_final_answer.take(),
                        );
                    }
                    active.remove(turn_id);
                    active_order.retain(|candidate| candidate != turn_id);
                }
            }
            _ => {}
        }
    }

    apply_pending_final_answer(&mut active, &mut active_order, pending_final_answer);

    Ok(active_order
        .into_iter()
        .filter_map(|turn_id| active.remove(&turn_id))
        .collect())
}

fn terminal_turn_in_file(path: &Path, turn_id: &str) -> io::Result<Option<TerminalTurn>> {
    let file = open_shared_tail_read(path, SESSION_TASK_SCAN_TAIL_BYTES)?;
    let reader = BufReader::new(file);
    let mut terminal = None;
    let mut active_order = VecDeque::new();
    let mut pending_final_answer = None;

    for line in reader.lines() {
        let line = line?;
        if !line_may_contain_terminal_task_record(&line) {
            continue;
        }
        let Ok(record) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        let payload = &record["payload"];
        if record_is_final_answer(&record, payload) {
            pending_final_answer =
                remember_latest_pending_final_answer_id(&active_order, &record, payload);
            continue;
        }

        let Some(kind) = session_record_kind(&record, payload) else {
            continue;
        };

        if kind == "task_started" {
            if let Some(started_turn_id) = payload.get("turn_id").and_then(Value::as_str) {
                if should_finalize_pending_final_answer(
                    pending_final_answer.as_ref(),
                    Some(started_turn_id),
                    kind,
                ) {
                    remember_terminal_from_pending_final_answer(
                        &mut terminal,
                        pending_final_answer.take(),
                        turn_id,
                    );
                    remove_latest_active_turn_id(&mut active_order);
                }
                remember_active_turn_id(&mut active_order, started_turn_id);
            }
            continue;
        }

        if kind == "turn_context" {
            if let Some(context_turn_id) = payload.get("turn_id").and_then(Value::as_str) {
                if pending_final_answer_turn_matches(pending_final_answer.as_ref(), context_turn_id)
                {
                    pending_final_answer = None;
                }
            }
            continue;
        }

        if kind != "task_complete" && kind != "turn_aborted" {
            continue;
        }
        let Some(terminal_turn_id) = payload.get("turn_id").and_then(Value::as_str) else {
            continue;
        };
        if should_finalize_pending_final_answer(
            pending_final_answer.as_ref(),
            Some(terminal_turn_id),
            kind,
        ) {
            remember_terminal_from_pending_final_answer(
                &mut terminal,
                pending_final_answer.take(),
                turn_id,
            );
            remove_latest_active_turn_id(&mut active_order);
        }
        forget_active_turn_id(&mut active_order, terminal_turn_id);
        if terminal_turn_id != turn_id {
            continue;
        }

        let previous_message = terminal.as_ref().and_then(|terminal: &TerminalTurn| {
            terminal
                .message
                .as_deref()
                .filter(|message| !message.is_empty())
                .map(str::to_string)
        });

        terminal = Some(TerminalTurn {
            kind: kind.to_string(),
            timestamp: record
                .get("timestamp")
                .and_then(Value::as_str)
                .map(str::to_string),
            message: terminal_message(payload).or(previous_message),
        });
    }

    remember_terminal_from_pending_final_answer(&mut terminal, pending_final_answer, turn_id);

    Ok(terminal)
}

fn active_task_has_newer_turn_in_session_file(path: &Path, event: &TaskEvent) -> io::Result<bool> {
    let Some(turn_id) = event.turn_id.as_deref() else {
        return Ok(false);
    };

    let file = open_shared_tail_read(path, SESSION_TASK_SCAN_TAIL_BYTES)?;
    let reader = BufReader::new(file);
    let mut saw_event_turn = false;

    for line in reader.lines() {
        let line = line?;
        if !line.contains("\"task_started\"") {
            continue;
        }
        let Ok(record) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        let payload = &record["payload"];
        if session_record_kind(&record, payload) != Some("task_started") {
            continue;
        }
        let Some(started_turn_id) = payload.get("turn_id").and_then(Value::as_str) else {
            continue;
        };
        if saw_event_turn && started_turn_id != turn_id {
            return Ok(true);
        }
        if started_turn_id == turn_id {
            saw_event_turn = true;
        }
    }

    Ok(false)
}

fn record_is_final_answer(record: &Value, payload: &Value) -> bool {
    record.get("type").and_then(Value::as_str) == Some("response_item")
        && payload.get("phase").and_then(Value::as_str) == Some("final_answer")
}

fn session_record_kind<'a>(record: &'a Value, payload: &'a Value) -> Option<&'a str> {
    payload
        .get("type")
        .and_then(Value::as_str)
        .or_else(|| record.get("type").and_then(Value::as_str))
}

fn terminal_message(payload: &Value) -> Option<String> {
    text_field(payload, &["last_agent_message", "message", "reason"])
        .map(|value| clip(value.trim(), 160))
        .filter(|value| !value.is_empty())
}

fn final_answer_message(payload: &Value) -> Option<String> {
    payload
        .get("content")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(|item| item.get("text").and_then(Value::as_str))
        .find_map(|text| {
            let text = text.trim();
            (!text.is_empty()).then(|| clip(text, 160))
        })
}

fn remember_latest_pending_final_answer(
    active: &HashMap<String, TaskEvent>,
    active_order: &VecDeque<String>,
    record: &Value,
    payload: &Value,
) -> Option<PendingFinalAnswer> {
    let turn_id = active_order
        .iter()
        .rev()
        .find(|candidate| active.contains_key(candidate.as_str()))?
        .clone();
    Some(PendingFinalAnswer {
        turn_id,
        timestamp: record
            .get("timestamp")
            .and_then(Value::as_str)
            .map(str::to_string),
        message: final_answer_message(payload),
    })
}

fn remember_latest_pending_final_answer_id(
    active_order: &VecDeque<String>,
    record: &Value,
    payload: &Value,
) -> Option<PendingFinalAnswer> {
    Some(PendingFinalAnswer {
        turn_id: active_order.back()?.clone(),
        timestamp: record
            .get("timestamp")
            .and_then(Value::as_str)
            .map(str::to_string),
        message: final_answer_message(payload),
    })
}

fn pending_final_answer_turn_matches(
    pending_final_answer: Option<&PendingFinalAnswer>,
    turn_id: &str,
) -> bool {
    pending_final_answer
        .map(|pending| pending.turn_id == turn_id)
        .unwrap_or(false)
}

fn should_finalize_pending_final_answer(
    pending_final_answer: Option<&PendingFinalAnswer>,
    turn_id: Option<&str>,
    kind: &str,
) -> bool {
    let Some(pending) = pending_final_answer else {
        return false;
    };
    match kind {
        "task_started" => turn_id
            .map(|turn_id| turn_id != pending.turn_id)
            .unwrap_or(true),
        "task_complete" | "turn_aborted" => turn_id
            .map(|turn_id| turn_id == pending.turn_id)
            .unwrap_or(false),
        _ => false,
    }
}

fn apply_pending_final_answer(
    active: &mut HashMap<String, TaskEvent>,
    active_order: &mut VecDeque<String>,
    pending_final_answer: Option<PendingFinalAnswer>,
) {
    let Some(pending) = pending_final_answer else {
        return;
    };
    active.remove(&pending.turn_id);
    active_order.retain(|candidate| candidate != &pending.turn_id);
}

fn remember_terminal_from_pending_final_answer(
    terminal: &mut Option<TerminalTurn>,
    pending_final_answer: Option<PendingFinalAnswer>,
    turn_id: &str,
) {
    let Some(pending) = pending_final_answer else {
        return;
    };
    if pending.turn_id != turn_id {
        return;
    }
    *terminal = Some(TerminalTurn {
        kind: "task_complete".to_string(),
        timestamp: pending.timestamp,
        message: pending.message,
    });
}

fn remember_active_turn_id(active_order: &mut VecDeque<String>, turn_id: &str) {
    if !active_order.iter().any(|candidate| candidate == turn_id) {
        active_order.push_back(turn_id.to_string());
    }
}

fn forget_active_turn_id(active_order: &mut VecDeque<String>, turn_id: &str) {
    active_order.retain(|candidate| candidate != turn_id);
}

fn remove_latest_active_turn_id(active_order: &mut VecDeque<String>) {
    active_order.pop_back();
}

fn timestamp_is_recent(timestamp: &str, window: Duration) -> bool {
    let Ok(timestamp) = DateTime::parse_from_rfc3339(timestamp) else {
        return false;
    };
    let age = Local::now().signed_duration_since(timestamp.with_timezone(&Local));
    age.to_std().map(|age| age <= window).unwrap_or(false)
}

fn system_time_is_recent(timestamp: std::time::SystemTime, window: Duration) -> bool {
    timestamp
        .elapsed()
        .map(|age| age <= window)
        .unwrap_or(false)
}

fn timestamp_is_older_than(timestamp: &str, window: Duration) -> bool {
    let Ok(timestamp) = DateTime::parse_from_rfc3339(timestamp) else {
        return false;
    };
    let age = Local::now().signed_duration_since(timestamp.with_timezone(&Local));
    age.to_std().map(|age| age > window).unwrap_or(false)
}

fn timestamp_is_before(left: &str, right: &str) -> bool {
    let Ok(left) = DateTime::parse_from_rfc3339(left) else {
        return false;
    };
    let Ok(right) = DateTime::parse_from_rfc3339(right) else {
        return false;
    };
    left < right
}

fn cached_or_refresh_quota(state: &Arc<Mutex<BridgeState>>) -> QuotaSnapshot {
    if let Ok(state) = state.lock() {
        if let Some(cache) = state.quota.as_ref() {
            if cache.fetched_at.elapsed() < QUOTA_CACHE_TTL {
                return cache.snapshot.clone();
            }
        }
    }

    store_quota_snapshot(state, get_quota_snapshot())
}

fn cached_or_refresh_quota_background(state: &Arc<Mutex<BridgeState>>) -> QuotaSnapshot {
    if let Ok(state) = state.lock() {
        if let Some(cache) = state.quota.as_ref() {
            if cache.fetched_at.elapsed() < QUOTA_CACHE_TTL {
                return cache.snapshot.clone();
            }
        }
    }

    maybe_spawn_quota_refresh(Arc::clone(state));

    if let Ok(state) = state.lock() {
        if let Some(cache) = state.quota.as_ref() {
            return cache.snapshot.clone();
        }
    }
    quota_unavailable()
}

fn maybe_spawn_quota_refresh(state: SharedBridgeState) {
    let should_spawn = {
        let Ok(mut state) = state.lock() else {
            return;
        };
        if state.quota_refreshing {
            false
        } else {
            state.quota_refreshing = true;
            true
        }
    };
    if !should_spawn {
        return;
    }

    std::thread::spawn(move || {
        store_quota_snapshot(&state, get_quota_snapshot());
    });
}

fn maybe_spawn_quota_refresh_if_due(state: SharedBridgeState) {
    let refresh_due = {
        let Ok(state) = state.lock() else {
            return;
        };
        quota_refresh_is_due(&state)
    };
    if refresh_due {
        maybe_spawn_quota_refresh(state);
    }
}

fn quota_refresh_is_due(state: &BridgeState) -> bool {
    quota_refresh_is_due_at(state, Instant::now())
}

fn quota_refresh_is_due_at(state: &BridgeState, now: Instant) -> bool {
    if state.quota_refreshing {
        return false;
    }
    state
        .quota
        .as_ref()
        .map(|cache| now.saturating_duration_since(cache.fetched_at) >= QUOTA_CACHE_TTL)
        .unwrap_or(true)
}

fn quota_unavailable() -> QuotaSnapshot {
    QuotaSnapshot {
        status: SnapshotStatus::NoData,
        source: "codex-wham".to_string(),
        source_label: Some("ChatGPT usage API".to_string()),
        web_url: Some(quota_core::USAGE_URL.to_string()),
        limit_id: Some("codex".to_string()),
        plan_type: None,
        primary_used_percent: None,
        primary_remaining_percent: None,
        primary_window_minutes: None,
        primary_resets_at: None,
        secondary_used_percent: None,
        secondary_remaining_percent: None,
        secondary_window_minutes: None,
        secondary_resets_at: None,
        credits: None,
        observed_at: Some(now_local()),
        captured_at: None,
        error: Some("quota refresh pending".to_string()),
    }
}

fn store_quota_snapshot(
    state: &Arc<Mutex<BridgeState>>,
    fetched_snapshot: QuotaSnapshot,
) -> QuotaSnapshot {
    let previous_snapshot = cached_quota_snapshot(state).or_else(load_persisted_quota_snapshot);
    let snapshot = merge_quota_snapshot(previous_snapshot.as_ref(), fetched_snapshot);
    let _ = write_state(state_path(), &snapshot);
    if let Ok(mut state) = state.lock() {
        state.quota = Some(CachedQuota {
            snapshot: snapshot.clone(),
            fetched_at: Instant::now(),
        });
        state.quota_refreshing = false;
    }
    snapshot
}

fn cached_quota_snapshot(state: &Arc<Mutex<BridgeState>>) -> Option<QuotaSnapshot> {
    state
        .lock()
        .ok()
        .and_then(|state| state.quota.as_ref().map(|cache| cache.snapshot.clone()))
}

fn load_persisted_quota_snapshot() -> Option<QuotaSnapshot> {
    read_state(state_path())
        .ok()
        .flatten()
        .filter(quota_snapshot_has_display_data)
}

fn merge_quota_snapshot(
    previous: Option<&QuotaSnapshot>,
    mut fetched: QuotaSnapshot,
) -> QuotaSnapshot {
    let Some(previous) = previous.filter(|snapshot| quota_snapshot_has_display_data(snapshot))
    else {
        return fetched;
    };

    if !quota_snapshot_has_actual_data(&fetched) {
        return previous.clone();
    }

    fetched.source_label = fetched
        .source_label
        .or_else(|| previous.source_label.clone());
    fetched.web_url = fetched.web_url.or_else(|| previous.web_url.clone());
    fetched.limit_id = fetched.limit_id.or_else(|| previous.limit_id.clone());
    fetched.plan_type = fetched.plan_type.or_else(|| previous.plan_type.clone());
    if primary_quota_window_present(&fetched) {
        fetched.primary_used_percent = fetched
            .primary_used_percent
            .or(previous.primary_used_percent);
        fetched.primary_remaining_percent = fetched
            .primary_remaining_percent
            .or(previous.primary_remaining_percent);
        fetched.primary_window_minutes = fetched
            .primary_window_minutes
            .or(previous.primary_window_minutes);
        fetched.primary_resets_at = merged_reset_time(
            previous.primary_resets_at.as_deref(),
            fetched.primary_resets_at,
        );
    }
    if secondary_quota_window_present(&fetched) {
        fetched.secondary_used_percent = fetched
            .secondary_used_percent
            .or(previous.secondary_used_percent);
        fetched.secondary_remaining_percent = fetched
            .secondary_remaining_percent
            .or(previous.secondary_remaining_percent);
        fetched.secondary_window_minutes = fetched
            .secondary_window_minutes
            .or(previous.secondary_window_minutes);
        fetched.secondary_resets_at = merged_reset_time(
            previous.secondary_resets_at.as_deref(),
            fetched.secondary_resets_at,
        );
    }
    fetched.credits = fetched.credits.or_else(|| previous.credits.clone());
    fetched
}

fn merged_reset_time(previous: Option<&str>, fetched: Option<String>) -> Option<String> {
    if let Some(previous) = previous {
        if !timestamp_has_passed(previous) {
            return Some(previous.to_string());
        }
    }
    fetched.or_else(|| previous.map(str::to_string))
}

fn timestamp_has_passed(timestamp: &str) -> bool {
    let Ok(timestamp) = DateTime::parse_from_rfc3339(timestamp) else {
        return true;
    };
    timestamp.with_timezone(&Local) <= Local::now()
}

fn quota_snapshot_has_actual_data(snapshot: &QuotaSnapshot) -> bool {
    snapshot.status == SnapshotStatus::Ok && quota_snapshot_has_display_data(snapshot)
}

fn primary_quota_window_present(snapshot: &QuotaSnapshot) -> bool {
    snapshot.primary_remaining_percent.is_some()
        || snapshot.primary_used_percent.is_some()
        || snapshot.primary_window_minutes.is_some()
        || snapshot.primary_resets_at.is_some()
}

fn secondary_quota_window_present(snapshot: &QuotaSnapshot) -> bool {
    snapshot.secondary_remaining_percent.is_some()
        || snapshot.secondary_used_percent.is_some()
        || snapshot.secondary_window_minutes.is_some()
        || snapshot.secondary_resets_at.is_some()
}

fn quota_snapshot_has_display_data(snapshot: &QuotaSnapshot) -> bool {
    primary_quota_window_present(snapshot) || secondary_quota_window_present(snapshot)
}

fn cached_or_refresh_weather(
    state: &Arc<Mutex<BridgeState>>,
    config: &BridgeConfig,
) -> WeatherSnapshot {
    let now = Instant::now();
    let cache_ttl = match state.lock() {
        Ok(state) => weather_refresh_interval_at(config, Some(&state), now),
        Err(_) => weather_refresh_interval_at(config, None, now),
    };
    if let Ok(state) = state.lock() {
        if let Some(cache) = state.weather.as_ref() {
            if cache.fetched_at.elapsed() < cache_ttl {
                return cache.snapshot.clone();
            }
        }
    }

    maybe_spawn_weather_refresh(Arc::clone(state), config.clone());

    if let Ok(state) = state.lock() {
        if let Some(cache) = state.weather.as_ref() {
            return cache.snapshot.clone();
        }
    }
    weather_unavailable(config)
}

fn maybe_spawn_weather_refresh(state: SharedBridgeState, config: BridgeConfig) {
    let should_spawn = {
        let Ok(mut state) = state.lock() else {
            return;
        };
        if !weather_refresh_is_due(&state, &config) {
            false
        } else {
            state.weather_refreshing = true;
            state.weather_last_attempt = Some(Instant::now());
            true
        }
    };
    if !should_spawn {
        return;
    }

    std::thread::spawn(move || {
        let fetched = fetch_weather_snapshot(&state, &config);
        let Ok(mut state) = state.lock() else {
            return;
        };
        match fetched {
            Ok(snapshot) => {
                state.weather = Some(CachedWeather {
                    snapshot,
                    fetched_at: Instant::now(),
                });
            }
            Err(error) => {
                eprintln!("weather fetch failed: {error}");
            }
        }
        state.weather_refreshing = false;
    });
}

fn weather_refresh_is_due(state: &BridgeState, config: &BridgeConfig) -> bool {
    weather_refresh_is_due_at(state, config, Instant::now())
}

fn weather_refresh_is_due_at(state: &BridgeState, config: &BridgeConfig, now: Instant) -> bool {
    if state.weather_refreshing {
        return false;
    }

    let interval = weather_refresh_interval_at(config, Some(state), now);
    if let Some(cache) = state.weather.as_ref() {
        if now.saturating_duration_since(cache.fetched_at) < interval {
            return false;
        }
    }
    if let Some(last_attempt) = state.weather_last_attempt {
        if now.saturating_duration_since(last_attempt) < interval {
            return false;
        }
    }
    true
}

fn weather_refresh_interval_at(
    config: &BridgeConfig,
    state: Option<&BridgeState>,
    now: Instant,
) -> Duration {
    if caiyun_backoff_active(state, now) {
        return DEFAULT_WEATHER_CACHE_TTL;
    }
    if caiyun_budget_applies(config) {
        return caiyun_refresh_interval_for_hour(Local::now().hour());
    }
    DEFAULT_WEATHER_CACHE_TTL
}

fn caiyun_backoff_active(state: Option<&BridgeState>, now: Instant) -> bool {
    state
        .and_then(|state| state.weather_caiyun_backoff_until)
        .is_some_and(|until| now < until)
}

fn caiyun_budget_applies(config: &BridgeConfig) -> bool {
    config.caiyun_token.is_some()
        && matches!(
            config.weather_provider,
            WeatherProvider::Auto | WeatherProvider::Caiyun
        )
}

fn caiyun_refresh_interval_for_hour(hour: u32) -> Duration {
    if hour < CAIYUN_NIGHT_END_HOUR {
        return CAIYUN_NIGHT_REFRESH_INTERVAL;
    }

    Duration::from_nanos(ceil_div_u128(
        u128::from(CAIYUN_DAY_SECONDS) * 1_000_000_000,
        u128::from(CAIYUN_DAY_CALL_BUDGET),
    ) as u64)
}

fn ceil_div_u128(numerator: u128, denominator: u128) -> u128 {
    numerator.div_ceil(denominator)
}

fn fetch_weather_snapshot(
    state: &SharedBridgeState,
    config: &BridgeConfig,
) -> io::Result<WeatherSnapshot> {
    let client = reqwest::blocking::Client::builder()
        .timeout(WEATHER_FETCH_TIMEOUT)
        .build()
        .map_err(io_other)?;
    let mut last_error = None;

    let now = Instant::now();
    let skip_caiyun = match state.lock() {
        Ok(state) => caiyun_backoff_active(Some(&state), now),
        Err(_) => false,
    };

    for provider in weather_fetch_order(config, skip_caiyun) {
        let fetched = match provider {
            WeatherProvider::Auto => unreachable!("auto is expanded by weather_fetch_order"),
            WeatherProvider::OpenMeteo => fetch_open_meteo_weather(&client, config),
            WeatherProvider::QWeather => fetch_qweather_weather(&client, config),
            WeatherProvider::Caiyun => fetch_caiyun_weather(&client, config),
        };
        match fetched {
            Ok(snapshot) => return Ok(snapshot),
            Err(error) => {
                log_weather_provider_failure(state, provider, &error);
                last_error = Some(error);
            }
        }
    }

    Err(last_error.unwrap_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "no weather provider configured",
        )
    }))
}

fn weather_fetch_order(config: &BridgeConfig, skip_caiyun: bool) -> Vec<WeatherProvider> {
    match config.weather_provider {
        WeatherProvider::Auto => {
            let mut providers = Vec::new();
            if config.caiyun_token.is_some() && !skip_caiyun {
                providers.push(WeatherProvider::Caiyun);
            }
            if config.qweather_host.is_some() && config.qweather_token.is_some() {
                providers.push(WeatherProvider::QWeather);
            }
            providers.push(WeatherProvider::OpenMeteo);
            providers
        }
        WeatherProvider::OpenMeteo => vec![WeatherProvider::OpenMeteo],
        WeatherProvider::QWeather => vec![WeatherProvider::QWeather, WeatherProvider::OpenMeteo],
        WeatherProvider::Caiyun => {
            if skip_caiyun {
                vec![WeatherProvider::OpenMeteo]
            } else {
                vec![WeatherProvider::Caiyun, WeatherProvider::OpenMeteo]
            }
        }
    }
}

fn log_weather_provider_failure(
    state: &SharedBridgeState,
    provider: WeatherProvider,
    error: &io::Error,
) {
    if provider == WeatherProvider::Caiyun && weather_error_is_rate_limited(error) {
        let now = Instant::now();
        let backoff_until = now + CAIYUN_RATE_LIMIT_BACKOFF;
        let should_log = match state.lock() {
            Ok(mut state) => {
                let already_backing_off = caiyun_backoff_active(Some(&state), now);
                state.weather_caiyun_backoff_until = Some(backoff_until);
                !already_backing_off
            }
            Err(_) => true,
        };
        if should_log {
            eprintln!(
                "weather provider caiyun rate limited; backing off for {} minutes and using fallback providers",
                CAIYUN_RATE_LIMIT_BACKOFF.as_secs() / 60
            );
        }
        return;
    }

    eprintln!(
        "weather provider {} failed: {error}",
        weather_provider_name(provider)
    );
}

fn weather_error_is_rate_limited(error: &io::Error) -> bool {
    let message = error.to_string();
    message.contains("429 TOO MANY REQUESTS") || message.contains("429 Too Many Requests")
}

fn fetch_open_meteo_weather(
    client: &reqwest::blocking::Client,
    config: &BridgeConfig,
) -> io::Result<WeatherSnapshot> {
    let url = open_meteo_url(config.weather_latitude, config.weather_longitude);
    let response = get_weather_json::<OpenMeteoResponse>(client, url, WeatherProvider::OpenMeteo)?;
    let Some(current) = response.current else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Open-Meteo response missing current weather",
        ));
    };

    let temperature_c = current.temperature_2m.map(round_f64_to_i32);
    let wind_kmh = current.wind_speed_10m.map(round_f64_to_i32);
    let summary = current
        .weather_code
        .map(open_meteo_weather_summary_for_code)
        .unwrap_or("WEATHER")
        .to_string();
    let icon = current
        .weather_code
        .map(open_meteo_weather_icon_for_code)
        .unwrap_or("unknown")
        .to_string();

    Ok(WeatherSnapshot {
        status: "ok".to_string(),
        label: config.weather_label.clone(),
        summary,
        icon,
        temperature_c,
        wind_kmh,
        weather_code: current.weather_code,
        observed_at: now_local(),
    })
}

fn fetch_qweather_weather(
    client: &reqwest::blocking::Client,
    config: &BridgeConfig,
) -> io::Result<WeatherSnapshot> {
    let host = config.qweather_host.as_deref().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "CODEX_ORNAMENT_QWEATHER_HOST is required for qweather",
        )
    })?;
    let token = config.qweather_token.as_deref().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "CODEX_ORNAMENT_QWEATHER_TOKEN is required for qweather",
        )
    })?;
    let url = qweather_url(host, config.weather_latitude, config.weather_longitude);
    let response = client
        .get(url)
        .bearer_auth(token)
        .send()
        .map_err(|error| weather_request_error(WeatherProvider::QWeather, error))?;
    let response = response
        .error_for_status()
        .map_err(|error| weather_status_error(WeatherProvider::QWeather, error))?
        .json::<QWeatherResponse>()
        .map_err(|_| weather_parse_error(WeatherProvider::QWeather))?;
    if response.code != "200" {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("QWeather response code {}", response.code),
        ));
    };
    let Some(now) = response.now else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "QWeather response missing now weather",
        ));
    };

    let summary = qweather_summary(&now).to_string();
    let icon = qweather_icon(&summary, now.icon.as_deref()).to_string();

    Ok(WeatherSnapshot {
        status: "ok".to_string(),
        label: config.weather_label.clone(),
        summary,
        icon,
        temperature_c: now.temp.as_deref().and_then(parse_i32_text),
        wind_kmh: now.wind_speed.as_deref().and_then(parse_i32_text),
        weather_code: now.icon.as_deref().and_then(parse_i32_text),
        observed_at: now.obs_time.unwrap_or_else(now_local),
    })
}

fn fetch_caiyun_weather(
    client: &reqwest::blocking::Client,
    config: &BridgeConfig,
) -> io::Result<WeatherSnapshot> {
    let token = config.caiyun_token.as_deref().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "CODEX_ORNAMENT_CAIYUN_TOKEN is required for caiyun",
        )
    })?;
    let url = caiyun_realtime_url(token, config.weather_latitude, config.weather_longitude);
    let response = get_weather_json::<CaiyunResponse>(client, url, WeatherProvider::Caiyun)?;
    if response.status != "ok" {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("Caiyun response status {}", response.status),
        ));
    }
    let Some(realtime) = response.result.and_then(|result| result.realtime) else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Caiyun response missing realtime weather",
        ));
    };
    let skycon = realtime.skycon.unwrap_or_else(|| "UNKNOWN".to_string());

    Ok(WeatherSnapshot {
        status: "ok".to_string(),
        label: config.weather_label.clone(),
        summary: caiyun_summary_for_skycon(&skycon).to_string(),
        icon: caiyun_icon_for_skycon(&skycon).to_string(),
        temperature_c: realtime.temperature.map(round_f64_to_i32),
        wind_kmh: realtime
            .wind
            .and_then(|wind| wind.speed)
            .map(round_f64_to_i32),
        weather_code: None,
        observed_at: now_local(),
    })
}

fn open_meteo_url(latitude: f64, longitude: f64) -> String {
    format!(
        "https://api.open-meteo.com/v1/forecast?latitude={latitude:.4}&longitude={longitude:.4}&current=temperature_2m,weather_code,wind_speed_10m&timezone=auto"
    )
}

fn qweather_url(host: &str, latitude: f64, longitude: f64) -> String {
    let host = host.trim_end_matches('/');
    format!("{host}/v7/weather/now?location={longitude:.6},{latitude:.6}")
}

fn caiyun_realtime_url(token: &str, latitude: f64, longitude: f64) -> String {
    format!("https://api.caiyunapp.com/v2.6/{token}/{longitude:.6},{latitude:.6}/realtime")
}

fn get_weather_json<T: for<'de> Deserialize<'de>>(
    client: &reqwest::blocking::Client,
    url: String,
    provider: WeatherProvider,
) -> io::Result<T> {
    let response = client
        .get(url)
        .send()
        .map_err(|error| weather_request_error(provider, error))?;
    response
        .error_for_status()
        .map_err(|error| weather_status_error(provider, error))?
        .json::<T>()
        .map_err(|_| weather_parse_error(provider))
}

fn weather_request_error(provider: WeatherProvider, error: reqwest::Error) -> io::Error {
    io::Error::other(format!(
        "{} weather request failed: {error}",
        weather_provider_name(provider)
    ))
}

fn weather_status_error(provider: WeatherProvider, error: reqwest::Error) -> io::Error {
    io::Error::other(format!(
        "{} weather HTTP status failed: {error}",
        weather_provider_name(provider)
    ))
}

fn weather_parse_error(provider: WeatherProvider) -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidData,
        format!(
            "{} weather response parse failed",
            weather_provider_name(provider)
        ),
    )
}

fn weather_provider_name(provider: WeatherProvider) -> &'static str {
    match provider {
        WeatherProvider::Auto => "auto",
        WeatherProvider::OpenMeteo => "openmeteo",
        WeatherProvider::QWeather => "qweather",
        WeatherProvider::Caiyun => "caiyun",
    }
}

fn weather_unavailable(config: &BridgeConfig) -> WeatherSnapshot {
    WeatherSnapshot {
        status: "unavailable".to_string(),
        label: config.weather_label.clone(),
        summary: "WEATHER --".to_string(),
        icon: "unknown".to_string(),
        temperature_c: None,
        wind_kmh: None,
        weather_code: None,
        observed_at: now_local(),
    }
}

fn round_f64_to_i32(value: f64) -> i32 {
    if value.is_finite() {
        value.round() as i32
    } else {
        0
    }
}

fn open_meteo_weather_summary_for_code(code: i32) -> &'static str {
    match code {
        0 => "CLEAR",
        1 | 2 => "PARTLY CLOUDY",
        3 => "CLOUDY",
        45 | 48 => "FOG",
        51 | 53 | 55 | 56 | 57 => "DRIZZLE",
        61 | 63 | 65 | 66 | 67 | 80 | 81 | 82 => "RAIN",
        71 | 73 | 75 | 77 | 85 | 86 => "SNOW",
        95 | 96 | 99 => "STORM",
        _ => "WEATHER",
    }
}

fn open_meteo_weather_icon_for_code(code: i32) -> &'static str {
    match code {
        0 => "sun",
        1 | 2 => "partly-cloudy",
        3 => "cloud",
        45 | 48 => "fog",
        51 | 53 | 55 | 80 | 81 => "drizzle",
        56 | 57 | 66 | 67 | 77 => "sleet",
        61 | 63 | 65 => "rain",
        82 => "heavy-rain",
        71 | 73 | 75 | 85 | 86 => "snow",
        95 | 96 | 99 => "storm",
        _ => "unknown",
    }
}

fn qweather_summary(now: &QWeatherNow) -> &str {
    now.text.as_deref().unwrap_or("WEATHER")
}

fn qweather_icon(summary: &str, icon: Option<&str>) -> &'static str {
    if let Some(icon) = icon {
        if let Ok(code) = icon.parse::<i32>() {
            return match code {
                100 => "sun",
                101 => "partly-cloudy",
                102..=104 => "cloud",
                150 => "sun",
                151..=153 => "partly-cloudy",
                154 => "cloud",
                300..=304 => "drizzle",
                305..=309 => "rain",
                310..=315 => "heavy-rain",
                400..=405 => "snow",
                406..=409 => "sleet",
                500..=515 => "fog",
                _ => "unknown",
            };
        }
    }
    compact_weather_icon(summary)
}

fn caiyun_summary_for_skycon(skycon: &str) -> &'static str {
    match skycon {
        "CLEAR_DAY" | "CLEAR_NIGHT" => "CLEAR",
        "PARTLY_CLOUDY_DAY" | "PARTLY_CLOUDY_NIGHT" => "PARTLY CLOUDY",
        "CLOUDY" => "CLOUDY",
        "LIGHT_HAZE" | "MODERATE_HAZE" | "HEAVY_HAZE" | "FOG" => "FOG",
        "LIGHT_RAIN" | "MODERATE_RAIN" | "HEAVY_RAIN" | "STORM_RAIN" => "RAIN",
        "LIGHT_SNOW" | "MODERATE_SNOW" | "HEAVY_SNOW" | "STORM_SNOW" => "SNOW",
        "DUST" | "SAND" | "WIND" => "WIND",
        _ => "WEATHER",
    }
}

fn caiyun_icon_for_skycon(skycon: &str) -> &'static str {
    match skycon {
        "CLEAR_DAY" | "CLEAR_NIGHT" => "sun",
        "PARTLY_CLOUDY_DAY" | "PARTLY_CLOUDY_NIGHT" => "partly-cloudy",
        "CLOUDY" => "cloud",
        "LIGHT_HAZE" | "MODERATE_HAZE" | "HEAVY_HAZE" | "DUST" | "SAND" => "haze",
        "FOG" => "fog",
        "LIGHT_RAIN" => "drizzle",
        "MODERATE_RAIN" => "rain",
        "HEAVY_RAIN" | "STORM_RAIN" => "heavy-rain",
        "LIGHT_SNOW" | "MODERATE_SNOW" | "HEAVY_SNOW" | "STORM_SNOW" => "snow",
        "WIND" => "windy",
        _ => "unknown",
    }
}

fn compact_weather_icon(summary: &str) -> &'static str {
    let summary = summary.to_ascii_lowercase();
    if summary.contains("clear") || summary.contains("sun") {
        "sun"
    } else if summary.contains("partly") {
        "partly-cloudy"
    } else if summary.contains("cloud") || summary.contains("overcast") {
        "cloud"
    } else if summary.contains("haze")
        || summary.contains("dust")
        || summary.contains("sand")
        || summary.contains("smog")
    {
        "haze"
    } else if summary.contains("fog") {
        "fog"
    } else if summary.contains("drizzle") {
        "drizzle"
    } else if summary.contains("sleet") || summary.contains("freezing rain") {
        "sleet"
    } else if summary.contains("storm") || summary.contains("heavy") {
        "heavy-rain"
    } else if summary.contains("rain") {
        "rain"
    } else if summary.contains("snow") {
        "snow"
    } else if summary.contains("wind") {
        "windy"
    } else {
        "unknown"
    }
}
fn parse_i32_text(value: &str) -> Option<i32> {
    value.parse::<f64>().ok().map(round_f64_to_i32)
}

fn io_other(error: impl std::error::Error + Send + Sync + 'static) -> io::Error {
    io::Error::other(error)
}

fn write_json<T: Serialize>(stream: &mut TcpStream, status: u16, value: &T) -> io::Result<()> {
    let body = serde_json::to_vec(value)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
    write_response(stream, status, "application/json; charset=utf-8", &body)
}

fn write_response(
    stream: &mut TcpStream,
    status: u16,
    content_type: &str,
    body: &[u8],
) -> io::Result<()> {
    write_response_with_headers(stream, status, content_type, &[], body)
}

fn write_response_with_headers(
    stream: &mut TcpStream,
    status: u16,
    content_type: &str,
    extra_headers: &[(&str, String)],
    body: &[u8],
) -> io::Result<()> {
    let reason = match status {
        200 => "OK",
        202 => "Accepted",
        204 => "No Content",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
        _ => "OK",
    };
    write!(
        stream,
        "HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type, X-Codex-Ornament-Token, Access-Control-Request-Private-Network\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Private-Network: true\r\nConnection: close\r\n",
        body.len()
    )?;
    for (name, value) in extra_headers {
        write!(stream, "{name}: {value}\r\n")?;
    }
    write!(stream, "\r\n")?;
    stream.write_all(body)
}

fn now_local() -> String {
    Local::now().to_rfc3339()
}

fn env_text(name: &str) -> Option<String> {
    env::var(name)
        .ok()
        .map(|value| value.trim().to_string())
        .filter(|value| !value.is_empty())
}

fn env_f64(name: &str) -> Option<f64> {
    env_text(name).and_then(|value| value.parse::<f64>().ok())
}

fn env_u64(name: &str) -> Option<u64> {
    env_text(name).and_then(|value| value.parse::<u64>().ok())
}

fn env_usize(name: &str) -> Option<usize> {
    env_text(name).and_then(|value| value.parse::<usize>().ok())
}

fn env_truthy(name: &str) -> bool {
    env_text(name)
        .map(|value| {
            matches!(
                value.to_ascii_lowercase().as_str(),
                "1" | "true" | "yes" | "on"
            )
        })
        .unwrap_or(false)
}

fn env_weather_provider(name: &str) -> Option<WeatherProvider> {
    env_text(name).and_then(|value| parse_weather_provider(&value))
}

fn parse_weather_provider(value: &str) -> Option<WeatherProvider> {
    match value.trim().to_ascii_lowercase().as_str() {
        "auto" => Some(WeatherProvider::Auto),
        "openmeteo" | "open-meteo" => Some(WeatherProvider::OpenMeteo),
        "qweather" | "q-weather" | "heweather" => Some(WeatherProvider::QWeather),
        "caiyun" | "caiyunapp" => Some(WeatherProvider::Caiyun),
        _ => None,
    }
}

fn codex_home() -> PathBuf {
    env_text("CODEX_HOME")
        .map(PathBuf::from)
        .or_else(|| env_text("USERPROFILE").map(|home| PathBuf::from(home).join(".codex")))
        .unwrap_or_else(|| PathBuf::from(".codex"))
}

fn bridge_event_log_path(codex_home: &Path) -> Option<PathBuf> {
    if !bridge_event_persistence_enabled() {
        return None;
    }
    Some(
        env_text("CODEX_ORNAMENT_EVENT_LOG")
            .map(PathBuf::from)
            .unwrap_or_else(|| codex_home.join("ornament").join("bridge-events.jsonl")),
    )
}

fn bridge_event_persistence_enabled() -> bool {
    env_text("CODEX_ORNAMENT_PERSIST_EVENTS")
        .map(|value| {
            !matches!(
                value.to_ascii_lowercase().as_str(),
                "0" | "false" | "no" | "off"
            )
        })
        .unwrap_or(true)
}

fn clip(value: &str, max_chars: usize) -> String {
    let mut output = String::new();
    for (index, ch) in value.chars().enumerate() {
        if index >= max_chars {
            output.push_str("...");
            break;
        }
        output.push(ch);
    }
    output
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_config(token: Option<&str>) -> BridgeConfig {
        BridgeConfig {
            bind: "127.0.0.1:8787".to_string(),
            token: token.map(str::to_string),
            event_log_path: None,
            event_log_max_bytes: DEFAULT_EVENT_LOG_MAX_BYTES,
            event_log_compact_keep_events: DEFAULT_EVENT_LOG_COMPACT_KEEP_EVENTS,
            yaohud_key: None,
            music_public_base_url: None,
            music_public_base_url_locked: false,
            tracked_session_id: None,
            codex_home: PathBuf::from(".codex-test"),
            weather_latitude: DEFAULT_WEATHER_LATITUDE,
            weather_longitude: DEFAULT_WEATHER_LONGITUDE,
            weather_label: DEFAULT_WEATHER_LABEL.to_string(),
            weather_provider: DEFAULT_WEATHER_PROVIDER,
            qweather_host: None,
            qweather_token: None,
            caiyun_token: None,
            xiaozhi_ws_url: None,
            xiaozhi_token: None,
            standby_wallpaper_dir: PathBuf::from(DEFAULT_STANDBY_WALLPAPER_DIR),
            standby_wallpaper_fixed: None,
        }
    }

    fn scoped_test_config(session_id: &str, codex_home: impl Into<PathBuf>) -> BridgeConfig {
        BridgeConfig {
            bind: "127.0.0.1:8787".to_string(),
            token: None,
            event_log_path: None,
            event_log_max_bytes: DEFAULT_EVENT_LOG_MAX_BYTES,
            event_log_compact_keep_events: DEFAULT_EVENT_LOG_COMPACT_KEEP_EVENTS,
            yaohud_key: None,
            music_public_base_url: None,
            music_public_base_url_locked: false,
            tracked_session_id: Some(session_id.to_string()),
            codex_home: codex_home.into(),
            weather_latitude: DEFAULT_WEATHER_LATITUDE,
            weather_longitude: DEFAULT_WEATHER_LONGITUDE,
            weather_label: DEFAULT_WEATHER_LABEL.to_string(),
            weather_provider: DEFAULT_WEATHER_PROVIDER,
            qweather_host: None,
            qweather_token: None,
            caiyun_token: None,
            xiaozhi_ws_url: None,
            xiaozhi_token: None,
            standby_wallpaper_dir: PathBuf::from(DEFAULT_STANDBY_WALLPAPER_DIR),
            standby_wallpaper_fixed: None,
        }
    }

    #[test]
    fn xiaozhi_session_api_tracks_requested_state() {
        let state = Arc::new(Mutex::new(BridgeState::default()));

        let started = xiaozhi_start_session(
            &state,
            XiaozhiSessionRequest {
                client_id: Some("esp32-test".to_string()),
                sample_rate: Some(16000),
                channels: Some(1),
                format: Some("pcm_s16le".to_string()),
                ws_url: None,
                token: None,
            },
            None,
        );
        assert!(started.ok);
        assert_eq!(started.state, "listening");
        assert!(started.session_requested);
        assert_eq!(started.client_id.as_deref(), Some("esp32-test"));

        let uplink = xiaozhi_push_uplink_pcm(&state, &[1, 0, 2, 0]);
        assert_eq!(uplink.uplink_frames, 2);

        let injected = xiaozhi_push_downlink_pcm(&state, &[3, 0, 4, 0, 5, 0]);
        assert_eq!(injected.state, "speaking");
        let downlink = xiaozhi_pop_downlink_pcm(&state, 4).unwrap();
        assert_eq!(downlink, vec![3, 0, 4, 0]);

        let stopped = xiaozhi_stop_session(&state);
        assert!(stopped.ok);
        assert_eq!(stopped.state, "idle");
        assert!(!stopped.session_requested);
    }

    #[test]
    fn xiaozhi_proxy_status_update_sets_text_and_state() {
        let state = Arc::new(Mutex::new(BridgeState::default()));

        let updated = xiaozhi_apply_status_update(
            &state,
            XiaozhiStatusUpdate {
                state: Some("speaking".to_string()),
                connected: Some(true),
                configured: Some(true),
                upstream_running: Some(true),
                session_id: Some("session-1".to_string()),
                last_error: Some(String::new()),
                last_stt: Some("播放好运来".to_string()),
                last_tts: Some("正在播放好运来".to_string()),
            },
        );

        assert_eq!(updated.state, "speaking");
        assert!(updated.connected);
        assert!(updated.upstream_running);
        assert_eq!(updated.session_id, "session-1");
        assert_eq!(updated.last_stt, "播放好运来");
        assert_eq!(updated.last_tts, "正在播放好运来");
        assert_eq!(updated.last_error, "");
    }

    #[test]
    fn xiaozhi_session_can_use_preconfigured_upstream() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let mut config = test_config(None);
        config.xiaozhi_ws_url = Some("wss://xiaozhi.example/ws".to_string());
        config.xiaozhi_token = Some("token-from-env".to_string());
        configure_xiaozhi_proxy_if_configured(config, Arc::clone(&state));

        let started = xiaozhi_start_session(
            &state,
            XiaozhiSessionRequest {
                client_id: Some("esp32-test".to_string()),
                sample_rate: Some(16000),
                channels: Some(1),
                format: Some("pcm_s16le".to_string()),
                ws_url: None,
                token: None,
            },
            None,
        );

        assert!(started.ok);
        assert!(started.session_requested);
        assert!(started.upstream_configured);
        assert_eq!(
            started.last_error,
            "Xiaozhi PCM bridge ready (16000 Hz, 1 ch, pcm_s16le)"
        );
    }

    fn recent_timestamp(seconds_ago: i64) -> String {
        (Local::now() - chrono::Duration::seconds(seconds_ago)).to_rfc3339()
    }

    fn future_timestamp(seconds_from_now: i64) -> String {
        (Local::now() + chrono::Duration::seconds(seconds_from_now)).to_rfc3339()
    }

    fn past_timestamp(seconds_ago: i64) -> String {
        recent_timestamp(seconds_ago)
    }

    fn quota_snapshot(
        primary_remaining: f64,
        secondary_remaining: f64,
        primary_reset: &str,
        secondary_reset: &str,
    ) -> QuotaSnapshot {
        QuotaSnapshot {
            status: SnapshotStatus::Ok,
            source: "codex-wham".to_string(),
            source_label: Some("ChatGPT usage API".to_string()),
            web_url: Some(quota_core::USAGE_URL.to_string()),
            limit_id: Some("codex".to_string()),
            plan_type: Some("team".to_string()),
            primary_used_percent: Some(100.0 - primary_remaining),
            primary_remaining_percent: Some(primary_remaining),
            primary_window_minutes: Some(300),
            primary_resets_at: Some(primary_reset.to_string()),
            secondary_used_percent: Some(100.0 - secondary_remaining),
            secondary_remaining_percent: Some(secondary_remaining),
            secondary_window_minutes: Some(10_080),
            secondary_resets_at: Some(secondary_reset.to_string()),
            credits: None,
            observed_at: Some(now_local()),
            captured_at: Some(now_local()),
            error: None,
        }
    }

    #[test]
    fn maps_open_meteo_weather_codes_to_compact_display_labels() {
        assert_eq!(open_meteo_weather_summary_for_code(0), "CLEAR");
        assert_eq!(open_meteo_weather_summary_for_code(2), "PARTLY CLOUDY");
        assert_eq!(open_meteo_weather_summary_for_code(45), "FOG");
        assert_eq!(open_meteo_weather_summary_for_code(65), "RAIN");
        assert_eq!(open_meteo_weather_summary_for_code(75), "SNOW");
        assert_eq!(open_meteo_weather_summary_for_code(95), "STORM");
        assert_eq!(open_meteo_weather_summary_for_code(999), "WEATHER");
    }

    #[test]
    fn maps_open_meteo_weather_codes_to_display_icons() {
        assert_eq!(open_meteo_weather_icon_for_code(0), "sun");
        assert_eq!(open_meteo_weather_icon_for_code(2), "partly-cloudy");
        assert_eq!(open_meteo_weather_icon_for_code(45), "fog");
        assert_eq!(open_meteo_weather_icon_for_code(65), "rain");
        assert_eq!(open_meteo_weather_icon_for_code(75), "snow");
        assert_eq!(open_meteo_weather_icon_for_code(95), "storm");
        assert_eq!(open_meteo_weather_icon_for_code(999), "unknown");
    }

    #[test]
    fn builds_open_meteo_url_from_configured_coordinates() {
        let url = open_meteo_url(31.23041, 121.47369);
        assert!(url.contains("latitude=31.2304"));
        assert!(url.contains("longitude=121.4737"));
        assert!(url.contains("current=temperature_2m,weather_code,wind_speed_10m"));
        assert!(url.contains("timezone=auto"));
    }

    #[test]
    fn builds_caiyun_realtime_url_with_token_in_path_and_lon_lat_order() {
        let url = caiyun_realtime_url(
            "token-123",
            DEFAULT_WEATHER_LATITUDE,
            DEFAULT_WEATHER_LONGITUDE,
        );
        assert_eq!(
            url,
            "https://api.caiyunapp.com/v2.6/token-123/116.341625,39.995401/realtime"
        );
    }

    #[test]
    fn auto_weather_prefers_caiyun_when_token_is_configured() {
        let mut config = test_config(None);
        config.caiyun_token = Some("token-123".to_string());
        assert_eq!(
            weather_fetch_order(&config, false),
            vec![WeatherProvider::Caiyun, WeatherProvider::OpenMeteo]
        );
    }

    #[test]
    fn caiyun_backoff_uses_default_refresh_interval() {
        let mut config = test_config(None);
        config.caiyun_token = Some("token-123".to_string());
        let now = Instant::now();
        let state = BridgeState {
            weather_caiyun_backoff_until: Some(now + CAIYUN_RATE_LIMIT_BACKOFF),
            ..BridgeState::default()
        };

        assert_eq!(
            weather_refresh_interval_at(&config, Some(&state), now),
            DEFAULT_WEATHER_CACHE_TTL
        );
    }

    #[test]
    fn skips_caiyun_provider_while_rate_limit_backoff_is_active() {
        let mut config = test_config(None);
        config.weather_provider = WeatherProvider::Caiyun;
        config.caiyun_token = Some("token-123".to_string());

        assert_eq!(
            weather_fetch_order(&config, true),
            vec![WeatherProvider::OpenMeteo]
        );
        assert_eq!(
            weather_fetch_order(&config, false),
            vec![WeatherProvider::Caiyun, WeatherProvider::OpenMeteo]
        );
    }

    #[test]
    fn parses_weather_provider_names() {
        assert_eq!(
            parse_weather_provider("caiyun"),
            Some(WeatherProvider::Caiyun)
        );
        assert_eq!(
            parse_weather_provider("open-meteo"),
            Some(WeatherProvider::OpenMeteo)
        );
        assert_eq!(parse_weather_provider("bad-provider"), None);
    }

    #[test]
    fn builds_music_stream_url_from_public_base() {
        let mut config = test_config(None);
        config.music_public_base_url = Some("http://192.168.1.102:8787/".to_string());
        config.music_public_base_url_locked = true;
        let request = MusicRequest {
            song: "lucky song".to_string(),
            artist: Some("zu hai".to_string()),
            index: 2,
        };

        assert_eq!(
            music_stream_url(&config, None, &request),
            "http://192.168.1.102:8787/v1/music/stream?song=lucky+song&artist=zu+hai&index=2"
        );
        assert_eq!(
            music_cover_url(&config, None, &request, None),
            "http://192.168.1.102:8787/v1/music/cover?song=lucky+song&artist=zu+hai&index=2"
        );
        let resolved = ResolvedSong {
            source: "yaohud",
            title: "lucky song".to_string(),
            artist: "zu hai".to_string(),
            album: "lucky album".to_string(),
            picture: "https://example.test/cover art.jpg".to_string(),
            url: "https://example.test/play.mp3".to_string(),
            duration_ms: Some(227_000),
            lyrics: None,
        };
        let cover_url = music_cover_url(&config, None, &request, Some(&resolved));
        assert!(cover_url.starts_with(
            "http://192.168.1.102:8787/v1/music/cover?song=lucky+song&artist=zu+hai&index=2&coverKey="
        ));
        assert!(!cover_url.contains("cover%20art"));
        assert!(cover_url.len() < 140);
    }

    #[test]
    fn unlocked_music_stream_url_uses_peer_reachable_host_before_configured_base() {
        let mut config = test_config(None);
        config.bind = "0.0.0.0:9876".to_string();
        config.music_public_base_url = Some("http://192.168.1.102:8787/".to_string());
        let peer = "192.168.1.44:50000".parse::<SocketAddr>().ok();
        let request = MusicRequest {
            song: "song".to_string(),
            artist: None,
            index: 1,
        };

        let url = music_stream_url(&config, peer, &request);

        assert!(url.starts_with("http://"));
        assert!(url.ends_with(":9876/v1/music/stream?song=song&index=1"));
        assert!(!url.starts_with("http://192.168.1.102:8787/"));
    }

    #[test]
    fn caches_music_resolve_for_matching_request_only() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let request = MusicRequest {
            song: "lucky song".to_string(),
            artist: None,
            index: 1,
        };
        let song = ResolvedSong {
            source: "yaohud",
            title: "lucky song".to_string(),
            artist: "test artist".to_string(),
            album: "lucky album".to_string(),
            picture: String::new(),
            url: "https://music.163.com/song/media/outer/url?id=333750.mp3".to_string(),
            duration_ms: Some(188_000),
            lyrics: None,
        };
        cache_music_resolve(&state, &request, &song);

        assert_eq!(cached_music_resolve(&state, &request), Some(song));
        assert!(cached_music_resolve(
            &state,
            &MusicRequest {
                index: 2,
                ..request
            }
        )
        .is_none());
    }

    #[test]
    fn stream_fallback_caches_selected_song_for_original_request() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let request = MusicRequest {
            song: "lucky song".to_string(),
            artist: None,
            index: 1,
        };
        let fallback_request = MusicRequest {
            index: 2,
            ..request.clone()
        };
        let fallback_song = ResolvedSong {
            source: "yaohud",
            title: "playable fallback".to_string(),
            artist: "test artist".to_string(),
            album: "fallback album".to_string(),
            picture: "https://example.test/fallback.jpg".to_string(),
            url: "https://example.test/fallback.mp3".to_string(),
            duration_ms: Some(205_000),
            lyrics: Some("[00:01.00]fallback".to_string()),
        };

        cache_music_resolve(&state, &fallback_request, &fallback_song);
        cache_music_resolve(&state, &request, &fallback_song);

        assert_eq!(cached_music_resolve(&state, &request), Some(fallback_song));
    }

    #[test]
    fn builds_yaohud_lrc_url_for_aggregate_lyrics_api() {
        assert_eq!(
            yaohud_lrc_url("key 1", "2058263034", "wy"),
            "https://api.yaohud.cn/api/music/lrc?key=key+1&mid=2058263034&type=wy"
        );
    }

    #[test]
    fn parses_yaohud_lyrics_json_response() {
        let body = r#"{"code":200,"data":{"lrc":"[00:01.00]hello\n[00:02.00]world"}}"#;

        assert_eq!(
            parse_yaohud_lyrics_body(body).as_deref(),
            Some("[00:01.00]hello\n[00:02.00]world")
        );
    }

    #[test]
    fn parses_yaohud_lyrics_when_data_is_string() {
        let body = r#"{"code":200,"data":"[00:01.00]hello"}"#;

        assert_eq!(
            parse_yaohud_lyrics_body(body).as_deref(),
            Some("[00:01.00]hello")
        );
    }

    #[test]
    fn picks_yaohud_cover_from_common_picture_fields() {
        let data = YaohudMusicData {
            pic_url: Some("https://example.test/cover.jpg".to_string()),
            ..YaohudMusicData::default()
        };

        assert_eq!(yaohud_picture_url(&data), "https://example.test/cover.jpg");
    }

    #[test]
    fn parses_ffprobe_duration_output_as_milliseconds() {
        assert_eq!(music_duration_ms_from_str("187.456000"), Some(187_456));
        assert_eq!(music_duration_ms_from_str("03:07"), Some(187_000));
    }

    #[test]
    fn standby_wallpaper_filter_covers_without_padding() {
        let filter = standby_wallpaper_ffmpeg_filter();

        assert!(filter.contains("force_original_aspect_ratio=increase"));
        assert!(filter.contains("crop=240:240"));
        assert!(!filter.contains("pad="));
    }

    #[test]
    fn netease_outer_url_points_to_mp3_proxy() {
        let request = MusicRequest {
            song: "lucky song".to_string(),
            artist: None,
            index: 1,
        };
        let song = resolve_song_netease(&request).unwrap();

        assert_eq!(song.source, "netease");
        assert!(!song.title.is_empty());
        assert!(song
            .url
            .starts_with("https://music.163.com/song/media/outer/url?id="));
        assert!(song.url.ends_with(".mp3"));
    }

    #[test]
    fn returns_cached_weather_before_fetching_again() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let cached = WeatherSnapshot {
            status: "ok".to_string(),
            label: "TEST".to_string(),
            summary: "CLEAR".to_string(),
            icon: "sun".to_string(),
            temperature_c: Some(25),
            wind_kmh: Some(6),
            weather_code: Some(0),
            observed_at: "2026-05-31T12:00:00+08:00".to_string(),
        };
        {
            let mut state = state.lock().unwrap();
            state.weather = Some(CachedWeather {
                snapshot: cached.clone(),
                fetched_at: Instant::now(),
            });
        }

        assert_eq!(cached_or_refresh_weather(&state, &config), cached);
    }

    #[test]
    fn caiyun_budget_refresh_interval_allocates_daily_calls() {
        assert_eq!(CAIYUN_NIGHT_CALL_BUDGET, 12);
        assert_eq!(CAIYUN_DAY_CALL_BUDGET, 9_988);
        let day_interval = Duration::from_nanos(
            ((u128::from(CAIYUN_DAY_SECONDS) * 1_000_000_000 + u128::from(CAIYUN_DAY_CALL_BUDGET)
                - 1)
                / u128::from(CAIYUN_DAY_CALL_BUDGET)) as u64,
        );
        assert_eq!(
            caiyun_refresh_interval_for_hour(0),
            Duration::from_secs(30 * 60)
        );
        assert_eq!(
            caiyun_refresh_interval_for_hour(5),
            Duration::from_secs(30 * 60)
        );
        assert_eq!(caiyun_refresh_interval_for_hour(6), day_interval);
        assert_eq!(caiyun_refresh_interval_for_hour(23), day_interval);
        assert_eq!(day_interval.as_secs(), 6);
        assert_eq!(day_interval.subsec_millis(), 487);
    }

    #[test]
    fn caiyun_budget_only_applies_when_caiyun_can_be_called() {
        let mut config = test_config(None);
        assert!(!caiyun_budget_applies(&config));

        config.caiyun_token = Some("token-123".to_string());
        assert!(caiyun_budget_applies(&config));

        config.weather_provider = WeatherProvider::QWeather;
        assert!(!caiyun_budget_applies(&config));
    }

    #[test]
    fn weather_refresh_due_throttles_failed_attempts() {
        let mut config = test_config(None);
        config.caiyun_token = Some("token-123".to_string());
        let now = Instant::now();

        let state = BridgeState {
            weather_last_attempt: Some(now),
            ..BridgeState::default()
        };
        assert!(!weather_refresh_is_due_at(&state, &config, now));

        assert!(weather_refresh_is_due_at(
            &state,
            &config,
            now + CAIYUN_NIGHT_REFRESH_INTERVAL + Duration::from_millis(1)
        ));
    }

    #[test]
    fn weather_refresh_due_respects_caiyun_backoff_window() {
        let mut config = test_config(None);
        config.caiyun_token = Some("token-123".to_string());
        let now = Instant::now();

        let state = BridgeState {
            weather_last_attempt: Some(now),
            weather_caiyun_backoff_until: Some(now + CAIYUN_RATE_LIMIT_BACKOFF),
            ..BridgeState::default()
        };
        assert!(!weather_refresh_is_due_at(
            &state,
            &config,
            now + Duration::from_secs(7)
        ));

        assert!(weather_refresh_is_due_at(
            &state,
            &config,
            now + DEFAULT_WEATHER_CACHE_TTL + Duration::from_millis(1)
        ));
    }

    #[test]
    fn maps_lifecycle_start_to_running() {
        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "prompt": "build the firmware"
        }));

        assert_eq!(event.kind, "UserPromptSubmit");
        assert_eq!(event.status, "running");
        assert_eq!(event.title, "Codex running");
        assert_eq!(event.message, "build the firmware");
    }

    #[test]
    fn recovers_identity_from_json_message_payload() {
        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "message": "{\"session_id\":\"session-1\",\"turn_id\":\"turn-1\",\"cwd\":\"D:\\\\Desktop\\\\codex\",\"model\":\"gpt-5.5\"}"
        }));

        assert_eq!(event.status, "running");
        assert_eq!(event.session_id.as_deref(), Some("session-1"));
        assert_eq!(event.turn_id.as_deref(), Some("turn-1"));
        assert_eq!(event.cwd.as_deref(), Some("D:\\Desktop\\codex"));
        assert_eq!(event.model.as_deref(), Some("gpt-5.5"));
    }

    #[test]
    fn recovers_identity_from_clipped_json_message_payload() {
        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "message": "{\"session_id\":\"session-1\",\"turn_id\":\"turn-1\",\"transcript_path\":null,\"cwd\":\"C:\\\\Program Files\\\\WindowsApps\\\\OpenAI.Codex_26.527.3686.0_x64__2p2nqsd0c76g0\\\\app"
        }));

        assert_eq!(event.status, "running");
        assert_eq!(event.session_id.as_deref(), Some("session-1"));
        assert_eq!(event.turn_id.as_deref(), Some("turn-1"));
        assert_eq!(
            event.cwd.as_deref(),
            Some("C:\\Program Files\\WindowsApps\\OpenAI.Codex_26.527.3686.0_x64__2p2nqsd0c76g0\\app")
        );
    }

    #[test]
    fn maps_stop_to_done() {
        let event = normalize_event(&json!({
            "hook_event_name": "Stop",
            "session_id": "session-1",
            "cwd": "D:\\Desktop\\codex"
        }));

        assert_eq!(event.status, "done");
        assert_eq!(event.title, "Codex done");
        assert_eq!(event.session_id.as_deref(), Some("session-1"));
        assert_eq!(event.cwd.as_deref(), Some("D:\\Desktop\\codex"));
    }

    #[test]
    fn maps_claude_stop_to_claude_done() {
        let event = normalize_event(&json!({
            "hook_event_name": "Stop",
            "source": "Claude"
        }));

        assert_eq!(event.status, "done");
        assert_eq!(event.title, "Claude done");
        assert_eq!(event.source.as_deref(), Some("claude"));
    }

    #[test]
    fn infers_claude_source_from_transcript_path() {
        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "claude-session",
            "transcript_path": "C:\\Users\\86147\\.claude\\projects\\demo\\session.jsonl",
            "prompt": "continue"
        }));

        assert_eq!(event.status, "running");
        assert_eq!(event.title, "Claude running");
        assert_eq!(event.source.as_deref(), Some("claude"));
        assert!(event
            .turn_id
            .as_deref()
            .unwrap_or_default()
            .starts_with("claude-transcript-"));
    }

    #[test]
    fn maps_notify_event_to_done() {
        let event = normalize_event(&json!({
            "type": "agent-turn-complete",
            "last-assistant-message": "finished"
        }));

        assert_eq!(event.kind, "agent-turn-complete");
        assert_eq!(event.status, "done");
        assert_eq!(event.title, "Codex done");
        assert_eq!(event.message, "finished");
    }

    #[test]
    fn rejects_invalid_hook_json_before_normalizing_event() {
        assert!(parse_hook_payload(br#"{"hook_event_name":"UserPromptSubmit"}"#).is_ok());
        assert!(parse_hook_payload(b"{hook_event_name:UserPromptSubmit}").is_err());
    }

    #[test]
    fn parses_hook_json_after_replacing_unpaired_surrogate_escape() {
        let payload = parse_hook_payload(
            br#"{"hook_event_name":"Stop","turn_id":"turn-1","message":"bad \uD800 text"}"#,
        )
        .unwrap();
        let event = normalize_event(&payload);

        assert_eq!(event.kind, "Stop");
        assert_eq!(event.status, "done");
        assert_eq!(event.turn_id.as_deref(), Some("turn-1"));
        assert!(event.message.contains('\u{FFFD}'));
    }

    #[test]
    fn quota_background_refresh_returns_placeholder_without_blocking() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let snapshot = cached_or_refresh_quota_background(&state);

        assert_eq!(snapshot.status, SnapshotStatus::NoData);
        assert!(snapshot.error.as_deref().unwrap_or("").contains("pending"));

        let state = state.lock().unwrap();
        assert!(state.quota_refreshing || state.quota.is_some());
    }

    #[test]
    fn quota_refresh_due_respects_one_minute_interval() {
        let now = Instant::now();
        let mut state = BridgeState::default();
        assert!(quota_refresh_is_due_at(&state, now));

        state.quota_refreshing = true;
        assert!(!quota_refresh_is_due_at(&state, now));

        state.quota_refreshing = false;
        state.quota = Some(CachedQuota {
            snapshot: quota_unavailable(),
            fetched_at: now,
        });
        assert!(!quota_refresh_is_due_at(&state, now));

        state.quota = Some(CachedQuota {
            snapshot: quota_unavailable(),
            fetched_at: now,
        });
        assert!(quota_refresh_is_due_at(
            &state,
            now + QUOTA_REFRESH_INTERVAL + Duration::from_millis(1)
        ));
    }

    #[test]
    fn quota_merge_keeps_previous_snapshot_when_refresh_has_no_actual_data() {
        let previous = quota_snapshot(
            64.0,
            92.0,
            "2026-06-02T18:00:00+08:00",
            "2026-06-08T18:00:00+08:00",
        );
        let fetched = quota_unavailable();

        let merged = merge_quota_snapshot(Some(&previous), fetched);

        assert_eq!(merged.status, SnapshotStatus::Ok);
        assert_eq!(merged.primary_remaining_percent, Some(64.0));
        assert_eq!(merged.secondary_remaining_percent, Some(92.0));
        assert_eq!(
            merged.primary_resets_at.as_deref(),
            Some("2026-06-02T18:00:00+08:00")
        );
    }

    #[test]
    fn quota_merge_keeps_reset_time_until_recorded_time_has_passed() {
        let previous_reset = future_timestamp(3600);
        let fetched_reset = future_timestamp(7200);
        let previous = quota_snapshot(64.0, 92.0, &previous_reset, &future_timestamp(86_400));
        let fetched = quota_snapshot(63.0, 91.0, &fetched_reset, &future_timestamp(172_800));

        let merged = merge_quota_snapshot(Some(&previous), fetched);

        assert_eq!(merged.primary_remaining_percent, Some(63.0));
        assert_eq!(merged.primary_resets_at, Some(previous_reset));
    }

    #[test]
    fn quota_merge_accepts_new_reset_time_after_recorded_time_has_passed() {
        let previous_reset = past_timestamp(60);
        let fetched_reset = future_timestamp(3600);
        let previous = quota_snapshot(64.0, 92.0, &previous_reset, &past_timestamp(60));
        let fetched = quota_snapshot(99.0, 100.0, &fetched_reset, &future_timestamp(604_800));

        let merged = merge_quota_snapshot(Some(&previous), fetched);

        assert_eq!(merged.primary_remaining_percent, Some(99.0));
        assert_eq!(merged.primary_resets_at, Some(fetched_reset));
    }

    #[test]
    fn quota_merge_drops_removed_short_window_after_successful_refresh() {
        let previous = quota_snapshot(
            64.0,
            92.0,
            &future_timestamp(3600),
            &future_timestamp(86_400),
        );
        let mut fetched = quota_snapshot(
            0.0,
            91.0,
            &future_timestamp(3600),
            &future_timestamp(172_800),
        );
        fetched.primary_used_percent = None;
        fetched.primary_remaining_percent = None;
        fetched.primary_window_minutes = None;
        fetched.primary_resets_at = None;

        let merged = merge_quota_snapshot(Some(&previous), fetched);

        assert_eq!(merged.status, SnapshotStatus::Ok);
        assert_eq!(merged.primary_remaining_percent, None);
        assert_eq!(merged.primary_resets_at, None);
        assert_eq!(merged.secondary_remaining_percent, Some(91.0));
    }

    #[test]
    fn clips_long_messages() {
        let input = "a".repeat(180);
        let clipped = clip(&input, 160);

        assert_eq!(clipped.chars().count(), 163);
        assert!(clipped.ends_with("..."));
    }

    #[test]
    fn allows_loopback_post_without_token() {
        let request = HttpRequest::default();
        let peer = "127.0.0.1:50000".parse::<SocketAddr>().ok();

        assert!(post_allowed(peer, &request, &test_config(None)));
    }

    #[test]
    fn rejects_lan_post_without_token() {
        let request = HttpRequest::default();
        let peer = "192.168.1.20:50000".parse::<SocketAddr>().ok();

        assert!(!post_allowed(peer, &request, &test_config(None)));
    }

    #[test]
    fn allows_lan_post_with_matching_token() {
        let mut request = HttpRequest::default();
        request.headers.insert(
            "x-codex-ornament-token".to_string(),
            "secret-token".to_string(),
        );
        let peer = "192.168.1.20:50000".parse::<SocketAddr>().ok();

        assert!(post_allowed(
            peer,
            &request,
            &test_config(Some("secret-token"))
        ));
    }

    #[test]
    fn discovery_info_for_lan_peer_uses_bridge_port() {
        let mut config = test_config(None);
        config.bind = "0.0.0.0:9876".to_string();
        let peer = "192.168.1.44:50000".parse::<SocketAddr>().unwrap();

        let info = discover_info_for_peer(&config, peer).unwrap();

        assert_eq!(info.service, "codex-ornament-bridge");
        assert!(info.state_url.starts_with("http://"));
        assert!(info.state_url.ends_with(":9876/state"));
        assert!(info.health_url.ends_with(":9876/health"));
        assert!(!info.local_ip.is_empty());
    }

    #[test]
    fn discovery_rejects_proxy_and_non_lan_addresses() {
        assert!(!lan_discovery_ip_is_usable("127.0.0.1".parse().unwrap()));
        assert!(!lan_discovery_ip_is_usable("169.254.1.2".parse().unwrap()));
        assert!(!lan_discovery_ip_is_usable("198.18.0.1".parse().unwrap()));
        assert!(!lan_discovery_ip_is_usable(
            "198.19.255.254".parse().unwrap()
        ));
        assert!(lan_discovery_ip_is_usable("192.168.1.101".parse().unwrap()));
        assert!(lan_discovery_ip_is_usable("10.0.0.2".parse().unwrap()));
        assert!(lan_discovery_ip_is_usable("172.16.0.2".parse().unwrap()));
    }

    #[test]
    fn empty_state_reports_done_with_zero_active_tasks() {
        let state = Arc::new(Mutex::new(BridgeState::default()));

        let config = test_config(None);
        let snapshot = task_snapshot(&state, &config);

        assert_eq!(snapshot.status, "done");
        assert_eq!(snapshot.active_task_count, 0);
        assert!(snapshot.task.is_none());
    }

    #[test]
    fn tracks_multiple_running_tasks() {
        let mut state = BridgeState::default();
        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));
        let second = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-2"
        }));

        apply_task_event(&mut state, first);
        apply_task_event(&mut state, second);

        assert_eq!(state.active_tasks.len(), 2);
        assert_eq!(
            state
                .active_order
                .iter()
                .map(String::as_str)
                .collect::<Vec<_>>(),
            vec![
                "source:codex:session:session-1:turn:turn-1",
                "source:codex:session:session-2:turn:turn-2"
            ]
        );
    }

    #[test]
    fn reopening_same_turn_in_another_session_replaces_stale_active_task() {
        let mut state = BridgeState::default();
        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-shared"
        }));
        let reopened = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-shared"
        }));

        apply_task_event(&mut state, first);
        apply_task_event(&mut state, reopened);

        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(
            state
                .active_order
                .iter()
                .map(String::as_str)
                .collect::<Vec<_>>(),
            vec!["source:codex:session:session-2:turn:turn-shared"]
        );
    }

    #[test]
    fn stopping_reopened_turn_clears_stale_active_task_from_other_session() {
        let mut state = BridgeState::default();
        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-shared"
        }));
        let reopened = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-shared"
        }));
        let stop = normalize_event(&json!({
            "hook_event_name": "Stop",
            "session_id": "session-2",
            "turn_id": "turn-shared"
        }));

        apply_task_event(&mut state, first);
        apply_task_event(&mut state, reopened);
        apply_task_event(&mut state, stop);

        assert_eq!(state.active_tasks.len(), 0);
        assert_eq!(state.done_tasks.len(), 1);
        assert_eq!(
            state.task.as_ref().map(|event| event.status.as_str()),
            Some("done")
        );
    }

    #[test]
    fn same_session_new_identified_turn_replaces_previous_active_task() {
        let mut state = BridgeState::default();
        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));
        let second = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-2"
        }));

        apply_task_event(&mut state, first);
        apply_task_event(&mut state, second);

        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(
            state
                .active_tasks
                .values()
                .next()
                .and_then(|event| event.turn_id.as_deref()),
            Some("turn-2")
        );
    }

    #[test]
    fn same_source_different_sessions_keep_concurrent_active_tasks() {
        let mut state = BridgeState::default();
        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));
        let second = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-2"
        }));

        apply_task_event(&mut state, first);
        apply_task_event(&mut state, second);

        assert_eq!(state.active_tasks.len(), 2);
    }

    #[test]
    fn same_session_identified_turn_replaces_anonymous_active_task() {
        let mut state = BridgeState::default();
        let anonymous = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "prompt": "anonymous work"
        }));
        let identified = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));

        apply_task_event(&mut state, anonymous);
        apply_task_event(&mut state, identified);

        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(
            state
                .active_tasks
                .values()
                .next()
                .and_then(|event| event.turn_id.as_deref()),
            Some("turn-1")
        );
    }

    #[test]
    fn queues_multiple_session_tasks_without_turn_id() {
        let mut state = BridgeState::default();
        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "prompt": "first"
        }));
        let second = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "prompt": "second"
        }));

        apply_task_event(&mut state, first);
        apply_task_event(&mut state, second);

        assert_eq!(state.active_tasks.len(), 2);
        assert_eq!(
            state
                .active_order
                .iter()
                .map(String::as_str)
                .collect::<Vec<_>>(),
            vec![
                "source:codex:session:session-1:anonymous:1",
                "source:codex:session:session-1:anonymous:2"
            ]
        );
    }

    #[test]
    fn stop_removes_matching_running_task() {
        let mut state = BridgeState::default();
        let start = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "message": "{\"session_id\":\"session-1\",\"turn_id\":\"turn-1\"}"
        }));
        let stop = normalize_event(&json!({
            "hook_event_name": "Stop",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));

        apply_task_event(&mut state, start);
        apply_task_event(&mut state, stop);

        assert_eq!(state.active_tasks.len(), 0);
        assert_eq!(
            state.task.as_ref().map(|event| event.status.as_str()),
            Some("done")
        );
    }

    #[test]
    fn session_only_stop_does_not_complete_identified_turn_task() {
        let mut state = BridgeState::default();
        let start = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1",
            "prompt": "long task"
        }));
        let permission_auto_allow_stop = normalize_event(&json!({
            "hook_event_name": "Stop",
            "session_id": "session-1"
        }));

        apply_task_event(&mut state, start);
        apply_task_event(&mut state, permission_auto_allow_stop);

        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(state.done_tasks.len(), 0);
        assert_eq!(state.unmatched_stops.len(), 1);
        assert_eq!(
            state.task.as_ref().map(|event| event.status.as_str()),
            Some("running")
        );
    }

    #[test]
    fn snapshot_dedupes_existing_duplicate_active_turns() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            insert_active_task(
                &mut state,
                "source:codex:session:session-1:turn:turn-shared".to_string(),
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-shared"
                })),
            );
            insert_active_task(
                &mut state,
                "source:codex:session:session-2:turn:turn-shared".to_string(),
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-2",
                    "turn_id": "turn-shared"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(
            snapshot
                .active_tasks
                .first()
                .and_then(|event| event.session_id.as_deref()),
            Some("session-2")
        );
    }

    #[test]
    fn anonymous_stop_decrements_single_running_task() {
        let mut state = BridgeState::default();
        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "prompt": "anonymous work"
            })),
        );

        apply_task_event(
            &mut state,
            normalize_event(&json!({"hook_event_name": "Stop"})),
        );

        assert_eq!(state.active_tasks.len(), 0);
        assert_eq!(state.done_tasks.len(), 1);
        assert_eq!(state.unmatched_stops.len(), 1);
    }

    #[test]
    fn empty_user_prompt_submit_does_not_start_task() {
        let mut state = BridgeState::default();

        apply_task_event(
            &mut state,
            normalize_event(&json!({"hook_event_name": "UserPromptSubmit"})),
        );

        assert_eq!(state.active_tasks.len(), 0);
        assert_eq!(state.done_tasks.len(), 0);
        assert_eq!(state.unmatched_stops.len(), 0);
        assert!(state.task.is_none());
    }

    #[test]
    fn control_only_hook_payload_does_not_start_task() {
        let mut state = BridgeState::default();

        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": "session-1",
                "message": "{\"exclude\":[]}"
            })),
        );

        assert_eq!(state.active_tasks.len(), 0);
        assert_eq!(state.done_tasks.len(), 0);
        assert_eq!(state.unmatched_stops.len(), 0);
        assert!(state.task.is_none());
    }

    #[test]
    fn control_only_stop_payload_is_not_recorded_as_done() {
        let mut state = BridgeState::default();

        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "Stop",
                "session_id": "session-1",
                "turn_id": "turn-1",
                "message": "{\"exclude\":[]}"
            })),
        );

        assert_eq!(state.done_seq, 0);
        assert_eq!(state.done_tasks.len(), 0);
        assert_eq!(state.unmatched_stops.len(), 0);
        assert!(state.task.is_none());
    }

    #[test]
    fn codex_desktop_history_lifecycle_does_not_flash_task_state() {
        let mut state = BridgeState::default();
        let desktop_cwd =
            "C:\\Program Files\\WindowsApps\\OpenAI.Codex_26.601.2237.0_x64__2p2nqsd0c76g0\\app";

        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": "history-session",
                "turn_id": "history-turn",
                "cwd": desktop_cwd,
                "model": "gpt-5.4-mini",
                "message": "conversation history sync"
            })),
        );
        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "Stop",
                "session_id": "history-session",
                "turn_id": "history-turn",
                "cwd": desktop_cwd,
                "model": "gpt-5.4-mini",
                "message": "conversation history sync complete"
            })),
        );

        assert!(state.active_tasks.is_empty());
        assert_eq!(state.done_seq, 0);
        assert!(state.done_tasks.is_empty());
        assert!(state.unmatched_stops.is_empty());
        assert!(state.task.is_none());
    }

    #[test]
    fn event_journal_restore_filters_codex_desktop_orphan_running_event() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-bridge-desktop-orphan-events-{}.jsonl",
            std::process::id()
        ));
        let _ = fs::remove_file(&path);
        let desktop_event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "desktop-session",
            "turn_id": "desktop-turn",
            "cwd": "C:\\Program Files\\WindowsApps\\OpenAI.Codex_26.609.4994.0_x64__2p2nqsd0c76g0\\app",
            "message": "desktop internal lifecycle"
        }));
        let workspace_event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "workspace-session",
            "turn_id": "workspace-turn",
            "cwd": "D:\\Desktop\\codex\\codex-quota-widget",
            "message": "real work"
        }));

        fs::write(
            &path,
            format!(
                "{}\n{}\n",
                serde_json::to_string(&desktop_event).unwrap(),
                serde_json::to_string(&workspace_event).unwrap()
            ),
        )
        .unwrap();

        let state = bridge_state_from_event_log(&path).unwrap();
        let _ = fs::remove_file(&path);

        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(
            ordered_active_tasks(&state)
                .first()
                .and_then(|event| event.session_id.as_deref()),
            Some("workspace-session")
        );
    }

    #[test]
    fn real_workspace_lifecycle_with_same_identity_still_runs_and_completes() {
        let mut state = BridgeState::default();

        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": "session-1",
                "turn_id": "turn-1",
                "cwd": "D:\\Desktop\\codex\\codex-quota-widget",
                "prompt": "build the bridge"
            })),
        );
        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "Stop",
                "session_id": "session-1",
                "turn_id": "turn-1",
                "cwd": "D:\\Desktop\\codex\\codex-quota-widget",
                "message": "build complete"
            })),
        );

        assert!(state.active_tasks.is_empty());
        assert_eq!(state.done_seq, 1);
        assert_eq!(
            state.done_tasks.back().map(|event| event.message.as_str()),
            Some("build complete")
        );
        assert_eq!(
            state.task.as_ref().map(|event| event.status.as_str()),
            Some("done")
        );
    }

    #[test]
    fn stop_with_no_active_task_does_not_emit_done() {
        let mut state = BridgeState::default();

        apply_task_event(
            &mut state,
            normalize_event(&json!({"hook_event_name": "Stop"})),
        );

        assert_eq!(state.active_tasks.len(), 0);
        assert_eq!(state.done_tasks.len(), 0);
        assert_eq!(state.unmatched_stops.len(), 0);
        assert!(state.task.is_none());
    }

    #[test]
    fn identified_stop_without_active_task_is_unmatched() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "codex-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "foreign-session",
                    "turn_id": "foreign-turn",
                    "cwd": "C:\\Program Files\\WindowsApps\\OpenAI.Codex\\app"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.done_seq, 0);
        assert_eq!(snapshot.done_task_count, 0);
        assert_eq!(snapshot.unmatched_stop_count, 1);
        assert_eq!(snapshot.source_tasks.codex.done_seq, 0);
    }

    #[test]
    fn claude_fallback_start_and_stop_update_claude_summary() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "message": "UserPromptSubmit"
                })),
            );
        }

        let running = task_snapshot(&state, &test_config(None));
        assert_eq!(running.source_tasks.claude.status, "running");
        assert_eq!(running.source_tasks.claude.active_count, 1);
        assert_eq!(running.source_tasks.claude.done_seq, 0);
        assert_eq!(running.source_tasks.codex.active_count, 0);

        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "message": "Stop"
                })),
            );
        }

        let done = task_snapshot(&state, &test_config(None));
        assert_eq!(done.source_tasks.claude.status, "done");
        assert_eq!(done.source_tasks.claude.active_count, 0);
        assert_eq!(done.source_tasks.claude.done_seq, 1);
        assert_eq!(done.source_tasks.codex.active_count, 0);
        assert_eq!(done.source_tasks.codex.done_seq, 0);
        assert_eq!(
            done.source_tasks
                .claude
                .task
                .as_ref()
                .map(|event| event.title.as_str()),
            Some("Claude done")
        );
    }

    #[test]
    fn claude_stop_without_active_task_is_unmatched() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "claude-session"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.done_seq, 0);
        assert_eq!(snapshot.source_tasks.claude.status, "done");
        assert_eq!(snapshot.source_tasks.claude.active_count, 0);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 0);
        assert_eq!(snapshot.source_tasks.codex.done_seq, 0);
        assert_eq!(snapshot.unmatched_stop_count, 1);
    }

    #[test]
    fn session_stop_without_turn_id_removes_recent_session_task() {
        let mut state = BridgeState::default();
        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": "session-1",
                "prompt": "first"
            })),
        );
        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": "session-1",
                "prompt": "second"
            })),
        );

        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "Stop",
                "session_id": "session-1"
            })),
        );

        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(state.done_tasks.len(), 1);
        assert!(state
            .active_tasks
            .values()
            .any(|event| event.message == "first"));
    }

    #[test]
    fn session_stop_without_turn_id_matches_only_same_source() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "shared-session",
                    "prompt": "codex work"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "shared-session",
                    "prompt": "claude work"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "shared-session",
                    "message": "claude done"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.source_tasks.codex.status, "running");
        assert_eq!(snapshot.source_tasks.codex.active_count, 1);
        assert_eq!(snapshot.source_tasks.claude.status, "done");
        assert_eq!(snapshot.source_tasks.claude.active_count, 0);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 1);
        assert_eq!(
            snapshot
                .active_tasks
                .first()
                .map(|event| event.message.as_str()),
            Some("codex work")
        );
    }

    #[test]
    fn source_aware_turn_keys_keep_codex_and_claude_independent() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "shared-session",
                    "turn_id": "turn-1",
                    "prompt": "codex work"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "shared-session",
                    "turn_id": "turn-1",
                    "prompt": "claude work"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "shared-session",
                    "turn_id": "turn-1",
                    "message": "claude done"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.source_tasks.codex.active_count, 1);
        assert_eq!(snapshot.source_tasks.claude.active_count, 0);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 1);
        assert_eq!(
            snapshot
                .source_tasks
                .codex
                .task
                .as_ref()
                .map(|event| event.message.as_str()),
            Some("codex work")
        );
    }

    #[test]
    fn repeated_claude_start_with_same_turn_replaces_previous_active_task_like_codex() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "shared-turn",
                    "prompt": "first"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session-continued",
                    "turn_id": "shared-turn",
                    "prompt": "second"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.source_tasks.claude.status, "running");
        assert_eq!(snapshot.source_tasks.claude.active_count, 1);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 0);
        assert_eq!(
            snapshot
                .source_tasks
                .claude
                .task
                .as_ref()
                .map(|event| event.message.as_str()),
            Some("second")
        );
        assert_eq!(
            snapshot
                .source_tasks
                .claude
                .task
                .as_ref()
                .and_then(|event| event.turn_id.as_deref()),
            Some("shared-turn")
        );
    }

    #[test]
    fn claude_transcript_identity_uses_codex_turn_matching_for_stop_and_continue() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let start = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "claude-session",
            "transcript_path": "C:\\Users\\86147\\.claude\\projects\\demo\\session.jsonl",
            "prompt": "first"
        }));
        let stop = normalize_event(&json!({
            "hook_event_name": "Stop",
            "session_id": "claude-session",
            "transcript_path": "C:\\Users\\86147\\.claude\\projects\\demo\\session.jsonl",
            "message": "stopped"
        }));
        let continued = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "claude-session",
            "transcript_path": "C:\\Users\\86147\\.claude\\projects\\demo\\session.jsonl",
            "prompt": "continued"
        }));

        let derived_turn_id = start.turn_id.as_deref().unwrap_or_default().to_string();
        assert!(derived_turn_id.starts_with("claude-transcript-"));
        assert_eq!(stop.turn_id.as_deref(), Some(derived_turn_id.as_str()));
        assert_eq!(continued.turn_id.as_deref(), Some(derived_turn_id.as_str()));

        {
            let mut state = state.lock().unwrap();
            apply_task_event(&mut state, start);
            apply_task_event(&mut state, stop);
            apply_task_event(&mut state, continued);
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.source_tasks.claude.status, "running");
        assert_eq!(snapshot.source_tasks.claude.active_count, 1);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 1);
        assert_eq!(snapshot.source_tasks.codex.active_count, 0);
        assert_eq!(
            snapshot
                .source_tasks
                .claude
                .task
                .as_ref()
                .map(|event| event.message.as_str()),
            Some("continued")
        );
        assert_eq!(
            snapshot
                .source_tasks
                .claude
                .task
                .as_ref()
                .and_then(|event| event.turn_id.as_deref()),
            Some(derived_turn_id.as_str())
        );
    }

    #[test]
    fn claude_transcript_identity_overrides_mismatched_raw_turn_id() {
        let start = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "source": "Claude",
            "session_id": "claude-session",
            "transcript_path": "C:\\Users\\86147\\.claude\\projects\\demo\\session.jsonl",
            "turn_id": "raw-running-turn"
        }));
        let stop = normalize_event(&json!({
            "hook_event_name": "Stop",
            "source": "Claude",
            "session_id": "claude-session",
            "transcript_path": "\\\\?\\C:\\Users\\86147\\.claude\\projects\\demo\\session.jsonl",
            "turn_id": "raw-stop-turn"
        }));

        assert_eq!(start.turn_id, stop.turn_id);
        assert!(start
            .turn_id
            .as_deref()
            .unwrap_or_default()
            .starts_with("claude-transcript-"));
    }

    #[test]
    fn claude_session_only_stop_does_not_complete_identified_turn_task() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1",
                    "prompt": "first"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "message": "stopped"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.source_tasks.claude.status, "running");
        assert_eq!(snapshot.source_tasks.claude.active_count, 1);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 0);
        assert_eq!(snapshot.unmatched_stop_count, 1);
    }

    #[test]
    fn unmatched_stop_does_not_complete_other_source_task() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "codex-session"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.done_seq, 0);
        assert_eq!(snapshot.unmatched_stop_count, 1);
        assert_eq!(snapshot.source_tasks.claude.status, "running");
        assert_eq!(snapshot.source_tasks.claude.active_count, 1);
        assert_eq!(snapshot.source_tasks.codex.done_seq, 0);
    }

    #[test]
    fn unmatched_stop_does_not_guess_when_multiple_tasks_are_active() {
        let mut state = BridgeState::default();
        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": "session-1",
                "prompt": "first task"
            })),
        );
        apply_task_event(
            &mut state,
            normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": "session-2",
                "prompt": "second task"
            })),
        );

        apply_task_event(
            &mut state,
            normalize_event(&json!({"hook_event_name": "Stop"})),
        );

        assert_eq!(state.active_tasks.len(), 2);
        assert_eq!(state.done_tasks.len(), 0);
        assert_eq!(state.unmatched_stops.len(), 1);
    }

    #[test]
    fn matching_stop_preserves_running_status_when_other_tasks_are_still_active() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-2",
                    "turn_id": "turn-2"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "session-1",
                    "turn_id": "turn-1",
                    "message": "first done"
                })),
            );
        }

        let config = test_config(None);
        let snapshot = task_snapshot(&state, &config);

        assert_eq!(snapshot.status, "running");
        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(
            snapshot
                .task
                .as_ref()
                .and_then(|event| event.session_id.as_deref()),
            Some("session-2")
        );
        assert_eq!(snapshot.done_seq, 1);
        assert_eq!(
            snapshot
                .last_done_task
                .as_ref()
                .map(|event| event.message.as_str()),
            Some("first done")
        );
        assert_eq!(snapshot.done_task_count, 1);
    }

    #[test]
    fn snapshot_combines_recent_claude_and_codex_done_sources() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "codex-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "codex-session",
                    "turn_id": "turn-1"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.status, "done");
        assert_eq!(snapshot.done_seq, 2);
        assert_eq!(snapshot.source_tasks.claude.status, "done");
        assert_eq!(snapshot.source_tasks.codex.status, "done");
        assert_eq!(snapshot.source_tasks.claude.done_seq, 1);
        assert_eq!(snapshot.source_tasks.codex.done_seq, 1);
        assert_eq!(
            snapshot.task.as_ref().map(|event| event.title.as_str()),
            Some("Claude + Codex done")
        );
        assert_eq!(
            snapshot
                .task
                .as_ref()
                .and_then(|event| event.source.as_deref()),
            Some("claude+codex")
        );
    }

    #[test]
    fn snapshot_keeps_source_task_summaries_separate_when_both_are_active() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "codex-session",
                    "turn_id": "turn-1",
                    "prompt": "codex work"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1",
                    "prompt": "claude work"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.status, "running");
        assert_eq!(snapshot.active_task_count, 2);
        assert_eq!(snapshot.source_tasks.codex.status, "running");
        assert_eq!(snapshot.source_tasks.codex.active_count, 1);
        assert_eq!(
            snapshot
                .source_tasks
                .codex
                .task
                .as_ref()
                .map(|event| event.title.as_str()),
            Some("Codex running")
        );
        assert_eq!(snapshot.source_tasks.claude.status, "running");
        assert_eq!(snapshot.source_tasks.claude.active_count, 1);
        assert_eq!(
            snapshot
                .source_tasks
                .claude
                .task
                .as_ref()
                .map(|event| event.title.as_str()),
            Some("Claude running")
        );
    }

    #[test]
    fn snapshot_combines_recent_done_sources_regardless_of_arrival_order() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "codex-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "codex-session",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(
            snapshot.task.as_ref().map(|event| event.title.as_str()),
            Some("Claude + Codex done")
        );
    }

    #[test]
    fn snapshot_keeps_single_done_source_when_previous_source_is_stale() {
        let mut state_value = BridgeState::default();
        remember_done_task(
            &mut state_value,
            TaskEvent {
                kind: "Stop".to_string(),
                status: "done".to_string(),
                title: "Claude done".to_string(),
                message: "Claude done".to_string(),
                received_at: "2026-05-30T00:00:00+08:00".to_string(),
                source: Some("claude".to_string()),
                session_id: Some("claude-session".to_string()),
                turn_id: Some("turn-1".to_string()),
                cwd: None,
                model: None,
            },
        );
        let codex_done = TaskEvent {
            kind: "Stop".to_string(),
            status: "done".to_string(),
            title: "Codex done".to_string(),
            message: "Codex done".to_string(),
            received_at: "2026-05-30T00:00:10+08:00".to_string(),
            source: None,
            session_id: Some("codex-session".to_string()),
            turn_id: Some("turn-1".to_string()),
            cwd: None,
            model: None,
        };
        remember_done_task(&mut state_value, codex_done.clone());
        state_value.task = Some(codex_done);
        let state = Arc::new(Mutex::new(state_value));

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(
            snapshot.task.as_ref().map(|event| event.title.as_str()),
            Some("Codex done")
        );
    }

    #[test]
    fn snapshot_preserves_active_task_order() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "prompt": "first task"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-2",
                    "prompt": "second task"
                })),
            );
        }

        let config = test_config(None);
        let snapshot = task_snapshot(&state, &config);

        assert_eq!(snapshot.status, "running");
        assert_eq!(snapshot.active_tasks.len(), 2);
        assert_eq!(
            snapshot
                .active_tasks
                .iter()
                .filter_map(|event| event.session_id.as_deref())
                .collect::<Vec<_>>(),
            vec!["session-1", "session-2"]
        );
    }

    #[test]
    fn snapshot_drops_completed_active_task_when_session_log_has_terminal_turn() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-stale-terminal-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:02+08:00\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"turn-1\",\"last_agent_message\":\"done\"}}\n"
            ),
        )
        .unwrap();
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-1"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &scoped_test_config("session-1", &codex_home));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
        assert_eq!(snapshot.done_seq, 0);
    }

    #[test]
    fn snapshot_replaces_stale_active_task_when_session_log_moved_to_new_turn() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-stale-active-turn-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        let old_started_at = recent_timestamp(3);
        let old_done_at = recent_timestamp(2);
        let new_started_at = recent_timestamp(1);
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            format!(
                "{{\"timestamp\":\"{old_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-old\"}}}}\n\
                 {{\"timestamp\":\"{old_done_at}\",\"payload\":{{\"type\":\"task_complete\",\"turn_id\":\"turn-old\"}}}}\n\
                 {{\"timestamp\":\"{new_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-new\"}}}}\n"
            ),
        )
        .unwrap();
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-old"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &scoped_test_config("session-1", &codex_home));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "running");
        assert_eq!(
            snapshot
                .active_tasks
                .first()
                .and_then(|event| event.turn_id.as_deref()),
            Some("turn-new")
        );
    }

    #[test]
    fn snapshot_recovers_recent_session_while_current_task_is_active() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-recover-with-active-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        let recovered_at = recent_timestamp(1);
        fs::write(
            session_dir.join("rollout-2026-05-31T20-42-42-11111111-1111-1111-1111-111111111111.jsonl"),
            format!(
                "{{\"timestamp\":\"{recovered_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"recovered-turn\"}}}}\n"
            ),
        )
        .unwrap();
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "22222222-2222-2222-2222-222222222222",
                    "turn_id": "memory-turn"
                })),
            );
        }

        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 2);
        assert_eq!(
            snapshot
                .active_tasks
                .iter()
                .filter_map(|event| event.session_id.as_deref())
                .collect::<Vec<_>>(),
            vec![
                "22222222-2222-2222-2222-222222222222",
                "11111111-1111-1111-1111-111111111111"
            ]
        );
    }

    #[test]
    fn snapshot_counts_current_task_with_recovered_recent_tasks() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-count-current-plus-recovered-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        let recovered_sessions = [
            (
                "11111111-1111-1111-1111-111111111111",
                "recovered-turn-1",
                4,
            ),
            (
                "33333333-3333-3333-3333-333333333333",
                "recovered-turn-2",
                3,
            ),
            (
                "44444444-4444-4444-4444-444444444444",
                "recovered-turn-3",
                2,
            ),
        ];
        for (session_id, turn_id, seconds_ago) in recovered_sessions {
            fs::write(
                session_dir.join(format!("rollout-2026-05-31T20-42-42-{session_id}.jsonl")),
                format!(
                    "{{\"timestamp\":\"{}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"{turn_id}\"}}}}\n",
                    recent_timestamp(seconds_ago)
                ),
            )
            .unwrap();
        }
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "22222222-2222-2222-2222-222222222222",
                    "turn_id": "current-turn"
                })),
            );
        }

        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        let active: HashSet<(Option<String>, Option<String>)> = snapshot
            .active_tasks
            .into_iter()
            .map(|event| (event.session_id, event.turn_id))
            .collect();

        assert_eq!(snapshot.active_task_count, 4);
        assert!(active.contains(&(
            Some("22222222-2222-2222-2222-222222222222".to_string()),
            Some("current-turn".to_string())
        )));
        for (session_id, turn_id, _) in recovered_sessions {
            assert!(active.contains(&(Some(session_id.to_string()), Some(turn_id.to_string()))));
        }
    }

    #[test]
    fn recovered_task_completed_during_cache_ttl_is_not_restored_again() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));
        {
            let mut state = state.lock().unwrap();
            state.active_recovery = Some(CachedActiveRecovery {
                events: vec![event.clone()],
                fetched_at: Instant::now(),
            });
            apply_task_event(&mut state, event);
        }

        let config = test_config(None);
        let first = task_snapshot(&state, &config);
        assert_eq!(first.active_task_count, 1);

        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "session-1",
                    "turn_id": "turn-1"
                })),
            );
        }

        let second = task_snapshot(&state, &config);
        assert_eq!(second.active_task_count, 0);
        assert_eq!(second.done_seq, 1);
        assert_eq!(second.status, "done");
    }

    #[test]
    fn snapshot_keeps_recent_active_task_when_session_file_is_not_written_yet() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-with-delayed-log",
                    "turn_id": "turn-1"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "running");
    }

    #[test]
    fn snapshot_drops_old_active_task_when_session_file_is_missing() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            insert_active_task(
                &mut state,
                "source:codex:session:missing-session:turn:turn-1".to_string(),
                TaskEvent {
                    kind: "UserPromptSubmit".to_string(),
                    status: "running".to_string(),
                    title: "Codex running".to_string(),
                    message: "old missing session".to_string(),
                    received_at: "2026-05-31T00:00:00+08:00".to_string(),
                    source: None,
                    session_id: Some("missing-session".to_string()),
                    turn_id: Some("turn-1".to_string()),
                    cwd: None,
                    model: None,
                },
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
    }

    #[test]
    fn snapshot_keeps_claude_task_when_no_codex_session_file_exists() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "source": "Claude",
                    "session_id": "claude-session",
                    "turn_id": "turn-1"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &test_config(None));

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.source_tasks.claude.active_count, 1);
        assert_eq!(snapshot.source_tasks.claude.status, "running");
    }

    #[test]
    fn snapshot_keeps_child_session_and_drops_forked_parent_session() {
        let codex_home =
            env::temp_dir().join(format!("codex-ornament-fork-active-{}", std::process::id()));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T12-17-11-parent-session.jsonl"),
            "{\"timestamp\":\"2026-05-31T08:00:00+08:00\",\"type\":\"session_meta\",\"payload\":{\"id\":\"parent-session\"}}\n",
        )
        .unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T17-00-13-child-session.jsonl"),
            "{\"timestamp\":\"2026-05-31T17:00:00+08:00\",\"type\":\"session_meta\",\"payload\":{\"id\":\"child-session\",\"forked_from_id\":\"parent-session\"}}\n",
        )
        .unwrap();
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                TaskEvent {
                    kind: "UserPromptSubmit".to_string(),
                    status: "running".to_string(),
                    title: "Codex running".to_string(),
                    message: "parent task".to_string(),
                    received_at: "2026-05-31T12:00:00+08:00".to_string(),
                    source: None,
                    session_id: Some("parent-session".to_string()),
                    turn_id: Some("parent-turn".to_string()),
                    cwd: None,
                    model: None,
                },
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "child-session",
                    "turn_id": "child-turn"
                })),
            );
        }

        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(
            snapshot
                .active_tasks
                .first()
                .and_then(|event| event.session_id.as_deref()),
            Some("child-session")
        );
    }

    #[test]
    fn snapshot_keeps_parent_task_started_after_child_session_fork() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-fork-parent-new-active-{}",
            std::process::id()
        ));
        let parent_started_at = recent_timestamp(2);
        let child_forked_at = recent_timestamp(4);
        let child_started_at = recent_timestamp(1);
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T12-17-11-11111111-1111-1111-1111-111111111111.jsonl"),
            format!(
                "{{\"timestamp\":\"{child_forked_at}\",\"type\":\"session_meta\",\"payload\":{{\"id\":\"11111111-1111-1111-1111-111111111111\"}}}}\n\
                 {{\"timestamp\":\"{parent_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"parent-new-turn\"}}}}\n"
            ),
        )
        .unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T17-00-13-22222222-2222-2222-2222-222222222222.jsonl"),
            format!(
                "{{\"timestamp\":\"{child_forked_at}\",\"type\":\"session_meta\",\"payload\":{{\"id\":\"22222222-2222-2222-2222-222222222222\",\"forked_from_id\":\"11111111-1111-1111-1111-111111111111\",\"timestamp\":\"{child_forked_at}\"}}}}\n\
                 {{\"timestamp\":\"{child_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"child-turn\"}}}}\n"
            ),
        )
        .unwrap();
        let state = Arc::new(Mutex::new(BridgeState::default()));

        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 2);
        assert_eq!(
            snapshot
                .active_tasks
                .iter()
                .filter_map(|event| event.session_id.as_deref())
                .collect::<Vec<_>>(),
            vec![
                "22222222-2222-2222-2222-222222222222",
                "11111111-1111-1111-1111-111111111111"
            ]
        );
    }

    #[test]
    fn session_log_parser_keeps_multiple_unfinished_turns() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-multiple-active-{}.jsonl",
            std::process::id()
        ));
        fs::write(
            &path,
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:01+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-2\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:02+08:00\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:03+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-3\"}}\n"
            ),
        )
        .unwrap();

        let active = active_tasks_in_session_file(&path, "session-1").unwrap();
        let _ = fs::remove_file(&path);

        assert_eq!(
            active
                .iter()
                .filter_map(|event| event.turn_id.as_deref())
                .collect::<Vec<_>>(),
            vec!["turn-2", "turn-3"]
        );
    }

    #[test]
    fn session_log_parser_closes_latest_turn_on_final_answer() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-final-answer-active-{}.jsonl",
            std::process::id()
        ));
        fs::write(
            &path,
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:01+08:00\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"turn one done\"}]}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:02+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-2\"}}\n"
            ),
        )
        .unwrap();

        let active = active_tasks_in_session_file(&path, "session-1").unwrap();
        let terminal = terminal_turn_in_file(&path, "turn-1").unwrap().unwrap();
        let _ = fs::remove_file(&path);

        assert_eq!(
            active
                .iter()
                .filter_map(|event| event.turn_id.as_deref())
                .collect::<Vec<_>>(),
            vec!["turn-2"]
        );
        assert_eq!(terminal.kind, "task_complete");
        assert_eq!(terminal.message.as_deref(), Some("turn one done"));
    }

    #[test]
    fn session_log_parser_keeps_turn_running_when_final_answer_is_followed_by_same_turn_context() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-final-answer-continued-{}.jsonl",
            std::process::id()
        ));
        fs::write(
            &path,
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:01+08:00\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"turn one provisional done\"}]}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:02+08:00\",\"payload\":{\"type\":\"turn_context\",\"turn_id\":\"turn-1\",\"cwd\":\"D:\\\\Desktop\\\\codex\",\"model\":\"gpt-5\"}}\n"
            ),
        )
        .unwrap();

        let active = active_tasks_in_session_file(&path, "session-1").unwrap();
        let terminal = terminal_turn_in_file(&path, "turn-1").unwrap();
        let _ = fs::remove_file(&path);

        assert_eq!(
            active
                .iter()
                .filter_map(|event| event.turn_id.as_deref())
                .collect::<Vec<_>>(),
            vec!["turn-1"]
        );
        assert_eq!(
            active.first().and_then(|event| event.model.as_deref()),
            Some("gpt-5")
        );
        assert!(terminal.is_none());
    }

    #[test]
    fn recent_recovery_ignores_orphaned_turn_when_newer_turn_started() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-orphan-newer-turn-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        let old_started_at = recent_timestamp(4);
        let new_started_at = recent_timestamp(2);
        let new_done_at = recent_timestamp(1);
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-019e7467-fc36-7750-8418-f2bf0397bd05.jsonl"),
            format!(
                "{{\"timestamp\":\"{old_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-orphan\"}}}}\n\
                 {{\"timestamp\":\"{new_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-new\"}}}}\n\
                 {{\"timestamp\":\"{new_done_at}\",\"payload\":{{\"type\":\"task_complete\",\"turn_id\":\"turn-new\"}}}}\n"
            ),
        )
        .unwrap();

        let active = active_tasks_in_recent_session_files(&codex_home);
        let _ = fs::remove_dir_all(&codex_home);

        assert!(active.is_empty(), "{active:?}");
    }

    #[test]
    fn snapshot_drops_orphaned_turn_after_final_answer_without_hook_complete() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-orphan-final-answer-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:10+08:00\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"firmware flashed\"}]}}\n"
            ),
        )
        .unwrap();
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-1"
                })),
            );
        }

        let snapshot = task_snapshot(&state, &scoped_test_config("session-1", &codex_home));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
        assert_eq!(snapshot.done_seq, 0);
    }

    #[test]
    fn snapshot_restores_running_after_compaction_continues_same_turn() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-compaction-continues-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            concat!(
                "{\"timestamp\":\"2026-05-31T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:10+08:00\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"phase\":\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"intermediate final answer\"}]}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:11+08:00\",\"type\":\"event_msg\",\"payload\":{\"type\":\"context_compacted\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-31T12:00:12+08:00\",\"payload\":{\"type\":\"turn_context\",\"turn_id\":\"turn-1\",\"cwd\":\"D:\\\\Desktop\\\\codex\",\"model\":\"gpt-5\"}}\n"
            ),
        )
        .unwrap();
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-1"
                })),
            );
            remember_done_task(
                &mut state,
                TaskEvent {
                    kind: "Stop".to_string(),
                    status: "done".to_string(),
                    title: "Codex done".to_string(),
                    message: "stale done".to_string(),
                    received_at: "2026-05-31T12:00:10+08:00".to_string(),
                    source: None,
                    session_id: Some("session-1".to_string()),
                    turn_id: Some("turn-1".to_string()),
                    cwd: None,
                    model: None,
                },
            );
            state.task = state.done_tasks.back().cloned();
        }

        let snapshot = task_snapshot(&state, &scoped_test_config("session-1", &codex_home));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "running");
        assert_eq!(
            snapshot.task.as_ref().map(|event| event.status.as_str()),
            Some("running")
        );
        assert_eq!(
            snapshot
                .task
                .as_ref()
                .and_then(|event| event.turn_id.as_deref()),
            Some("turn-1")
        );
    }

    #[test]
    fn final_answer_reconcile_preserves_other_running_sessions() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-orphan-concurrent-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        let first_started_at = recent_timestamp(4);
        let second_started_at = recent_timestamp(3);
        let first_done_at = recent_timestamp(2);
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            format!(
                "{{\"timestamp\":\"{first_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}}}\n\
                 {{\"timestamp\":\"{first_done_at}\",\"type\":\"response_item\",\"payload\":{{\"type\":\"message\",\"phase\":\"final_answer\",\"content\":[{{\"type\":\"output_text\",\"text\":\"first done\"}}]}}}}\n"
            ),
        )
        .unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-10-session-2.jsonl"),
            format!(
                "{{\"timestamp\":\"{second_started_at}\",\"payload\":{{\"type\":\"task_started\",\"turn_id\":\"turn-2\"}}}}\n"
            ),
        )
        .unwrap();
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-1"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-2",
                    "turn_id": "turn-2"
                })),
            );
        }

        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(
            snapshot
                .active_tasks
                .first()
                .and_then(|event| event.session_id.as_deref()),
            Some("session-2")
        );
        assert_eq!(snapshot.status, "running");
        assert_eq!(snapshot.done_seq, 1);
        assert_eq!(
            snapshot
                .last_done_task
                .as_ref()
                .map(|event| event.message.as_str()),
            Some("first done")
        );
    }

    #[test]
    fn scoped_bridge_accepts_only_tracked_session_events() {
        let config = scoped_test_config("session-1", ".codex-test");
        let matching = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1"
        }));
        let other = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2"
        }));
        let missing = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit"
        }));

        assert!(event_in_scope(&matching, &config));
        assert!(!event_in_scope(&other, &config));
        assert!(!event_in_scope(&missing, &config));
    }

    #[test]
    fn unscoped_bridge_accepts_events_from_any_session() {
        let config = test_config(None);
        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2"
        }));

        assert!(event_in_scope(&event, &config));
    }

    #[test]
    fn bridge_info_reports_event_journal_status() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-bridge-info-events-{}.jsonl",
            std::process::id()
        ));
        fs::write(&path, b"{}\n").unwrap();

        let mut config = test_config(None);
        config.event_log_path = Some(path.clone());

        let info = bridge_info(&config);

        assert!(info.event_log_enabled);
        assert_eq!(info.event_log_path.as_deref(), Some(path.to_str().unwrap()));
        assert_eq!(info.event_log_bytes, Some(3));

        let _ = fs::remove_file(&path);
    }

    #[test]
    fn ready_info_reports_event_journal_writability() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-bridge-ready-events-{}.jsonl",
            std::process::id()
        ));
        let _ = fs::remove_file(&path);

        let mut config = test_config(None);
        config.event_log_path = Some(path.clone());

        let ready = ready_info(&config);

        assert!(ready.ok);
        assert!(ready.event_log_enabled);
        assert!(ready.event_log_writable);
        assert_eq!(
            ready.event_log_path.as_deref(),
            Some(path.to_str().unwrap())
        );
        assert_eq!(ready.event_log_error, None);

        let _ = fs::remove_file(&path);
    }

    #[test]
    fn ready_info_allows_disabled_event_journal() {
        let config = test_config(None);

        let ready = ready_info(&config);

        assert!(ready.ok);
        assert!(!ready.event_log_enabled);
        assert!(ready.event_log_writable);
        assert_eq!(ready.event_log_path, None);
    }

    #[test]
    fn bridge_metrics_reports_readiness_and_bounded_state() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-bridge-metrics-events-{}.jsonl",
            std::process::id()
        ));
        fs::write(&path, b"{}\n").unwrap();

        let mut config = test_config(None);
        config.event_log_path = Some(path.clone());
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-1",
                    "prompt": "build"
                })),
            );
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "session-1",
                    "turn_id": "turn-1",
                    "message": "done"
                })),
            );
        }

        let metrics = bridge_metrics(&state, &config);

        assert!(metrics.contains("codex_ornament_bridge_ready 1\n"));
        assert!(metrics.contains("codex_ornament_bridge_event_log_writable 1\n"));
        assert!(metrics.contains("codex_ornament_bridge_event_log_bytes 3\n"));
        assert!(metrics.contains("codex_ornament_bridge_active_tasks 0\n"));
        assert!(metrics.contains("codex_ornament_bridge_done_seq{source=\"all\"} 1\n"));
        assert!(metrics.contains("codex_ornament_bridge_done_seq{source=\"codex\"} 1\n"));
        assert!(metrics.contains("codex_ornament_bridge_done_tasks 1\n"));

        let _ = fs::remove_file(&path);
    }

    #[test]
    fn event_journal_restores_applied_task_state() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-bridge-events-{}.jsonl",
            std::process::id()
        ));
        let _ = fs::remove_file(&path);

        {
            let mut journal = TaskEventJournal::open(
                Some(&path),
                DEFAULT_EVENT_LOG_MAX_BYTES,
                DEFAULT_EVENT_LOG_COMPACT_KEEP_EVENTS,
            )
            .unwrap();
            journal
                .append(&normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-1",
                    "prompt": "first"
                })))
                .unwrap();
            journal
                .append(&normalize_event(&json!({
                    "hook_event_name": "Stop",
                    "session_id": "session-1",
                    "turn_id": "turn-1",
                    "message": "done"
                })))
                .unwrap();
            journal
                .append(&normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-2",
                    "turn_id": "turn-2",
                    "prompt": "second"
                })))
                .unwrap();
        }

        let state = bridge_state_from_event_log(&path).unwrap();
        assert_eq!(state.done_seq, 1);
        assert_eq!(state.done_tasks.len(), 1);
        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(
            ordered_active_tasks(&state)
                .first()
                .and_then(|event| event.session_id.as_deref()),
            Some("session-2")
        );

        let _ = fs::remove_file(&path);
    }

    #[test]
    fn event_journal_compacts_to_recent_valid_events() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-bridge-compact-events-{}.jsonl",
            std::process::id()
        ));
        let _ = fs::remove_file(&path);

        {
            let mut journal = TaskEventJournal::open(Some(&path), 1, 3).unwrap();
            for index in 0..6 {
                journal
                    .append(&normalize_event(&json!({
                        "hook_event_name": "UserPromptSubmit",
                        "session_id": format!("session-{index}"),
                        "turn_id": format!("turn-{index}"),
                        "prompt": format!("task {index}")
                    })))
                    .unwrap();
            }
        }

        let lines = fs::read_to_string(&path).unwrap();
        assert_eq!(lines.lines().count(), 3);
        assert!(!lines.contains("session-2"));
        assert!(lines.contains("session-3"));
        assert!(lines.contains("session-5"));

        let state = bridge_state_from_event_log(&path).unwrap();
        assert_eq!(state.active_tasks.len(), 3);

        let _ = fs::remove_file(&path);
    }

    #[test]
    fn task_consumer_appends_only_applied_events_to_journal() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-bridge-consumer-events-{}.jsonl",
            std::process::id()
        ));
        let _ = fs::remove_file(&path);
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = scoped_test_config("session-1", ".codex-test");
        let mut journal = TaskEventJournal::open(
            Some(&path),
            DEFAULT_EVENT_LOG_MAX_BYTES,
            DEFAULT_EVENT_LOG_COMPACT_KEEP_EVENTS,
        )
        .unwrap();

        let applied = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));
        let filtered = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-2"
        }));

        assert_eq!(
            consume_task_event(&state, &config, Some(&mut journal), applied),
            TaskDispatchResult::Applied
        );
        assert_eq!(
            consume_task_event(&state, &config, Some(&mut journal), filtered),
            TaskDispatchResult::Filtered
        );
        drop(journal);

        let lines = fs::read_to_string(&path).unwrap();
        assert_eq!(lines.lines().count(), 1);
        assert!(lines.contains("\"sessionId\":\"session-1\""));
        assert!(!lines.contains("session-2"));

        let _ = fs::remove_file(&path);
    }

    #[test]
    fn task_consumer_applies_queued_events_in_order() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, None, receiver);

        let first = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));
        let second = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-2"
        }));

        assert_eq!(
            dispatch_task_event(&sender, first),
            Ok(TaskDispatchResult::Applied)
        );
        assert_eq!(
            dispatch_task_event(&sender, second),
            Ok(TaskDispatchResult::Applied)
        );

        let state = state.lock().unwrap();
        assert_eq!(state.active_tasks.len(), 2);
        assert_eq!(
            state
                .active_order
                .iter()
                .map(String::as_str)
                .collect::<Vec<_>>(),
            vec![
                "source:codex:session:session-1:turn:turn-1",
                "source:codex:session:session-2:turn:turn-2"
            ]
        );
    }

    #[test]
    fn cloned_task_producers_can_submit_concurrently() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, None, receiver);

        let producers = (0..8)
            .map(|index| {
                let sender = sender.clone();
                std::thread::spawn(move || {
                    let event = normalize_event(&json!({
                        "hook_event_name": "UserPromptSubmit",
                        "session_id": format!("session-{index}"),
                        "turn_id": format!("turn-{index}")
                    }));
                    dispatch_task_event(&sender, event)
                })
            })
            .collect::<Vec<_>>();

        for producer in producers {
            assert_eq!(producer.join().unwrap(), Ok(TaskDispatchResult::Applied));
        }

        let state = state.lock().unwrap();
        assert_eq!(state.active_tasks.len(), 8);
        for index in 0..8 {
            assert!(state.active_tasks.contains_key(&format!(
                "source:codex:session:session-{index}:turn:turn-{index}"
            )));
        }
    }

    #[test]
    fn concurrent_task_completions_are_applied_without_losing_done_events() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, None, receiver);

        for index in 0..8 {
            let event = normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "session_id": format!("session-{index}"),
                "turn_id": format!("turn-{index}")
            }));
            assert_eq!(
                dispatch_task_event(&sender, event),
                Ok(TaskDispatchResult::Applied)
            );
        }

        let producers = (0..8)
            .map(|index| {
                let sender = sender.clone();
                std::thread::spawn(move || {
                    let event = normalize_event(&json!({
                        "hook_event_name": "Stop",
                        "session_id": format!("session-{index}"),
                        "turn_id": format!("turn-{index}"),
                        "message": format!("session-{index} done")
                    }));
                    dispatch_task_event(&sender, event)
                })
            })
            .collect::<Vec<_>>();

        for producer in producers {
            assert_eq!(producer.join().unwrap(), Ok(TaskDispatchResult::Applied));
        }

        let state = state.lock().unwrap();
        assert!(state.active_tasks.is_empty());
        assert_eq!(state.done_tasks.len(), 8);
        assert_eq!(state.done_seq, 8);
        for index in 0..8 {
            assert!(state.done_tasks.iter().any(|event| {
                event.session_id.as_deref() == Some(&format!("session-{index}"))
                    && event.message == format!("session-{index} done")
            }));
        }
    }

    #[test]
    fn concurrent_mixed_source_tasks_complete_independently() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, None, receiver);

        let starts = (0..6)
            .map(|index| {
                let sender = sender.clone();
                std::thread::spawn(move || {
                    let mut payload = json!({
                        "hook_event_name": "UserPromptSubmit",
                        "session_id": format!("shared-session-{index}"),
                        "turn_id": format!("turn-{index}"),
                        "prompt": format!("task-{index}")
                    });
                    if index % 2 == 1 {
                        payload["source"] = json!("Claude");
                    }
                    dispatch_task_event(&sender, normalize_event(&payload))
                })
            })
            .collect::<Vec<_>>();

        for start in starts {
            assert_eq!(start.join().unwrap(), Ok(TaskDispatchResult::Applied));
        }

        {
            let snapshot = task_snapshot(&state, &test_config(None));
            assert_eq!(snapshot.active_task_count, 6);
            assert_eq!(snapshot.source_tasks.codex.active_count, 3);
            assert_eq!(snapshot.source_tasks.claude.active_count, 3);
            assert_eq!(snapshot.source_tasks.codex.done_seq, 0);
            assert_eq!(snapshot.source_tasks.claude.done_seq, 0);
        }

        let stops = (0..6)
            .map(|index| {
                let sender = sender.clone();
                std::thread::spawn(move || {
                    let mut payload = json!({
                        "hook_event_name": "Stop",
                        "session_id": format!("shared-session-{index}"),
                        "turn_id": format!("turn-{index}"),
                        "message": format!("task-{index} done")
                    });
                    if index % 2 == 1 {
                        payload["source"] = json!("Claude");
                    }
                    dispatch_task_event(&sender, normalize_event(&payload))
                })
            })
            .collect::<Vec<_>>();

        for stop in stops {
            assert_eq!(stop.join().unwrap(), Ok(TaskDispatchResult::Applied));
        }

        let snapshot = task_snapshot(&state, &test_config(None));
        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.done_seq, 6);
        assert_eq!(snapshot.source_tasks.codex.active_count, 0);
        assert_eq!(snapshot.source_tasks.codex.done_seq, 3);
        assert_eq!(snapshot.source_tasks.claude.active_count, 0);
        assert_eq!(snapshot.source_tasks.claude.done_seq, 3);
    }

    #[test]
    fn task_dispatch_waits_until_consumer_updates_state() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, None, receiver);

        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-1",
            "turn_id": "turn-1"
        }));

        assert_eq!(
            dispatch_task_event(&sender, event),
            Ok(TaskDispatchResult::Applied)
        );

        let state = state.lock().unwrap();
        assert_eq!(state.active_tasks.len(), 1);
        assert_eq!(
            state.task.as_ref().map(|event| event.status.as_str()),
            Some("running")
        );
    }

    #[test]
    fn task_consumer_filters_events_outside_tracked_session() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = scoped_test_config("session-1", ".codex-test");
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, None, receiver);

        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "session-2",
            "turn_id": "turn-1"
        }));

        assert_eq!(
            dispatch_task_event(&sender, event),
            Ok(TaskDispatchResult::Filtered)
        );

        let state = state.lock().unwrap();
        assert!(state.active_tasks.is_empty());
        assert!(state.task.is_none());
    }

    #[test]
    fn task_consumer_filters_codex_desktop_history_lifecycle_events() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = test_config(None);
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        spawn_task_event_consumer(Arc::clone(&state), config, None, receiver);
        let desktop_cwd =
            "C:\\Program Files\\WindowsApps\\OpenAI.Codex_26.601.2237.0_x64__2p2nqsd0c76g0\\app";

        let running = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "session_id": "history-session",
            "turn_id": "history-turn",
            "cwd": desktop_cwd,
            "message": "{\"exclude\":[]}"
        }));
        let done = normalize_event(&json!({
            "hook_event_name": "Stop",
            "session_id": "history-session",
            "turn_id": "history-turn",
            "cwd": desktop_cwd,
            "message": "{\"exclude\":[]}"
        }));

        assert_eq!(
            dispatch_task_event(&sender, running),
            Ok(TaskDispatchResult::Filtered)
        );
        assert_eq!(
            dispatch_task_event(&sender, done),
            Ok(TaskDispatchResult::Filtered)
        );

        let state = state.lock().unwrap();
        assert!(state.active_tasks.is_empty());
        assert!(state.done_tasks.is_empty());
        assert!(state.unmatched_stops.is_empty());
        assert!(state.task.is_none());
    }

    #[test]
    fn dispatch_reports_full_queue_without_blocking() {
        let (sender, _receiver) = mpsc::sync_channel(1);
        let event = normalize_event(&json!({"hook_event_name": "UserPromptSubmit"}));
        let (completion, _completed) = mpsc::sync_channel(0);
        sender
            .try_send(QueuedTaskEvent {
                event: event.clone(),
                completion,
            })
            .unwrap();

        assert_eq!(
            dispatch_task_event(&sender, event),
            Err(TaskQueueError::Full)
        );
    }

    #[test]
    fn dispatch_reports_closed_consumer() {
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        drop(receiver);

        let event = normalize_event(&json!({"hook_event_name": "UserPromptSubmit"}));

        assert_eq!(
            dispatch_task_event(&sender, event),
            Err(TaskQueueError::Closed)
        );
    }

    #[test]
    fn dispatch_reports_completion_drop_when_consumer_exits_without_ack() {
        let (sender, receiver) = mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        let consumer = std::thread::spawn(move || {
            let _ = receiver.recv();
        });

        let event = normalize_event(&json!({"hook_event_name": "UserPromptSubmit"}));

        assert_eq!(
            dispatch_task_event(&sender, event),
            Err(TaskQueueError::CompletionDropped)
        );
        consumer.join().unwrap();
    }

    #[test]
    fn consumer_reports_lock_unavailable_when_state_lock_is_poisoned() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        let poisoned_state = Arc::clone(&state);
        let _ = std::thread::spawn(move || {
            let _guard = poisoned_state.lock().unwrap();
            panic!("poison task state lock for test");
        })
        .join();

        let event = normalize_event(&json!({
            "hook_event_name": "UserPromptSubmit",
            "turn_id": "turn-1",
            "prompt": "real work"
        }));

        assert_eq!(
            consume_task_event(&state, &test_config(None), None, event),
            TaskDispatchResult::LockUnavailable
        );
    }

    #[test]
    fn dispatch_reports_queued_when_consumer_ack_is_slow() {
        let (sender, receiver): (TaskEventSender, TaskEventReceiver) =
            mpsc::sync_channel(TASK_EVENT_QUEUE_CAPACITY);
        let consumer = std::thread::spawn(move || {
            let queued = receiver.recv().unwrap();
            std::thread::sleep(TASK_EVENT_ACK_TIMEOUT + Duration::from_millis(50));
            let _ = queued.completion.send(TaskDispatchResult::Applied);
        });

        let event = normalize_event(&json!({"hook_event_name": "UserPromptSubmit"}));

        assert_eq!(
            dispatch_task_event(&sender, event),
            Ok(TaskDispatchResult::Queued)
        );
        consumer.join().unwrap();
    }

    #[test]
    fn terminal_turn_in_file_detects_completed_turn() {
        let path = env::temp_dir().join(format!(
            "codex-ornament-terminal-turn-{}-complete.jsonl",
            std::process::id()
        ));
        fs::write(
            &path,
            concat!(
                "{\"timestamp\":\"2026-05-25T12:00:00+08:00\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"turn-1\",\"last_agent_message\":\"finished\"}}\n",
                "{\"timestamp\":\"2026-05-25T12:00:01+08:00\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"turn-2\",\"last_agent_message\":\"other\"}}\n"
            ),
        )
        .unwrap();

        let terminal = terminal_turn_in_file(&path, "turn-1").unwrap().unwrap();
        let _ = fs::remove_file(&path);

        assert_eq!(terminal.kind, "task_complete");
        assert_eq!(terminal.message.as_deref(), Some("finished"));
    }

    #[test]
    fn reconcile_clears_aborted_turn_from_session_log() {
        let codex_home =
            env::temp_dir().join(format!("codex-ornament-reconcile-{}", std::process::id()));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("25");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-25T13-36-13-session-1.jsonl"),
            "{\"timestamp\":\"2026-05-25T12:00:00+08:00\",\"payload\":{\"type\":\"turn_aborted\",\"turn_id\":\"turn-1\",\"reason\":\"interrupted\"}}\n",
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-1",
                    "turn_id": "turn-1"
                })),
            );
        }

        let config = scoped_test_config("session-1", &codex_home);
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
    }

    #[test]
    fn recovers_active_turn_from_session_log_after_bridge_restart() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-recover-active-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("25");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-25T13-36-13-session-1.jsonl"),
            format!(
                "{}\n{}\n",
                json!({
                    "timestamp": recent_timestamp(2),
                    "payload": {
                        "type": "task_started",
                        "turn_id": "turn-1"
                    }
                }),
                json!({
                    "timestamp": recent_timestamp(1),
                    "payload": {
                        "type": "turn_context",
                        "turn_id": "turn-1",
                        "cwd": "D:\\Desktop\\codex",
                        "model": "gpt-5.5"
                    }
                })
            ),
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = scoped_test_config("session-1", &codex_home);
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "running");
        let task = snapshot.task.unwrap();
        assert_eq!(task.turn_id.as_deref(), Some("turn-1"));
        assert_eq!(task.cwd.as_deref(), Some("D:\\Desktop\\codex"));
        assert_eq!(task.model.as_deref(), Some("gpt-5.5"));
    }

    #[test]
    fn finds_bridge_restart_root_from_target_debug_exe_path() {
        let root = env::temp_dir().join(format!(
            "codex-ornament-restart-root-{}",
            std::process::id()
        ));
        let script_dir = root.join("scripts");
        let exe_dir = root.join("target").join("debug");
        fs::create_dir_all(&script_dir).unwrap();
        fs::create_dir_all(&exe_dir).unwrap();
        fs::write(
            script_dir.join("start-codex-ornament-bridge.ps1"),
            "Write-Output 'ok'",
        )
        .unwrap();

        let exe = exe_dir.join("codex-ornament-bridge.exe");
        let found = find_bridge_restart_root(&exe);
        let _ = fs::remove_dir_all(&root);

        assert_eq!(found.as_deref(), Some(root.as_path()));
    }

    #[test]
    fn recovers_active_turn_from_recent_session_log_without_tracked_session() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-recover-recent-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();
        let session_file = session_dir
            .join("rollout-2026-05-31T11-43-03-019e7467-fc36-7750-8418-f2bf0397bd05.jsonl");
        fs::write(
            &session_file,
            format!(
                "{}\n{}\n",
                json!({
                    "timestamp": now_local(),
                    "type": "event_msg",
                    "payload": {
                        "type": "task_started",
                        "turn_id": "turn-1"
                    }
                }),
                json!({
                    "timestamp": now_local(),
                    "type": "turn_context",
                    "payload": {
                        "turn_id": "turn-1",
                        "cwd": "D:\\Desktop\\codex",
                        "model": "gpt-5.5"
                    }
                })
            ),
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "running");
        let task = snapshot.task.unwrap();
        assert_eq!(
            task.session_id.as_deref(),
            Some("019e7467-fc36-7750-8418-f2bf0397bd05")
        );
        assert_eq!(task.turn_id.as_deref(), Some("turn-1"));
        assert_eq!(task.cwd.as_deref(), Some("D:\\Desktop\\codex"));
        assert_eq!(task.model.as_deref(), Some("gpt-5.5"));
    }

    #[test]
    fn recovers_multiple_recent_turns_without_tracked_session() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-recover-multi-recent-{}",
            std::process::id()
        ));
        let older_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("30");
        let newer_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&older_dir).unwrap();
        fs::create_dir_all(&newer_dir).unwrap();

        let older_session_id = "11111111-1111-1111-1111-111111111111";
        let newer_session_id = "22222222-2222-2222-2222-222222222222";
        let older_path = older_dir.join(format!(
            "rollout-2026-05-30T11-43-03-{older_session_id}.jsonl"
        ));
        let newer_path = newer_dir.join(format!(
            "rollout-2026-05-31T11-43-03-{newer_session_id}.jsonl"
        ));
        fs::write(
            &older_path,
            format!(
                "{}\n{}\n",
                json!({
                    "timestamp": recent_timestamp(4),
                    "type": "event_msg",
                    "payload": {
                        "type": "task_started",
                        "turn_id": "turn-old"
                    }
                }),
                json!({
                    "timestamp": recent_timestamp(4),
                    "type": "turn_context",
                    "payload": {
                        "turn_id": "turn-old",
                        "cwd": "D:\\Desktop\\codex\\old",
                        "model": "gpt-5.5"
                    }
                })
            ),
        )
        .unwrap();
        fs::write(
            &newer_path,
            format!(
                "{}\n{}\n",
                json!({
                    "timestamp": recent_timestamp(1),
                    "type": "event_msg",
                    "payload": {
                        "type": "task_started",
                        "turn_id": "turn-new"
                    }
                }),
                json!({
                    "timestamp": recent_timestamp(1),
                    "type": "turn_context",
                    "payload": {
                        "turn_id": "turn-new",
                        "cwd": "D:\\Desktop\\codex\\new",
                        "model": "gpt-5.5"
                    }
                })
            ),
        )
        .unwrap();

        let active = active_tasks_in_recent_session_files(&codex_home);
        let _ = fs::remove_dir_all(&codex_home);

        let recovered: HashSet<(Option<String>, Option<String>)> = active
            .into_iter()
            .map(|event| (event.session_id, event.turn_id))
            .collect();

        assert_eq!(recovered.len(), 2, "{recovered:?}");
        assert!(recovered.contains(&(
            Some(older_session_id.to_string()),
            Some("turn-old".to_string())
        )));
        assert!(recovered.contains(&(
            Some(newer_session_id.to_string()),
            Some("turn-new".to_string())
        )));
    }

    #[test]
    fn snapshot_drops_claude_task_after_stop_hook_summary() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-claude-stop-hook-{}",
            std::process::id()
        ));
        let claude_dir = codex_home
            .parent()
            .unwrap()
            .join(".claude")
            .join("projects")
            .join("D--Desktop");
        fs::create_dir_all(&claude_dir).unwrap();

        let session_id = "claude-session-1";
        let running_at = recent_timestamp(3);
        let stopped_at = recent_timestamp(2);
        fs::write(
            claude_dir.join(format!("{session_id}.jsonl")),
            format!(
                "{}\n{}\n",
                json!({
                    "timestamp": running_at,
                    "type": "assistant",
                    "message": { "role": "assistant" }
                }),
                json!({
                    "timestamp": stopped_at,
                    "type": "system",
                    "subtype": "stop_hook_summary"
                })
            ),
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            let mut event = normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "source": "claude",
                "session_id": session_id,
                "transcript_path": format!("C:\\Users\\tester\\.claude\\projects\\D--Desktop\\{session_id}.jsonl"),
            }));
            event.received_at = running_at;
            apply_task_event(&mut state, event);
        }

        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(codex_home.parent().unwrap().join(".claude"));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
    }

    #[test]
    fn snapshot_keeps_claude_task_when_stop_hook_summary_is_older_than_running_event() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-claude-resume-after-stop-{}",
            std::process::id()
        ));
        let claude_dir = codex_home
            .parent()
            .unwrap()
            .join(".claude")
            .join("projects")
            .join("D--Desktop");
        fs::create_dir_all(&claude_dir).unwrap();

        let session_id = "claude-session-resumed";
        let initial_reply_at = recent_timestamp(4);
        let stopped_at = recent_timestamp(3);
        let resumed_at = recent_timestamp(1);
        fs::write(
            claude_dir.join(format!("{session_id}.jsonl")),
            format!(
                "{}\n{}\n{}\n{}\n",
                json!({
                    "timestamp": initial_reply_at,
                    "type": "assistant",
                    "message": { "role": "assistant" }
                }),
                json!({
                    "timestamp": stopped_at,
                    "type": "system",
                    "subtype": "stop_hook_summary"
                }),
                json!({
                    "timestamp": resumed_at,
                    "type": "system",
                    "subtype": "local_command",
                    "content": "<command-name>/resume</command-name>"
                }),
                json!({
                    "timestamp": resumed_at,
                    "type": "user",
                    "message": { "role": "user", "content": "resume" }
                })
            ),
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            let mut event = normalize_event(&json!({
                "hook_event_name": "UserPromptSubmit",
                "source": "claude",
                "session_id": session_id,
                "transcript_path": format!("C:\\Users\\tester\\.claude\\projects\\D--Desktop\\{session_id}.jsonl"),
            }));
            event.received_at = resumed_at;
            apply_task_event(&mut state, event);
        }

        let mut config = test_config(None);
        config.codex_home = codex_home.clone();
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(codex_home.parent().unwrap().join(".claude"));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "running");
        assert_eq!(snapshot.source_tasks.claude.active_count, 1);
        assert_eq!(snapshot.source_tasks.claude.status, "running");
    }

    #[test]
    fn snapshot_drops_idle_codex_started_only_turn() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-idle-started-only-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();

        let old_timestamp = past_timestamp((ACTIVE_SESSION_IDLE_STALE_GRACE.as_secs() as i64) + 60);
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            format!(
                "{}\n{}\n",
                json!({
                    "timestamp": old_timestamp,
                    "payload": {
                        "type": "task_started",
                        "turn_id": "turn-1"
                    }
                }),
                json!({
                    "timestamp": old_timestamp,
                    "payload": {
                        "type": "turn_context",
                        "turn_id": "turn-1",
                        "cwd": "D:\\Desktop\\codex\\firmware",
                        "model": "gpt-5.5"
                    }
                })
            ),
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                TaskEvent {
                    kind: "UserPromptSubmit".to_string(),
                    status: "running".to_string(),
                    title: "Codex running".to_string(),
                    message: "Codex running".to_string(),
                    received_at: old_timestamp.clone(),
                    source: None,
                    session_id: Some("session-1".to_string()),
                    turn_id: Some("turn-1".to_string()),
                    cwd: Some("D:\\Desktop\\codex\\firmware".to_string()),
                    model: Some("gpt-5.5".to_string()),
                },
            );
        }

        let snapshot = task_snapshot(&state, &scoped_test_config("session-1", &codex_home));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
    }

    #[test]
    fn snapshot_drops_unconfirmed_codex_turn_after_session_idle_grace() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-unconfirmed-stale-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();

        let old_timestamp = past_timestamp((ACTIVE_SESSION_IDLE_STALE_GRACE.as_secs() as i64) + 60);
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            format!(
                "{}\n{}\n",
                json!({
                    "timestamp": old_timestamp,
                    "payload": {
                        "type": "task_started",
                        "turn_id": "turn-1"
                    }
                }),
                json!({
                    "timestamp": old_timestamp,
                    "payload": {
                        "type": "turn_context",
                        "turn_id": "turn-1",
                        "cwd": "D:\\Desktop\\codex\\firmware",
                        "model": "gpt-5.5"
                    }
                })
            ),
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                TaskEvent {
                    kind: "UserPromptSubmit".to_string(),
                    status: "running".to_string(),
                    title: "Codex running".to_string(),
                    message: "Codex running".to_string(),
                    received_at: old_timestamp,
                    source: None,
                    session_id: Some("session-1".to_string()),
                    turn_id: Some("turn-1".to_string()),
                    cwd: Some("D:\\Desktop\\codex\\firmware".to_string()),
                    model: Some("gpt-5.5".to_string()),
                },
            );
        }

        let snapshot = task_snapshot(&state, &scoped_test_config("session-1", &codex_home));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
    }

    #[test]
    fn snapshot_keeps_recent_unconfirmed_codex_turn() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-unconfirmed-recent-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("31");
        fs::create_dir_all(&session_dir).unwrap();

        let recent = recent_timestamp(60);
        fs::write(
            session_dir.join("rollout-2026-05-31T16-59-09-session-1.jsonl"),
            format!(
                "{}\n{}\n",
                json!({
                    "timestamp": recent,
                    "payload": {
                        "type": "task_started",
                        "turn_id": "turn-1"
                    }
                }),
                json!({
                    "timestamp": recent,
                    "payload": {
                        "type": "turn_context",
                        "turn_id": "turn-1",
                        "cwd": "D:\\Desktop\\codex\\firmware",
                        "model": "gpt-5.5"
                    }
                })
            ),
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                TaskEvent {
                    kind: "UserPromptSubmit".to_string(),
                    status: "running".to_string(),
                    title: "Codex running".to_string(),
                    message: "Codex running".to_string(),
                    received_at: recent,
                    source: None,
                    session_id: Some("session-1".to_string()),
                    turn_id: Some("turn-1".to_string()),
                    cwd: Some("D:\\Desktop\\codex\\firmware".to_string()),
                    model: Some("gpt-5.5".to_string()),
                },
            );
        }

        let snapshot = task_snapshot(&state, &scoped_test_config("session-1", &codex_home));
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 1);
        assert_eq!(snapshot.status, "running");
    }

    #[test]
    fn does_not_recover_completed_turn_from_session_log() {
        let codex_home = env::temp_dir().join(format!(
            "codex-ornament-recover-complete-{}",
            std::process::id()
        ));
        let session_dir = codex_home
            .join("sessions")
            .join("2026")
            .join("05")
            .join("25");
        fs::create_dir_all(&session_dir).unwrap();
        fs::write(
            session_dir.join("rollout-2026-05-25T13-36-13-session-1.jsonl"),
            concat!(
                "{\"timestamp\":\"2026-05-25T12:00:00+08:00\",\"payload\":{\"type\":\"task_started\",\"turn_id\":\"turn-1\"}}\n",
                "{\"timestamp\":\"2026-05-25T12:00:02+08:00\",\"payload\":{\"type\":\"task_complete\",\"turn_id\":\"turn-1\"}}\n"
            ),
        )
        .unwrap();

        let state = Arc::new(Mutex::new(BridgeState::default()));
        let config = scoped_test_config("session-1", &codex_home);
        let snapshot = task_snapshot(&state, &config);
        let _ = fs::remove_dir_all(&codex_home);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
    }

    #[test]
    fn scope_reconcile_drops_stale_display_task() {
        let state = Arc::new(Mutex::new(BridgeState::default()));
        {
            let mut state = state.lock().unwrap();
            apply_task_event(
                &mut state,
                normalize_event(&json!({
                    "hook_event_name": "UserPromptSubmit",
                    "session_id": "session-2"
                })),
            );
        }

        let config = scoped_test_config("session-1", ".codex-test");
        let snapshot = task_snapshot(&state, &config);

        assert_eq!(snapshot.active_task_count, 0);
        assert_eq!(snapshot.status, "done");
        assert!(snapshot.task.is_none());
    }
}
