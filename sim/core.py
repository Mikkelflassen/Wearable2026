"""Python copy of firmware/bracelet/Core.h, used by the terminal simulator.

Keep the two in step: if you change the rules in Core.h, change them here too
(tests/test_interop.py runs the real C++ logic against this one to catch drift).
"""

NONE, TALK, TOPIC, PAUSE = "none", "talk", "topic", "pause"

HEARTBEAT_MS = 1000
RESEND_MS = 120
PEER_TIMEOUT_MS = 5000
TOPIC_SETTLE_MS = 1500
TOPIC_GRACE_MS = 8000
EVENT_REPEAT_MS = 80
EVENT_REPEATS = 3
FEEDBACK_MERGE_MS = 3000


class Peer:
    def __init__(self, now):
        self.talk = 0
        self.topic = 0
        self.pause = False
        self.epoch = 0
        self.last_seen = now
        self.seen = []


class Core:
    def __init__(self, ident, now, event_seed=0):
        self.id = ident
        self.snap = NONE
        self.talk = 0
        self.topic = 0
        self.pause = False
        self.epoch = 0
        self.max_ticket = 0
        self.synced = False
        self.was_waiting = False
        self.topic_notified = False
        self.in_transit = False
        self.transit_until = 0
        self.empty_since = None
        self.last_state_at = now
        self.resend_at = None
        self.next_eid = event_seed
        self.peers = {}
        self.fed_at = {}  # last time each kind of feedback was felt
        self.pending = []  # [eid, type, left, at]
        self.lines = []  # packets to broadcast
        self.patterns = []  # vibration patterns to play
        self._send_state(now)

    # ---- inputs from the wearer -------------------------------------------
    def set_snap(self, snap, now):
        if snap == self.snap:
            return
        old, self.snap = self.snap, snap
        if old == TALK:
            self.talk = 0
        if old == TOPIC:
            if self.topic_notified and snap == NONE:
                self.in_transit = True
                self.transit_until = now + TOPIC_GRACE_MS
            else:
                self.topic = 0
                self.topic_notified = False
        if snap != NONE and self.in_transit:
            self.in_transit = False
            self.topic = 0
            self.topic_notified = False
        if snap == TALK:
            self.talk = self._next_ticket()
        if snap == TOPIC:
            self.topic = self._next_ticket()
        if snap == PAUSE:
            self.pause = True
        self._changed(now)

    def press(self, kind, now):
        if kind not in ("A", "D", "C") or self.is_speaker():
            return
        self.next_eid = (self.next_eid + 1) & 0xFFFF
        self.pending.append([self.next_eid, kind, EVENT_REPEATS, now])
        self._send_events(now)

    def clear_pause(self, now):
        if not self.pause:
            return
        self.pause = False
        self.patterns.append("ACK")
        self._changed(now)

    # ---- input from the radio / serial ------------------------------------
    def on_line(self, line, now):
        parts = line.split()
        if len(parts) < 2 or parts[0] != "W26":
            return
        try:
            if parts[1] == "S" and len(parts) == 7:
                ident, talk, topic, pause, epoch = (int(x) for x in parts[2:7])
                if ident in (self.id, 0):
                    return
                p = self._peer(ident, now)
                p.talk, p.topic, p.pause, p.epoch = talk, topic, pause != 0, epoch
                self.max_ticket = max(self.max_ticket, talk, topic)
                if epoch > self.epoch:
                    notify = self.synced or self.pause
                    self.epoch = epoch
                    self.pause = False
                    if notify:
                        self.patterns.append("BREAK")
                    self._send_state(now)
                self.synced = True
                self._evaluate(now)
            elif parts[1] == "E" and len(parts) == 5:
                ident, eid, kind = int(parts[2]), int(parts[3]), parts[4]
                if ident in (self.id, 0):
                    return
                p = self._peer(ident, now)
                if eid in p.seen:
                    return
                p.seen = (p.seen + [eid])[-4:]
                if self.is_speaker():
                    name = {"A": "AGREE", "D": "DISAGREE", "C": "COMMENT"}.get(kind)
                    last = self.fed_at.get(name)
                    if name and (last is None or now - last >= FEEDBACK_MERGE_MS):
                        self.fed_at[name] = now
                        self.patterns.append(name)
        except ValueError:
            return

    def tick(self, now):
        if self.in_transit and now >= self.transit_until:
            self.in_transit = False
            self.topic = 0
            self.topic_notified = False
            self._changed(now)
        self._send_events(now)
        if self.resend_at is not None and now >= self.resend_at:
            self._send_state(now)
        if now - self.last_state_at >= HEARTBEAT_MS:
            self._send_state(now)
        self._evaluate(now)

    # ---- outputs ----------------------------------------------------------
    def pop_lines(self):
        out, self.lines = self.lines, []
        return out

    def pop_patterns(self):
        out, self.patterns = self.patterns, []
        return out

    # ---- status -----------------------------------------------------------
    def is_speaker(self):
        return self.talk > 0 and self._head("talk") == self.id

    def group_size(self):
        return 1 + len(self.peers)

    def pause_votes(self):
        return int(self.pause) + sum(
            1 for p in self.peers.values() if p.pause and p.epoch == self.epoch
        )

    def position(self, queue):
        mine = getattr(self, queue)
        if not mine:
            return 0
        return 1 + sum(
            1
            for i, p in self.peers.items()
            if getattr(p, queue) and (getattr(p, queue), i) < (mine, self.id)
        )

    def status(self):
        return "id=%d snap=%s talkPos=%d topicPos=%d break=%d/%d%s" % (
            self.id,
            self.snap,
            self.position("talk"),
            self.position("topic"),
            self.pause_votes(),
            self.group_size(),
            " (I want a break)" if self.pause else "",
        )

    # ---- internals --------------------------------------------------------
    def _peer(self, ident, now):
        p = self.peers.get(ident)
        if p is None:
            p = self.peers[ident] = Peer(now)
        p.last_seen = now
        return p

    def _next_ticket(self):
        self.max_ticket += 1
        return self.max_ticket

    def _head(self, queue):
        entries = [(getattr(p, queue), i) for i, p in self.peers.items() if getattr(p, queue)]
        if getattr(self, queue):
            entries.append((getattr(self, queue), self.id))
        return min(entries)[1] if entries else 0

    def _changed(self, now):
        self._send_state(now)
        self.resend_at = now + RESEND_MS
        self._evaluate(now)

    def _evaluate(self, now):
        for i in [i for i, p in self.peers.items() if now - p.last_seen > PEER_TIMEOUT_MS]:
            del self.peers[i]

        speaker = self.is_speaker()
        if speaker and self.was_waiting:
            self.patterns.append("YOUR_TURN")
        self.was_waiting = self.talk > 0 and not speaker

        if self._head("talk") != 0:
            self.empty_since = None
            if self.topic_notified and self.snap == TOPIC:
                self.topic_notified = False
        else:
            if self.empty_since is None:
                self.empty_since = now
            if (
                self.snap == TOPIC
                and self.topic > 0
                and not self.topic_notified
                and self._head("topic") == self.id
                and now - self.empty_since >= TOPIC_SETTLE_MS
            ):
                self.topic_notified = True
                self.patterns.append("TOPIC_TURN")

        n = self.group_size()
        if n >= 2 and self.pause_votes() * 2 > n:
            self.epoch += 1
            self.pause = False
            self.synced = True
            self.patterns.append("BREAK")
            self._send_state(now)
            self.resend_at = now + RESEND_MS

    def _send_state(self, now):
        self.lines.append(
            "W26 S %d %d %d %d %d" % (self.id, self.talk, self.topic, int(self.pause), self.epoch)
        )
        self.last_state_at = now
        self.resend_at = None

    def _send_events(self, now):
        for e in self.pending:
            if e[2] > 0 and now >= e[3]:
                self.lines.append("W26 E %d %d %s" % (self.id, e[0], e[1]))
                e[2] -= 1
                e[3] = now + EVENT_REPEAT_MS
        self.pending = [e for e in self.pending if e[2] > 0]


COMMANDS = {
    "talk": lambda c, now: c.set_snap(TALK, now),
    "topic": lambda c, now: c.set_snap(TOPIC, now),
    "pause": lambda c, now: c.set_snap(PAUSE, now),
    "off": lambda c, now: c.set_snap(NONE, now),
    "a": lambda c, now: c.press("A", now),
    "d": lambda c, now: c.press("D", now),
    "c": lambda c, now: c.press("C", now),
    "unpause": lambda c, now: c.clear_pause(now),
}
