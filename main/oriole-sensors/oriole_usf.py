# SPDX-License-Identifier: GPL-3.0-or-later
# Minimal client for Google's USF sensor framework on the Pixel 6 AoC.
# Reverse engineered from the stock libusf.so / sensors.usf.so (see README.md).
#
# Transport: /dev/acd-com.google.usf (aoc_channel_dev), one FlatBuffer per
# read()/write(), at most 1020 bytes; larger messages go as fragments.
#
#   UsfMsg      { 0 type: u32, 1 payload: [u8] }
#                 type 1 request, 2 response, 3 fragment, 5 event, 9 sample event
#   UsfMsgReq   { 0 req_type: i32, 1 req_id: i32, 2 server: i32, 3 payload: [u8] }
#   UsfMsgResp  { 0 req_type: i32, 1 req_id: i32, 2 status: i32, 3 payload: [u8] }
#   UsfMsgFrag  { 0 1, 1 total_len: i32, 2 offset: i32, 3 chunk: [u8] }  (of a whole UsfMsg)
import os, select, struct, time

DEV = "/dev/acd-com.google.usf"
SENSOR_MGR_UUID = bytes.fromhex("6b31cbf0744a14a1d4e8ae8a3e09fc4d")
REGISTRY_UUID = bytes.fromhex("d76a14cc264deea1c6406f8b461682c0")

MSG_REQ, MSG_RESP, MSG_FRAG, MSG_EVENT, MSG_SAMPLE = 1, 2, 3, 5, 9
MAX_MSG = 1000
FRAG_CHUNK = 768

REQ_GET_SERVER = 3            # server 1, {0: uuid} -> {0: handle}
REQ_SENSOR_INFO = 4           # server = sensor id -> {0: name, 1: type, ...}
REQ_SENSOR_LIST = 5           # server = sensor manager -> {0: [u32 ids]}
REQ_START_SAMPLING = 8        # server = sensor id, {0: mode, 1: period ns, 2: max latency ns} -> {0: handle}
REQ_STOP_SAMPLING = 9         # server = sensor id, {0: handle}
REQ_REGISTRY_LOAD_SCRIPT = 0x18   # server = registry, {0: script\0, 1: cdt, 2: mlb}

MODE_CONTINUOUS = 1
MODE_ON_CHANGE = 2


class UsfError(Exception):
    pass


# ---- FlatBuffers: tiny builder ([root][vtable][table][vectors]) and reader ----
_SIZES = {"i32": 4, "u32": 4, "i64": 8, "u8": 1, "f32": 4, "bytes": 4}
_FMT = {"i32": "<i", "u32": "<I", "i64": "<q", "u8": "<B", "f32": "<f"}


def fb_table(fields):
    """fields: [(index, kind, value)], kind in i32 u32 i64 u8 f32 bytes."""
    fields = sorted(fields, key=lambda f: f[0])
    nslots = fields[-1][0] + 1 if fields else 0
    vt_size = 4 + 2 * nslots
    tbl_pos = 4 + vt_size
    tbl_pos += (-tbl_pos) % 8
    inline, slot_off, vecs = bytearray(), {}, []
    for idx, kind, val in fields:
        size = _SIZES[kind]
        inline += b"\0" * ((-(tbl_pos + 4 + len(inline))) % size)
        slot_off[idx] = 4 + len(inline)
        if kind == "bytes":
            vecs.append((slot_off[idx], bytes(val)))
            inline += b"\0" * 4
        else:
            inline += struct.pack(_FMT[kind], val)
    tbl_size = 4 + len(inline)
    buf = bytearray(tbl_pos + tbl_size)
    struct.pack_into("<I", buf, 0, tbl_pos)
    buf[4:4 + vt_size] = struct.pack("<HH", vt_size, tbl_size) + b"".join(
        struct.pack("<H", slot_off.get(i, 0)) for i in range(nslots))
    struct.pack_into("<i", buf, tbl_pos, tbl_pos - 4)
    buf[tbl_pos + 4:tbl_pos + tbl_size] = inline
    for off, data in vecs:
        buf += b"\0" * ((-len(buf)) % 4)
        vpos = len(buf)
        buf += struct.pack("<I", len(data)) + data
        struct.pack_into("<I", buf, tbl_pos + off, vpos - (tbl_pos + off))
    buf += b"\0" * ((-len(buf)) % 4)
    return bytes(buf)


class Table:
    def __init__(self, buf, pos=None):
        self.b = buf
        self.p = struct.unpack_from("<I", buf, 0)[0] if pos is None else pos
        self.vt = self.p - struct.unpack_from("<i", buf, self.p)[0]
        self.vts = struct.unpack_from("<H", buf, self.vt)[0]

    def off(self, i):
        o = 4 + 2 * i
        return struct.unpack_from("<H", self.b, self.vt + o)[0] if o < self.vts else 0

    def scalar(self, i, fmt="<i", default=None):
        o = self.off(i)
        return struct.unpack_from(fmt, self.b, self.p + o)[0] if o else default

    def bytes(self, i):
        o = self.off(i)
        if not o:
            return None
        vp = self.p + o + struct.unpack_from("<I", self.b, self.p + o)[0]
        n = struct.unpack_from("<I", self.b, vp)[0]
        return self.b[vp + 4:vp + 4 + n]

    def u32_vector(self, i):
        o = self.off(i)
        if not o:
            return []
        vp = self.p + o + struct.unpack_from("<I", self.b, self.p + o)[0]
        n = struct.unpack_from("<I", self.b, vp)[0]
        return list(struct.unpack_from(f"<{n}I", self.b, vp + 4))

    def string(self, i):
        v = self.bytes(i)
        return None if v is None else v.split(b"\0", 1)[0].decode(errors="replace")


def parse_samples(payload):
    """Sample event payload: u32 1, u32 0, u32 handle, u16 sensor type,
    u8 count, u8 value bytes per sample, then per sample u64 timestamp +
    f32 values. Batches (max latency > 0) carry many samples and arrive
    fragmented."""
    _, _, handle = struct.unpack_from("<III", payload, 0)
    stype, count, size = struct.unpack_from("<HBB", payload, 12)
    out, pos = [], 16
    for _ in range(count):
        if pos + 8 + size > len(payload):
            break
        ts = struct.unpack_from("<Q", payload, pos)[0]
        out.append((handle, ts, struct.unpack_from(f"<{size // 4}f", payload, pos + 8)))
        pos += 8 + size
    return out


class Usf:
    def __init__(self, dev=DEV):
        self.fd = os.open(dev, os.O_RDWR | os.O_CLOEXEC)
        self.req_id = int(time.time()) & 0xffff
        self.pending = []   # sample/event messages that arrived during a request

    def close(self):
        os.close(self.fd)

    def _send(self, msg):
        if len(msg) <= MAX_MSG:
            os.write(self.fd, msg)
            return
        for off in range(0, len(msg), FRAG_CHUNK):
            frag = fb_table([(0, "i32", 1), (1, "i32", len(msg)), (2, "i32", off),
                             (3, "bytes", msg[off:off + FRAG_CHUNK])])
            os.write(self.fd, fb_table([(0, "u32", MSG_FRAG), (1, "bytes", frag)]))

    def recv(self, timeout):
        """Next complete UsfMsg (fragments reassembled), None on timeout."""
        end = time.monotonic() + timeout
        while True:
            r, _, _ = select.select([self.fd], [], [], max(0.0, end - time.monotonic()))
            if not r:
                return None
            d = os.read(self.fd, 1024)
            m = Table(d)
            if m.scalar(0, "<I", 0) != MSG_FRAG:
                return d
            f = Table(m.bytes(1))
            total, off, chunk = f.scalar(1), f.scalar(2, default=0), f.bytes(3)
            if off == 0:
                self.frag = bytearray()
            if getattr(self, "frag", None) is None or off != len(self.frag):
                self.frag = None          # lost a piece: drop the message
                continue
            self.frag += chunk
            if len(self.frag) >= total:
                d, self.frag = bytes(self.frag[:total]), None
                return d

    def request(self, req_type, server, payload=None, timeout=5.0):
        """Returns (status, payload table or None). Raises UsfError on timeout."""
        self.req_id = (self.req_id + 1) & 0x7fffffff
        f = [(0, "i32", req_type), (1, "i32", self.req_id), (2, "i32", server)]
        if payload is not None:
            f.append((3, "bytes", payload))
        self._send(fb_table([(0, "u32", MSG_REQ), (1, "bytes", fb_table(f))]))
        end = time.monotonic() + timeout
        while True:
            d = self.recv(end - time.monotonic())
            if d is None:
                raise UsfError(f"no response to request {req_type} (server {server})")
            m = Table(d)
            if m.scalar(0, "<I", 0) != MSG_RESP:
                self.pending.append(d)
                continue
            r = Table(m.bytes(1))
            if r.scalar(1) != self.req_id:
                continue
            p = r.bytes(3)
            return r.scalar(2, default=0), (Table(p) if p else None)

    def get_server(self, uuid):
        st, p = self.request(REQ_GET_SERVER, 1, fb_table([(0, "bytes", uuid)]))
        if st or p is None:
            raise UsfError(f"get server failed: {st}")
        return p.scalar(0)

    def sensor_list(self):
        st, p = self.request(REQ_SENSOR_LIST, self.get_server(SENSOR_MGR_UUID))
        return p.u32_vector(0) if p else []

    def sensor_info(self, sid):
        st, p = self.request(REQ_SENSOR_INFO, sid)
        if st or p is None:
            return None
        return {"id": sid, "name": p.string(0), "type": p.scalar(1)}

    def find_sensor(self, name):
        for sid in self.sensor_list():
            i = self.sensor_info(sid)
            if i and i["name"] == name:
                return sid
        return None

    def start(self, sid, mode=MODE_CONTINUOUS, period_ns=0, latency_ns=0):
        p = fb_table([(0, "i32", mode), (1, "i64", period_ns), (2, "i64", latency_ns)])
        st, r = self.request(REQ_START_SAMPLING, sid, p)
        if st or r is None:
            raise UsfError(f"start sampling on {sid} failed: {st}")
        return r.scalar(0)

    def stop(self, sid, handle):
        st, _ = self.request(REQ_STOP_SAMPLING, sid, fb_table([(0, "i32", handle)]))
        return st

    def next_samples(self, timeout):
        """[(handle, ts, values)] from the next sample event, [] on timeout."""
        d = self.pending.pop(0) if self.pending else self.recv(timeout)
        if d is None:
            return []
        m = Table(d)
        if m.scalar(0, "<I", 0) != MSG_SAMPLE:
            return []
        return parse_samples(m.bytes(1))


# ---- registry (what SensorHal::LoadRegistry does) ----
PLAT = "/sys/firmware/devicetree/base/chosen/plat/"


def device_cdt():
    def item(n):
        return struct.unpack(">I", open(PLAT + n, "rb").read(4))[0]
    return (item("product") << 16 | item("stage") << 12 | item("major") << 8 |
            item("minor") << 4 | item("variant"))


def device_mlb():
    # libusf: pcbcfg "MLB<n>" -> n, anything else -> skip MLB comparison (-1)
    try:
        cfg = open("/sys/firmware/devicetree/base/chosen/config/pcbcfg", "rb").read()
        cfg = cfg.split(b"\0", 1)[0].decode()
        return int(cfg[3:]) if cfg.startswith("MLB") else -1
    except (OSError, ValueError):
        return -1


def load_registry(u, scripts, log=print):
    """scripts: ordered list of .reg paths. Ends with "/" loaded=1."""
    rs = u.get_server(REGISTRY_UUID)
    cdt, mlb = device_cdt(), device_mlb()
    log(f"registry server {rs}, cdt 0x{cdt:x}, mlb {mlb}")
    for path in scripts + [None]:
        text = (open(path, "rb").read() if path else b"+/\nloaded=1\n") + b"\0"
        p = fb_table([(0, "bytes", text), (1, "i32", cdt), (2, "i32", mlb)])
        st, _ = u.request(REQ_REGISTRY_LOAD_SCRIPT, rs, p, timeout=8)
        if st:
            raise UsfError(f"loading {path or 'loaded=1'} failed: {st}")
        log(f"loaded {path or '/ loaded=1'}")
