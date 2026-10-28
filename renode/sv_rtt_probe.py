#
# sv_rtt_probe.py - Renode-side stand-in for a J-Link in the SystemView setup.
#
# The firmware is the J-Link build: SystemView writes events into its RTT
# up-buffer in RAM and polls its RTT down-buffer for host commands; no
# target code moves data anywhere. This script does what the probe does over
# SWD: it reads and writes the `_SEGGER_RTT` control block through the system
# bus from a host thread, without stopping or hooking the CPU.
#
# Host side it serves SystemView's IP Recorder protocol, so SystemView
# connects with Target > Recorder Configuration > IP, 127.0.0.1:<port>:
#   host -> 32-byte hello, target <- 32-byte hello (sent in one write)
#   host -> [len][len bytes] frames, written into the "SysView" down-buffer
#   up-buffer bytes -> host, unframed
#
#   (monitor) include @renode/sv_rtt_probe.py
#   (monitor) sv_rtt_probe 19111            # after the ELF is loaded
#
# Runs in Renode's IronPython 2.7.
#
import socket, threading, time

SV_HELLO_SIZE = 32
SV_CMD_STOP = 2

# SEGGER_RTT_CB on a 32-bit target (SEGGER_RTT.h)
RTT_ID = "SEGGER RTT"
RTT_OFF_MAX_UP = 16
RTT_OFF_MAX_DOWN = 20
RTT_OFF_BUFFERS = 24
RTT_BUF_SIZE = 24          # sName, pBuffer, SizeOfBuffer, WrOff, RdOff, Flags
B_NAME, B_PBUF, B_SIZE, B_WROFF, B_RDOFF = 0, 4, 8, 12, 16


class RttChannel(object):
    """One RTT ring buffer, accessed from the host side like a debug probe."""

    def __init__(self, bus, desc):
        self.bus = bus
        self.desc = desc
        self.base = bus.ReadDoubleWord(desc + B_PBUF)
        self.size = bus.ReadDoubleWord(desc + B_SIZE)

    def _rd(self, off):
        return self.bus.ReadDoubleWord(self.desc + off)

    def _wr(self, off, value):
        self.bus.WriteDoubleWord(self.desc + off, value)

    def read_up(self, limit):
        """Up-buffer: target writes WrOff, host consumes and advances RdOff."""
        wr, rd = self._rd(B_WROFF), self._rd(B_RDOFF)
        if wr == rd or wr >= self.size or rd >= self.size:
            return bytearray()
        n = (wr - rd) if wr > rd else (self.size - rd)     # up to the wrap point
        n = min(n, limit)
        data = bytearray(self.bus.ReadBytes(self.base + rd, n))
        rd += n
        if rd == self.size:
            rd = 0
        self._wr(B_RDOFF, rd)
        return data

    def discard_up(self):
        self._wr(B_RDOFF, self._rd(B_WROFF))

    def write_down(self, data):
        """Down-buffer: host writes data and advances WrOff. Returns bytes written."""
        wr, rd = self._rd(B_WROFF), self._rd(B_RDOFF)
        free = (rd - wr - 1) % self.size                   # one byte is never used
        n = min(free, len(data))
        for i in range(n):
            self.bus.WriteByte(self.base + wr, data[i])
            wr = (wr + 1) % self.size
        if n:
            self._wr(B_WROFF, wr)
        return n


def _read_cstr(bus, addr, limit=32):
    out = []
    for i in range(limit):
        c = bus.ReadByte(addr + i)
        if c == 0:
            break
        out.append(chr(c))
    return "".join(out)


def _find_sysview(bus, cb, name="SysView"):
    """Locate the SystemView up/down channels by name, as SystemView does."""
    if _read_cstr(bus, cb, 16) != RTT_ID:
        return None                                        # SEGGER_RTT_Init not run yet
    n_up = bus.ReadDoubleWord(cb + RTT_OFF_MAX_UP)
    n_down = bus.ReadDoubleWord(cb + RTT_OFF_MAX_DOWN)
    if n_up > 16 or n_down > 16:
        return None
    up = down = None
    for i in range(n_up + n_down):
        desc = cb + RTT_OFF_BUFFERS + i * RTT_BUF_SIZE
        p = bus.ReadDoubleWord(desc + B_NAME)
        if p and _read_cstr(bus, p) == name and bus.ReadDoubleWord(desc + B_SIZE):
            if i < n_up and up is None:
                up = RttChannel(bus, desc)
            elif i >= n_up and down is None:
                down = RttChannel(bus, desc)
    if up is None or down is None:
        return None                                        # SEGGER_SYSVIEW_Init not run yet
    return up, down


def _target_hello(version):
    major, minor = version.split(".")[:2]
    h = bytearray("SEGGER SystemView V%s.%02d.00" % (major, int(minor)), "ascii")
    return bytes(h + bytearray(SV_HELLO_SIZE - len(h)))


class SysViewProbe(object):
    def __init__(self, bus, port, poll_ms, version):
        self.bus = bus
        self.port = port
        self.poll = poll_ms / 1000.0
        self.hello = _target_hello(version)

    def info(self, msg):
        self.bus.InfoLog("sv_rtt_probe: " + msg)

    def channels(self):
        """Wait for the target's RTT/SystemView init. Re-resolved per session,
        so a reset or a newly loaded ELF is picked up."""
        while True:
            cpu = list(self.bus.GetCPUs())[0]
            found, addrs = self.bus.TryGetAllSymbolAddresses("_SEGGER_RTT", context=cpu)
            addrs = list(addrs) if found else []
            if addrs:
                ch = _find_sysview(self.bus, addrs[0])
                if ch is not None:
                    return ch
            time.sleep(0.1)

    def serve(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", self.port))
        srv.listen(1)
        self.info("SystemView IP Recorder on 127.0.0.1:%d" % self.port)
        while True:
            conn, peer = srv.accept()
            self.info("host connected from %s:%d" % peer)
            try:
                self.session(conn)
            except Exception as e:                     # host gone or socket error
                self.info("session ended: %s" % e)
            finally:
                try:
                    conn.close()
                except Exception:
                    pass

    def session(self, conn):
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        hello = bytearray()
        while len(hello) < SV_HELLO_SIZE:
            d = conn.recv(SV_HELLO_SIZE - len(hello))
            if not d:
                return
            hello += bytearray(d)
        up, down = self.channels()
        up.discard_up()                                # nothing from before this session
        conn.sendall(self.hello)
        conn.settimeout(self.poll)
        rx = bytearray()                               # host frames not yet parsed
        pending = bytearray()                          # payload waiting for down-buffer room
        sent = 0
        try:
            while True:
                try:
                    d = conn.recv(256)
                    if not d:
                        break
                    rx += bytearray(d)
                except socket.timeout:
                    pass
                while len(rx) >= 1 and len(rx) >= 1 + rx[0]:
                    pending += rx[1:1 + rx[0]]
                    rx = rx[1 + rx[0]:]
                if pending:
                    pending = pending[down.write_down(pending):]
                while True:                            # drain like the probe: as fast as it can
                    data = up.read_up(4096)
                    if not data:
                        break
                    conn.sendall(bytes(data))
                    sent += len(data)
        finally:
            # The host may vanish without sending STOP; stop the target like
            # SystemView does on disconnect so it doesn't fill the ring for nobody.
            down.write_down(bytearray([SV_CMD_STOP]))
            self.info("host disconnected after %d bytes" % sent)


def mc_sv_rtt_probe(port=19111, poll_ms=1, version="3.32"):
    bus = monitor.Machine.SystemBus
    probe = SysViewProbe(bus, int(port), int(poll_ms), str(version))
    t = threading.Thread(target=probe.serve, name="sv_rtt_probe")
    t.daemon = True
    t.start()
