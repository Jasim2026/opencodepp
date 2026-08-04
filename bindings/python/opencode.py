"""opencode.py -- Python (ctypes) binding for the OpenCode++ C ABI.

Pure standard library, no build step: it dlopens libopencodepp and calls the
frozen C ABI from include/opencode/opencode.h. It is the Python binding from
Phase 12 task 5 (dependency policy: stdlib only, no cffi).

Locate the library with the OPENCODE_LIB environment variable, or let ctypes
try the usual CMake output paths for libopencodepp.so.

Example:
    import opencode
    cfg = opencode.Config(workspace="/tmp/ws",
                          base_url="http://127.0.0.1:8123")
    eng = opencode.Engine(cfg)
    out = eng.run("write a file")
    print(out.status, out.text)
"""

import ctypes
import os

__all__ = [
    "ABI_VERSION", "Status", "Policy", "MemoryKind", "EventKind", "Lane",
    "MetricKind", "Config", "Event", "Engine", "LibError",
]

_here = os.path.dirname(os.path.abspath(__file__))


def _default_lib_paths():
    paths = [
        os.environ.get("OPENCODE_LIB", ""),
        os.path.join(_here, "libopencodepp.so"),
        os.path.join(_here, "..", "..", "build", "dev", "src", "libopencodepp.so"),
        os.path.join(_here, "..", "..", "build", "release", "src", "libopencodepp.so"),
        os.path.join(_here, "..", "..", "build", "dev", "libopencodepp.so"),
        "/usr/local/lib/libopencodepp.so",
        "/usr/lib/libopencodepp.so",
    ]
    return [p for p in paths if p]


def _load():
    last = None
    for p in _default_lib_paths():
        try:
            return ctypes.CDLL(p)
        except OSError as exc:
            last = exc
    raise LibError("cannot load libopencodepp.so (set OPENCODE_LIB): %s" % last)


class LibError(RuntimeError):
    """Raised when the shared library cannot be loaded."""


_lib = _load()

# --------------------------------------------------------------------------
# Constants (mirror include/opencode/opencode.h -- FROZEN v1)
# --------------------------------------------------------------------------

ABI_VERSION = _lib.opencode_abi_version() if hasattr(_lib, "opencode_abi_version") else 1


class Status:
    OK = 0
    NETWORK = 1
    AUTH = 2
    VALIDATION = 3
    BUSY = 4
    CANCELLED = 5
    FATAL = 6
    NO_NETWORK = 7

    NAMES = {
        OK: "ok", NETWORK: "network error", AUTH: "auth error",
        VALIDATION: "validation error", BUSY: "busy", CANCELLED: "cancelled",
        FATAL: "fatal", NO_NETWORK: "offline",
    }


class Policy:
    DENY = 0
    ASK = 1
    ALLOW = 2
    ALLOW_READONLY = 3


class MemoryKind:
    DECISION = 0
    FACT = 1
    TASK_STATE = 2
    REPO_RULE = 3
    LESSON = 4
    USER_PREF = 5


class EventKind:
    LOG = 1
    PREPARING = 2
    CONNECTING = 3
    STREAMING = 4
    TOOL_PHASE = 5
    VERIFYING = 6
    APPLYING = 7
    DONE = 8
    FAILED = 9
    CANCELLED = 10
    FOLD = 11

    NAMES = {
        LOG: "log", PREPARING: "preparing", CONNECTING: "connecting",
        STREAMING: "streaming", TOOL_PHASE: "tool", VERIFYING: "verifying",
        APPLYING: "applying", DONE: "done", FAILED: "failed",
        CANCELLED: "cancelled", FOLD: "fold",
    }


class Lane:
    NONE = 0
    BACKOFF = 1
    OFFLINE = 2
    PAUSED = 3


class MetricKind:
    COUNTER = 0
    GAUGE = 1
    HISTOGRAM = 2


EVENT_TEXT_MAX = 4096

# --------------------------------------------------------------------------
# ctypes structs (field order must match the header exactly)
# --------------------------------------------------------------------------

_opencode_event_fn = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                      ctypes.c_void_p)
_opencode_permission_fn = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                           ctypes.c_char_p, ctypes.c_char_p)
_opencode_consent_fn = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                        ctypes.c_char_p)
_opencode_log_fn = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int,
                                    ctypes.c_char_p)
_opencode_metric_fn = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p,
                                       ctypes.c_int, ctypes.c_double,
                                       ctypes.c_uint64)


class _EventStruct(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("session_id", ctypes.c_uint32),
        ("kind", ctypes.c_int),
        ("text", ctypes.c_char_p),
        ("text_cap", ctypes.c_size_t),
        ("text_len", ctypes.c_size_t),
        ("data_i64", ctypes.c_int64),
        ("status", ctypes.c_int32),
        ("lane", ctypes.c_int),
    ]


class _ConfigStruct(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint32),
        ("workspace", ctypes.c_char_p),
        ("config_path", ctypes.c_char_p),
        ("prompt_dir", ctypes.c_char_p),
        ("provider", ctypes.c_char_p),
        ("base_url", ctypes.c_char_p),
        ("api_key", ctypes.c_char_p),
        ("model", ctypes.c_char_p),
        ("agent", ctypes.c_char_p),
        ("network_timeout_ms", ctypes.c_uint32),
        ("tool_policy", ctypes.c_int),
        ("memory_max_entries", ctypes.c_uint32),
        ("memory_max_entries_per_task", ctypes.c_uint32),
        ("memory_max_value_chars", ctypes.c_uint32),
        ("on_event", _opencode_event_fn),
        ("on_permission", _opencode_permission_fn),
        ("on_consent", _opencode_consent_fn),
        ("on_log", _opencode_log_fn),
        ("userdata", ctypes.c_void_p),
    ]


_engine_t = ctypes.c_void_p

# Signatures ----------------------------------------------------------------
_lib.opencode_engine_create.restype = ctypes.c_int
_lib.opencode_engine_create.argtypes = [ctypes.POINTER(_ConfigStruct),
                                        ctypes.POINTER(_engine_t)]
_lib.opencode_engine_destroy.restype = ctypes.c_int
_lib.opencode_engine_destroy.argtypes = [_engine_t]
_lib.opencode_engine_set_config.restype = ctypes.c_int
_lib.opencode_engine_set_config.argtypes = [_engine_t, ctypes.POINTER(_ConfigStruct)]
_lib.opencode_engine_drive.restype = ctypes.c_int
_lib.opencode_engine_drive.argtypes = [_engine_t, ctypes.c_int32]
_lib.opencode_engine_run.restype = ctypes.c_int
_lib.opencode_engine_run.argtypes = [_engine_t, ctypes.c_char_p,
                                    _opencode_event_fn, ctypes.c_void_p]
_lib.opencode_engine_cancel.restype = ctypes.c_int
_lib.opencode_engine_cancel.argtypes = [_engine_t]
_lib.opencode_metrics_snapshot.restype = ctypes.c_int
_lib.opencode_metrics_snapshot.argtypes = [_engine_t, _opencode_metric_fn,
                                          ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
_lib.opencode_memory_write.restype = ctypes.c_int
_lib.opencode_memory_write.argtypes = [_engine_t, ctypes.c_int, ctypes.c_char_p,
                                      ctypes.c_char_p, ctypes.c_char_p,
                                      ctypes.c_char_p, ctypes.c_size_t]
_lib.opencode_memory_read.restype = ctypes.c_int
_lib.opencode_memory_read.argtypes = [_engine_t, ctypes.c_int, ctypes.c_char_p,
                                     ctypes.c_char_p, ctypes.c_size_t,
                                     ctypes.POINTER(ctypes.c_size_t)]


class Config:
    """Build an opencode_config_t. String values are copied by the engine
    before the call returns, so Python strings are safe to pass directly."""

    def __init__(self, workspace=None, config_path=None, prompt_dir=None,
                 provider="openai_compat", base_url="http://127.0.0.1:8123",
                 api_key="sk-test", model="mock-model", agent="default",
                 network_timeout_ms=30000, tool_policy=Policy.ALLOW_READONLY,
                 on_event=None, on_permission=None, on_consent=None,
                 on_log=None):
        self._c = _ConfigStruct()
        self._c.version = 1
        self._c.workspace = (workspace or "/tmp/opencode_py_ws").encode()
        self._c.config_path = (config_path or "").encode() or None
        self._c.prompt_dir = (prompt_dir or "").encode() or None
        self._c.provider = provider.encode()
        self._c.base_url = base_url.encode()
        self._c.api_key = api_key.encode()
        self._c.model = model.encode()
        self._c.agent = agent.encode()
        self._c.network_timeout_ms = network_timeout_ms
        self._c.tool_policy = tool_policy
        if on_event is not None:
            self._c.on_event = on_event
        if on_permission is not None:
            self._c.on_permission = on_permission
        if on_consent is not None:
            self._c.on_consent = on_consent
        if on_log is not None:
            self._c.on_log = on_log
        self._c.userdata = None


class Event:
    """A snapshot of an opencode_event_t delivered to a callback."""

    def __init__(self, raw):
        self._raw = raw
        c = raw.contents
        self.kind = c.kind
        self.session_id = c.session_id
        self.data_i64 = c.data_i64
        self.status = c.status
        self.lane = c.lane
        self.text = ""
        if c.text and c.text_len > 0:
            self.text = c.text[:c.text_len].decode("utf-8", "replace")

    @property
    def kind_name(self):
        return EventKind.NAMES.get(self.kind, "?")


class _Result:
    """Result of a run() call: status code plus the last text."""

    def __init__(self, status, text, saw_done, saw_failed, events):
        self.status = status
        self.status_name = Status.NAMES.get(status, "?")
        self.text = text
        self.saw_done = saw_done
        self.saw_failed = saw_failed
        self.events = events

    def __repr__(self):
        return ("<_Result status=%s saw_done=%s saw_failed=%s>"
                % (self.status_name, self.saw_done, self.saw_failed))


class _CallbackState:
    def __init__(self, cb):
        self.cb = cb
        self.done_text = ""
        self.saw_done = False
        self.saw_failed = False
        self.events = []


def _make_event_callback(state):
    @_opencode_event_fn
    def _cb(userdata, ev_ptr):
        _raw = ctypes.cast(ev_ptr, ctypes.POINTER(_EventStruct))
        # Event snapshots every field (including a copy of the text) while the
        # engine's transient buffer is still valid, so the host may keep the
        # event object beyond the callback.
        ev = Event(_raw)
        if state.cb is not None:
            state.cb(ev)
        if ev.kind == EventKind.DONE:
            state.saw_done = True
            state.done_text = ev.text
        elif ev.kind == EventKind.FAILED:
            state.saw_failed = True
            state.done_text = ev.text
        state.events.append(ev.kind)
        return Status.OK

    return _cb


def _make_metric_callback(sink):
    @_opencode_metric_fn
    def _cb(userdata, name, kind, value, count):
        if sink is not None:
            sink(name.decode("utf-8", "replace"), kind, value, count)

    return _cb


class Engine:
    """RAII handle over opencode_engine_t. Closes on GC/del."""

    def __init__(self, cfg=None):
        self._eng = _engine_t(None)
        cstruct = cfg._c if isinstance(cfg, Config) else None
        rc = _lib.opencode_engine_create(ctypes.byref(cstruct) if cstruct else None,
                                         ctypes.byref(self._eng))
        if rc != Status.OK:
            raise LibError("engine create failed: %s" % Status.NAMES.get(rc))
        self._cfg = cfg

    def __del__(self):
        if self._eng:
            _lib.opencode_engine_destroy(self._eng)
            self._eng = _engine_t(None)

    def valid(self):
        return bool(self._eng)

    def get(self):
        return self._eng

    def set_config(self, cfg):
        cstruct = cfg._c if isinstance(cfg, Config) else None
        arg = ctypes.byref(cstruct) if cstruct is not None else None
        return _lib.opencode_engine_set_config(self._eng, arg)

    def drive(self, wait_ms=-1):
        return _lib.opencode_engine_drive(self._eng, wait_ms)

    def cancel(self):
        return _lib.opencode_engine_cancel(self._eng)

    def run(self, prompt, on_event=None):
        state = _CallbackState(on_event)
        cb = _make_event_callback(state)
        rc = _lib.opencode_engine_run(self._eng, prompt.encode(), cb, None)
        status = rc
        return _Result(status, state.done_text, state.saw_done,
                       state.saw_failed, state.events)

    def metrics(self, sink=None):
        state = {"count": 0}
        cb = _make_metric_callback(sink)
        out = ctypes.c_uint32(0)
        rc = _lib.opencode_metrics_snapshot(self._eng, cb, None,
                                            ctypes.byref(out))
        if rc != Status.OK:
            raise LibError("metrics snapshot failed: %s" % Status.NAMES.get(rc))
        return out.value

    def memory_write(self, kind, key, value, tags=None):
        tags_json = b"[]"
        if tags:
            import json
            tags_json = json.dumps(list(tags)).encode()
        out_id = ctypes.create_string_buffer(64)
        rc = _lib.opencode_memory_write(self._eng, kind, key.encode(),
                                        value.encode(), tags_json,
                                        out_id, len(out_id))
        if rc != Status.OK:
            raise LibError("memory_write failed: %s" % Status.NAMES.get(rc))
        return out_id.value.decode("utf-8", "replace") or None

    def memory_read(self, kind, keywords=None):
        import json
        kw = json.dumps(list(keywords or [])).encode()
        out = ctypes.create_string_buffer(8192)
        out_len = ctypes.c_size_t(0)
        rc = _lib.opencode_memory_read(self._eng, kind, kw, out, len(out),
                                       ctypes.byref(out_len))
        if rc != Status.OK:
            raise LibError("memory_read failed: %s" % Status.NAMES.get(rc))
        return out.value[:out_len.value].decode("utf-8", "replace")
