use chrono::{DateTime, Local, Utc};
use reqwest::blocking::Client;
use reqwest::header::{HeaderMap, HeaderValue, ACCEPT, AUTHORIZATION, ORIGIN, REFERER, USER_AGENT};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    env, fs,
    io::{self, ErrorKind},
    path::{Path, PathBuf},
    time::Duration,
};

pub const USAGE_URL: &str = "https://chatgpt.com/backend-api/wham/usage";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum SnapshotStatus {
    Ok,
    NoData,
    ParseError,
    AuthRequired,
    RequestFailed,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct QuotaSnapshot {
    pub status: SnapshotStatus,
    pub source: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub source_label: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub web_url: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub limit_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub plan_type: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub primary_used_percent: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub primary_remaining_percent: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub primary_window_minutes: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub primary_resets_at: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub secondary_used_percent: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub secondary_remaining_percent: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub secondary_window_minutes: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub secondary_resets_at: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub credits: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub observed_at: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub captured_at: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

#[derive(Debug, Deserialize)]
struct CodexAuth {
    tokens: Option<AuthTokens>,
}

#[derive(Debug, Deserialize)]
struct AuthTokens {
    access_token: Option<String>,
    account_id: Option<String>,
}

#[derive(Debug, Deserialize)]
struct UsageResponse {
    plan_type: Option<String>,
    rate_limit: Option<Value>,
    rate_limits: Option<Value>,
    credits: Option<Value>,
}

#[derive(Debug, Clone, Default)]
struct LimitWindow {
    used_percent: Option<f64>,
    percent_left: Option<f64>,
    remaining_percent: Option<f64>,
    reset_at: Option<i64>,
    reset_time_ms: Option<i64>,
    limit_window_seconds: Option<u64>,
}

impl QuotaSnapshot {
    fn status(status: SnapshotStatus, error: impl Into<String>) -> Self {
        Self {
            status,
            source: "codex-wham".to_string(),
            source_label: Some("ChatGPT usage API".to_string()),
            web_url: Some(USAGE_URL.to_string()),
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
            error: Some(error.into()),
        }
    }

    pub fn current_used_percent(&self) -> Option<f64> {
        if self.has_secondary_limit() {
            self.secondary_used_percent
        } else {
            self.primary_used_percent
        }
    }

    pub fn current_remaining_percent(&self) -> Option<f64> {
        if self.has_secondary_limit() {
            self.secondary_remaining_percent
        } else {
            self.primary_remaining_percent
        }
    }

    pub fn current_window_minutes(&self) -> Option<u64> {
        if self.has_secondary_limit() {
            self.secondary_window_minutes
        } else {
            self.primary_window_minutes
        }
    }

    pub fn current_resets_at(&self) -> Option<&str> {
        if self.has_secondary_limit() {
            self.secondary_resets_at.as_deref()
        } else {
            self.primary_resets_at.as_deref()
        }
    }

    pub fn current_limit_label(&self) -> &'static str {
        if self.has_secondary_limit() {
            "周额度"
        } else {
            "当前额度"
        }
    }

    fn has_secondary_limit(&self) -> bool {
        self.secondary_remaining_percent.is_some()
            || self.secondary_used_percent.is_some()
            || self.secondary_window_minutes.is_some()
            || self.secondary_resets_at.is_some()
    }
}

pub fn get_quota_snapshot() -> QuotaSnapshot {
    let auth = match load_auth() {
        Ok(auth) => auth,
        Err(snapshot) => return *snapshot,
    };

    let usage = match fetch_usage(&auth) {
        Ok(usage) => usage,
        Err(snapshot) => return *snapshot,
    };

    snapshot_from_usage(usage)
}

pub fn read_state(path: impl AsRef<Path>) -> io::Result<Option<QuotaSnapshot>> {
    let path = path.as_ref();
    match fs::read_to_string(path) {
        Ok(content) => serde_json::from_str(&content)
            .map(Some)
            .map_err(|error| io::Error::new(ErrorKind::InvalidData, error)),
        Err(error) if error.kind() == ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error),
    }
}

pub fn write_state(path: impl AsRef<Path>, snapshot: &QuotaSnapshot) -> io::Result<()> {
    let path = path.as_ref();
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let content = serde_json::to_string_pretty(snapshot)
        .map_err(|error| io::Error::new(ErrorKind::InvalidData, error))?;
    fs::write(path, content)
}

pub fn state_path() -> PathBuf {
    env::var_os("CODEX_QUOTA_STATE")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/var/lib/codex-quota/state.json"))
}

pub fn summary_text(snapshot: &QuotaSnapshot) -> String {
    if snapshot.status != SnapshotStatus::Ok {
        return format!(
            "Codex 额度查询失败：{}\n状态：{:?}\n更新时间：{}",
            snapshot.error.as_deref().unwrap_or("未知错误"),
            snapshot.status,
            display_time(
                snapshot
                    .captured_at
                    .as_deref()
                    .or(snapshot.observed_at.as_deref())
            )
        );
    }

    let window = format_window(snapshot.current_window_minutes());
    format!(
        "Codex 额度\n计划：{}\n{}：已用 {}，剩余 {}，窗口 {}，重置时间 {}\n更新时间：{}",
        snapshot.plan_type.as_deref().unwrap_or("--"),
        snapshot.current_limit_label(),
        format_percent(snapshot.current_used_percent()),
        format_percent(snapshot.current_remaining_percent()),
        window,
        display_time(snapshot.current_resets_at()),
        display_time(
            snapshot
                .captured_at
                .as_deref()
                .or(snapshot.observed_at.as_deref())
        ),
    )
}

pub fn notification_signature(snapshot: &QuotaSnapshot) -> String {
    let rounded = |value: Option<f64>| {
        value
            .map(|number| format!("{:.1}", number))
            .unwrap_or_else(|| "-".to_string())
    };

    format!(
        "{:?}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
        snapshot.status,
        snapshot.plan_type.as_deref().unwrap_or("-"),
        rounded(snapshot.primary_used_percent),
        rounded(snapshot.primary_remaining_percent),
        snapshot.primary_resets_at.as_deref().unwrap_or("-"),
        rounded(snapshot.secondary_used_percent),
        rounded(snapshot.secondary_remaining_percent),
        snapshot.secondary_resets_at.as_deref().unwrap_or("-"),
        snapshot.error.as_deref().unwrap_or("-"),
        snapshot.limit_id.as_deref().unwrap_or("-"),
    )
}

type SnapshotError = Box<QuotaSnapshot>;

fn snapshot_error(status: SnapshotStatus, error: impl Into<String>) -> SnapshotError {
    Box::new(QuotaSnapshot::status(status, error))
}

fn load_auth() -> Result<AuthTokens, SnapshotError> {
    let path = default_auth_path().ok_or_else(|| {
        snapshot_error(
            SnapshotStatus::AuthRequired,
            "无法定位 auth.json。请设置 CODEX_AUTH_PATH、CODEX_HOME，或先登录 Codex。",
        )
    })?;

    let content = fs::read_to_string(&path).map_err(|error| {
        snapshot_error(
            SnapshotStatus::AuthRequired,
            format!("无法读取 {}：{error}", path.display()),
        )
    })?;

    let auth = serde_json::from_str::<CodexAuth>(&content).map_err(|error| {
        snapshot_error(
            SnapshotStatus::ParseError,
            format!("auth.json 格式无法解析：{error}"),
        )
    })?;

    let tokens = auth.tokens.ok_or_else(|| {
        snapshot_error(
            SnapshotStatus::AuthRequired,
            "auth.json 中没有 tokens 字段。请先在 Codex 中登录 ChatGPT。",
        )
    })?;

    if tokens
        .access_token
        .as_deref()
        .unwrap_or_default()
        .is_empty()
    {
        return Err(snapshot_error(
            SnapshotStatus::AuthRequired,
            "auth.json 中没有 access_token。请重新登录 Codex。",
        ));
    }

    if tokens.account_id.as_deref().unwrap_or_default().is_empty() {
        return Err(snapshot_error(
            SnapshotStatus::AuthRequired,
            "auth.json 中没有 account_id。请重新登录 Codex。",
        ));
    }

    Ok(tokens)
}

fn fetch_usage(auth: &AuthTokens) -> Result<UsageResponse, SnapshotError> {
    let access_token = auth.access_token.as_deref().unwrap_or_default();
    let account_id = auth.account_id.as_deref().unwrap_or_default();
    let mut headers = HeaderMap::new();
    headers.insert(ACCEPT, HeaderValue::from_static("application/json"));
    headers.insert(ORIGIN, HeaderValue::from_static("https://chatgpt.com"));
    headers.insert(REFERER, HeaderValue::from_static("https://chatgpt.com/"));
    headers.insert(USER_AGENT, HeaderValue::from_static("Mozilla/5.0"));
    headers.insert(
        AUTHORIZATION,
        HeaderValue::from_str(&format!("Bearer {access_token}")).map_err(|_| {
            snapshot_error(SnapshotStatus::ParseError, "access_token 无法构造请求头。")
        })?,
    );
    headers.insert(
        "ChatGPT-Account-Id",
        HeaderValue::from_str(account_id).map_err(|_| {
            snapshot_error(SnapshotStatus::ParseError, "account_id 无法构造请求头。")
        })?,
    );

    let client = Client::builder()
        .timeout(Duration::from_secs(15))
        .build()
        .map_err(|error| {
            snapshot_error(
                SnapshotStatus::RequestFailed,
                format!("HTTP 客户端创建失败：{error}"),
            )
        })?;

    let response = client
        .get(USAGE_URL)
        .headers(headers)
        .send()
        .map_err(|error| {
            snapshot_error(
                SnapshotStatus::RequestFailed,
                format!("请求 ChatGPT Codex 用量接口失败：{error}"),
            )
        })?;

    let status = response.status();
    if status.as_u16() == 401 {
        return Err(snapshot_error(
            SnapshotStatus::AuthRequired,
            "ChatGPT 返回 401，access_token 已过期或无效。请重新登录 Codex。",
        ));
    }
    if status.as_u16() == 403 {
        return Err(snapshot_error(
            SnapshotStatus::AuthRequired,
            "ChatGPT 返回 403，当前账号可能无权访问 Codex 用量接口。",
        ));
    }
    if !status.is_success() {
        return Err(snapshot_error(
            SnapshotStatus::RequestFailed,
            format!("ChatGPT 用量接口返回 HTTP {status}。"),
        ));
    }

    response.json::<UsageResponse>().map_err(|error| {
        snapshot_error(
            SnapshotStatus::ParseError,
            format!("ChatGPT 用量接口响应无法解析：{error}"),
        )
    })
}

fn snapshot_from_usage(usage: UsageResponse) -> QuotaSnapshot {
    let Some(rate_limits) = usage.rate_limit.as_ref().or(usage.rate_limits.as_ref()) else {
        return QuotaSnapshot::status(SnapshotStatus::NoData, "响应中没有 rate_limit 字段。");
    };

    let (primary, secondary) = parse_rate_limits(rate_limits);
    if primary.is_none() && secondary.is_none() {
        return QuotaSnapshot::status(SnapshotStatus::NoData, "响应中没有可用额度窗口。");
    }

    let primary_remaining = primary.as_ref().and_then(remaining_percent);
    let primary_used = primary
        .as_ref()
        .and_then(|window| used_percent(window, primary_remaining));
    let secondary_remaining = secondary.as_ref().and_then(remaining_percent);
    let secondary_used = secondary
        .as_ref()
        .and_then(|window| used_percent(window, secondary_remaining));
    let now = now_local();

    QuotaSnapshot {
        status: SnapshotStatus::Ok,
        source: "codex-wham".to_string(),
        source_label: Some("ChatGPT usage API".to_string()),
        web_url: Some(USAGE_URL.to_string()),
        limit_id: Some("codex".to_string()),
        plan_type: usage.plan_type,
        primary_used_percent: primary_used,
        primary_remaining_percent: primary_remaining,
        primary_window_minutes: primary
            .as_ref()
            .and_then(|window| window_minutes(window, Some(300))),
        primary_resets_at: primary.as_ref().and_then(reset_time),
        secondary_used_percent: secondary_used,
        secondary_remaining_percent: secondary_remaining,
        secondary_window_minutes: secondary
            .as_ref()
            .and_then(|window| window_minutes(window, Some(10_080))),
        secondary_resets_at: secondary.as_ref().and_then(reset_time),
        credits: usage.credits.filter(|value| !value.is_null()),
        observed_at: Some(now.clone()),
        captured_at: Some(now),
        error: None,
    }
}

fn parse_rate_limits(rate_limits: &Value) -> (Option<LimitWindow>, Option<LimitWindow>) {
    let primary = first_limit(
        rate_limits,
        &[
            "five_hour",
            "five_hour_limit",
            "five_hour_rate_limit",
            "primary",
            "primary_window",
        ],
        &["primary_window"],
    );
    let secondary = first_limit(
        rate_limits,
        &[
            "weekly",
            "weekly_limit",
            "weekly_rate_limit",
            "secondary",
            "secondary_window",
        ],
        &["secondary_window"],
    );

    match (primary, secondary) {
        (Some(primary), Some(secondary)) => relabel_windows(primary, secondary),
        (Some(window), None) => match infer_limit_name(&window) {
            Some("weekly") => (None, Some(window)),
            _ => (Some(window), None),
        },
        (None, Some(window)) => match infer_limit_name(&window) {
            Some("five_hour") => (Some(window), None),
            _ => (None, Some(window)),
        },
        (None, None) => (None, None),
    }
}

fn first_limit(rate_limits: &Value, keys: &[&str], nested_keys: &[&str]) -> Option<LimitWindow> {
    keys.iter()
        .filter_map(|key| rate_limits.get(key))
        .find_map(|value| parse_limit_entry(value, nested_keys))
}

fn parse_limit_entry(value: &Value, nested_keys: &[&str]) -> Option<LimitWindow> {
    let resolved = resolve_limit_window(value, nested_keys);
    if !resolved.is_object() {
        return None;
    }

    Some(LimitWindow {
        used_percent: number_field(resolved, "used_percent"),
        percent_left: number_field(resolved, "percent_left"),
        remaining_percent: number_field(resolved, "remaining_percent"),
        reset_at: integer_field(resolved, "reset_at"),
        reset_time_ms: integer_field(resolved, "reset_time_ms"),
        limit_window_seconds: unsigned_field(resolved, "limit_window_seconds"),
    })
}

fn resolve_limit_window<'a>(value: &'a Value, nested_keys: &[&str]) -> &'a Value {
    let has_direct_window_fields = value.get("reset_at").is_some()
        || value.get("reset_time_ms").is_some()
        || value.get("limit_window_seconds").is_some()
        || value.get("percent_left").is_some()
        || value.get("remaining_percent").is_some()
        || value.get("used_percent").is_some();

    if has_direct_window_fields {
        return value;
    }

    nested_keys
        .iter()
        .filter_map(|key| value.get(key))
        .find(|nested| nested.is_object())
        .unwrap_or(value)
}

fn relabel_windows(
    primary: LimitWindow,
    secondary: LimitWindow,
) -> (Option<LimitWindow>, Option<LimitWindow>) {
    match (infer_limit_name(&primary), infer_limit_name(&secondary)) {
        (Some("weekly"), Some("five_hour")) => (Some(secondary), Some(primary)),
        (Some("weekly"), _) => (None, Some(primary)),
        (_, Some("five_hour")) => (Some(secondary), None),
        _ => (Some(primary), Some(secondary)),
    }
}

fn infer_limit_name(window: &LimitWindow) -> Option<&'static str> {
    let seconds = window.limit_window_seconds?;
    if seconds <= 6 * 3600 {
        return Some("five_hour");
    }
    if seconds >= 6 * 24 * 3600 {
        return Some("weekly");
    }
    None
}

fn number_field(value: &Value, key: &str) -> Option<f64> {
    value.get(key).and_then(Value::as_f64)
}

fn integer_field(value: &Value, key: &str) -> Option<i64> {
    value.get(key).and_then(Value::as_i64)
}

fn unsigned_field(value: &Value, key: &str) -> Option<u64> {
    value.get(key).and_then(Value::as_u64)
}

fn remaining_percent(window: &LimitWindow) -> Option<f64> {
    window
        .percent_left
        .or(window.remaining_percent)
        .or_else(|| window.used_percent.map(|value| 100.0 - value))
        .map(|value| value.clamp(0.0, 100.0))
}

fn used_percent(window: &LimitWindow, remaining: Option<f64>) -> Option<f64> {
    window
        .used_percent
        .or_else(|| remaining.map(|value| 100.0 - value))
        .map(|value| value.clamp(0.0, 100.0))
}

fn reset_time(window: &LimitWindow) -> Option<String> {
    window
        .reset_at
        .or(window.reset_time_ms)
        .and_then(format_epoch)
}

fn window_minutes(window: &LimitWindow, default_minutes: Option<u64>) -> Option<u64> {
    window
        .limit_window_seconds
        .map(|seconds| seconds / 60)
        .or(default_minutes)
}

fn default_auth_path() -> Option<PathBuf> {
    if let Some(auth_path) = env::var_os("CODEX_AUTH_PATH") {
        return Some(PathBuf::from(auth_path));
    }

    if let Some(codex_home) = env::var_os("CODEX_HOME") {
        return Some(PathBuf::from(codex_home).join("auth.json"));
    }

    env::var_os("USERPROFILE")
        .or_else(|| env::var_os("HOME"))
        .map(|profile| PathBuf::from(profile).join(".codex").join("auth.json"))
}

fn format_epoch(value: i64) -> Option<String> {
    let seconds = if value > 100_000_000_000 {
        value / 1000
    } else {
        value
    };
    let utc: DateTime<Utc> = DateTime::from_timestamp(seconds, 0)?;
    Some(utc.with_timezone(&Local).to_rfc3339())
}

fn now_local() -> String {
    Local::now().to_rfc3339()
}

fn format_percent(value: Option<f64>) -> String {
    value
        .map(|number| {
            if number < 10.0 {
                format!("{number:.1}%")
            } else {
                format!("{number:.0}%")
            }
        })
        .unwrap_or_else(|| "--".to_string())
}

fn format_window(minutes: Option<u64>) -> String {
    let Some(minutes) = minutes else {
        return "--".to_string();
    };
    if minutes < 60 {
        return format!("{minutes} 分钟");
    }
    if minutes % 1440 == 0 {
        return format!("{} 天", minutes / 1440);
    }
    if minutes % 60 == 0 {
        return format!("{} 小时", minutes / 60);
    }
    format!("{minutes} 分钟")
}

fn display_time(value: Option<&str>) -> String {
    value.unwrap_or("--").to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_primary_and_secondary_windows() {
        let usage = serde_json::from_str::<UsageResponse>(
            r#"{
              "plan_type": "team",
              "rate_limit": {
                "primary_window": {"used_percent": 72, "reset_at": 1779262879, "limit_window_seconds": 18000},
                "secondary_window": {"used_percent": 11, "reset_at": 1779849679, "limit_window_seconds": 604800}
              },
              "credits": null
            }"#,
        )
        .unwrap();

        let snapshot = snapshot_from_usage(usage);

        assert_eq!(snapshot.status, SnapshotStatus::Ok);
        assert_eq!(snapshot.plan_type.as_deref(), Some("team"));
        assert_eq!(snapshot.primary_used_percent, Some(72.0));
        assert_eq!(snapshot.primary_remaining_percent, Some(28.0));
        assert_eq!(snapshot.primary_window_minutes, Some(300));
        assert_eq!(snapshot.secondary_remaining_percent, Some(89.0));
        assert_eq!(snapshot.secondary_window_minutes, Some(10_080));
    }

    #[test]
    fn accepts_remaining_percent_variants() {
        let usage = serde_json::from_str::<UsageResponse>(
            r#"{
              "rate_limits": {
                "five_hour": {"remaining_percent": 64, "limit_window_seconds": 18000},
                "weekly": {"percent_left": 92, "limit_window_seconds": 604800}
              }
            }"#,
        )
        .unwrap();

        let snapshot = snapshot_from_usage(usage);

        assert_eq!(snapshot.primary_remaining_percent, Some(64.0));
        assert_eq!(snapshot.primary_used_percent, Some(36.0));
        assert_eq!(snapshot.secondary_remaining_percent, Some(92.0));
        assert_eq!(snapshot.secondary_used_percent, Some(8.0));
    }

    #[test]
    fn unwraps_nested_primary_window() {
        let usage = serde_json::from_str::<UsageResponse>(
            r#"{
              "rate_limit": {
                "five_hour_limit": {
                  "primary_window": {"percent_left": 42, "limit_window_seconds": 18000}
                },
                "weekly_limit": {
                  "secondary_window": {"remaining_percent": 77, "limit_window_seconds": 604800}
                }
              }
            }"#,
        )
        .unwrap();

        let snapshot = snapshot_from_usage(usage);

        assert_eq!(snapshot.primary_remaining_percent, Some(42.0));
        assert_eq!(snapshot.secondary_remaining_percent, Some(77.0));
    }

    #[test]
    fn missing_rate_limit_returns_no_data() {
        let usage = serde_json::from_str::<UsageResponse>(r#"{"plan_type":"team"}"#).unwrap();

        let snapshot = snapshot_from_usage(usage);

        assert_eq!(snapshot.status, SnapshotStatus::NoData);
    }

    #[test]
    fn accepts_weekly_only_limit_after_short_window_removal() {
        let usage = serde_json::from_str::<UsageResponse>(
            r#"{
              "rate_limit": {
                "secondary_window": {
                  "used_percent": 27,
                  "reset_at": 1779849679,
                  "limit_window_seconds": 604800
                }
              }
            }"#,
        )
        .unwrap();

        let snapshot = snapshot_from_usage(usage);

        assert_eq!(snapshot.status, SnapshotStatus::Ok);
        assert_eq!(snapshot.primary_remaining_percent, None);
        assert_eq!(snapshot.secondary_remaining_percent, Some(73.0));
        assert_eq!(snapshot.current_remaining_percent(), Some(73.0));
        assert_eq!(snapshot.current_window_minutes(), Some(10_080));
        assert_eq!(snapshot.current_limit_label(), "周额度");
    }

    #[test]
    fn current_limit_does_not_mix_fields_from_different_windows() {
        let usage = serde_json::from_str::<UsageResponse>(
            r#"{
              "rate_limit": {
                "primary_window": {
                  "used_percent": 10,
                  "reset_at": 1779262879,
                  "limit_window_seconds": 18000
                },
                "secondary_window": {
                  "used_percent": 25,
                  "limit_window_seconds": 604800
                }
              }
            }"#,
        )
        .unwrap();

        let snapshot = snapshot_from_usage(usage);

        assert_eq!(snapshot.current_remaining_percent(), Some(75.0));
        assert_eq!(snapshot.current_resets_at(), None);
    }

    #[test]
    fn supports_epoch_seconds_and_milliseconds() {
        assert!(format_epoch(1_779_262_879).unwrap().contains("2026"));
        assert!(format_epoch(1_779_262_879_000).unwrap().contains("2026"));
    }

    #[test]
    fn summary_contains_quota_fields() {
        let snapshot = QuotaSnapshot {
            status: SnapshotStatus::Ok,
            source: "codex-wham".to_string(),
            source_label: None,
            web_url: None,
            limit_id: Some("codex".to_string()),
            plan_type: Some("team".to_string()),
            primary_used_percent: Some(36.0),
            primary_remaining_percent: Some(64.0),
            primary_window_minutes: Some(300),
            primary_resets_at: Some("2026-05-21T10:00:00+08:00".to_string()),
            secondary_used_percent: Some(8.0),
            secondary_remaining_percent: Some(92.0),
            secondary_window_minutes: Some(10_080),
            secondary_resets_at: Some("2026-05-28T10:00:00+08:00".to_string()),
            credits: None,
            observed_at: Some("2026-05-21T09:00:00+08:00".to_string()),
            captured_at: Some("2026-05-21T09:00:00+08:00".to_string()),
            error: None,
        };

        let summary = summary_text(&snapshot);

        assert!(summary.contains("周额度"));
        assert!(summary.contains("重置时间"));
        assert!(summary.contains("team"));
    }

    #[test]
    fn notification_signature_ignores_capture_time() {
        let mut first = QuotaSnapshot::status(SnapshotStatus::RequestFailed, "network");
        let mut second = first.clone();
        first.observed_at = Some("2026-05-21T09:00:00+08:00".to_string());
        second.observed_at = Some("2026-05-21T09:00:30+08:00".to_string());

        assert_eq!(
            notification_signature(&first),
            notification_signature(&second)
        );
    }
}
