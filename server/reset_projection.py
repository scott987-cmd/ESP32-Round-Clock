"""Project public observations, not an account reset or an invented forecast."""
from datetime import datetime, timezone, timedelta
import math

BEIJING = timezone(timedelta(hours=8))
MAX_SOURCE_AGE = 30 * 60
MAX_ALERT_AGE = 24 * 60 * 60

def stamp(value):
    if not isinstance(value, str):
        return None
    try:
        dt = datetime.fromisoformat(value.replace('Z', '+00:00'))
        return dt.timestamp() if dt.tzinfo else None
    except (ValueError, OverflowError):
        return None

def fresh(value, now):
    at = stamp(value)
    return at is not None and 0 <= now - at <= MAX_SOURCE_AGE

def local_time(value):
    at = stamp(value)
    try:
        return datetime.fromtimestamp(at, BEIJING).strftime('%Y-%m-%d %H:%M') if at is not None else '时间未提供'
    except (ValueError, OverflowError, OSError):
        return '时间未提供'

def percent(value):
    return value if type(value) in (int, float) and math.isfinite(value) and 0 <= value <= 100 else None

def recent_tease(feed, now):
    """Return a fresh, source-classified hint; never infer one from wording."""
    for post in feed.get('tweets', []) if isinstance(feed.get('tweets'), list) else []:
        if not isinstance(post, dict) or not fresh(post.get('at'), now):
            continue
        tease = post.get('tease_classification')
        if not (isinstance(tease, dict) and tease.get('status') == 'ok' and
                tease.get('teasing') is True):
            continue
        ident, text = post.get('id'), post.get('text')
        if isinstance(ident, str) and isinstance(text, str) and text.strip():
            return ident[:31], text.strip().replace('\n', ' ')[:120], post.get('at')
    return '', '', ''

def project(feed, forecast, now=None):
    now = datetime.now(timezone.utc).timestamp() if now is None else now
    feed = feed if isinstance(feed, dict) else {}
    forecast = forecast if isinstance(forecast, dict) else {}
    signal = feed.get('signal') if isinstance(feed.get('signal'), dict) else {}
    announced = signal.get('at') if stamp(signal.get('at')) is not None else ''
    source_fresh = feed.get('stale') is False and fresh(feed.get('fetched_at'), now)
    signal_epoch = stamp(announced)
    recent = signal_epoch is not None and 0 <= now - signal_epoch <= MAX_ALERT_AGE
    # A fresh poll does not make an old announcement a fresh event.
    alert = forecast.get('latest_alert')
    confirmed = (isinstance(alert,dict) and alert.get('kind')=='reset' and
                 alert.get('state')=='confirmed' and alert.get('corrected') is not True and
                 alert.get('id')==signal.get('tweet_id'))
    eligible = source_fresh and signal.get('active') is True and recent and confirmed and fresh(forecast.get('updated_at'),now)
    latest = forecast.get('last_reset_at')
    if stamp(latest) is None or stamp(latest) > now:
        latest = ''
    # A candidate/credit post is not a verified completed global reset.
    event_epoch = stamp(latest)
    event_label = '上次重置（北京时间）' if latest else '最近消息（非完成证明）'
    shown_at = latest or announced
    if event_epoch is None:
        event_epoch = stamp(shown_at)
    if event_epoch is None or event_epoch > now:
        event_epoch = None
        shown_at = ''
    age = int(now - event_epoch) if event_epoch is not None else None
    age_label = '时间未知，不触发提醒' if age is None else (
        f'{age//86400}天{age%86400//3600}小时前 · 历史记录' if age >= 86400 else
        f'{age//3600}小时{age%3600//60}分钟前 · 非当前发生')
    probabilities = forecast.get('probabilities')
    probabilities = probabilities if isinstance(probabilities, dict) else {}
    p24, p48 = (percent(probabilities.get('rounded_24h')), percent(probabilities.get('rounded_48h')))
    forecast_fresh = fresh(forecast.get('updated_at'), now)
    available = forecast_fresh and p24 is not None and p48 is not None and p24 <= p48
    confidence = {'low':'低可信','medium':'中等可信','high':'高可信'}.get(forecast.get('confidence'), '可信度未知')
    mode = forecast.get('mode')
    basis = '历史模型' if mode == 'model' else '网站信号估计'
    meta = f'{confidence} · {basis}，非官方' if available else '预测不可用或已过期'
    lines = ['概率来源：codex-reset.com', '这不是个人账户的重置时间。']
    if available:
        for key, label in [('range_24h','24小时区间'), ('range_48h','48小时区间')]:
            bounds = probabilities.get(key)
            if isinstance(bounds, dict):
                lo, hi = bounds.get('lower'), bounds.get('upper')
                if type(lo) in (int,float) and type(hi) in (int,float) and 0 <= lo <= hi <= 1:
                    lines.append(f'{label}：{int(lo*100+0.5)}% - {int(hi*100+0.5)}%')
        lines.append(f'未来 24 小时：{p24}% · 48 小时：{p48}%')
    lines.append('预测更新（北京时间）\n' + local_time(forecast.get('updated_at')))
    context = forecast.get('context_copy')
    if isinstance(context, dict) and isinstance(context.get('zh'), str):
        lines.append(context['zh'][:100])
    wait_copy = forecast.get('wait_copy')
    if isinstance(wait_copy, dict) and isinstance(wait_copy.get('zh'), str):
        lines.append(wait_copy['zh'][:180])
    if forecast.get('official_signal') is None:
        lines.append('网站未给出官方重置窗口。')
    else:
        lines.append('网站有信号，具体窗口请查原站。')
    if isinstance(forecast.get('backtest'), dict) and forecast['backtest'].get('status') == 'experimental':
        lines.append('实验模型：尚未稳定优于对照基准，不是承诺或倒计时。')
    lines.append('消息发布（北京时间）\n' + local_time(announced))
    summary = signal.get('summary')
    sid = signal.get('tweet_id')
    watch_id, watch_summary, watch_at = recent_tease(feed, now)
    watch = bool(watch_id and source_fresh and forecast_fresh)
    if watch:
        lines.insert(0, '近期提示（未确认）：' + watch_summary)
    return {
        'schemaVersion':2, 'fetchedAt':feed.get('fetched_at'), 'stale':not source_fresh,
        'active':eligible, 'alertEligible':eligible,
        # Do not consume an unconfirmed new signal: it may be confirmed on a
        # later poll, after a partial source failure or an upcoming announcement.
        'rememberSignal':source_fresh and (eligible or watch or (signal_epoch is not None and now-signal_epoch > MAX_ALERT_AGE)),
        'signalId':sid[:31] if isinstance(sid,str) else '',
        'announcedAt':announced, 'summary':summary[:120] if isinstance(summary,str) else '',
        'analysis':'历史消息，不代表刚刚发生重置。' if not recent else '社区信号，请到原站和个人用量页核实。',
        'analysisProvider':'rule', 'kind':str(signal.get('kind',''))[:32],
        'source':'codex-reset.com', 'eventLabel':event_label,
        'eventTime':local_time(shown_at), 'eventAge':age_label, 'eventEpoch':event_epoch or 0,
        'forecastAvailable':available, 'probability24':p24 if available else None,
        'probability48':p48 if available else None, 'forecastMeta':meta,
        'forecastExpiresAt':int(stamp(forecast.get('updated_at'))+MAX_SOURCE_AGE) if forecast_fresh else 0,
        'details':'\n\n'.join(lines),
        'updateLabel':'来源更新 '+local_time(feed.get('fetched_at')) if source_fresh else '来源过期 · 不触发提醒',
        # Optional schema-2 extension: older firmwares ignore it; newer ones
        # can announce a current classified hint without calling it a reset.
        'watchEligible':watch, 'watchId':watch_id, 'watchSummary':watch_summary,
        'watchTime':local_time(watch_at) if watch else '',
    }
