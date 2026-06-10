var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

var settings = {
  host:   'https://tautulli.example.com',
  port:   '8181',
  path:   '',
  apiKey: 'your32characterapikeygoeshere000'
};

// ─── URL builder ─────────────────────────────────────────────
function buildUrl(cmd, extra) {
  var base = (settings.host || '').replace(/\/+$/, '');
  var port = (settings.port || '').trim();
  var isDefault = (base.indexOf('https://') === 0 && port === '443') ||
                  (base.indexOf('http://')  === 0 && port === '80');
  if (port && !isDefault) base += ':' + port;
  var path = (settings.path || '').replace(/^\/+|\/+$/g, '');
  if (path) base += '/' + path;
  base += '/api/v2?apikey=' + settings.apiKey + '&cmd=' + cmd;
  if (extra) base += extra;
  return base;
}

// ─── Helpers ─────────────────────────────────────────────────
function trunc(str, max) {
  if (!str) return '';
  var s = String(str);
  return s.length > max ? s.substring(0, max - 1) + '…' : s;
}

function fmtTime(unix) {
  if (!unix) return '?';
  var d = new Date(unix * 1000);
  var h = d.getHours(), m = d.getMinutes();
  return (h % 12 || 12) + ':' + (m < 10 ? '0' : '') + m + (h < 12 ? 'am' : 'pm');
}

function fmtDur(secs) {
  if (!secs) return '';
  var m = Math.floor(secs / 60);
  return m >= 60 ? Math.floor(m / 60) + 'h ' + (m % 60) + 'm' : m + 'm';
}

function fmtBw(kbps) {
  var n = parseInt(kbps, 10) || 0;
  return n >= 1000 ? (n / 1000).toFixed(1) + ' Mbps' : n + ' kbps';
}

// ─── Geo lookup (cached) ─────────────────────────────────────
var geoCache = {};

function isLocalIp(ip) {
  if (!ip) return true;
  return ip === '::1' || ip === 'localhost' ||
    ip.indexOf('192.168.') === 0 ||
    ip.indexOf('10.')      === 0 ||
    ip.indexOf('172.16.')  === 0 ||
    ip.indexOf('127.')     === 0;
}

function geoLookup(ip, cb) {
  if (!ip || isLocalIp(ip)) { cb('LAN'); return; }
  if (geoCache[ip]) { cb(geoCache[ip]); return; }
  apiGet(buildUrl('get_geoip_lookup', '&ip_address=' + encodeURIComponent(ip)),
    function(err, data) {
      if (err || !data) { geoCache[ip] = ip; cb(ip); return; }
      var city    = data.city    || '';
      var region  = data.region  || '';
      var country = data.country_code || data.country || '';
      var loc;
      if (city && region && country) {
        loc = city + ', ' + region + ', ' + country;
      } else if (city && country) {
        loc = city + ', ' + country;
      } else {
        loc = region || country || ip;
      }
      geoCache[ip] = loc;
      cb(loc);
    }
  );
}

// Resolve geo for an array of IPs, then call cb with a map {ip: locString}
function resolveGeo(ips, cb) {
  var unique = ips.filter(function(ip, i) { return ip && ips.indexOf(ip) === i; });
  if (unique.length === 0) { cb({}); return; }
  var map = {};
  var remaining = unique.length;
  unique.forEach(function(ip) {
    geoLookup(ip, function(loc) {
      map[ip] = loc;
      if (--remaining === 0) cb(map);
    });
  });
}

// ─── Network ─────────────────────────────────────────────────
function apiGet(url, cb) {
  console.log('[Tautulli] GET ' + url);
  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function() {
    if (this.readyState !== 4) return;
    if (this.status >= 200 && this.status < 300) {
      try {
        var d = JSON.parse(this.responseText);
        if (d.response && d.response.result === 'success') {
          cb(null, d.response.data);
        } else {
          cb('API: ' + (d.response ? d.response.message : 'unknown'));
        }
      } catch(e) {
        cb('Bad JSON from server');
      }
    } else if (this.status === 0) {
      var hint = url.indexOf('https://') === 0
        ? 'SSL/network fail – try HTTP?' : 'No response (status 0)';
      console.log('[Tautulli] ' + hint);
      cb(hint);
    } else {
      cb('HTTP ' + this.status);
    }
  };
  xhr.open('GET', url);
  xhr.timeout = 10000;
  xhr.ontimeout = function() { cb('Timeout after 10s'); };
  xhr.send();
}

function sendError(msg) {
  Pebble.sendAppMessage({'DATA_ERROR': trunc(msg, 126)});
}

// Send one item; stagger to avoid AppMessage inbox overflow
function sendItem(i, title, subtitle, detail, ratingKey) {
  Pebble.sendAppMessage({
    'DATA_INDEX':      i,
    'DATA_TITLE':      trunc(title,    126),
    'DATA_SUBTITLE':   trunc(subtitle, 126),
    'DATA_DETAIL':     trunc(detail,   126),
    'DATA_RATING_KEY': ratingKey ? String(ratingKey).substring(0, 14) : ''
  },
  function()  { console.log('Item ' + i + ' sent'); },
  function(e) { console.log('Item ' + i + ' fail: ' + JSON.stringify(e)); }
  );
}

function sendItems(items, hasMore) {
  var count = items.length;
  Pebble.sendAppMessage({'DATA_COUNT': count, 'DATA_HAS_MORE': hasMore ? 1 : 0});
  if (count === 0) return;
  var i = 0;
  function next() {
    if (i >= count) return;
    var it = items[i];
    sendItem(i, it.title, it.subtitle, it.detail, it.rk);
    i++;
    setTimeout(next, 150);
  }
  setTimeout(next, 100);
}

// ─── Fetch: Now Playing ──────────────────────────────────────
function fetchActivity() {
  apiGet(buildUrl('get_activity'), function(err, data) {
    if (err) { sendError(err); return; }
    var sessions = data.sessions || [];
    if (sessions.length === 0) { sendItems([], false); return; }

    var ips = sessions.map(function(s) { return s.ip_address; });
    resolveGeo(ips, function(geoMap) {
      var mapped = sessions.map(function(s) {
        var title = s.grandparent_title
          ? s.grandparent_title + ' – ' + s.title : s.title;
        var user  = s.friendly_name || s.user || 'Unknown';
        var pct   = parseInt(s.progress_percent, 10) || 0;
        var geo   = geoMap[s.ip_address] || s.ip_address || '?';
        var vDec  = s.video_decision || s.transcode_decision || '?';
        var aDec  = s.audio_decision || '?';
        var sub   = user + ' · ' + pct + '% · ' + (s.player || '?');
        var det   = (s.ip_address || '?') + ' (' + geo + ')' +
                    ' · ' + fmtBw(s.bandwidth) +
                    ' · ' + (s.video_codec || '?') + '[' + vDec + ']' +
                    ' · ' + (s.audio_codec || '?') + '[' + aDec + ']';
        return {title: title, subtitle: sub, detail: det, rk: s.rating_key};
      });
      sendItems(mapped, false);
    });
  });
}

// ─── Fetch: History (paginated) ──────────────────────────────
function fetchHistory(start) {
  var s = start || 0;
  apiGet(buildUrl('get_history', '&length=8&start=' + s), function(err, data) {
    if (err) { sendError(err); return; }
    var records = data.data || [];
    var total   = parseInt(data.recordsFiltered, 10) || 0;
    var hasMore = (s + records.length) < total;
    if (records.length === 0) { sendItems([], hasMore); return; }

    var ips = records.map(function(r) { return r.ip_address; });
    resolveGeo(ips, function(geoMap) {
      var mapped = records.map(function(r) {
        var title = r.grandparent_title
          ? r.grandparent_title + ': ' + r.title : r.title;
        var user  = r.friendly_name || r.user || 'Unknown';
        var pct   = r.percent_complete || 0;
        var dur   = fmtDur(r.duration);
        var geo   = geoMap[r.ip_address] || r.ip_address || '?';
        var sub   = user + ' · ' + geo + ' · ' + pct + '%' +
                    (dur ? ' · ' + dur : '');
        var det   = (r.player || r.platform || '?') +
                    ' · ' + fmtTime(r.started) + '–' + fmtTime(r.stopped) +
                    ' · ' + (r.transcode_decision || '?');
        return {title: title, subtitle: sub, detail: det, rk: r.rating_key};
      });
      sendItems(mapped, hasMore);
    });
  });
}

// ─── Fetch: Recently Added (paginated) ───────────────────────
function fetchRecentlyAdded(start) {
  var s = start || 0;
  apiGet(buildUrl('get_recently_added', '&count=8&start=' + s), function(err, data) {
    if (err) { sendError(err); return; }
    var items   = data.recently_added || [];
    // Tautulli doesn't expose a total for recently_added; assume more if full page returned
    var hasMore = items.length >= 8;
    var mapped  = items.map(function(it) {
      var show  = it.grandparent_title || it.parent_title || '';
      var title = show ? show + ' – ' + it.title : it.title;
      var sub   = (it.library_name || '') + (it.year ? ' · ' + it.year : '');
      var det   = (it.media_type || '') +
                  (it.child_count ? ' · ' + it.child_count + ' ep' : '');
      var rk    = it.rating_key || it.grandparent_rating_key || '';
      return {title: title, subtitle: sub, detail: det, rk: rk};
    });
    sendItems(mapped, hasMore);
  });
}

// ─── Fetch: Metadata (rich detail for Recently Added tap) ────
function fetchMetadata(ratingKey) {
  apiGet(buildUrl('get_metadata', '&rating_key=' + ratingKey), function(err, data) {
    if (err) { sendError(err); return; }

    var title   = data.title || 'Unknown';
    var year    = data.year   || '';
    var mtype   = data.media_type    || '';
    var rating  = data.content_rating || '';
    var studio  = data.studio || '';
    var dur     = data.duration ? fmtDur(Math.round(data.duration / 1000)) : '';
    var summary = data.summary || '';

    var genres  = (Array.isArray(data.genres) ? data.genres : [])
                    .slice(0, 3).map(function(g) { return g.tag || g; }).join(', ');
    var cast    = (Array.isArray(data.actors) ? data.actors : [])
                    .slice(0, 4).map(function(a) { return a.tag || a; }).join(', ');

    // subtitle: year · type · rating · duration
    var subParts = [year, mtype, rating, dur].filter(Boolean);
    var sub = subParts.join(' · ');

    // detail: studio, genres, cast, summary — all on separate lines for readability
    var detParts = [];
    if (studio)  detParts.push(studio);
    if (genres)  detParts.push(genres);
    if (cast)    detParts.push('Cast: ' + cast);
    if (summary) detParts.push('\n' + summary);
    var det = detParts.join('\n');

    Pebble.sendAppMessage({'DATA_COUNT': 1, 'DATA_HAS_MORE': 0});
    setTimeout(function() {
      // Use longer truncation for metadata detail so summary isn't cut short
      Pebble.sendAppMessage({
        'DATA_INDEX':      0,
        'DATA_TITLE':      trunc(title, 126),
        'DATA_SUBTITLE':   trunc(sub,   126),
        'DATA_DETAIL':     trunc(det,   900),
        'DATA_RATING_KEY': String(data.rating_key || ratingKey).substring(0, 14)
      });
    }, 100);
  });
}

// ─── Message handler ─────────────────────────────────────────
Pebble.addEventListener('appmessage', function(e) {
  var p = e.payload;
  console.log('Watch msg: ' + JSON.stringify(p));

  // Clay settings
  if (p.TautulliHost   !== undefined) settings.host   = p.TautulliHost   || settings.host;
  if (p.TautulliPort   !== undefined) settings.port   = p.TautulliPort   || '';
  if (p.TautulliPath   !== undefined) settings.path   = p.TautulliPath   || '';
  if (p.TautulliApiKey !== undefined) settings.apiKey = p.TautulliApiKey || settings.apiKey;

  if (p.CMD_TYPE !== undefined) {
    var cmd    = p.CMD_TYPE;
    var param  = p.CMD_PARAM || '';
    var offset = param ? parseInt(param, 10) : 0;

    if      (cmd === 1) fetchActivity();
    else if (cmd === 2) fetchHistory(offset);
    else if (cmd === 3) fetchRecentlyAdded(offset);
    else if (cmd === 4) {
      if (param) fetchMetadata(param);
      else sendError('No rating key');
    }
  }
});

// ─── Apply settings from a Clay dict ─────────────────────────
function applySettings(s) {
  if (!s) return;
  if (s.TautulliHost)               settings.host   = s.TautulliHost;
  if (s.TautulliPort  !== undefined) settings.port  = s.TautulliPort  || '';
  if (s.TautulliPath  !== undefined) settings.path  = s.TautulliPath  || '';
  if (s.TautulliApiKey)             settings.apiKey = s.TautulliApiKey;
  geoCache = {};  // invalidate geo cache when server changes
  console.log('[Tautulli] Settings applied: ' + settings.host +
              (settings.path ? '/' + settings.path : '') +
              ' key=' + (settings.apiKey ? settings.apiKey.substring(0, 6) + '...' : 'none'));
}

// clay.getSettings(response) in @rebble/clay expects the webviewclosed response
// string as its argument — it is NOT a "read stored settings" getter.
// Read directly from localStorage instead.
function loadStoredSettings() {
  try {
    var stored = localStorage.getItem('clay-settings');
    if (stored) {
      applySettings(JSON.parse(stored));
    } else {
      console.log('[Tautulli] No Clay settings saved yet — configure via app settings');
    }
  } catch(e) {
    console.log('[Tautulli] Failed to load settings from localStorage: ' + e);
  }
}

// ─── Ready ───────────────────────────────────────────────────
Pebble.addEventListener('ready', function() {
  console.log('[Tautulli] ready');
  loadStoredSettings();
});

// ─── Settings saved via Clay config UI ───────────────────────
// Clay's own webviewclosed handler saves to localStorage first,
// then we defer 100ms and reload from there.
Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) return;
  setTimeout(loadStoredSettings, 100);
});
