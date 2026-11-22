/*
 * Stash Native Player - UI plugin
 *
 * When Stash is opened inside the Stash Native Player desktop app, this plugin
 * replaces the stock video.js ScenePlayer with a thin HTML control layer. The
 * video itself is decoded and drawn by mpv in the desktop app, directly behind
 * a transparent "hole" in the page. In a normal browser nothing is changed.
 */
(function () {
  "use strict";

  var api = window.PluginApi;
  if (!api) return;

  // Only activate inside the desktop app, which injects qwebchannel.js and
  // exposes a Qt WebChannel transport.
  var transport = window.qt && window.qt.webChannelTransport;
  if (!transport || typeof window.QWebChannel !== "function") return;

  var React = api.React;
  var h = React.createElement;
  var useState = React.useState;
  var useEffect = React.useEffect;
  var useRef = React.useRef;
  var useCallback = React.useCallback;
  var useMemo = React.useMemo;

  var FA = api.libraries.FontAwesomeSolid;
  var Icon = api.libraries.ReactFontAwesome.FontAwesomeIcon;

  var SEEK_SMALL = 5;
  var SEEK_LARGE = 30;
  var HIDE_CONTROLS_MS = 2500;
  var SEND_ACTIVITY_EVERY = 10; // seconds, same as the stock player

  // ---------------------------------------------------------------------------
  // Native bridge
  // ---------------------------------------------------------------------------

  var bridgePromise = new Promise(function (resolve) {
    // eslint-disable-next-line no-new
    new window.QWebChannel(transport, function (channel) {
      resolve(channel.objects.stashNative);
    });
  });

  // Only one player instance may drive the native player at a time. When the
  // user navigates from one scene to another, the new instance takes over and
  // the old one must not stop playback on unmount.
  // "Fill window" mode is remembered while the app runs, so moving to the next
  // scene keeps the player filling the window.
  var fillWindowMode = false;

  var ownerSeq = 0;
  var currentOwner = 0;

  function useBridge() {
    var s = useState(null);
    var bridge = s[0];
    var setBridge = s[1];
    useEffect(function () {
      var alive = true;
      bridgePromise.then(function (b) {
        if (alive) setBridge(b);
      });
      return function () {
        alive = false;
      };
    }, []);
    return bridge;
  }

  // ---------------------------------------------------------------------------
  // Hole punching: make the page transparent where the video sits so the native
  // video layer underneath the web view shows through.
  //
  // Inside the app the page root is kept transparent at all times (class
  // "snp-app", which the app adds as soon as the document is created) and the
  // page background is painted by a fixed html::before layer instead. Chromium
  // skips clearing layers it considers opaque, so if the root started out
  // opaque, pixels of removed content (e.g. the poster) would stay visible.
  // ---------------------------------------------------------------------------

  (function setUpTransparentRoot() {
    var root = document.documentElement;
    root.classList.remove("snp-app");
    var cs = getComputedStyle(document.body || root);
    var bg = cs.backgroundColor;
    if (!bg || bg === "rgba(0, 0, 0, 0)" || bg === "transparent") {
      bg = getComputedStyle(root).backgroundColor;
    }
    if (!bg || bg === "rgba(0, 0, 0, 0)" || bg === "transparent") bg = "#202b33";
    root.style.setProperty("--snp-bg", bg);
    if (cs.backgroundImage && cs.backgroundImage !== "none") {
      root.style.setProperty("--snp-bg-image", cs.backgroundImage);
    }
    root.classList.add("snp-app");
  })();

  var hole = {
    active: false,
    cleared: [],
    lastPath: "",
  };

  function isTransparent(cs) {
    return (
      (cs.backgroundColor === "transparent" ||
        cs.backgroundColor === "rgba(0, 0, 0, 0)") &&
      cs.backgroundImage === "none"
    );
  }

  function holeActivate(videoEl) {
    if (hole.active) holeDeactivate();
    // Any ancestor of the video box that paints a background would cover the
    // hole, so clear those while the native player is active.
    var cleared = [];
    for (var n = videoEl.parentElement; n && n !== document.body; n = n.parentElement) {
      if (!isTransparent(getComputedStyle(n))) {
        cleared.push([
          n,
          n.style.getPropertyValue("background"),
          n.style.getPropertyPriority("background"),
        ]);
        n.style.setProperty("background", "transparent", "important");
      }
    }
    document.documentElement.classList.add("snp-active");
    hole.active = true;
    hole.cleared = cleared;
    hole.lastPath = "";
  }

  function holeUpdate(rect) {
    if (!hole.active) return;
    var W = window.innerWidth;
    var H = window.innerHeight;
    var p = "M0 0H" + W + "V" + H + "H0Z";
    if (rect && rect.width > 0 && rect.height > 0) {
      p +=
        "M" + rect.x + " " + rect.y +
        "h" + rect.width + "v" + rect.height +
        "h" + -rect.width + "Z";
    }
    if (p !== hole.lastPath) {
      hole.lastPath = p;
      document.documentElement.style.setProperty("--snp-hole", 'path(evenodd, "' + p + '")');
    }
  }

  function holeDeactivate() {
    if (!hole.active) return;
    var root = document.documentElement;
    root.classList.remove("snp-active");
    root.style.removeProperty("--snp-hole");
    hole.cleared.forEach(function (entry) {
      entry[0].style.setProperty("background", entry[1], entry[2]);
    });
    hole.active = false;
    hole.cleared = [];
  }

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------

  function formatTime(sec) {
    if (!isFinite(sec) || sec < 0) sec = 0;
    sec = Math.floor(sec);
    var hh = Math.floor(sec / 3600);
    var mm = Math.floor((sec % 3600) / 60);
    var ss = sec % 60;
    var two = function (v) {
      return (v < 10 ? "0" : "") + v;
    };
    return hh > 0 ? hh + ":" + two(mm) + ":" + two(ss) : mm + ":" + two(ss);
  }

  function absoluteUrl(path) {
    try {
      return new URL(path, window.location.href).toString();
    } catch (e) {
      return path;
    }
  }

  function storageGet(key, fallback) {
    try {
      var v = window.localStorage.getItem(key);
      return v === null ? fallback : JSON.parse(v);
    } catch (e) {
      return fallback;
    }
  }

  function storageSet(key, value) {
    try {
      window.localStorage.setItem(key, JSON.stringify(value));
    } catch (e) {
      /* ignore */
    }
  }

  function clamp(v, lo, hi) {
    return Math.min(hi, Math.max(lo, v));
  }

  function trackLabel(t) {
    var parts = [];
    if (t.title) parts.push(t.title);
    if (t.lang) parts.push("[" + t.lang + "]");
    if (t.codec) parts.push("(" + t.codec + ")");
    if (!parts.length) parts.push("Track " + t.id);
    return parts.join(" ");
  }

  // ---------------------------------------------------------------------------
  // Seek bar with sprite thumbnails and marker ticks
  // ---------------------------------------------------------------------------

  function SeekBar(props) {
    var duration = props.duration;
    var position = props.position;
    var sprites = props.sprites;
    var markers = props.markers;
    var onSeek = props.onSeek;

    var barRef = useRef(null);
    var hs = useState(null); // hover position in seconds
    var hover = hs[0];
    var setHover = hs[1];
    var ds = useState(null); // drag position in seconds
    var drag = ds[0];
    var setDrag = ds[1];

    var toSeconds = useCallback(
      function (clientX) {
        var el = barRef.current;
        if (!el || !duration) return 0;
        var r = el.getBoundingClientRect();
        return clamp((clientX - r.left) / r.width, 0, 1) * duration;
      },
      [duration]
    );

    function onPointerDown(e) {
      if (e.button !== 0) return;
      e.preventDefault();
      e.stopPropagation();
      var el = barRef.current;
      el.setPointerCapture(e.pointerId);
      setDrag(toSeconds(e.clientX));
    }
    function onPointerMove(e) {
      var s = toSeconds(e.clientX);
      setHover(s);
      if (drag !== null) setDrag(s);
    }
    function onPointerUp(e) {
      if (drag === null) return;
      e.stopPropagation();
      onSeek(toSeconds(e.clientX));
      setDrag(null);
    }

    var shown = drag !== null ? drag : position;
    var pct = duration ? (shown / duration) * 100 : 0;
    var hoverSec = drag !== null ? drag : hover;

    var preview = null;
    if (hoverSec !== null && duration) {
      var sprite = null;
      if (sprites) {
        for (var i = 0; i < sprites.length; i++) {
          if (hoverSec >= sprites[i].start && hoverSec < sprites[i].end) {
            sprite = sprites[i];
            break;
          }
        }
      }
      var leftPct = (hoverSec / duration) * 100;
      preview = h(
        "div",
        { className: "snp-preview", style: { left: leftPct + "%" } },
        sprite
          ? h("div", {
              className: "snp-preview-img",
              style: {
                width: sprite.w + "px",
                height: sprite.h + "px",
                backgroundImage: "url('" + sprite.url + "')",
                backgroundPosition: -sprite.x + "px " + -sprite.y + "px",
              },
            })
          : null,
        h("div", { className: "snp-preview-time" }, formatTime(hoverSec))
      );
    }

    return h(
      "div",
      {
        className: "snp-seek",
        ref: barRef,
        onPointerDown: onPointerDown,
        onPointerMove: onPointerMove,
        onPointerUp: onPointerUp,
        onPointerLeave: function () {
          setHover(null);
        },
      },
      h(
        "div",
        { className: "snp-seek-track" },
        h("div", { className: "snp-seek-fill", style: { width: pct + "%" } }),
        duration
          ? markers.map(function (m) {
              var left = (m.seconds / duration) * 100;
              var width =
                m.end_seconds != null && m.end_seconds > m.seconds
                  ? ((m.end_seconds - m.seconds) / duration) * 100
                  : 0;
              return h("div", {
                key: m.id,
                className: width ? "snp-marker snp-marker-range" : "snp-marker",
                style: width
                  ? { left: left + "%", width: width + "%" }
                  : { left: left + "%" },
                title: m.title || (m.primary_tag && m.primary_tag.name) || "",
                onPointerDown: function (e) {
                  e.stopPropagation();
                },
                onClick: function (e) {
                  e.stopPropagation();
                  onSeek(m.seconds);
                },
              });
            })
          : null
      ),
      h("div", { className: "snp-seek-thumb", style: { left: pct + "%" } }),
      preview
    );
  }

  // ---------------------------------------------------------------------------
  // Small popup menu (speed / audio / subtitles)
  // ---------------------------------------------------------------------------

  function Menu(props) {
    return h(
      "div",
      {
        className: "snp-menu",
        onPointerDown: function (e) {
          e.stopPropagation();
        },
        onClick: function (e) {
          e.stopPropagation();
        },
      },
      props.sections.map(function (sec) {
        return h(
          "div",
          { key: sec.title, className: "snp-menu-section" },
          h("div", { className: "snp-menu-title" }, sec.title),
          sec.items.map(function (it) {
            return h(
              "button",
              {
                key: String(it.key),
                type: "button",
                className: "snp-menu-item" + (it.selected ? " selected" : ""),
                onClick: function () {
                  it.onSelect();
                  props.onClose();
                },
              },
              h(Icon, { icon: FA.faCheck, className: "snp-menu-check", fixedWidth: true }),
              it.label
            );
          })
        );
      })
    );
  }

  // ---------------------------------------------------------------------------
  // The replacement ScenePlayer
  // ---------------------------------------------------------------------------

  function NativeScenePlayer(props) {
    var scene = props.scene;
    var file = scene.files && scene.files[0];
    var bridge = useBridge();

    var cfgQuery = api.GQL.useConfigurationQuery();
    var config = cfgQuery.data && cfgQuery.data.configuration;
    var uiConfig = (config && config.ui) || {};
    var interfaceConfig = (config && config.interface) || {};
    var trackActivity = uiConfig.trackActivity !== false;
    var minimumPlayPercent = uiConfig.minimumPlayPercent || 0;

    var saveActivityMutation = api.utils.StashService.useSceneSaveActivity()[0];
    var addPlayMutation = api.utils.StashService.useSceneIncrementPlayCount()[0];

    var sprites = api.hooks.useSpriteInfo(scene.paths && scene.paths.vtt ? scene.paths.vtt : undefined);

    var rootRef = useRef(null);
    var videoRef = useRef(null);
    var ownerRef = useRef(0);
    var autoStartedFor = useRef(null);

    var st = useState(false);
    var started = st[0];
    var setStarted = st[1];

    var ps = useState({
      position: 0,
      duration: (file && file.duration) || 0,
      paused: true,
      buffering: false,
      volume: storageGet("snp-volume", 100),
      muted: storageGet("snp-muted", false),
      speed: 1,
    });
    var playback = ps[0];
    var setPlayback = ps[1];

    var ts = useState([]);
    var tracks = ts[0];
    var setTracks = ts[1];

    var es = useState(null);
    var error = es[0];
    var setError = es[1];

    var cs = useState(true);
    var controlsVisible = cs[0];
    var setControlsVisible = cs[1];

    var ms = useState(null); // open menu: "settings" | null
    var openMenu = ms[0];
    var setOpenMenu = ms[1];

    var fs = useState(false);
    var fullscreen = fs[0];
    var setFullscreen = fs[1];

    var fw = useState(fillWindowMode);
    var fillWindow = fw[0];
    var setFillWindowState = fw[1];
    var setFillWindow = useCallback(function (on) {
      fillWindowMode = on;
      setFillWindowState(on);
    }, []);

    useEffect(
      function () {
        document.documentElement.classList.toggle("snp-fill-active", fillWindow);
        if (!fillWindow) return undefined;
        function onEsc(e) {
          if (e.key === "Escape" && !document.fullscreenElement) {
            e.preventDefault();
            setFillWindow(false);
          }
        }
        document.addEventListener("keydown", onEsc, true);
        return function () {
          document.removeEventListener("keydown", onEsc, true);
          document.documentElement.classList.remove("snp-fill-active");
        };
      },
      [fillWindow, setFillWindow]
    );

    var playbackRef = useRef(playback);
    playbackRef.current = playback;
    var propsRef = useRef(props);
    propsRef.current = props;

    // ---- activity tracking (mirrors the stock track-activity plugin) -------
    var activity = useRef({ total: 0, current: 0, playCounted: false });

    var sendActivity = useCallback(
      function () {
        var a = activity.current;
        var pb = playbackRef.current;
        if (!trackActivity || a.total <= 0 || !pb.duration) return;
        var resume = pb.position;
        var percentCompleted = (100 / pb.duration) * resume;
        var percentPlayed = (100 / pb.duration) * a.total;
        if (!a.playCounted && percentPlayed >= minimumPlayPercent) {
          a.playCounted = true;
          addPlayMutation({ variables: { id: scene.id } }).catch(function () {});
        }
        if (percentCompleted >= 98) resume = 0;
        saveActivityMutation({
          variables: { id: scene.id, resume_time: resume, playDuration: a.current },
        }).catch(function () {});
        a.current = 0;
      },
      [scene.id, trackActivity, minimumPlayPercent, saveActivityMutation, addPlayMutation]
    );

    useEffect(
      function () {
        if (!started || playback.paused || !trackActivity) return undefined;
        var id = window.setInterval(function () {
          var a = activity.current;
          a.total += 1;
          a.current += 1;
          if (a.total % SEND_ACTIVITY_EVERY === 0) sendActivity();
        }, 1000);
        return function () {
          window.clearInterval(id);
        };
      },
      [started, playback.paused, trackActivity, sendActivity]
    );

    // save activity when pausing
    var wasPaused = useRef(true);
    useEffect(
      function () {
        if (started && playback.paused && !wasPaused.current) sendActivity();
        wasPaused.current = playback.paused;
      },
      [started, playback.paused, sendActivity]
    );

    // ---- start / load -------------------------------------------------------
    var begin = useCallback(
      function (startAt) {
        if (!bridge || !file || !scene.paths.stream) return;
        autoStartedFor.current = scene.id;
        ownerRef.current = ++ownerSeq;
        currentOwner = ownerRef.current;
        setError(null);
        setStarted(true);
        var pb = playbackRef.current;
        bridge.setVolume(pb.volume);
        bridge.setMuted(pb.muted);
        bridge.load(absoluteUrl(scene.paths.stream), startAt || 0, false);

        if (scene.paths.caption && scene.captions && scene.captions.length) {
          var lang = (navigator.language || "en").split(/[-_]/)[0];
          scene.captions.forEach(function (c) {
            var u = new URL(scene.paths.caption, window.location.href);
            u.searchParams.set("lang", c.language_code);
            u.searchParams.set("type", c.caption_type);
            bridge.addSubtitle(
              u.toString(),
              c.language_code + " (" + c.caption_type + ")",
              c.language_code,
              c.language_code === lang
            );
          });
        }
        if (rootRef.current) rootRef.current.focus({ preventScroll: true });
      },
      [bridge, file, scene]
    );

    // Initial position / autostart, re-evaluated whenever the scene changes.
    var startPosition = useMemo(
      function () {
        var initial = props.initialTimestamp || 0;
        if (initial) return initial;
        var resume = scene.resume_time || 0;
        if (!uiConfig.alwaysStartFromBeginning && file && file.duration > resume) return resume;
        return 0;
      },
      // eslint-disable-next-line react-hooks/exhaustive-deps
      [scene.id, props.initialTimestamp, uiConfig.alwaysStartFromBeginning]
    );

    useEffect(
      function () {
        // new scene: reset state
        activity.current = { total: 0, current: 0, playCounted: false };
        setStarted(false);
        setTracks([]);
        setPlayback(function (p) {
          return Object.assign({}, p, {
            position: startPosition,
            duration: (file && file.duration) || 0,
            paused: true,
            buffering: false,
          });
        });
      },
      // eslint-disable-next-line react-hooks/exhaustive-deps
      [scene.id]
    );

    var autoplayWanted =
      !!props.autoplay || !!interfaceConfig.autostartVideo || (props.initialTimestamp || 0) > 0;

    // Autostart once per scene, as soon as the bridge and configuration are
    // available. begin() also records the scene so a manual start is not
    // repeated here.
    useEffect(
      function () {
        if (!bridge || !cfgQuery.data) return;
        if (autoStartedFor.current === scene.id) return;
        if (autoplayWanted) begin(startPosition);
      },
      // eslint-disable-next-line react-hooks/exhaustive-deps
      [bridge, cfgQuery.data, scene.id]
    );

    // ---- native events -----------------------------------------------------
    useEffect(
      function () {
        if (!bridge) return undefined;
        function onState(s) {
          if (currentOwner !== ownerRef.current) return;
          setPlayback(function (p) {
            var n = Object.assign({}, p);
            Object.keys(s).forEach(function (k) {
              if (s[k] !== null && s[k] !== undefined) n[k] = s[k];
            });
            if (!n.duration && file) n.duration = file.duration;
            return n;
          });
        }
        function onTracks(list) {
          if (currentOwner !== ownerRef.current) return;
          setTracks(list || []);
        }
        function onEnd() {
          if (currentOwner !== ownerRef.current) return;
          sendActivity();
          if (propsRef.current.onComplete) propsRef.current.onComplete();
        }
        function onError(msg) {
          if (currentOwner !== ownerRef.current) return;
          setError(msg || "Playback failed");
        }
        bridge.stateChanged.connect(onState);
        bridge.tracksChanged.connect(onTracks);
        bridge.endReached.connect(onEnd);
        bridge.errorOccurred.connect(onError);
        return function () {
          bridge.stateChanged.disconnect(onState);
          bridge.tracksChanged.disconnect(onTracks);
          bridge.endReached.disconnect(onEnd);
          bridge.errorOccurred.disconnect(onError);
        };
      },
      [bridge, file, sendActivity]
    );

    // ---- stop on unmount / scene change ------------------------------------
    useEffect(
      function () {
        return function () {
          if (ownerRef.current && currentOwner === ownerRef.current) {
            sendActivity();
            currentOwner = 0;
            bridgePromise.then(function (b) {
              if (currentOwner === 0) {
                b.stop();
                b.setVideoGeometry(0, 0, 0, 0, false);
              }
            });
          }
          ownerRef.current = 0;
        };
      },
      // eslint-disable-next-line react-hooks/exhaustive-deps
      [scene.id]
    );

    // ---- geometry sync + hole punching -------------------------------------
    useEffect(
      function () {
        if (!started || !bridge || !videoRef.current) return undefined;
        holeActivate(videoRef.current);
        var last = "";
        var raf = 0;
        function tick() {
          var el = videoRef.current;
          if (el) {
            var r = el.getBoundingClientRect();
            var visible = !document.hidden && r.width > 1 && r.height > 1;
            var rect = {
              x: Math.round(r.left),
              y: Math.round(r.top),
              width: Math.round(r.width),
              height: Math.round(r.height),
            };
            var key = rect.x + "," + rect.y + "," + rect.width + "," + rect.height + "," + visible + "," + window.innerWidth + "," + window.innerHeight;
            if (key !== last) {
              last = key;
              holeUpdate(visible ? rect : null);
              bridge.setVideoGeometry(rect.x, rect.y, rect.width, rect.height, visible);
            }
          }
          raf = window.requestAnimationFrame(tick);
        }
        tick();
        return function () {
          window.cancelAnimationFrame(raf);
          holeDeactivate();
        };
      },
      [started, bridge]
    );

    // ---- seeking from outside (markers panel etc.) -------------------------
    var startedRef = useRef(started);
    startedRef.current = started;
    var beginRef = useRef(begin);
    beginRef.current = begin;

    useEffect(
      function () {
        var send = props.sendSetTimestamp;
        if (!send) return undefined;
        send(function (value) {
          if (startedRef.current && bridge) {
            bridge.seek(value);
            bridge.setPaused(false);
          } else {
            beginRef.current(value);
          }
        });
        return function () {
          send(function () {});
        };
      },
      [props.sendSetTimestamp, bridge]
    );

    // ---- controls ----------------------------------------------------------
    var hideTimer = useRef(0);
    var poke = useCallback(function () {
      setControlsVisible(true);
      window.clearTimeout(hideTimer.current);
      hideTimer.current = window.setTimeout(function () {
        setControlsVisible(false);
      }, HIDE_CONTROLS_MS);
    }, []);
    useEffect(function () {
      return function () {
        window.clearTimeout(hideTimer.current);
      };
    }, []);

    var showControls = !started || playback.paused || controlsVisible || !!openMenu;

    function togglePause() {
      if (!started) {
        begin(startPosition);
        return;
      }
      bridge.setPaused(!playback.paused);
    }

    function seekTo(sec) {
      if (!started) {
        begin(sec);
        return;
      }
      sec = clamp(sec, 0, playback.duration || sec);
      setPlayback(function (p) {
        return Object.assign({}, p, { position: sec });
      });
      bridge.seek(sec);
    }

    function setVolume(v) {
      v = clamp(Math.round(v), 0, 100);
      storageSet("snp-volume", v);
      storageSet("snp-muted", false);
      setPlayback(function (p) {
        return Object.assign({}, p, { volume: v, muted: false });
      });
      if (bridge) {
        bridge.setVolume(v);
        bridge.setMuted(false);
      }
    }

    function toggleMute() {
      var m = !playback.muted;
      storageSet("snp-muted", m);
      setPlayback(function (p) {
        return Object.assign({}, p, { muted: m });
      });
      if (bridge) bridge.setMuted(m);
    }

    function setSpeed(s) {
      setPlayback(function (p) {
        return Object.assign({}, p, { speed: s });
      });
      if (bridge) bridge.setSpeed(s);
    }

    function toggleFullscreen() {
      var el = rootRef.current;
      if (!el) return;
      if (document.fullscreenElement) {
        document.exitFullscreen().catch(function () {});
      } else {
        el.requestFullscreen().catch(function () {});
      }
    }

    useEffect(function () {
      function onFs() {
        setFullscreen(document.fullscreenElement === rootRef.current);
      }
      document.addEventListener("fullscreenchange", onFs);
      return function () {
        document.removeEventListener("fullscreenchange", onFs);
      };
    }, []);

    function onKeyDown(e) {
      if (e.altKey || e.ctrlKey || e.metaKey) return;
      var t = e.target;
      if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.isContentEditable)) return;
      var handled = true;
      switch (e.key) {
        case " ":
          togglePause();
          break;
        case "ArrowLeft":
          seekTo(playback.position - (e.shiftKey ? SEEK_LARGE : SEEK_SMALL));
          break;
        case "ArrowRight":
          seekTo(playback.position + (e.shiftKey ? SEEK_LARGE : SEEK_SMALL));
          break;
        case "ArrowUp":
          setVolume(playback.volume + 5);
          break;
        case "ArrowDown":
          setVolume(playback.volume - 5);
          break;
        case "f":
        case "F":
          toggleFullscreen();
          break;
        case "m":
        case "M":
          toggleMute();
          break;
        case "t":
        case "T":
          setFillWindow(!fillWindow);
          break;
        default:
          handled = false;
      }
      if (handled) {
        e.preventDefault();
        e.stopPropagation();
        poke();
      }
    }

    // ---- menus -------------------------------------------------------------
    var menuSections = [];
    if (openMenu === "settings") {
      menuSections.push({
        title: "Speed",
        items: [0.5, 0.75, 1, 1.25, 1.5, 2].map(function (s) {
          return {
            key: s,
            label: s === 1 ? "Normal" : s + "x",
            selected: playback.speed === s,
            onSelect: function () {
              setSpeed(s);
            },
          };
        }),
      });
      var audio = tracks.filter(function (t) {
        return t.type === "audio";
      });
      if (audio.length > 1) {
        menuSections.push({
          title: "Audio",
          items: audio.map(function (t) {
            return {
              key: "a" + t.id,
              label: trackLabel(t),
              selected: !!t.selected,
              onSelect: function () {
                bridge.setTrack("aid", t.id);
              },
            };
          }),
        });
      }
      var subs = tracks.filter(function (t) {
        return t.type === "sub";
      });
      if (subs.length) {
        menuSections.push({
          title: "Subtitles",
          items: [
            {
              key: "s-off",
              label: "Off",
              selected: !subs.some(function (t) {
                return t.selected;
              }),
              onSelect: function () {
                bridge.setTrack("sid", 0);
              },
            },
          ].concat(
            subs.map(function (t) {
              return {
                key: "s" + t.id,
                label: trackLabel(t),
                selected: !!t.selected,
                onSelect: function () {
                  bridge.setTrack("sid", t.id);
                },
              };
            })
          ),
        });
      }
    }

    // ---- render ------------------------------------------------------------
    var isPortrait = file && file.height && file.width && file.height > file.width;
    var markers = scene.scene_markers || [];

    if (!file) {
      // Fall back to the stock "no file" rendering.
      return h("div", { className: "VideoPlayer no-file" }, h("div", { className: "video-wrapper" }));
    }

    var btn = function (icon, title, onClick, extra) {
      return h(
        "button",
        {
          type: "button",
          className: "snp-btn" + (extra ? " " + extra : ""),
          title: title,
          onClick: function (e) {
            e.stopPropagation();
            onClick();
          },
        },
        h(Icon, { icon: icon, fixedWidth: true })
      );
    };

    var volumeIcon = playback.muted || playback.volume === 0
      ? FA.faVolumeMute
      : playback.volume < 50
      ? FA.faVolumeDown
      : FA.faVolumeUp;

    return h(
      "div",
      {
        className:
          "VideoPlayer snp-root" +
          (isPortrait ? " portrait" : "") +
          (started ? " snp-started" : "") +
          (showControls ? "" : " snp-idle") +
          (fullscreen ? " snp-fullscreen" : "") +
          (fillWindow ? " snp-fill" : ""),
        ref: rootRef,
        tabIndex: 0,
        onKeyDown: onKeyDown,
        onMouseMove: poke,
        onPointerDown: function (e) {
          // close the menu when clicking elsewhere in the player (the settings
          // button toggles it itself)
          if (openMenu && !(e.target.closest && e.target.closest(".snp-menu-anchor"))) {
            setOpenMenu(null);
          }
        },
      },
      h(
        "div",
        {
          className: "video-wrapper snp-video",
          ref: videoRef,
          onClick: function () {
            if (openMenu) {
              setOpenMenu(null);
              return;
            }
            togglePause();
          },
          onDoubleClick: toggleFullscreen,
        },
        !started
          ? h(
              "div",
              {
                className: "snp-poster",
                style: scene.paths.screenshot
                  ? { backgroundImage: "url('" + scene.paths.screenshot + "')" }
                  : undefined,
              },
              h(
                "div",
                { className: "snp-bigplay" },
                h(Icon, { icon: FA.faPlay })
              )
            )
          : null,
        started && playback.buffering
          ? h("div", { className: "snp-spinner" }, h(Icon, { icon: FA.faSpinner, spin: true }))
          : null,
        error
          ? h(
              "div",
              { className: "snp-error" },
              h(Icon, { icon: FA.faExclamationTriangle }),
              " ",
              error
            )
          : null
      ),
      h(
        "div",
        {
          className: "snp-controls",
          onClick: function (e) {
            e.stopPropagation();
          },
        },
        h(SeekBar, {
          duration: playback.duration,
          position: playback.position,
          sprites: sprites || null,
          markers: markers,
          onSeek: seekTo,
        }),
        h(
          "div",
          { className: "snp-row" },
          btn(FA.faStepBackward, "Previous", function () {
            if (props.onPrevious) props.onPrevious();
          }),
          btn(
            started && !playback.paused ? FA.faPause : FA.faPlay,
            started && !playback.paused ? "Pause (space)" : "Play (space)",
            togglePause
          ),
          btn(FA.faStepForward, "Next", function () {
            if (props.onNext) props.onNext();
          }),
          h(
            "div",
            { className: "snp-volume" },
            btn(volumeIcon, "Mute (m)", toggleMute),
            h("input", {
              type: "range",
              min: 0,
              max: 100,
              value: playback.muted ? 0 : playback.volume,
              onChange: function (e) {
                setVolume(Number(e.target.value));
              },
              onKeyDown: function (e) {
                e.stopPropagation();
              },
            })
          ),
          h(
            "div",
            { className: "snp-time" },
            formatTime(playback.position) + " / " + formatTime(playback.duration)
          ),
          h("div", { className: "snp-spacer" }),
          playback.speed !== 1 ? h("div", { className: "snp-badge" }, playback.speed + "x") : null,
          h(
            "div",
            { className: "snp-menu-anchor" },
            btn(FA.faCog, "Settings", function () {
              setOpenMenu(openMenu ? null : "settings");
            }),
            openMenu
              ? h(Menu, {
                  sections: menuSections,
                  onClose: function () {
                    setOpenMenu(null);
                  },
                })
              : null
          ),
          btn(
            fillWindow ? FA.faWindowRestore : FA.faWindowMaximize,
            fillWindow ? "Exit fill window (t / Esc)" : "Fill window (t)",
            function () {
              setFillWindow(!fillWindow);
            },
            fillWindow ? "snp-active-btn" : ""
          ),
          btn(fullscreen ? FA.faCompress : FA.faExpand, "Fullscreen (f)", toggleFullscreen)
        )
      )
    );
  }

  api.patch.instead("ScenePlayer", function () {
    var props = arguments[0];
    return h(NativeScenePlayer, props);
  });
})();
